# NATIVE ROADMAP: from the PM4 hybrid to a 100% native shipping path

**Status:** working plan, 2026-10-01 (iter-463). This is the draft
`archive/v1_native/NATIVE_ROADMAP.md` with every item of its critique applied
(A1-A8, B1-B7, C1-C5, D1-D5, E1-E5, F). Five critique items were settled by static checks,
and their answers replace the open questions they raised (A7, B2, B4, C3, E5; details in
Appendix C).

## NATIVE STATUS - generated from the obligation ledger (do not edit by hand)

The status of record is `docs/native_ledger.json`: one record per native obligation, its status COMPUTED by
`python tools/native_ledger.py` from the evidence the record cites (TOOLKIT 1.8i). Regenerate this block with
`python tools/native_ledger.py --write-status docs/NATIVE_ROADMAP.md`; `--strict` fails while it is stale. Where a
section below says more than this block, the block and the ledger win.

<!-- native-ledger:begin:roadmap -->
Computed from 26 records (digest `6924d13710d0e99e`): **closed 26, in progress 0, blocked 0, open 0.**
**Meter** (the default build, per ingame frame; roll mrb3/mrb_meter_on (native/mem-native fb2a626c, exe 023cf6bafe53: the mem.resources bake - SR_MEM_RESOURCES default ON - over dev 1c2da235's defaults), the latest default-path meter roll): pm4 0.0 | edram_res 0.0 | edram_xfer 0.0 | edram_load 0.0 | xma 0.0 | dispreg 0.0 | shm 0.0.

| Obligation | Status | Docs claim | First reason (`python tools/native_ledger.py -v --id <id>`) |
|---|---|---|---|
| `p1.s1_flush` S1 draw flush: the XDK draw flushes 820FC000 / 820FBBF8 write native draw records (indexed draws) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.rows12` Rows 1+2: the shader bind 82108668 (+82107110 / 82108470 / 82108300) and its constant loads as records | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.rows467` Rows 4+6+7: BeginVertices 820FB6E8 / DrawVerticesUP 820FBBB0 / EndVertices 820FBBA0 (the third draw flush) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.row3` Row 3: the tiled window-scissor section 821072F8 as Driver items (SR_NATIVE_WSCISSOR) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.row3_construns` Row 3 residue: shared native GpuCmdEmitShaderConstantRuns 821075E0 SDK entry and Driver section; default ON... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.row5` Row 5: the setters' one emitter D3DDeviceEmitScissorTileRects 820F0208 as an Entry (SR_NATIVE_TILERECTS) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.row3b` Row 3b: D3DDeviceSetPredication 8210D400 as an Entry (SR_NATIVE_TILEPRED) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.row9` Row 9: Resolve 8210C5F8, ClearF 82101A58 / Clear, SetShaderGPRAllocation 820F5570 (+ ClearFullSurface) as o... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p1.flush_residue` Draw-path PM4 leftovers: the S1 flushes' segment-limit section fallbacks (Driver constRuns / regRuns / fetc... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p2.ringless` P2-A: the ring writers 820FD218 / 82110140 as a host IB queue (SR_NATIVE_RINGLESS) - the CP parses 1 ring p... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p2.segmentless` P2-B step 1: GpuCommandBufferFlush 820FD8C0 as host code - the segment as records + FENCE / INVALIDATE (SR_... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p2.transport` Row 10 / P2-B transport: fences, BlockUntilIdle, the swap block and N-1 throttle, the Z-pass bracket replay... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p2.setconst_mem` Per-tile constants: D3DDeviceEmitPm4Packet 82113010 (Lion SetPerTileConstC100 827333E0) as kRkSetConstMem | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `p2.zpd` Occlusion queries: EVENT_WRITE_ZPD / EXT sample-count write-backs (HUD glow 82304058; the sun flare 8272111... | closed | done | all evidence passes; guards met; no open unknown / contradiction |
| `boot.device_create` Native device creation: the device-create producers of the boot PM4 block as host code - 820FDE90 (ME_INIT ... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `display.registers` Display registers: the V_COUNTER / viewport scan-out answers (SR_VD_SCANOUT_REGS) and the system command bu... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `edram.b1` B1: transfers into XDK clears elided (SR_NB_CLEAR_ELIDE), aliased clears as host clears (SR_NB_CLEAR_PROP),... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `edram.b2` B2 / B2+ / B3: resolves write native target textures (SR_NB_RESOLVE_TEX, DEPTH_SRV, ARRAY_LAYERS, OVERHANG,... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `edram.b4` B4: the front-buffer resolve writes a front target the scan-out reads (SR_NB_FRONT) | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `edram.b5` B5: scaled resolves from mixed owners take the native path (SR_NB_SCALED_REMAP); the 1x native resolve (SR_... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `edram.zero` EDRAM model out of the shipping path: AOT kernels (SR_NB_AOT), power-on zero (SR_NB_ZERO_INIT), the last ED... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `audio.xma` N6: XMA native - the register-0x600 lwbrx replaced (SR_XMA_ARRAY_NATIVE), the XMA* exports' native front en... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `defect.black_world` The black world: the CP waits for the live D3D12 backend before its first command (SR_PM4_WAIT_BACKEND) - t... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `mem.resources` GPU memory as resources: the vertex / index / texture bytes of every draw and the resolve write-back as hos... | closed | baked | all evidence passes; guards met; no open unknown / contradiction |
| `release.composition` C1 release composition: no emulation components in the executable, shipping DLLs or package, including disa... | closed | done | all evidence passes; guards met; no open unknown / contradiction |
| `acceptance.zero` C1: no emulation components in the release package and 0 emulated hardware work across whole runs - boot, m... | closed | done | all evidence passes; guards met; no open unknown / contradiction |
<!-- native-ledger:end -->

2026-10-12: `SR_NATIVE_SHADER_CONST_RUNS` is built as a shared SDK entry replacement
and remains default OFF. Direct and production-table indirect dispatch, scoped
SDK fallback and zero-primitive tessellated-caller state fixtures pass alongside
actual SDK comparisons and record execution. The isolated matrix passes 23
positive cases (6,020 assertions) and a separate expected deadlock control.
Historical renderer replay, bounded live frozen A/B, CPPM4, stress and the
existing flicker gate pass. Astra passes this engineering checkpoint. Rendered
tessellation, live caller distribution, arbitrary admission modes, live
internal chunk/backpressure, default baking and release composition
remain open; the ledger therefore keeps this row in progress. PM4 is still
nonzero. The diagnostic meter is not FPS/performance acceptance.

Fresh pure-ON segmentless/shared-SDK integration now passes a quiet 12-minute
strict flight soak and saved/new-game span/freeze gates on `37bc09a0a84c`.
The saved fixture is tool-assisted; the fresh start uses an empty private seed.
Producer ownership/order and first-application witnesses pass beside the
drawn-world checks. Periodic shader `chunked=0`, no final drained reconciliation,
unmatched live scenes and unchanged default-OFF flags limit this checkpoint;
neither larger obligation nor release milestone 2 closes.

**CONSTRUNS BAKED (2026-10-15, crb1).** The live bake set (v1_native/crb1, exe 636b6ef22bec vs dev exe 2087ca4acdb4,
staged on J:) passed every step: replay 9/9, the strict gate, the traced probe, CPPM4, STRESS and the per-present A/B
15/15, the census pair under `--construns-zero` (with the UNSTAMPED preamble check, Astra's P2), the meter; the three
residual unknowns resolved and p1.row3_construns CLOSED (merge 2026-10-15 on dev).

**CONSTRUNS BAKE PLAN (native/construns-bake, 2026-10-14).** `SR_NATIVE_SHADER_CONST_RUNS` is default ON (=0 keeps
the SDK body; the A/B unchanged). It removes 15.0 PM4 a present on the default path (the census: 820FC000's r20 /
r0A / 3C / r23 keys, 3 calls a present). The live proof is `native_proof.py --plan construns_bake` (prefix crb): one
exe, every item from it, the strict witness and the construns counters in every pure-ON roll, same-exe =0 arms (no
pin). Each residual unknown and what closes it:
- *default_bake* (live): replay 9/9 vs the dev 4aa4f963 build; the strict new-game gate (span + freeze,
  `SR_NATIVE_SEG_TILE=1`); CPPM4 exact with `cpPm4` > 0 (the CP renders kRkShaderConstSync back itself); STRESS exact;
  the frozen-pass per-present A/B (`SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS`, both arms counted); the PRODUCER census
  pair (=0 vs the default, `SR_NATIVE_BRACKETS=1`) graded by `transport_census.py --construns-zero`; the meter (=0 vs
  the default, pre-registered bound -10.0 a frame: 15.0 less a per-pair noise allowance; the census pair is the exact
  measure).
- *How the census pair grades it.* Each SDK body of 821075E0 writes its 13-dword preamble once per call (IDA: 0x2007 =
  r20, COHER r0A x2, WAIT_REG_MEM 3C). The S1 flushes' own fallback leaves write neither 3C nor r0A, but their register
  runs do write r20 / r23 (frs1 census0: 820FC000 r20 3.001, r23 3.001 against 3C 3.000). So the observation is no
  3C + r0A on the S1 producer lines in the target (`construns.sync_pk` = 0) while the base shows them. The target's
  r20 / r23 are reported as the fallbacks' residue (p1.flush_residue's).
- *segmentless_backpressure* (offline + live). The internal chunk path has no live case. IDA: the four callers
  (820FB6E8 / 820FBBF8 / 820FC000 / 820FC478) pass `rldicr r4,r27,9,5` behind `and r27, 63<<49`, so at most 6 groups
  and never 0. GpuCmdBufBeginNewSegment 820FCF90 sets dev+48 = dev+44 - 160 on both branches, and the SDK flushes at
  entry when cursor > dev+48. `tools/difftest/construns_bake_run.py` runs all 63 masks x cursor%8 0 / 4 at
  cursor == dev+48 against the SDK: none chunks, native or SDK. A 152-byte control does chunk.
  Live, every roll shows the bound's preconditions: `chunked=0`, `marginBad=0`. The entry-check segment switch shows
  under STRESS (`kick` > 0; compared against the SDK offline at both alignments). The gate and the traced probe carry
  the strict witness and the P2 trace's terminal reconciliation, with construns records riding every segment.
- *library_coverage* (offline + live). Every roll prints the live caller distribution: `drv` (the S1 Driver section),
  `sdk` (the shared leaf), `decl` by reason (off / oracle / mask / span / open / emit) and `abGuest`. Pure-ON rolls
  need decl 0 and abGuest 0. Rendered tessellation is a live counter, not an assertion: an observe override on
  D3DDeviceEmitDrawTessellated 820FC478 gives `tessDraws` / `tessSdk`, 0 in every roll, and no 820FC478 census line in
  either census roll (now mapped, so one would be named). Admission: the leaf takes any nonzero mask. The
  23-case unit matrix covers 64 single bits, pairs, patterns and internal multi-flush. The live domain is the 63
  caller masks above.
- *Honestly open after the bake:* eras off the rolled tapes (new game + the gameplay tape). A tessellated draw or an SDK
  body there is measured only by acceptance.zero's whole-run census. Whole-game fidelity stays release.composition /
  Native0's.
- *Offline on the worker:* `construns_bake_run.py` (the default, the caller domain, the counters, six compiled
  faults each failing exactly its assertions), `shader_const_runs_test_run.py`'s matrix, every difftest suite's exit
  code vs the dev 4aa4f963 baseline built in a worker tree, the release build, the forcing audit, the Python suites.
  **Results (lane build of d6a7be6e, exe 636b6ef22bec; outputs `archive/crb/` of this worktree):**
  - `bake_run3` valid. The four modes pass 13 / 13 / 7 / 14 assertions. The domain is 126 cases, SDK chunked 0, native
    chunked 0, r3 equal 126. All six faults are detected, each failing exactly its named assertions. The source is
    restored and the rebuilt exe passes every mode again.
  - `bake_run2` is void: one restored cppm4 process exited 0 with an empty stdout, cause unknown. Six reruns of that
    exe pass 13 / 0 (`rerun_cppm4`).
  - The construns matrix (`scr_matrix`): 24 rows, all valid.
  - Difftest suites vs the dev 4aa4f963 baseline (worker tree crb0, exe fa0f0e8fc636): 78 suites, exit codes
    identical, 59 exit 0 in both.
  - Python suites: 99, exit codes identical (63 exit 0). test_native_proof's one failure there, "ledger paths start
    at _worktrees/", is the worker tree's path, in both trees; locally 0 failed.
  - Release build (`build-rel`, SR_RELEASE_COMPOSITION=ON): exit 0. The 820FC478 observe override is SR_ORACLE-only,
    as the generated shim it replaces was.
  - Forcing audit: count 0, baseline 0. gen_override_manifest and gen_native_brackets reproduce the committed
    OVERRIDES.json and native_brackets.gen.cpp.

**P2-B segmentless baked and closed (2026-10-14): the bake meter passes on the pre-registered re-measure**: the
ledger record p2.segmentless closes. The sgb4 pool (below) failed its bound; native/coher-sentinel (c8fb9c43) then
fixed a default-path defect of the segmentless close (the all-ones coherency sentinel's bogus invalidate after a
native reset, PM4 only the ON arm carried). Under a rule written before any roll (v1_native/sgb5/NOTES.txt: the same
frame-weighted pool of four pairs, the same bound -58.4, none dropped) on exe 4292f86c1eec (dev 00865e25), the pairs
read -58.9 / -60.2 / -61.2 / -57.9, pooled -59.59. This is a post-fix measurement on updated dev, not an isolated
measurement of the fix: row 9's record mode also changed both arms between the exes. sgb4's pool stays in the
ledger as an expected-failure witness (contradiction bake_meter_pre_sentinel). The coherency fix's own live
validation: v1_native/csn2. The frame-evidence recovery (tools/frame-evidence) restored the two SDK segmentless
gates, whose frames the GC had deleted.

**P2-B segmentless re-bake (2026-10-13): the strict re-proof passes, the PM4 meter misses its census bound**: `SR_NATIVE_SEGMENTLESS` is default ON again (92e35dbd; =0 keeps the
guest body for the A/B, and segmentless implies ringless). The 2026-10-11 bake was reverted over the save-started
gate's frozen-pass drift. That drift was the freeze oracle's own instrument: the sea's wall-clock shader time 0x29
(held since f2b83734) and the eye adaptation (held since 871bd0cf), not segmentless. The strict re-proof on exe
177f74cd14c1 (5dc672a6: the full witness in every roll - tile, first application, close-id order, the drained
reconciliation at BlockUntilIdle, and a FORCED drained point every 10 s through gameplay, where the game itself
never drains; `archive/v1_native/sgb4`, ledger `p2.segmentless`; sgb3 on b556bf471207 lacked the forced points):
- replay 9/9 byte-identical vs the pre-bake worker build; the new-game gate span 0 of 358 and freeze 15/15 (world
  14); the save-started gate span 0 of 358 and freeze 15/15 (world 15); the sea arms ON and OFF 16/16 each, every
  freeze 0 differing pairs. Every roll's last tile line has gap / overlap / order / dup / reorder / idreorder 0,
  drainMiss 0 and forcedMiss 0 (23 forced drained points in the gate, 25 in the save gate).
- The 13-minute soak: NEVER-EXECUTED=0, no drop, no hang trigger; 1.86 M segments and 168 M records with every
  witness counter 0; 80 forced drained points, every one registered == first-applied, the last at present 62,300
  of 62,598; the P2 trace's first takes of all 1,861,400 segments in close order (late 0, skipped 0) and nothing
  published before the last drained point left untaken.
- The meter misses: the default path against `SR_NATIVE_SEGMENTLESS=0` removes 54.8 PM4 a frame (the
  frame-weighted pool of four pairs on the proof exe, -52.4 to -57.6; the rule was written before any sgb4 roll;
  b556bf471207's pool read -54.4) against the bound -58.4 (the census sgc1's -61.5 on exe 0ec08d087f23, less 5%).
  Per kind the removal matches the census for sync (25 of 26.8), other (8.8 of 8.9) and bin (6.9 of 7.9); t0
  removes 14-16 of 17.9 and the t2nop padding moves by -1 to +3 a frame. `p2.segmentless` stays `built`, not
  closed, on the meter.
Contradiction `save_gate_drift` is resolved. The two old rolls that now grade ROLL FAIL (sgb1's gate span, sgd6b's
ON sea arm) are superseded stalled runs with their cause unresolved (each had a ~3 s stall; sgb1's coincided with a
whole-process audio delay, sgd6b's 9.7 s audio delay came after its hang report); they stay as void witnesses. Still open and owned elsewhere: the Z-pass bracket replay's lookup by physical address and the
native pump's generation identity (`p2.transport`), and every PM4 the gaps still carry (the C1 meter).

## RE-PLAN 2026-10-08 (after the flicker): the shortest path to 0 PM4 packets

**Goal (owner, confirmed 2026-10-08): full library replacement.** The game's graphics calls go
straight to native code that builds the D3D12 work: no ring, no PM4 encode, no PM4 parse.
- The Xenos register file stays only as the host device's state container. The translated shaders
  read constants and fetch constants in that layout, so it is the shader ABI, not emulation.
- Strand B removes EDRAM. This plan is strand A's half.

**The lesson of the flicker (PROGRESS 2026-10-08, ffa023de).** The defect lived where native and
guest code SHARE bookkeeping in a half-native stage:
- an S1 section aborted to the guest leaf at the ring's segment limit;
- the dedup mirror had already been told that the section's groups were emitted.

The segment limit and the XDK-length padding exist only because of the ring. Rules for every stage
below:
1. **A converted entry is total.** It never hands half its work back to guest code. A case it cannot
   handle declines the WHOLE call before it writes anything, and drops every native cache it could
   have touched.
2. **Mirrors and caches update at commit, never while planning** (ENGINE_MAP).
3. **Each stage's CHECK covers EXECUTION, not only generation.** The S1 CHECK passed for days
   because it never applied the records it compared.
4. **Every stage passes `tools/perf/flicker_gate.py`** (dense freeze, exact) before it is baked
   (CLAUDE.md a9c1f34a). Its per-present A/B half grades on the same-transport control since 2026-10-10 (rule R,
   `freeze_diff.py --ab-control`, 9fa67185): a pair p->p+1 counts only when p-1->p+1 is exact, a differing static pair
   beyond the unchanged tolerance FAILS, a freeze below 20 static pairs or with a control above the spike tolerance
   (the frame moved) is NO DATA, a roll below 200 static pairs is NO DATA; the excluded pairs and the worst control
   are always reported. Why: two stress A/Bs failed on a renderer-side LSB transient that both transports share
   (ledger abps_transient / abp_transient); the rule keeps the flicker rolls failing and voids the black-flip roll.
   **World rule, menu cap (2026-10-10, the orchestrator; landed with the owed re-rolls):** of the graded freezes that
   show the world, at most 4 may be the pause menu over it (`world_floor.MAX_MENU_FREEZES`); more is MENU-HEAVY and
   FAILS (a static menu over a dimmed world is not the draw path under test). The menu-heavy rolls it caught
   (gate_trunk, p1b_abp, fg42: gameplay 2 + menu 10, the player tape's own Esc round trip) are witnesses in the ledger,
   superseded by ow1_gate / ow1_abp12 on the pause-free tape (gameplay 12 + menu 0 each).

(Ledger, 2026-10-09: the S1 draw flush itself, baked before these rules, had never had the frozen-pass per-present A/B against the PM4 path - paid 2026-10-10 by s1b_abp and s1b_abps (PASS 11/11 each under rule R, gameplay 11 + menu 0; p1.s1_flush closed, claimed baked); its proof is the dense freeze, the dry CHECK and the execution dedup CHECK.
That A/B is owed, like every other P1 entry's.)

**What is left** (meter kinds, roll dc1: 6,873 PM4 packets per ingame frame; indexed draws are
already 0):

| Kind | Packets per frame |
|---|---|
| type-0 register runs: setters and non-draw flushes | 2,410 |
| bin mask / select: Z-pass predication | 1,523 |
| shader load: the shader bind 82108668 | 975 |
| constant loads | 823 |
| other type-3 | 388 |
| NOP pads | 355 |
| auto-indexed draws: UP ~80, Begin/End ~200, cloth | 245 |
| sync: fences, waits, interrupt, swap | 74 |
| events: resolve / EDRAM (strand B) | 31 |

**Stages:**

| Stage | What | Removes | Oracle | Size |
|---|---|---|---|---|
| **P0 producer census** | Observe-only, one roll. The tier-1 brackets (section 4.1, `SR_NATIVE_BRACKETS`: the dev+0x28 cursor before / after every XDK entry) attribute each remaining PM4 byte to the entry that wrote it. Bytes outside any bracket are inlined sites. The output is the P1 work list, ordered by packets per frame | - | Per-entry sums equal the meter's count | 0.5 day |
| **P1 total records** | Every remaining producer writes S1 records. New record types, see the list below. Entries convert in census order, each one whole (rule 1) | The meter falls to the transport markers (~1 NFLU per span) | Per entry: (a) generation CHECK against the XDK bytes (the S1 model); (b) EXECUTION check by frozen-pass A/B (see below); (c) the flicker gate; (d) replay 9/9 untouched, since traces run no guest code | ~1 day per big type: setters, predication, shader bind, constants, UP / Begin-End |
| **P2 ring cutover** | Records travel through a host queue instead of the ring. Gate `SR_NATIVE_RINGLESS`, default OFF until proven. Details below | **0 PM4 packets interpreted.** The parser stays only as the oracle and for the replay fixtures | Frozen-pass A/B at frame boundaries (ring vs host queue: ledger and pixels identical); records produced = consumed; whole-run sheets every era; flicker gate; Xenia golden frames via replay (that path is unchanged) | The largest stage, several days |
| **P3 speed on the native path** | Four pieces, see below | CP / drain and guest tick | In-roll A/B (cpf, p50) plus the flicker gate per piece | Per piece |

P1's new record types:
- the shader bind: ucode address + hash;
- bin mask / select;
- constant loads: LOAD_ALU_CONSTANT values captured at record time;
- effect records: fences, interrupt, swap, events;
- UP / Begin-End draws: their vertex bytes copied into the arena;
- resolves, through strand B's interface.

P1's EXECUTION check, frozen-pass A/B: during a sim freeze, the entry's gate alternates per present.
The per-draw ledger (`SR_DRAW_LEDGER`) and the pixels of consecutive presents must be identical.

P2 in detail (contracts and design answers: "P2 CONTRACTS (research)" below):
- The XDK ring management is replaced by native code that runs against the consumer's progress:
  - the kick and the segment switch 820FCF90;
  - GpuRingWaitForSpace / WaitProgress;
  - the fence write-backs, BlockUntilIdle and the N-2 swap throttle;
  - the interrupt callback.
- Z-pass (BeginZPass 8210DF38 / EndZPass 8210E1A0) and the predication entries keep a host list of
  the bracket's records. The list runs twice: depth, then colour under the bin mask. Today the ring
  range is replayed instead.
- These go away: the NFLU markers, the XDK-length padding, the segment-limit aborts and the dedup's
  segment epoch. Rules 1 and 2 become structural, because no fallback is left.
- The CP thread becomes the record consumer: apply, emitDraw and drain on ONE thread (the drain's
  cache locality, see the executor finding below).

P3's four pieces:
- the per-object draw cache at the five-entry cut (draw builder S2 / S3: 96-98% of render objects'
  compiled bytes repeat, so only changed constants and the one dynamic fetch word are re-applied);
- strand B's pre-resolved draw tickets;
- dirty bits instead of the dedup mirror;
- Begin/End vertices written straight into host buffers.

**Skipped, or kept brief:**
- **SR_NATIVE_EXEC and FRONTEND are OFF the path.** After P2 the parse is only a record apply, and
  the consumer drains on its own thread as the CP does today. A second hand-off would add only what
  the executor measured (below). Parked, gated OFF, removed with the PM4 path's retirement.
- The draw builder's "records padded to the XDK length" step moves after P2 (into P3), without
  padding.
- The 1a 'NDRW' / mode-B projection (`SR_NATIVE_DRAW`) is superseded by records and not resumed.
- S2b (pre-resolved vertex staging in the record) folds into P3.
- The P1 mixed state (records and PM4 in one ring) cannot be avoided, but it is bounded: it lasts
  until the census list is done, and every entry converts whole.

**The executor's -8 fps, diagnosed from rolls q0 / q1.** Setup: EXEC fixed OFF / ON, 250 Hz host
profiler, exe c29dec2f0230, the same gameplay; ingame 125.7 vs 117.6 fps; profiles in
`archive/v1_native/q{0,1}/profile.txt`.

| Thread | EXEC OFF (q0) | EXEC ON (q1) |
|---|---|---|
| guest render thread (slot17, the frame limiter) | 100% busy | Same function distribution (no fence spin, no new hot spot); about +3% cycles per frame; waits ~3% of the time |
| GPU_CP: samples in `NtWaitForAlertByThreadId` | 19.5% | 61% (blocked on the executor at syncs and slot reservation, woken per kick) |
| GPU_CP: `emitDraw` self time | 3.2% | 7.6% (it writes ~2 KB queue slots that the executor reads on another core) |
| SR native exec: samples in the same wait | - | 35% (sleep / wake per empty spell); its drain otherwise profiles like q0's CP drain |
| Cycles over the era | CP alone: 327 G | CP + executor: 457 G (+40%) |

So the cost is the fine-grained hand-off: per-draw wakes and cross-core slot traffic. It is not
missing overlap. The lesson for P2: move work between threads in batches (per IB or per frame), and
keep apply and drain on one thread.

**P0 RESULT (2026-10-08, roll p0d) [measured].**
- Setup: exe 1b27b69dd6ab = dev 2ac2e541 + per-owner dwords in `[PM4_COVMAP]`; gameplay / port,
  `SR_NATIVE_BRACKETS=1`; ingame 6,656 frames (57 fps under the brackets).
- Tool: `tools/perf/pm4_producers.py <roll>`.
- Totals: **6,776 PM4 packets = 64.2K dwords (251 KB) per frame.** Only **4 packets per frame (0.06%) are
  UNSTAMPED** (a ring-level SET_BIN_SELECT_LO, the Z-pass replay's). So every other remaining packet has
  a known XDK entry as its producer. No unknown inlined writers stand between P1 and P2.
- Owner dwords: the two S1 draw flushes 53.0K (83%), Resolve 2.7K, DrawIndexedGen 2.4K, DrawVerticesUP
  2.0K, EndVertices 1.5K, everything else under 1K each.

The work list, ordered by dwords removed per unit of effort (fences, interrupts and swap LAST, per the
orchestrator):

| # | Producer | Packets / frame | Record type and effort |
|---|---|---|---|
| 1 | **The shader bind 82108668**, a guest call inside both draw flushes: IM_LOAD 545, IM_LOAD_IMMEDIATE 293, INVALIDATE_STATE 300 | 1,138 | ~40K of the flushes' dwords are the immediate microcode, so ~65% of all PM4 bytes. A shader-bind record: shader object, ucode address / size / stage. Applied at the CP the way onShaderLoad applies IM_LOAD today. **M** |
| 2 | LOAD_ALU_CONSTANT inside the flushes | 752 | A record of (address, count). The CP reads memory at apply, as the PM4 path does. **S** |
| 3 | The tiled branch inside the flushes (821072F8 / 821075E0): SET_BIN_MASK_LO / HI 1,436, the tile's r21xx writes 170. Plus SetPredication 8210D400: 14 | ~1,620 | A bin-mask record; the branch's guest calls become host code. **S-M** |
| 4 | EmitDrawIndexedGen 820FB6E8 (a third draw flush, not under S1): regs 430, NOP pads 134 | 597 | S1 coverage of this flush. **M** |
| 5 | Setters: SetViewport / SetScissorRect / SetTexture | 80 | Register records. **S** |
| 6 | DrawVerticesUP 820FBBB0 | 470 | A UP record: vertex bytes into the arena. **M** |
| 7 | EndVertices 820FBBA0 (Begin/End) | 377 | A Begin/End record: the vertices the guest wrote into ring space. **M** |
| 8 | GpuRingEmitRegisterBlock 820F5570 (96 DRAW_INDX_2: an XDK-internal draw) + EmitPm4Packet 82113010 | 288 | Attribute their callers first (IDA), then records. **S-M** |
| 9 | Resolve 8210C5F8 + ClearF / Clear | 985 | Records through strand B's interface. **M, with B.** Interface (strand B, 2026-10-08 via the coordinator): B's resolve entry point is `pm4ExecResolveRegs(const edram::Regs&, bi)` and the register snapshot IS the record - surface / colour / depth info, the copy and clear registers, window offset, scissors, PA_SU_VTX_CNTL and the resolve rectangle (in B's worktree, lands with their EDRAM-zero steps 2-3). Strand A's producer hook in 8210C5F8 / ClearF / Clear fills that snapshot at record time and the record's position in the stream is where the XDK emitted the resolve packets; the CP calls `pm4ExecResolveRegs` with it. EVENT_WRITE records (kind EVENT) are safe from `nativeRecord` (B: the handler writes only VGT_EVENT_INITIATOR 0x21F9, which nothing reads; resolves are triggered by copy-mode draws). |
| 10 (LAST, with P2) | See the list below | ~190 | Effect records, designed with P2's ring management and the host Z-pass replay. **L** |

Row 10's producers:
- the segment kicks inside the flushes: EVENT_WRITE_SHD / INDIRECT_BUFFER / WAIT_REG_MEM ~45;
- Swap 82112050 (66, mostly pads) and GpuPresentEmitCommandBlock (WAIT_REG_MEM, INTERRUPT);
- InsertFence;
- the Lock / Invalidate waits;
- BeginZPass / EndZPass and GpuCommandBufferReplayDriver 82110840 (the 24 Z-pass IB replays per frame);
- RING_UNSTAMPED (4).

The type-2 NOP pads (~360 per frame) disappear with the PM4 spans they align. Each row converts whole
(rule 1). Rows that share a gate roll each get their own toggle and their own pass line.

**Whole-run census (2026-10-09, research/p2) [measured].** P0 and the work list above came from ingame frames,
but the acceptance bar is 0 PM4 packets in a WHOLE run. So the question was whether any other era emits PM4 that
no row covers. By the charter's mechanical rule, such PM4 is a missed site.

Setup:
- **Exe and gates.** Exe 2f383cbcf9d4 (dev e22478ab code). `SR_NATIVE_BRACKETS=1 SR_NATIVE_METER=1
  SR_PRESENT_DUMP=0`, so no BMP writes.
- **Roll 1, the player whole run.** `tools/player/roll_player.ps1` with a copy of `player_wholerun.json` whose two
  title-visible gates are plain dwells. It runs as two processes:
  - generation 0: boot, intro movies, title, MAIN MENU, MODS, RESTART NOW;
  - generation 1, the relaunched child: boot, intro, title, MAIN MENU, NEW GAME, the opening FMV, then 210 s of
    tutorial gameplay.
- **Roll 2, Warworld.** `perf_run.py --scenario arena` through the dev-tool level skip (DEV-TOOL ASSISTED): the same
  front half, the tutorial, the skip through MST01-03, then 125 s in the arena.
- **Tool.** `python tools/perf/pm4_census_eras.py <log> [<log> ...]` (new). Per era, it reports:
  - packets per frame from the per-frame `[PM4_COV] F` lines;
  - producers from the `[PM4_COVMAP]` reports inside the era (one every 256 frames or 20 s);
  - the buckets below, the UNMAPPED producers, and ZPD / EXT counts with timestamps.
- **Artifacts.**
  - Logs: `archive/v1_checkroll/p2census/roll/drive/attempt1/{run,run_r1}.log` in the main tree, and
    `archive/v1_p2/census/arena/arena_full.log` in the research/p2 worktree.
  - Tables: `archive/v1_p2/census/all_eras.{txt,json}`.
- **Cross-check.** The meter gives 3,026 PM4 packets per frame over generation 1 and coverage gives 3,023. Owner
  sums match the F-line totals to within 1-3% per era.

The buckets follow the work list above:
- **S1 + rows 1+2 residue:** whatever the S1 draw flushes 820FC000 / 820FBBF8 still emit other than kicks, pads and
  bin masks, i.e. S1 declines.
- **Rows 4+6+7:** 820FB6E8 / 820FBBB0 / 820FBBA0.
- **Row 9 (strand B):** Resolve, ClearF, Clear, 820F5570, ClearFullSurface.
- **P2 transport:** Swap, the present block, fences, BlockUntilIdle, Lock / Invalidate coherency, Z-pass begin / end,
  the replay driver, RING_UNSTAMPED, and the kicks and NOP pads inside the flushes.
- **P2-B items:** setters, EmitPm4Packet 82113010, SetPredication, and bin masks / tile writes inside the flushes.

PM4 packets per frame (an era needs two reports for its producer split; "-" means too short):

| Run / era | s | Frames | PM4 / frame | S1 + 1/2 residue | Rows 4+6+7 | Row 9 | P2 transport | P2-B | UNMAPPED | ZPD / EXT |
|---|---|---|---|---|---|---|---|---|---|---|
| gen 0 intro movies | 35.0 | 1,227 | 1,947 | 0 | 470 | 1,247 | 123 | 79 | **21.1** | 0 / 0 |
| gen 0 title | 13.9 | 802 | 2,917 | 61 | 1,295 | 1,224 | 241 | 87 | 0 | 0 / 0 |
| gen 0 MAIN MENU | 15.4 | 798 | 3,620 | 104 | 1,878 | 1,227 | 325 | 88 | 0 | 0 / 0 |
| gen 0 MODS | 17.5 | 1,032 | 3,575 | 100 | 1,848 | 1,227 | 299 | 89 | 0 | 0 / 0 |
| gen 1 intro movies | 36.1 | 1,299 | 1,960 | 0 | 488 | 1,249 | 120 | 80 | **19.8** | 0 / 0 |
| gen 1 title | 8.6 | 498 | 2,852 | 33 | 1,355 | 1,225 | 183 | 86 | 0 | 0 / 0 |
| gen 1 MAIN MENU | 9.6 | 571 | 3,476 | 92 | 1,866 | 1,226 | 293 | 89 | 0 | 0 / 0 |
| gen 1 opening FMV (HUD requested) | 31.1 | 1,090 | 2,705 | 0 | 1,196 | 1,235 | 158 | 77 | 0 | 0 / 0 |
| gen 1 tutorial gameplay | 209.5 | 11,486 | 3,156 | 115 | 1,333 | 1,231 | 345 | 132 | 0 | 0 / 0 |
| arena roll: intro movies | 35.6 | 1,255 | 1,955 | 0 | 470 | 1,249 | 120 | 79 | **21.2** | 0 / 0 |
| arena roll: tutorial + skip loads | 140.2 | 5,112 | 3,024 | 22 | 1,460 | 1,235 | 190 | 82 | 0 | 0 / 0 |
| arena roll: Warworld | 124.8 | 7,140 | 4,149 | 16 | 2,439 | 1,230 | 160 | 286 | 0 | 0 / 0 |

Every process also logs a one-off boot block before its first present; that is UNMAPPED item 2 below. The custom
front end's "loading" era lasts 0.1 s, because the NEW GAME answer and the HUD request arrive together.

**UNMAPPED (the missed sites).** Everything else in every era maps to a planned bucket.

1. **The intro movies: 19.8-21.2 packets per frame, UNSTAMPED, the same in all three processes.**
   - **Packet signature, per frame:**
     - SCRATCH0 0x578 / SCRATCH4-5 0x57C / WAIT_UNTIL 0x5C8 (page r05): 3.0;
     - CP_CPU_INT_ACK 0x45E (r04): 1.0;
     - INTERRUPT: 1.0;
     - COHER_STATUS_HOST 0xA31 + COHER_SIZE / BASE 0xA2F (r0A): 9.4;
     - WAIT_REG_MEM: 6.7.
   - **Producers (by signature).** The first three lines plus 2 WAIT_REG_MEM are exactly one
     `GpuEmitCpuCallbackPackets_820FDA80` block [IDA]. The rest is ~4.7 coherency IBs a frame from
     `GpuBuildFlushMemoryPackets_820FD3B8`, which `GpuCommandBufferFlush_820FD8C0` builds for the deferred CPU-write
     range at dev+0x2D18.
   - **Caller [corrected iter-467 p2, measured].** Not `D3DDeviceInsertCallback_820FDE38`: it runs 0 times in four
     gated rolls, and the movie callback `82471890` occurs in none of our traces or Xenia's (ENGINE_MAP, Movie). The
     block is the 23-dword Z-pass replay kick that `GpuReplayListEndRecordingKick_82110C78` appends (cb `82110B00`,
     no WAIT_UNTIL prefix). Every CP interrupt in the P2 trace (8,743) carries `82110B00` or the vblank gate's
     `82111E38`, once each per present, in every era. So this is P2 transport (P2.2), not a missed library site.
   - **Why only the intro shows it unstamped [inferred].** 82110C78 has no bracket of its own (table row 82110B30 /
     82110C78). In the other eras the kick and its flush land inside the S1 flush owners' brackets; in the intro the
     census drops about 0.96 pieces per frame as a cursor jump, which fits the kick's flush moving the cursor into a
     new segment.
   - **Where it belongs.** P2-B's bracket list / FENCE items (P2.2, P2.8) with the rest of the transport. The P2-B
     CALLBACK item (820FDE38 as a record) was built and proven inert (`research/p2` e7a61346: dry CHECK clean,
     replay 9/9, movies 6/6 to EOM, flicker PASS) but was not landed, because nothing calls it.
2. **Boot, once per process: the init block of `D3DDeviceCreate_820F34C0`**, called from
   `RenderDeviceInit_82621258` at 0x826212BC.
   - **Why no bracket covers it.** Its bracket opens before the device pointer exists (`noDevice=1`), so it
     records nothing.
   - **The first report** (frame 1, which includes all boot work): 110 UNSTAMPED packets plus 6 RING_UNSTAMPED.
     - UNSTAMPED: 24 DRAW_INDX_2 (init clears, with the r0D 0xD0x writes of the ClearFullSurface preamble); 2
       IM_LOAD_IMMEDIATE (default shaders); 9 WAIT_REG_MEM; 3 EVENT_WRITE_SHD; 2 REG_RMW; INVALIDATE_STATE; a
       SET_BIN_MASK_LO; type-0 0x1DC / 0x1DD (SCRATCH_UMSK / ADDR), 0x5C8, 0xE42, 0xC85, 0x57B / 0x57C. The first
       movie frame's callback and coherency packets are in the same report.
     - RING_UNSTAMPED: ME_INIT, one IB, 4 bin selects.
   - **Where it belongs.** P2 (the ring and ME_INIT) plus a native device creation. The plan does not list it today.
   - **boot.device_create BUILD PLAN (native-a, 2026-10-10, from the generated bodies - IDA was down, the generated
     guest code is the oracle; built the same day under `SR_NATIVE_DEVCREATE`, default OFF, on the native transport
     only):** the device create's own producers, read from `D3DDeviceCreate_82106010`'s callees, are five, and only
     they are this item's:
     1. `D3DDeviceInitRingBuffer_820FDE90` - ME_INIT (19 dwords straight into the ring: the one ring packet the CP
        still parsed under ringless, `[GPU_CP] census ... packets=1`) and 8 type-0 dwords: SQ_INST_STORE_MANAGEMENT
        0xD02 = 0x10800; PG_START / PG_SIZE / WRITEBACK_START / WRITEBACK_SIZE 0xA02..0xA05 = C0100000 / 07F00000 /
        C0000000 / 00100000; SCRATCH_ADDR 0x1DD = phys(the 32-byte scratch block); SCRATCH_UMSK 0x1DC = 0x20033;
        WAIT_UNTIL 0x5C8 = 0x20000; SQ_EO_RT 0xD04 = 0. Host: the allocations and kernel calls as the asm (the ring,
        the 2 MB command buffer in 32 segments, the 96-byte fence block, the 32-byte scratch block,
        VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, the GPU identifier address), every device field as
        the asm stores it, the ME_INIT NOT written (the host CP's ME_INIT handler is a no-op - there is no
        micro-engine to initialise - and under the transport nothing parses the ring; the write index stays 0), the
        register writes as run records with the kick checks at the asm's sites.
     2. `D3DDeviceResetState_8210E520` - after its device writes: SQ_FLOW_CNTL 0xD01 = 0x04000000; REG_RMW
        SCLK_PWRMGT_CNTL2 0x81 |= (dword 829AC0F8 | 0x80010000); REG_RMW SCLK_PWRMGT_CNTL3 0x82 |= dword 829AC0FC;
        VC_FIFO_DEPTHS 0xE42 = 0x1F60; PA_CL_ENHANCE 0xC85 = 3 (15 with behaviour-flag bit 1); SCRATCH_REG4 0x57C =
        0x0BADF00D (pc+0x10's first write-back); SCRATCH_REG3 0x57B = 0. Host: the writes, then Driver items - runs
        and the new record **kRkRegRmw** {rmwInfo, and, or} (`Pm4Engine::applyRegRmw`, the packet's own apply);
        dry CHECK kind 8.
     3. `GpuCreateBlockInterruptThreads_82110EB8` - one EVENT_WRITE_SHD {3, the CPU's interrupt word 0xC0000002 +
        cpu * 0x80, the 32 KB window base} per CPU of the behaviour mask (one: CPU 2 for 0x04000001), then
        ExCreateThread(82110A30) / KeSetBasePriorityThread 17. Host: the device blocks as the asm, the kernel calls,
        the packet as the new record **kRkEventShd** (`applyEventWriteShd`).
     4. `D3DDeviceCmdBufSegmentBeginReemitState_820F0048` with r4 == 0 - the boot call AND Swap's per-frame call
        (0x82112744): SET_BIN_MASK_LO {dev+12432}, SET_BIN_SELECT_LO {~0}, SET_BIN_MASK_HI {dev+12436},
        SET_BIN_SELECT_HI {0}, WAIT_UNTIL = 0x20000, RB_BC_CONTROL 0xF01 = dev+10788, then
        `EmitScissorTileRects_820F0208` (row 5's entry) and the dirty masks. Host: the six as Driver items (kRkBinReg
        + runs) in one Entry, the tile rects as row 5's own entry after it closed (two entries never nest), the
        masks; dry CHECK kind 9 (the nested row-5 bracket inside is skipped). These are 6 literal PM4 per frame of
        the segmentless bridge, so the per-frame half has a frozen-pass A/B (`SR_AB_TOGGLE=SR_NATIVE_DEVCREATE`).
     5. The flushes between them (`820FD8C0`) - already kRkFence / kRkInvalidate under segmentless (their
        WAIT_REG_MEMs and EVENT_WRITE_SHDs are the records' apply).
     NOT this item's, by the same reading: the 24 DRAW_INDX_2 init clears (XDK Clear calls from the game's own
     `RenderDeviceInit` - row 9), the 2 IM_LOAD_IMMEDIATE (the first shader binds - rows 1-2), `82110058` (EDRAM
     retrain: nothing emitted with our generic stub), `82105B18` (the swap chain: CPU-only), `8210BA28` (PIX
     notifications: CPU-only), `820F34C0`'s flags=2 recorder device (dead). The item's observation is therefore the
     five producers at 0 - `[GPU_CP] census ... packets=0`, the `[NATIVE_DEVCREATE]` line, the Swap-side 6 per frame
     gone from the meter - with frame 1's remaining UNSTAMPED packets owed by rows 9 and 1-2, not by this item.
     **BUILT (native-a, 2026-10-10, `SR_NATIVE_DEVCREATE` default OFF)**: kernel/native_devcreate.cpp (1, 3),
     native_flush.cpp (2, 4; the record kinds 15 / 16; CHECK kinds 8 / 9 with the nested-bracket guard the tile-rects
     call needs), gfx/pm4_engine `applyRegRmw`. Rule 1: every port decides at the top (the gate, the transport, the
     re-init path, an Entry probe); the one post-allocation decline (1, arena pressure, unreachable at boot) is
     logged and runs the guest body whole. Proof: `tools/perf/native_proof.py --plan devcreate` (replay, the dry
     CHECK, the probe roll's counters, the per-frame A/B, CPPM4, the meter, the flicker gate).
     **IDA cross-check (native-a, 2026-10-10, the oracle back):** the four producers decompiled and 8210E520
     disassembled against the ports - 820FDE90: the teardown, the parameter defaults (0x8000 / 0x200000 / 32), the
     allocation flags (0xB5800000, 0x87800000 or 0xB7800000 below 0x80000, 0xA5800000, 0x95800000), every device
     field (13516 / 13476 / 13484 / 13504 / 13488 / 13508 / 13492 / 13480 / 10820 / 40 / 44 / 48 / 13496 / 10780 /
     fb+0 / fb+4), the eight register writes and the two kick checks identical; 8210E520: every field write
     (the 26- and 18-entry loops, the thirteen stores, the two OR-ins) and the 72-byte tail identical - the
     decompiler shows the dirty masks as 0xEFFFFFFFF, the asm is `li r11, -1; std r11, 0..0x20(r31)` = ~0 as the
     port writes, and dev+11544 is `clrldi r30, r11, 32` = 0x00000000FFFFFFFF as the port writes; 82110EB8: the
     fields, the per-CPU block, the EVENT_WRITE_SHD word `(blk-32 & ~3) | (blk-28 & 3)`, ExCreateThread's
     arguments and the priority 17 identical; 820F0048: the four bin registers, the second kick check,
     WAIT_UNTIL / RB_BC_CONTROL and the lo15 / hi15 scissor halves identical. Nothing to change.
     **CLOSE DESIGN (native/devcreate-close, 2026-10-14; fixed before any roll; revised after Astra round 1 the same
     day, still before any roll - `archive/dcl/astra/round1.txt` in that worktree; plan `native_proof.py --plan
     devcreate_close`, evaluators `tools/perf/boot_equiv.py` and `tools/perf/boot_stall.py`, numbers from
     `archive/dcl/power_sim.py` / `survey_boot.py`).** Two owed items, one staged set (`--stage J:\sr_stage`, one
     exe, a FRESH `--out` per run: the plan's `fresh_out` refuses an --out holding its proof JSON or any of its roll
     folders, so the rolls the ledger cites are never overwritten). 42 rolls (warm-up, 16 boot, 8 meter, 17 stall),
     ~4.2 min each staged on J: (bat2) - about 3 h of game slot.
     1. *Pooled meter* (the failed `meter`: dc3 total -2.28 vs <= -3). Four meter pairs, alternating m1 OFF/ON, m2
        ON/OFF, m3 OFF/ON, m4 ON/OFF, arms exactly dc3's (`SR_NATIVE_SEGMENTLESS=1` vs `+SR_NATIVE_DEVCREATE=1`,
        sparse dumps kept for the world rule), graded by the ledger's own `pool` (`meter_delta`, the form of
        p2.segmentless's `bake_meter`) with `"pool_weight": "pair"`: the statistic is the MEAN OF THE PER-PAIR DELTAS
        (total pm4 per ingame frame ON - OFF) <= -3 (kept) - the statistic the power below is computed for (Astra
        P2-5: the frame-weighted arm means turn zero-delta pairs with opposite frame counts into -33.33); every roll
        world_min 0.5 and >= 8,000 ingame frames (new `min_roll_frames`; the archived meter rolls hold 11,158-12,220),
        no pair dropped; `provenance` checked by the grader (one exe hash, `SR_NATIVE_DEVCREATE=1` in every target's
        provenance and in no base's, the `[NATIVE_DEVCREATE] ring init native` line in every target log and in no
        base log, the env equal beyond the arm and path keys, the declared alternation and sequence from the logs'
        first lines). The step is `always` (Astra P1-4): once any meter roll ran it is written on every outcome - a
        crashed meter roll is a FAIL with an item, never a SKIP without one - with a run-unique RECEIPT (committed
        last: the runner verdict, each roll's run row and the sha256 of its run.log / provenance / outcome / steps.log)
        that the item names: the ledger grades the receipt first (a pool whose rolls crashed or never ran is a FAIL,
        not "unavailable") and fails a roll whose files changed since the run. Power: 820F0048's per-frame call is 6
        packets (4 bin + 2 t0). The pair-to-pair spread of the
        total is SD 1.45 over sgb5's four pairs (-58.9 / -60.2 / -61.2 / -57.9; its same-config rolls SD 1.33-1.50)
        and 1.58 over dc2 / dc3 / dc4 (-5.2 / -2.28 / -4.8), pooled 1.50. Four pairs: SE ~0.75, so -3 sits 4 SE
        above the expected -6 (power ~1.000), 0.93 if the true drop is only the dc pairs' mean -4.1 (0.86 at SD
        2.0); a true 0 passes with < 0.01%. One pair had power 0.77 at -4.1: dc3's miss is what that spread
        predicts. scb1 is one pair (no spread of its own; its -31.1 total moved t2nop by -14.6 - the run-to-run
        kinds are why a single-pair total bound is weak). Ledger: `meter_pool` lands in the evidence on every
        outcome; only when the ledger's grader passes it does `meter` move into a new contradiction
        `meter_single_pair` (witness, expect fail; resolved_by `meter_pool` - the gate_v1_superseded pattern). On a
        FAIL both stay in the evidence as FAIL. A later run's `meter_pool` supersedes an earlier one only through
        apply_ledger's rule (Astra P1-3, dev's shared code): the existing item is graded first - the ledger tool's
        results, else its direct grade of the line (a custom `--ledger`) - and is replaced in place only when it
        positively passes or names the same path; a failing, unavailable or ungraded one becomes a witness (expect
        fail), never dropped.
     2. *BOOT_EQUIV* (the MEAN criterion of `boot_order_gap`; its own evaluator, a separate tool so boot_pairs.py stays
        the diagnostic that never prints the tag, sharing boot_pairs' roll parser so the markers cannot drift). Design
        `dcl-v2` (dcl-v1 + Astra round 1): a declared warm-up roll (the OFF arm = the default build; run, recorded,
        never graded; it must reach the HUD, its run row be DONE, and its log END before the first graded roll's log
        begins - P2-7: a start-only check let a warm-up still booting at pair 1's launch through - in the same
        native_proof invocation, item 4; bat1's pair 1, +1.88 s with CP ready at
        1950 vs 754-1143 ms, was the session's first roll), then 8 pairs in the fixed alternation p1 ON/OFF, p2 OFF/ON, ... (ABBA blocks: each arm first in 4
        pairs, a linear drift cancels in every block - a random order of 8 does not guarantee that balance, so the
        ledger's "randomized within pairs" is met by a pre-registered balanced order), devcreate_ctl's arms, no
        dumps (dcc4's / bat1's config, the variance basis). Endpoints, paired ON - OFF, with fixed margins: time to
        the HUD 2.0 s (primary), time to the attach 0.5 s, the boot-load duration 0.5 s, attach -> HUD 2.0 s. Why:
        the unknown is dcc4's +18.3 s (pairs +17.1 / -0.2 / +31.5 s, the attach 20-36 s late in 2 of 3 ON rolls);
        2.0 s is a ninth of it and 2.4% of the ~83 s boot; on J: the attach is 0.53-0.85 s and the load 0.12-0.14 s
        in all seven async rolls (bat1, bat2w). Statistic: per endpoint the one-sided 95% upper bound of the mean
        paired difference (t, n-1 df) must lie below its margin. N: the six paired HUD differences with no backend
        wait in either roll - bat1 +1.88 / +0.03 / -0.97, dcc4 p2 -0.14, dc3's meter pair +2.63, dc4's +0.11 - have
        SD 1.37 (0.50 without bat1 p1 and dc3); simulated P(EQUIVALENT | no effect) at 8 pairs is 0.975 at SD 1.4
        (0.81 at SD 2.0; 6 pairs 0.91, 10 pairs 0.99), 0.86 under a true +0.5 s, and 5% under a true +2.0 s.
        INCONCLUSIVE (not evidence): a design fault (not 8 pairs in that order, two exes or exe paths, an SR_*
        difference beyond SR_NATIVE_DEVCREATE, unequal or unparsed save seeding, an arm not engaged, the warm-up
        missing / not ended before pair 1 / without a HUD, the logs out of the declared sequence, a roll never run,
        the run not identified - Astra P1-2: `tools/perf/proof_rows.py` finds the ONE `*_proof.json` beside the rolls
        whose `out` is that folder and whose rows name every roll folder by its CANONICAL umbrella-relative path
        (never a basename: another run's rows with the same names are refused), the run's exe id a prefix of every
        roll's exeSha256 and its start no later than every roll's first log line) or a missing or inconsistent marker
        (boot_pairs' rules, amended: a load on the loader thread may end before the swap chain or after the attach;
        P2-6: EVERY recorded wait, 0.0 ms included, must match [CP up, attach] within 1 s, and a load of L s ending at
        t must have begun no earlier than the log - and on the loader thread the uncovered wait is the validated
        interval [CP up, attach], never the recorded number); an OFF roll attaching > 5 s after its log began or a
        pair more than 5 s in OFF's disfavour (an OFF-arm stall: the conditions were not the design's); a position
        effect (below); or, with nothing OPEN, a bound at or over its margin (under-powered as run). OPEN, never
        discarded: a crash / timeout / missing outcome, no HUD, a backend wait not "attached", the run's row for a
        roll not DONE or an abort marker anywhere in its log (THREAD_FATAL, ICALL_FAIL, a CP stall, a GPU hang
        trigger, [FATAL]) whatever its outcome.json says (P1-2), an attach after guest VdSwaps or with queued draws
        (the black-world race), an ON roll attaching > 5.0 s after its log began or with an uncovered wait > 1.0 s
        (BOOT_STALL's events A / W, one definition - a stall in both arms leaves no paired difference), any pair over
        5 s on any endpoint (a false trigger in 8 pairs is 0.14% at SD 1.4), a mean difference over its margin. A
        crash ends the plan (native_proof's abort rule) but the evaluator still runs over what was rolled.
        POSITION, per endpoint (Astra P2-8: ON in the first slot is NOT conservative for every endpoint - a first-slot
        delay of the attach alone LOWERS attach -> HUD in that pair; the earlier claim is dropped): for each endpoint
        the first graded roll must lie within that endpoint's margin of the median of the other graded rolls of its
        arm, in either direction, else INCONCLUSIVE; within it, a first-slot effect moves the 8-pair mean by less than
        margin / 8. EQUIVALENT (tag BOOT_EQUIV) claims only: on this exe, staged, in this scenario, the MEAN paired
        devcreate boot regression of every endpoint is below its margin (one-sided 95%). It does NOT bound an
        intermittent stall (P1-1 / P3-9): no stall in 8 ON boots alone bounds a per-boot stall rate only below
        31.23% (exact Clopper-Pearson, 1 - 0.05^(1/8), assuming independent boots) - item 5 is the stall criterion,
        and `boot_order_gap` resolves only when both resolvers pass. Ledger: the run-unique JSON binds
        `boot_order.powered` on every outcome (committed, runner.verdict PASS, verdict EQUIVALENT, tag BOOT_EQUIV,
        design dcl-v2, pairs 8); a bound run that fails moves into the unknown's witnesses (expect fail) before a
        later run is bound.
     3. *No re-proof on the current exe is owed.* native_ledger.py has no exe-currency rule: every item names its exe
        and grades on its own archive, all on bases with the race fix 41572437; the replay runs no guest code (it
        cannot exercise device creation) and the pixel / guest-answer guards are met by dc3's abp, cppm4 and probe.
        So the plan has no replay and no gate (`no_replay`, `no_ab`); its 16 boot rolls run devcreate's boot with
        the async attach on the current exe, the black-world race an OPEN class. The boot_attach plan gets the same
        declared warm-up (step `warmup`, the default build) and `boot_attach_eval.py --warmup`, which never grades it.
     4. *The position effect (bat2, 2026-10-14; the coordinator's input).* The FIRST LAUNCH OF EACH native_proof
        INVOCATION consumed its first CP command ~1 s late - bat2w on1 +1385, bat2 on1 +2092, bat1 on1 +1950 ms against
        +1017..+1143 in the ten later rolls of both arms - even though bat2w's warm-up ended 1 s before bat2 began: a
        warm-up in its own invocation absorbs nothing, and every first graded slot so far was ON (arm and position
        confounded). Handling: (a) the warm-up is a step INSIDE the plan's invocation (devcreate_close and boot_attach
        `warmup`), and both evaluators make that a design condition - the warm-up and every graded roll must be roll
        rows of ONE run's `*_proof.json` (each invocation rewrites it), else INCONCLUSIVE (bat2 regraded with bat2w as
        its warm-up reads INCONCLUSIVE); (b) BOOT_EQUIV grades position PER ENDPOINT (item 2: the first graded roll
        within each endpoint's margin of its arm's median, either direction, else INCONCLUSIVE) - the first version's
        claim that ON in the first slot is conservative held for time to HUD only and is withdrawn (Astra P2-8); the
        boot_attach follow-up keeps its position line as information, and its warm-up must have ENDED before the
        first graded launch (P2-7) with the run identified by canonical rows and a failed row or abort marker OPEN
        (P1-2); (c) the boot_attach plan now runs the MIRROR of bat1's order (OFF/ON, ON/OFF, OFF/ON;
        `boot_attach_eval.py` accepts either), so for the first time an OFF roll takes the first graded slot - position
        and arm separate; (d) both evaluators print a position line (the first graded roll beside its arm's median,
        and the warm-up's), never graded. What the tools do once per invocation, read offline:
        `tools/perf/native_proof.py:2652` the --out lock, `:1546` the sha256 of the whole --exe snapshot (248 MB, D:),
        `:2061` the proof JSON's first write, `:2068` the game-slot acquire (`tools/game_slot.py`: tasklist + an mkdir
        lock). Every roll then takes the same path: `flicker_gate.py:187` roll -> `perf_run.py:311-326` stage_roll.py
        stage --verify quick + a lease -> `sr_drive.ps1:809-826` Invoke-SrStage (the full pass: the 253 MB exe-folder
        sources hashed on D:, every staged file on J: verified, a lease) -> the launch; bat2 copied 0 B in every roll
        (stage_pre.json). None of the once-per-invocation steps runs while the game runs and nothing per roll differs
        for the first, so the tools do not explain it. Inside the game the excess is a stall, not work: bat2 on1's main
        thread sat 0.91 s between two log lines separated only by function-table writes (`kernel/fe_flow_watch.cpp:
        1969-1978`, [SHIMS] -> [RDY_INSTALL]) while the process ran 0.11 cores in its second 1-2 s with the system 4.0
        cores busy (perf/mem.csv; the later rolls ~1.0 core); bat1 on1 / bat2w on1 lost 1.03 / 0.67 s between
        [PC_CONTROLS] and the CRT start (0.14-0.41 s in the later rolls; `archive/dcl/first_roll_gaps.txt`). Consistent
        with a host-side stall on an invocation's first launch (cold image pages, another process's work); the
        mechanism is NOT established offline.
     5. *BOOT_STALL* (the INTERMITTENT-STALL criterion of `boot_order_gap`, pre-registered separately - Astra P1-1: ON
        +30 s on 10% of launches is a true mean regression of 3 s, yet 8 clean pairs, which the mean rule then
        passes, happen 0.9^8 = 43% of the time). Design `dcl-stall-v1`, `tools/perf/boot_stall.py`, step
        `boot_stall` (always) after a block of 17 more ON boots (`s1_on`..`s17_on`, the boot pairs' ON arm). The
        event per SR_NATIVE_DEVCREATE=1 boot, one definition for every boot counted: F a failure (its run row not
        DONE, an abort marker, outcome not SUCCESS or missing, no HUD, a wait not "attached"), A the backend attached
        > 5.0 s after the log's first line (every async-load roll on J: attached within 0.85 s - bat1, bat2, bat2w,
        tcmp1; dcc4's stalled ON rolls at 26.7 / 42.0 s), W the CP waited > 1.0 s (the uncovered wait above), R the
        black-world race. Counting: the evaluator enumerates the run's ON boots itself from its proof JSON (every roll
        row not SKIP whose provenance says SR_NATIVE_DEVCREATE=1: 8 boot + 4 meter + 17 = 29) and refuses a set that
        omits one; a crash counts as F even without a roll folder. Pooling across declared runs is allowed on ONE exe
        hash with the event identical; the meter rolls' dump keys may differ. BOUNDED (tag BOOT_STALL) only with no
        event in >= 29 ON boots: the exact one-sided 95% upper bound on the per-boot event probability is then
        1 - 0.05^(1/29) = 9.81%, ASSUMING independent boots with one common probability (a stall tied to time of day,
        disk load, position in a run or the build is bounded only for the conditions sampled). Why 29: a stall of
        >= 20 s (dcc4's smallest backend wait, 20.4 s) on a fraction p of boots adds >= 20 p s to the mean boot; p =
        10% is 2.0 s, the HUD margin, and 29 is the least N with 1 - 0.05^(1/N) < 10% (28: 10.15%); an ON stall on
        10% of boots passes with probability 0.9^29 = 4.7%. OPEN on any event (never discarded), INCONCLUSIVE on a
        design fault, an unclassifiable boot or < 29 boots. It bounds this event only; a slow boot without a late
        attach, a wait or a failure is item 2's. tcmp1's four composed DEVCREATE=1 rolls (exe 9ee9e7908692, attach
        0.51-0.55 s, no wait) do NOT count: another intervention (12-14 other native kinds ON, plus freeze / A-B /
        trace / CPPM4 / stress configurations), not a declared stall set - information only. Ledger: a second
        pre-registered resolver `boot_order.stall` (OWED; committed, runner.verdict PASS, verdict BOUNDED, tag
        BOOT_STALL, design dcl-stall-v1, events 0, on_boots >= 29), bound on every outcome like item 2's.
     **DEVCREATE BAKED (2026-10-15, dcf2 + dcf3).** SR_NATIVE_DEVCREATE default ON (=0 keeps the guest bodies and the
     A/B). dcf2 (exe 864eb1c17db4): replay 9/9, the pooled meter 6.00 -> 0.00 PM4 per ingame frame, chk per emitter, the
     A/B, CPPM4, the census with NO allowance ("target remaining []": the default build's gameplay holds no PM4), the gate,
     boot_stall BOUNDED over 29 ON boots (dcf-stall-v2, no event). Its boot pairs overlapped the orchestrator's D: activity
     (dcf2 NOTES' 18:01 amendment), so boot_order.powered was taken from dcf3 on the same exe: four clean alternating pairs,
     EQUIVALENT (hud +0.47 < 2.0, attach +0.02 < 0.5, L +0.01 < 0.5). Both resolvers bound by hand (the apply left them on
     the placeholder folder).
     **ROOT CAUSE + BAKE PLAN (native/devcreate-fix, 2026-10-15; written before any bake roll; the CLOSE DESIGN above
     is superseded by it before any of its rolls ran - devcreate_close stays in the tool, parked, its OFF arms pinned
     =0).** Full note: `archive/dcf/ROOT_CAUSE.md` in that worktree.
     1. *The ON-boot backend wait was NOT devcreate's.* On the exes of 2026-10-11..14 (4292f86c1eec = dev 00865e25;
        671a0e244a33) `D3D12Backend::init` -> `createXlatPipeline` -> `ShaderCache::ensureLoaded` (00865e25
        gfx/d3d12_backend.cpp:2651: the installed cache's 856 reads, 5.1 MB, from `data/shader_cache/dxil` on D:, a
        7200-rpm HDD) ran on the main thread BEFORE `setPm4LiveBackend` (00865e25 app/main.cpp:557), while the CP -
        started by the guest's device create - waited for the attach (`SR_PM4_WAIT_BACKEND`). The wait is the load's
        end minus the CP's start, and the load's duration is the disk's state: 0.17-0.18 s with the files in the file
        cache, 20-48 s cold or contended, in BOTH arms. In every slow roll of either arm the attach follows the
        `[SHADER_CACHE] loaded` line (dcc4 p1 ON: CP up +5.8 s, loaded +23.3 s, attached +26.2 s, waited 20.4 s; p3 ON
        +5.2 / +36.3 / +41.6 / 36.4 s; dcc2 OFF, first on a quiet disk: +5.3 / +23.1 / +25.1 / 19.8 s; dcc2 ON +4.2 /
        +26.6 / +28.3 / 24.1 s); in every fast roll the load took 0.17-0.18 s and the attach came before the CP (dcc4
        p1 OFF, p2 OFF and p2 ON - the ON arm is itself bimodal). The direct measurement on the exe with the boot-load
        lines: dc5c census0 (OFF) `installed cache 48441.9 ms ... 856 reads, 5.1 MB, read time 48338.4 ms, slowest
        4008.5 ms`, `CP waited 37646.0 ms`; census1 (ON) 25404.8 ms / 23912.8 ms; dc5d census0 (OFF) 9.6 s. More OFF
        controls: dcc1 OFF 18.2 s, dcc4 p3 OFF 2.0 s, sgb5 OFF 52.6 / 30.4 s, scb1 meter_off 25.6 s (survey_boot.txt);
        the dcb census 6/19 (32%) vs 59/236 (25%) slow, both arms splitting the same way by the pre-launch proxy. The
        ports (`kernel/native_devcreate.cpp`) run on the guest thread with the asm's allocations and kernel calls
        only, no file I/O; in every ON roll the ring init line and `[GPU_CP] command processor thread up` share a
        timestamp; H' = HUD - wait is ON 80.3 / 83.4 / 82.7 vs OFF 83.6 / 83.6 / 85.6 s (information). Why the ON arm
        looked worse: position (dcc1's ON first after a long hold, dcc4's p1 ON the set's first roll) and a bimodal
        0 / 20-40 s nuisance over 3 pairs (the lane's own Fisher p = 0.40). Not the ring init, the interrupt threads
        or the reset state.
     2. *Gone from the attach path since render/boot-attach (f9c08c2d, 2026-10-14).* `ShaderCache::beginBootLoad()`
        runs the load on "SR shader load"; the attach follows the device / swap chain / PSOs; the CP waits for the
        attach with no limit, then (WP1) for the installed cache before its FIRST command
        (kernel/gpu_command_processor.cpp:357-376) - a slow cold load is now `[SHADER_CACHE] CP start: waited N ms`,
        never a backend wait. On current exes staged on J: every boot of tcmp2 (403bd798be42) and tcmp3
        (2bfd499ebe58) - 15 fixed DEVCREATE=1 boots, 2 A/B boots (SR_AB_TOGGLE naming the gate) and 8 OFF boots across
        the two runs (Astra round 1 on the branch corrected the earlier "30 ON") - attached at +500..+704 ms, `CP
        start: waited 0.0 ms`, the HUD at 82.3-83.6 s, no `CP waited` line in any. This explains the archived waits'
        synchronous-cache component; it does NOT by itself exclude a devcreate interaction under a cold or contended
        cache - that is what the stall sample of (h) is sized for. What remains is not this record's: on a cold D: the
        first command still waits for the load (WP1); staging on J: avoids it; a packed single-file cache would remove
        most of the 856 opens (for render/boot-attach's follow-up).
     3. *The bake.* `SR_NATIVE_DEVCREATE` default ON in both readers (`kernel/native_devcreate.cpp` devCreateGate,
        `kernel/native_flush.cpp` devCreateOn: `!e || e[0] != '0'`; =0 keeps the guest bodies and the A/B arm;
        `tools/difftest/present_composed_test.cpp` reads it with defaultOn), on top of the transport bake
        (native/transport-bake + -r1 merged into the branch), so the default path's gameplay PM4 is 0: the re-emit's 6
        a present (tbk1 meter_on 6.0 exactly: t0 2.0, bin 4.0) was the last producer on it. The older devcreate plans
        (devcreate, devcreate_ctl, devcreate_close) pin their OFF arms `SR_NATIVE_DEVCREATE=0` (DEVCREATE_OFF), as the
        per-kind transport plans pin the baked kinds; the four OVERRIDES.json entries say so.
     4. *The live plan (`native_proof.py --plan devcreate_bake`, prefix dcf; ONE exe, `same_exe` enforced, a fresh
        `--out`, staged on J:; 37 game rolls, ~2 h 45 min of slot + the replay - 17 rolls / ~75 min before Astra's
        round 1 on the branch restored the 29-boot stall sample, see (h)).* Every item but the two boot resolvers
        resolves the new unknown `default_bake`; the two resolvers bind `boot_order_gap`'s (resolver_bind).
        - (a) static: `default_on` (native_devcreate.cpp) and `default_on_flush` (native_flush.cpp).
        - (b) `replay` 9/9 against the pre-bake exe (`--base`: the transport-baked exe with the census-quit fix and
          without this flip - d92f94aef9cf, snapshot `d92f94aef9cf-tbkcq`; the branch carries native/transport-bake's
          03dd5a5e, whose terminal fold the graceful census pair (f) needs).
        - (c) `warmup`: the default build, the invocation's first launch (bat2's position effect), the pair rolls'
          exact configuration (the sparse dump kept, `sparse=True`); never graded by the mean criterion, counted by
          the stall criterion; its devcreate witness is evidence.
        - (d) four alternating pairs `p1_on/p1_off, p2_off/p2_on, p3_on/p3_off, p4_off/p4_on` - the default against
          `SR_NATIVE_DEVCREATE=0`, each arm first twice, one configuration - graded TWICE: `boot_equiv.py --design
          dcf-v1` (the MEAN criterion: dcl-v2's margins - HUD 2.0 s, attach 0.5 s, load 0.5 s, attach -> HUD 2.0 s -
          markers, OPEN classes, position rule and statistic over 4 pairs, t(0.95, 3) = 2.353; the warm-up must be the
          default build with the key UNSET; the gate read as default ON - boot_pairs.arm_of: ON = unset or 1, OFF = 0,
          an SR_AB_TOGGLE naming the gate neither) -> `boot_order.powered`; and the ledger's pooled meter
          `dcf_meter_pool` (meter_delta, pool_weight pair: the mean of the per-pair deltas <= -5.7 = 6.0 less 5%, the
          transport bake's convention; AND every default roll <= 0.1 pm4 per ingame frame (`max`, the zero form); AND
          every =0 roll >= 5.5 (`base_min`: the base shows the re-emit); every roll >= 8,000 ingame frames, world_min
          0.5; provenance: one exe, `arm_unset` "1" so the default reads as the target, the engaged line in every
          default log and in no =0 log, the declared alternation target-first) - on its PASS the single-pair `meter`
          (dc3, -2.28 vs -3) retires into `meter_single_pair`. Why 4 pairs: the paired HUD spread on J: with an
          in-invocation warm-up is ~0.5 s (bat1 / bat2 pairs 2-3: +0.03 / -0.97 / -0.16 / +0.21), so the bound sits
          2.353 x 0.53 / 2 = 0.62 s above the mean against 2.0 s (0.89 s at 3 pairs); a 2.0 s true effect passes with
          ~1%. Why the total meter is now exact: nothing but the re-emit is left on the path, so the total IS the
          producer's count (the -3 bound of 2026-10-14 measured a 373-packet path whose other kinds moved 2-6 a frame).
        - (e) `chk` (SR_NATIVE_FLUSH_CHECK=1: the dry CHECK of kinds 8 / 9 on the default path - three items: the
          last periodic CHECK line's aggregate `BAD bytes=0 len=0 dev=0` AND, from its per-kind tail `kinds full/bad:
          k8=<completed>/<bad> k9=...` (native_flush.cpp's CHECK report since this branch), at least one COMPLETED
          comparison of kind 8 (ResetState 8210E520) and of kind 9 (the re-emit 820F0048) with none bad; Astra round 1:
          the aggregate alone passed a build whose kind-8 / kind-9 CHECK calls were removed, the draw checks satisfying
          it - tools/perf/test_chk_items.py shows each removal failing its item), `abp` (the frozen per-present A/B,
          `SR_AB_TOGGLE=SR_NATIVE_DEVCREATE`: the default against =0, pixel-identical), `cppm4` (CPPM4 on the default
          - tcmp3's cppm4 ran it with DEVCREATE=1 - with the devcreate witness).
        - (f) the census pair, both `SR_NATIVE_BRACKETS=1` and ended by the graceful quit (tbk2's lesson: the tail
          after the final report is attributed only by the terminal report): `census0` = `SR_NATIVE_DEVCREATE=0`
          (the devcreate plan's census0 items: the re-emit on Swap's line, the boot writes UNSTAMPED; the ME_INIT
          witness), `census1` = the default (its census1 items: no 63 / r0E / r0C on any producer line or completed
          frame; the devcreate witness), graded by `transport_census.py --pm4-zero --base-shows devcreate`: the
          default arm's gameplay holds NO PM4 with NO allowance (every F line from the ingame marker to the terminal
          report, every producer line over the interval; `allow` empty, `ingame_pk` 0, `reemit_pk` 0) and the base
          shows all six re-emit keys on 82112050's line (`base_devcreate_exercised`; the transport is native in both
          arms, so the transport group cannot be the base's witness). This is acceptance.zero's gameplay half.
        - (g) the stall block `s1_on..s20_on` (the default build, plain, the strict witness in each), then `gate`: the
          world-rule flicker gate on the default exe, the devcreate witness in both rolls.
        - (h) `boot_stall.py --design dcf-stall-v2` over EVERY ON boot of the run - the warm-up, `p1_on..p4_on`,
          `cppm4`, `census1`, `s1_on..s20_on`, and the gate's two boots `gate/freeze`, `gate/span` (29; the =0 rolls,
          `abp` (its toggle names the gate: neither arm at boot) and `chk` (the guest bodies run) are not ON boots;
          the gate's boots are enumerated from the gate's proof row by `proof_rows.gate_boots`, a retained attempt of
          the gate - a re-placed span's first roll - counted from its own log) with the plan's observe-only
          instruments declared (`--allow-key`: SR_NATIVE_BRACKETS, SR_NATIVE_FLUSH_CPPM4, SR_NATIVE_CONST_DEDUP_CHECK,
          SR_DIAG_SIM_FREEZE, SR_DRIVE_GRACEFUL_QUIT - each from the tool's fixed list, never a native kind) ->
          `boot_order.stall`. Astra round 1 on the branch (2026-10-15) withdrew v1 (7 boots) before any roll: a 30 s
          stall on 10% of boots escapes 7 boots 47.8% of the time and four clean pairs then pass the mean criterion,
          and v1 read a pixel-graded roll's PASS row as a failure, so a healthy bake could never close. v2: (i) a
          success row is DONE or PASS (`ROLL_OK`; the runner's own row shapes, tcmp3's cppm4 in the tests); (ii) N >=
          29 as dcl's - the least N with 1 - 0.05^(1/N) < 10%, 0.9^29 = 4.7% - because the root cause explains the
          archived waits' synchronous-cache component but not a devcreate interaction under a cold or contended cache,
          which only the sample excludes; (iii) the events F / A > 5.0 s / W > 1.0 s / R plus C: the CP's first
          command waited > 1.0 s for the installed cache AFTER the attach (`[SHADER_CACHE] CP start: waited N ms`, the
          WP1 wait the attach no longer carries) and H: the HUD request later than 95.0 s after the log's first line
          (the catch-all; the current exes' ON boots on J: reach it at 82.3-83.6 s, the slowest archived boot without
          a backend wait on any disk 85.6 s); the WP1 line is required of every counted boot (unclassifiable without
          it); (iv) the controlled cache condition: the run staged (the proof's `stage`) and every counted boot
          launched from ONE exe path under that root (provenance exePath), so the bound speaks for staged-SSD boots -
          a boot from another path is a design fault, never a quiet mixture of disks; (v) "every ON boot" includes
          the gate's two and their retained attempts, with their failure accounting (the gate's flicker_gate.json
          exit codes, a retained attempt's outcome). 0 events in 29 bounds the per-boot rate below 9.81% (exact,
          independent boots assumed, for the conditions sampled).
        - The strict witness in EVERY roll (Astra's bar): `devcreate_item` + `ring0_item` (`[NATIVE_DEVCREATE] ring
          init native`, `[GPU_CP] census ... packets=0`) in every ON roll - the stall block included - and both gate
          rolls, `me_init_item` (`packets=1`) in every =0 roll, `segcounters_item` in all; one exe (every roll
          stamped).
        - INCONCLUSIVE / OPEN rules, the receipt, the run-unique JSONs, `fresh_out`: as devcreate_close's (the same
          tools, the same routes).
     5. *Offline proof (the cloud worker, this branch against the transport-bake-r1 tree it stacks on):* the C++ build,
        the difftest suites (exit codes and masked outputs compared), the Python suites (tools/perf and tools; the
        boot evaluators' new design tests and their mutants), the forcing audit and gen_override_manifest, the dry
        run of the plan. Recorded in `archive/dcf/` and the commit messages.
     6. *Not pre-registered here, by design:* a diagnostic roll of the bake exe (one, information only) if the slot
        frees before the set; the owner runs the live set and the one Astra review.

**Other findings.**
- **Row 9 is a fixed floor in EVERY era, movies included: ~1,225-1,250 packets per frame.** That is Resolve
  ~775-800, 820F5570 ~246, ClearF 174 and Clear ~32. DrawVerticesUP adds ~460-470 everywhere. So strand B's row 9
  removes ~1,230 per frame from the whole run, not only from gameplay.
- **Rows 4+6+7 dominate the front end and Warworld.**
  - MAIN MENU / MODS: 1,850-1,880 per frame (EmitDrawIndexedGen ~935, EndVertices ~470, DrawVerticesUP ~460).
  - Warworld: 2,439 (EmitDrawIndexedGen 1,490).
  - Native-a's row-4 bake (-44.5% of ingame PM4) should therefore cut the menus even more.
- **Warworld's P2-B** is 286 per frame. Of that, 155 is EmitPm4Packet 82113010 (103 NOP pads and 52 SET_CONSTANT:
  the Lion per-tile constants 827333E0), and 52 is SetPredication.
- **ZPD / EXT: 0 in every era of both rolls,** including 210 s of tutorial Metropolis and 125 s of Warworld. No
  flare or HUD-glow query fired, so the flare test (Q11) still needs a scene with the sun or a flare in view.
- **GPU watchdog: 0 H1 triggers** in the 392 s whole run and the 356 s arena roll, both without present dumps.
  The two H1 episodes in the p2merged span roll (P2.9) fell inside its synchronous 360-BMP dump window.

**P1 rows 1+2 (2026-10-08, native-a; commits 71d63502 / b423239e; gate `SR_NATIVE_BIND`).** Rows 1
and 2 are one function family, converted together as one entry:
- **Records** (`kernel/native_flush.hpp RecKind`): PACKET tags `0x80000000 | kind<<24 | n<<8 |
  flags`, kind 0 = the draw as before (old records parse unchanged); SHADER_PTR {addrType,
  startSize}, SHADER_IMM {type, startSize, the PATCHED ucode}, CONST_LOAD {srcPhys, offsetType,
  size} (the CP reads memory at apply, as the PM4 path), SET_CONST, BIN_REG (n = which), EVENT.
  Flag bit 1 = predicated: the CP re-tests `binSelect & binMask` at every execution, Z-pass replays
  included, as `execute()` does for a predicated type-3 packet.
- **CP** (`gfx/pm4_engine.cpp`): the bodies of 0x2D / 0x2F / 0x27 (with its window rescue) / 0x2B /
  0x46 / 0x60..0x63 moved verbatim into `Pm4Engine::apply*`; `execute()` and `nativeRecord()` call
  the same methods. Nothing PM4-shaped lives in a record; the CPPM4 split renders every kind back to
  its packet for the oracle run.
- **Producer** (`Driver::bindState`): 82108668 + 82107110 + 82108470 + 82108300 ported with the
  masks from the asm (ENGINE_MAP "The shader bind"); the guest helpers 82108520 (in-place patch
  decision + cache), 82107A30 / 821081C0 (fetch patching of the ring copy) and the reserve 820FC910
  (the XDK's own segment flush) stay guest calls - computations and ring management, never an
  emitter. Rule 1: one entry, no fallback inside it (there is no decline case: the XDK's `!vs` early
  return is the only exit). Rule 2: a CONST_LOAD invalidates its ALU groups in the dedup mirror at
  commit, exactly as the adapter's scan treated the guest's packet. The immediate VS load keeps the
  XDK's ring copy (patched there by the guest helpers), and the record carries the patched bytes.
- **CHECKs.** Generation: the S1 dry CHECK runs pointer binds to the end (82108520's decision ported
  side-effect free; the dword +0x2A38 is now part of the device compare) and stops at the immediate
  path (`immStops=`). Execution: `SR_NATIVE_FLUSH_CPPM4=1` + `SR_NATIVE_CONST_DEDUP_CHECK=1` under
  the dense sim freeze, exact; the frozen-pass A/B `SR_AB_TOGGLE=SR_NATIVE_BIND,SR_AB_PRESENT=1`
  (even presents native, odd the guest call), exact; both repeated under `SR_NATIVE_SEG_STRESS=1024`
  (the XDK's own kick at the XDK's own check sites once 1 KB went into the segment: the switch
  inside a bind / reserve on roughly every other draw; `epochStale=` must stay 0).
- **Results (proof batch 1, exe eb60db4baff2 = commits through a1ac4535, gameplay / port, uncapped,
  `archive/v1_native/p1a_*`; all numbers measured):**
  - Replay 9/9 byte-identical against the P0 census exe 1b27b69dd6ab (`p1a_replay`): the handler
    factoring left `execute()` unchanged.
  - Dry CHECK (`p1a_chk`, every draw): 22.8M draws, 19.98M full comparisons, **BAD bytes 0, len 0,
    device 0** (the +0x2A38 flag dword included); 8.64M binds, 3.61M pointer IM_LOADs, 5.74M
    LOAD_ALU_CONSTANTs byte-identical to the XDK's; 2.65M dry runs stopped at the immediate path.
    Native dry bind 0.119 us/draw against the guest body's 0.400.
  - Execution CHECK (`p1a_cppm4`, CPPM4 + dedup CHECK, dense freeze), gate grader: **PASS**, 12
    ingame freezes, 10 exact over 39 pairs each, 2 with a 1-3 px speck at maxdiff 2; `bad=0 torn=0
    NEVER-EXECUTED=0 epochStale=0`; dedup `cpMarked=952,611 ok=952,611 BAD=0`; 16.2M binds (5.14M
    immediate, 6.69M pointer, 10.9M constant loads, 13.5M helper calls, 6,218 reserve flushes).
  - The same under `SR_NATIVE_SEG_STRESS=1024` (`p1a_stress`): the kick fired on 20.0M of 38.2M
    draws (52%), 4,187 reserve flushes, **epochStale=0**, dedup `ok=585,603 BAD=0`; gate grader
    **PASS** (8 exact, 4 specks of 1-2 px at maxdiff 2).
  - Frozen-pass A/B under stress (`p1a_abp`, `SR_AB_TOGGLE=SR_NATIVE_BIND,SR_AB_PRESENT=1`): gate
    grader **PASS** (7 exact, 5 specks of 1-3 px at maxdiff 2); binds 6.24M against 10.97M always
    on, so the flip took effect; the `[AB_PRESENT]` lines were lost to the log tag table (registered
    since) - batch 2 repeats it with the log and a guest-bind control freeze.
  - Meter (`tools/perf/meter_delta.py p1a_meter0 p1a_meter1`, per ingame frame, bind off -> on):
    **pm4 6,869 -> 3,283 (-3,587, -52%)**; by kind shader 938 -> 107, const 789 -> 37, bin 1,494 ->
    58, other 355 -> 46, t0 2,395 -> 2,246; native records 4,476 -> 3,347 per frame. Rows 1+2
    removed more than the census' 1,890 because the bind's Z-pass bin masks and SET_CONSTANTs had
    been filed under row 3.
  - Speed (`p1a_ab`, in-roll A/B, 59 windows): tick +0.005 +-0.437 ms (paired +0.098 +-0.368), frame
    p50 -0.065 +-0.323 ms, fps -1.0 +-6.3 (paired +0.57 +-2.78), process CPU -70 +-109: no slower.
  - Bugs caught before the bake: a SET_BIN_MASK record with two body dwords (b423239e); the fifth
    argument of 82107A30 (a1ac4535, ENGINE_MAP 7.3a).
  - The flicker gate on this exe with `SR_NATIVE_BIND=1` (`p1a_gate2`): **FLICKER_GATE PASS** - span 0 of
    358 presents, freeze 12 ingame freezes counted, 0 bad, all 12 exact (no speck in the plain record
    path; the specks above came from the CPPM4 / stress / toggle instrument rolls).
  - Batch 2 on the baked exe 0c1a0131b6c1 (default ON): PROGRESS (replay 9/9, dry CHECK 0 BAD, CPPM4
    12/12 exact, the logged frozen-pass A/B 12/12 exact, control 11/11, speed A/B no slower, gate
    PASS once its freeze era starts at play). Merged with dev fc7bc907: gate PASS (span 0/358,
    freeze 12/12). **On dev at 10d75253.**
  - **Ledger correction (2026-10-09):** the guest-bind control (`p1b_ctrl0`, `SR_NATIVE_BIND=0`) is no
    control: its 3D world is black in all 11 graded freezes (the HUD over black; with the bind OFF, so the
    startup race fixed in 41572437, which blacks rolls of every build, is the likely cause), so its "11/11 exact"
    says nothing about the guest path's draws. The frozen-pass A/B that carries
    rows 1+2 is `p1b_abp` (2,062 `[AB_PRESENT]` lines, 12/12, the world drawn in all 12: 2 in play, 10 on the
    pause menu over the drawn world); batch 1's `p1a_abp` logged no `[AB_PRESENT]` line, so its toggle is
    unwitnessed and it is not cited as the A/B. **2026-10-10:** re-rolled on the pause-free tape as `ow1_abp12`
    (exe 1a80874edac7, 12/12, gameplay 12 + menu 0, 458 static pairs under rule R); p1b_abp is a MENU-HEAVY witness.
**Record interface for strand B (2026-10-08, the owner's "speed up the conversion": B joins P1 once
its EDRAM surface table merges).**
*Extension (2026-10-09, the row 9 decision, native-a; kernel/native_flush.hpp `class Entry`):* an entry
is a SEQUENCE of spans cut where the XDK cuts them. `kick()` = the XDK's kick check at its own site
(close the span, 820FD8C0, reopen; STRESS honoured); `alloc(dwords, align)` = 820FCDF0 between spans
(the cursor published first, re-read after; the abstraction P2-B swaps for a host staging block);
`guest(fn, r3..r7)` = any other helper between spans; `opaque(kind, payload, n, xdkBytes, pred)` =
the owner's item, any size, several per span; `run(reg, count, beValues, pad)` = a plain type-0 run.
A span shorter than the 16-byte marker is written as literal PM4 by the Driver, through the owner's
renderer registered with `setOpaqueHandler(kind, apply, render)` (the same renderer serves the CPPM4
oracle split, which now covers opaque spans too); without a renderer such a span is NOPs and
counted (`noRender=`). B's payload is its own pre-decoded op list (type-0 runs + type-3 packets in
XDK order, predicated as written, applied through nativeRun / nativeDraw / applyImLoadImmediate /
applyEventWrite; the resolve through pm4ExecResolveRegs), not the bare edram::Regs snapshot - the
copy registers, the Phase-C runs, VGT_INDX_OFFSET and SQ_GPR_MANAGEMENT persist in CP state with no
dirty bit. Rows 4+6+7 keep their Driver code (the same primitives); the Entry's implementation, not
its interface, changes at P2-B. Split of the remaining producers (IDA xrefs, 2026-10-08):
- **B takes row 9 whole:** `Resolve 8210C5F8`, `ClearF 82101A58`, `Clear`, and
  `GpuRingEmitRegisterBlock 820F5570` (it is the ClearFullSurface preamble: 0x5C8 + 8x0xD00 then
  `D3DDeviceClearFullSurface_82100488`; callers InitDefaultRenderStates, MovieCapture,
  RenderScriptRunTechnique) - the row-8 "XDK-internal draws" (DRAW_INDX_2) are these clears.
- **A keeps:** row 3 (821072F8 tiled scissor, 821075E0 constant runs, `SetPredication 8210D400` - an
  8-byte SET_BIN_MASK_LO from ClearF / Begin-EndZPass / EndTiling / RwDrawInstancedConstBatched /
  SetPerTileConst / RenderScriptRunTechnique, too short for a P1 record on its own: P2), rows 4+6+7
  (the third flush), row 5 (setters), `EmitPm4Packet 82113010` (a SET_CONSTANT emitter the game
  calls from `LionRenderBackend_SetPerTileConstC100_827333E0`: the SET_CONST kind), row 10 / P2.
- **The contract B's rows follow** (kernel/native_flush.hpp, the same one rows 1+2 use):
  1. Kinds `kRkResolve = 7`, `kRkClear = 8` (`kRkClearFull = 9` if ClearFullSurface's snapshot
     differs); payload = B's `edram::Regs` snapshot memcpy'd (host order, POD, no pointers), the
     tag's n = its dword count; the CP rejects a record whose n differs from `sizeof(edram::Regs)/4`
     (the version check). Flag bit 1 = predicated, re-tested at every execution as for every kind.
  2. Execution: `Pm4Engine::nativeRecord` dispatches the kind to `pm4ExecResolveRegs(regs, bi)` /
     B's clear entry with the same arguments the PM4 path hands them at the copy-mode draw; A adds
     the dispatch lines (pm4_engine.cpp is A's), B owns the functions. The CPPM4 oracle split needs
     `renderResolvePm4(const edram::Regs&, std::vector<uint8_t>&)` from B (the XDK's packet sequence
     for the snapshot) or row 9's execution check is the frozen-pass A/B + B's EDRAM oracle alone.
  3. Producer: A provides `sr::native::flush::Entry` (native_flush.hpp; built, unused until B's hooks):
     `Entry e(ctx, base, dev); if (!e.open()) -> the guest entry runs; if (!e.packet(kind, payload,
     dwords, xdkBytes)) { e.abandon(); guest entry; } else e.close();` - it opens a record span at
     the cursor (the marker, the arena, the dedup stream position, the kick when the XDK's threshold
     is crossed) exactly as the draw flush does; the payload is copied at packet(); abandon() leaves
     no trace. The CP side: `setOpaqueHandler(kind, fn)` at init, `fn(kind, w, n, eng)` runs the
     snapshot (false = the record counts as bad). The `[NATIVE_FLUSH]` line reports `entries=
     entryPkts= entryDecl=`.
     `xdkBytes` = the ring bytes the XDK entry would have written for this call (the marker covers
     them in P1; the P0 bracket census per call type or a dry port of the entry gives the number; it
     must be exact, as the dry CHECK proves for rows 1+2). B hooks the entries in its own files and
     fills the snapshot from the device shadow and the call's arguments the way the XDK's packets set
     the registers.
  4. The four rules: the entry converts whole or declines whole before writing (a decline = the
     guest entry, counted); no cache updates while planning (a resolve writes no ALU constant, so
     the dedup mirror is untouched; the Entry still keeps the stream position); the execution
     CHECK = the frozen-pass A/B per present under `SR_NATIVE_SEG_STRESS` + the EDRAM oracle; the
     flicker gate before the bake; replay 9/9 (traces run no guest code); the meter delta per row.
**P1 census 2 (2026-10-09, merged exe 69ac605365e5 = rows 1+2 + strand B's EDRAM zero; roll
`p1m_census`, `SR_NATIVE_BRACKETS=1`, ingame 113.8-232.0 s, 6,400 frames) [measured]:
3,409 packets / 16,774 dwords (65.5 KB) per frame**, against the P0 census' 6,776 / 64.2K (251 KB).

| Owner | pk/frame | dw/frame | What is left there | Row |
|---|---|---|---|---|
| 820FBBF8 DrawVertices flush | 285.6 | 4,600 | the span markers (other 140.7) and the segment-limit fallbacks to the guest leaves (type-0 0x44xx 31, 0x48xx 18, 0x40xx 18, 0x0Axx 15) | P2 (no ring, no limit, no markers) |
| 820FB6E8 BeginVertices | 691.7 | 2,792 | the third flush's sections and bind | **4** (built, SR_NATIVE_BEGINV) |
| 8210C5F8 Resolve | 782.6 | 2,698 | register sets 0x20xx-0x23xx, the copy draw | **9** (strand B) |
| 820FBBB0 DrawVerticesUP | 470.8 | 1,996 | the flush inside it (same hook) | **4** |
| 820FBBA0 EndVertices | 408.6 | 1,634 | the draw packets the flush left before dev+40 (DRAW_INDX 102, 0x21xx / 0x48xx / 0x50xx 102 each) | **4** |
| 820F5570 ClearFullSurface preamble | 249.1 | 876 | DRAW_INDX_2 96, 0x0Dxx 36 | **9** (strand B) |
| 82101A58 ClearF / 82101998 Clear | 174.0 / 27.2 | 647 / 117 | register sets, IM_LOAD_IMMEDIATE 8 | **9** (strand B) |
| 820FC000 DrawIndexed flush | 56.2 | 495 | kicks and fences inside it (EVENT_WRITE_SHD 7, INDIRECT_BUFFER 7, WAIT_REG_MEM 7, 0x0Axx 13) | 10 / P2 |
| 82113010 EmitPm4Packet (SetPerTileConstC100) | 38.8 | 267 | 3 alignment NOPs + SET_CONSTANT per call | A, small (SET_CONST kind) |
| SetViewport / SetScissorRect / SetTexture | 38.5 / 21.3 / 20.3 | 320 | single 0x20xx registers, 8-byte packets | 5: P2 (too short for a marker) |
| Swap, ReplayDriver (IBs), VertexBuffer_Lock, PresentEmit, SetPredication, BeginZPass, InsertFence, EndZPass, RING_UNSTAMPED | 66 / 24 / 12 / 11 / 13 / 7 / 5 / 2 / 4 | ~330 | sync, swap, replays, predication | 10 / P2 |

Rows 4 (1,571 pk) and 9 (1,233 pk) take the count to ~600 per frame; what remains is the ring's
own transport (markers, fallbacks, kicks, fences, swap) and the 8-byte setters = P2.

**P1 rows 4+6+7 RESULTS (2026-10-09, native-a; commits 61e21413 / 976b1871 / 3b5c63f9; gate `SR_NATIVE_BEGINV`,
default ON since 3b5c63f9) [measured, archive/v1_native/p1c_* and p1d_*]:**
- Batch 3 (exe 2ccd17629efa) CAUGHT A BUG: dry CHECK `BAD bytes=897,670` of 23.86M draws (every example kind=2: a
  pointer IM_LOAD + INVALIDATE_STATE of 20 bytes the XDK did not emit), execution CHECK freezes differing. Root
  cause from the asm (820FB704 / 820FB71C): bit 51 (re-patch the fetches) is set when the new stride/4 differs
  from the stride the bound VS was PATCHED with (dev+0x2D20), not from the stream stride dev+0x2FE0. Fixed in
  976b1871; the rest of 820FB6E8 re-read against the asm and found as ported.
- Batch 3b (exe 89367177b121): replay 9/9 byte-identical vs 1b27b69dd6ab; dry CHECK (p1d_chk) `draws=24,641,536
  BAD bytes=0 len=0 dev=0`; execution CHECK (p1d_cppm4, CPPM4 + dedup CHECK, dense freeze) grade PASS 12/12 with 0
  differing pairs in every ingame freeze (freeze_diff's "FAIL 1,2,3" = the pre-play cutscene / loading freezes
  outside the graded era - read grade.py, not freeze_diff's verdict), dedup `cpMarked=32,074 ok=32,074 BAD=0`;
  stress (p1d_stress, `SR_NATIVE_SEG_STRESS=1024`) grade PASS 12/12 (five freezes with 1-30 px specks at
  maxdiff 2, the rows 1+2 precedent), `epochStale=0`, `beginV=1,701,449 allocFail=0`; frozen-pass A/B under stress
  (p1d_abp) grade PASS 12/12, `[AB_PRESENT]` 1,635 lines in strict alternation, `beginV=1,011,459`.
  **Ledger correction (2026-10-09): that A/B is void.** Its 3D world is black in all 12 graded freezes (11 are
  black frames - the HUD over a black world, later not even the HUD - and one is the pause menu over black), so
  native and guest presents were compared over black frames. The roll predates the attach line of the startup-race
  fix 41572437, so it cannot say whether the race or the toggle blacked it (p1d_cppm4 / p1d_stress, row on, drew
  the world). Rows 4+6+7 were baked (3b5c63f9) without a valid frozen-pass A/B; it was
  owed on a base with 41572437 (`SR_AB_TOGGLE=SR_NATIVE_BEGINV,SR_AB_PRESENT=1` under the dense freeze) and
  **paid (2026-10-10) by r467b_abp** (the stress A/B on the post-hooks base, `SR_NATIVE_SEG_STRESS=1024`, exe
  d855d0e75a9b): grade PASS counted=12 bad=[], world 12 (gameplay 11 + menu over world 1), 2,524 `[AB_PRESENT]`
  lines, 451 static pairs graded under rule R with 17 unsettled pairs excluded (its freeze-6 burst was a
  same-transport transient: `abp_transient`, resolved). The obligation p1.rows467 is closed: rows 4+6+7 are baked
  with their A/B. The rest
  of the execution evidence stands: p1d_cppm4 and p1d_stress drew the world in all 12 graded freezes, and so did
  the gates p1d_gate and p1e_gate.
- Meter (`meter_delta.py p1d_meter0 p1d_meter1`, per ingame frame, BEGINV off -> on): **pm4 3,275.6 -> 1,817.9
  (-1,457.7, -44.5%)**; by kind t0 2,241 -> 1,169, t2 pads 395 -> 208, auto-index draws 245 -> 131, shader
  107 -> 44, const 35.5 -> 13.8; native records 3,344 -> 3,507 (+163). From the P0 start: 6,776 -> 1,818
  (-73%).
- Flicker gate on the gated exe (p1d_gate): **FLICKER_GATE PASS** - span 1 of 358 presents with one A-B-A
  outlier block (p20137, lo-diff 6.7 at threshold 6.0, in a cape-in-motion triple; fail at 2), freeze
  counted=11 bad=[].
- Speed (p1d_ab, in-roll A/B, 59 windows): tick -0.084 +-0.334 ms (paired -0.110 +-0.265), frame p50 +0.096
  +-0.296 (paired +0.114 +-0.161), fps -1.3 +-4.9: neutral.
- The bake (3b5c63f9, exe 98bb82be4686 on dev e22478ab; p1_bake4.sh, p1e_*): replay 9/9; flicker gate with NO
  gates **PASS** (span 0 of 358, freeze counted=12 bad=[]); default meter roll **pm4 1,825.9 per ingame frame**
  (`meter_delta.py p1d_meter0 p1e_meter`: -1,449.7; t0 1,172, t2 pads 211, auto draws 131, shader 44, const
  14.3, sync 74, event 29, bin 58, other 46), `edram_xfer=0`, `beginV=2,681,349 allocFail=0`, 0 ICALL_FAIL /
  NATIVE_FLUSH_BAD / GPU_HANG. Merged to dev d0559db8 (fast-forward).
- **Row 3 BUILT (10008511, exe 2b1d4bc48e41, gate `SR_NATIVE_WSCISSOR` default OFF):** the tiled window-scissor
  section `GpuCmdEmitWindowScissorState_821072F8` (the guest call inside the row-4 flush) as Driver items from its
  asm: the entry kick check, SET_BIN_MASK_HI {0}, per tile SET_BIN_MASK_LO {3 << 2i} + the predicated SET_CONSTANT
  {0x40203, (tileOff << 17 | (a3 & 0x1FFFF)) & ~1} with the kick check after each tile, the Z-pass tile (byte
  10808 & 0x40, a3 bit 0), LO / HI from dev+12432 / 12436; untiled = the 8-byte run {0x2203, a3}; the 64-bit
  `r4 & ~0x100` return. Proof batch `archive/v1_native/p3_batch1.sh` (p3a_*) queued behind the segmentless proof.
  Still guest inside the flush: `GpuCmdEmitShaderConstantRuns_821075E0` (the 0x2388 constant groups, d4 bits
  49-54: a COHER_DEST_BASE_1 / COHER_SIZE / BASE / STATUS preamble + the register-mode WAIT_REG_MEM, then runs
  with the chunked fallback 82107058) - a kind-10 Pm4 record for the WAIT once the census ranks it.
  **Row 3 proof (p3a_*, 2026-10-09, exe 2b1d4bc48e41) - NOT proven (ledger, 2026-10-09).** What holds: replay 9/9
  byte-identical; dry CHECK `draws=23,068,672 BAD bytes=0 len=0 dev=0`; the CPPM4 roll p3a_cppm4 grade PASS 11 with
  the world drawn in all 11; the flicker gate p3a_gate PASS (span 0 of 358, scene luma 23.4; freeze 12/12, 10 of
  them on the pause menu over the drawn world). What does not: the frozen-pass A/B p3a_abp (1,699 `[AB_PRESENT]`
  lines, grade PASS 12) and the stress roll p3a_stress (PASS 11) graded BLACK frames - in every graded freeze the
  3D world is black (the HUD over black, then the pause menu over black) - and the meter roll p3a_meter1 is black
  in all 46 ingame sparse frames, so its 1,768.6 -> 1,763.6 (-5.0) is no measurement of the drawn game. These rolls
  predate the attach line of the startup-race fix 41572437, so they cannot separate the race from the row. Row 3 stays default OFF until the A/B and stress rolls pass on a base with the fix.
- **Row 3 BAKED (native-a, 2026-10-10, the orchestrator's go; `SR_NATIVE_WSCISSOR` default ON; exe 47e80a6f28a0 on the
  dev merge 09945515):** the proof on the post-hooks base (p1.row3, exe d855d0e75a9b, a drawn world, 11 in-game freezes,
  gameplay-dominant - gameplay 10-11 + menu 0-1): replay 9/9; dry CHECK 0 BAD; CPPM4 11/11; stress 11/11 (the re-run
  r3b_rerun: the first roll's 68-px one-present LSB step on freeze 16 did not reproduce, `stress_lsb_step` resolved);
  frozen-pass per-present A/B vs the PM4 path PASS under rule R (406 static pairs, 23 unsettled excluded, 0 differing);
  flicker gate PASS (span 0 of 358, freeze 11/11); meter -23.1 pm4 per ingame frame (t2 pads 222.4 -> 206.0). The
  bake proof on the baked exe with no env (plan `row3_bake`, r3e): replay 9/9 byte-identical vs the pre-bake exe
  d855d0e75a9b; the world-rule flicker gate PASS (span 0 of 358, freeze 11/11, gameplay 11 + menu 0); the default-path
  meter roll comes with the rows 5+3b bake's combined roll (its first attempt, r3e_meter_void1, was a boot H1 void at
  9.8 s - the first-present hang the installer lane owns). The `recb::checkArmed()` guard stays owed with row 9's
  landing.
- **Row 5 BUILT (28fe875d, exe 566501cc211a, gate `SR_NATIVE_TILERECTS` default OFF):** the setters' one emitter
  `D3DDeviceEmitScissorTileRects_820F0208` (SetScissorRect 820F2720 tail-calls it; SetViewport and the
  render-target setters reach it through SetScissorRect; also the segment re-emit 820F0048 from DeviceCreate /
  EndTiling / InvokeRenderPass / Swap, BeginTiled 820FAEE0, BeginTiling 8210D588, BeginZPass 8210DF38, and strand
  B's ClearFull / ClearImpl / Resolve guest bodies - B: its own entries never nest it) as an Entry, from the asm
  (`Driver::tileRects`, shared with the dry CHECK kind 5): the entry kick check, SET_BIN_MASK_HI {0}, per tile
  SET_BIN_MASK_LO {3 << 2i} + the predicated SET_CONSTANT {0x40080, (-y, -x) 15-bit, TL, BR} of the rect clamped
  to the tile (signed compares, all zero when empty) with the kick check after each tile, the Z-pass tile
  (bytes 10811 & 0x20 and 10808 & 0x40: LO {1} + SET_CONSTANT {0x40081, TL, BR}), LO / HI from dev+12432 /
  12436; untiled the 16-byte run {0x2080: 0, TL, BR}; r3 as the asm leaves it (dev, the flush's cursor after a
  kick, 3 after a tile). `Entry::pkt` (BinReg / SetConst packets) and `Entry::tileRects` added to the Entry API.
  The CHECK: the override brackets the guest body with checkBefore / checkAfter kind 5 under
  `SR_NATIVE_FLUSH_CHECK` (the leaf declines there, g_on off), the items rendered as PM4 against the guest
  bytes. Before the bake: the leaf takes the guest body while strand B's `recb::checkArmed()` is set on the
  thread (B's dry CHECK compares its producers' guest bytes; lands with row 9). Proof batch
  `archive/v1_native/p5_batch1.sh` (p5a_*) queued behind row 3.
- **Rows 5 + 3b BAKED (native-a, 2026-10-10, the orchestrator's go; `SR_NATIVE_TILERECTS` and `SR_NATIVE_TILEPRED`
  default ON):** the proof on the post-hooks base (p1.row5 / p1.row3b, plan rows53b, exe d855d0e75a9b, a drawn world,
  gameplay-dominant freezes - gameplay 11-12 + menu 0-1): replay 9/9; dry CHECK 0 BAD; CPPM4 11/11; the first stress
  roll a boot void (r5b_stress: the live backend never attached under HDD contention, kept as recorded), the repeat
  (chain 5, r5b_rerun) stress 12/12, frozen-pass per-present A/B vs the PM4 path PASS under rule R (440 static pairs,
  28 unsettled excluded, 0 differing), meter -107.9 pm4 per ingame frame (1,771.7 -> 1,663.8; t0 1,172.5 -> 1,063.9),
  flicker gate PASS (span 0 of 358, freeze 12/12). The bake proof on the baked exe with no env (plan `rows53b_bake`,
  r5e, one slot hold with row 3's meter roll): replay vs the pre-bake exe 47e80a6f28a0, the world-rule gate with its
  gameplay / menu split, the default-path meter roll (the combined drop with row 3). The `recb::checkArmed()` guards
  stay owed with row 9's landing.
- **Row 3b BUILT (1a13ec7d, exe 8d733b47ee26 = rows 3 + 5 + 3b, gate `SR_NATIVE_TILEPRED` default OFF):**
  `D3DDeviceSetPredication_8210D400` (GpuSetTilePredicationMask; game callers RwDrawInstancedConstBatched 8261B050,
  Lion SetPerTileConstC100 827333E0, RenderScriptRunTechnique 8277B998; XDK callers ClearF, EndTiling, BeginZPass,
  EndZPass) as an Entry from the asm (`Driver::setPredication`, shared with the dry CHECK kind 6): the device writes
  (dev+12432 = mask or -1, dev+12440 = mask != 0, byte 10808 bit 1 = tiled && !(byte & 8) && (byte 10811 & 0x20 ||
  bytes 11849 == 11915 == 0) when mask == 0, else cleared; dev+12436 = 0, dev+10800 = mask, dev+10804 = 0), the
  XDK kick check, SET_BIN_MASK_LO {dev+12432}; r3 = the cursor. Its generated N0 bracket shim is replaced by the
  override (325 shims). `Driver::tiledSurfaces()` = the XDK's shared tiled test (byte 10808 & 0x10, or & 0x20 with
  the five surfaces dev+12168.. equal to their shadows or zero), used by rows 3 / 3b / 5. To be proven together
  with row 5 in `p5_batch1.sh` (gates `SR_NATIVE_TILERECTS=1,SR_NATIVE_TILEPRED=1`, the frozen-pass A/B toggling
  both: `SR_AB_TOGGLE` splits on `,;+`, so `A+B` inside a `--gates` list).
- **Rows 5 + 3b proof (p5a_*, 2026-10-09, exe 9c6213c19acc) - NOT proven (ledger, 2026-10-09).**
  - Replay 9/9 byte-identical (the jobs file calls the tag "p3a", a clone leftover; it ran this exe).
  - The dry CHECK p5a_chk is not clean: `draws=24,379,396 BAD bytes=0 len=0 dev=768`, all 768 at dev+0x2D18 /
    0x2D1C inside the loading window (the deferred CPU-write range another thread moves). The CHECK excludes that
    range since, but no CHECK roll has run on the rows since.
  - The frozen-pass A/B p5a_abp (both gates toggled per present, under stress) FAILS: grade bad = freezes 5, 6
    and 8, which are themselves black frames.
  - The CPPM4 roll p5a_cppm4 (PASS 10) and the stress roll p5a_stress (PASS 11) graded black frames only: the 3D
    world is black in every graded freeze.
  - The isolation roll p5b_rects (row 5 alone, with strand B's row 9) went black from present ~14,400 to its end
    (68 of its 97 sparse frames under the luma floor).
  - What holds: the flicker gate p5a_gate PASS (span 0 of 358, scene luma 24.9; freeze 11, 10 of them on the
    pause menu over the drawn world) and the meter p5a_meter1 (1,768.6 -> 1,672.2, -96.4 per ingame frame; the
    world drawn in 39 of 40 ingame sparse frames).
  - These rolls predate the `[DISPLAY] PM4 live backend attached` line, so they cannot say whether their black
    frames came from the startup race (41572437; it blacks rolls of every build) or from these rows. Either way the
    bar has to pass again on a base with the fix before rows 5 and 3b are called proven.

**P1 -> P2 plan (strand A, 2026-10-09; the decisions on "P2 CONTRACTS (research)", which this follows).**
- **Order after batch 3 (rows 4+6+7):** (1) the `SR_P2_TRACE` baseline (P2.7 item 1, observe-only hooks) on
  the ring path; (2) **P2-A**, the host IB queue; (3) **P2-B** with the **virtual cursor** as the proof mode
  (P2.8); (4) measure the coarse-close mode against it (renames per frame, pixels) before choosing. Strand
  B's row 9 and the research agent's P2.9 fixes run in parallel; both are P2-B prerequisites (no guest
  emitter left; pacing matching the oracle first).
- **Rows 5, 82113010 and the row-3 residue are P2-B items by construction, not P1 rows:** the setters and
  `SetPredication` write 8-byte packets, which cannot carry a 16-byte marker; `82113010` returns a pointer the
  caller fills after the call (hazard 1) so its values cannot travel in a P1 record; `821075E0`'s preamble is a
  WAIT_REG_MEM (a sync item). Without the ring they are plain register / constant / sync records.
- **P2-A design.** Override `GpuRingUpdateWritePointer_820FD218` and `GpuRingEmitBinSelectLo_82110140`
  (platform-fix rows in OVERRIDES.json, gate `SR_NATIVE_RINGLESS`): items `{IB phys, dwords}` / `{BS value}`
  go to a host queue the CP thread consumes in order (`execute(IB)` as today / `applyBinReg`); the override
  keeps `dev+10820` advancing mod mask, the consumer writes fb+0x3C = its consumed index, no doorbell MMIO.
  Everything else stays guest (segments, fence tails, kick blocks, pump, bracket replay). Proof: the
  `{IB,BS}` sequence per frame identical ring vs queue (toggle per present at the throttle point with the CP
  drained), fence values identical, plus the standard gates. First in-roll A/B: queue vs ring parse cost.
- **P2-B design.** Records go straight to host lists: the S1 Driver without markers or padding, its Item byte
  lengths feeding the virtual cursor so closes happen where the XDK's would (the ring path's S sequence kept,
  the fence-trace oracle exact). New kinds (one implementation each, CPPM4-renderable where a PM4 form
  exists): FENCE `{S, progress}` (the consumer writes fb+4 then fb+0 = C on reaching it; held to R for
  submissions carrying a CPU-readback item), INVALIDATE `{base, size}` (drop host mirrors over the range),
  CALLBACK `{cb, ctx, cpuMask, waitIdle}` (pc+0x10/+0x14/+0 written, the guest ISR `820FDBA0(1, dev)` run as
  a DPC, pc+0x10 = 0x0BADF00D after), GATE `{arg}` (pc+4 = 1, the pacing callback, then every later item held
  until pc+4 == 0), SWAP `{fetch constant, fb, w, h}` with native Swap porting the body's device writes and
  keeping the N-1 throttle as the guest's own wait, BRACKET begin / end + BS (the Z-pass list owned by the
  consumer and replayed twice: 0x15555555 depth, 0x2AAAAAAA colour; `dev+0x2A74` never moved by a native bracket
  (the BRACKET build note; in the SDK 820FD800 reads it unlocked and goes direct when 0, takes the spinlock
  dev+0x2A78 and re-checks when non-zero, EndSync 82110768 decrements it under the lock)), ZPD / EXT (slot words
  written before the covering fence). Deferred-fill pointers (BeginVertices, 82113010): a host-owned guest
  staging block handed out, sealed at EndVertices / the next device entry, recycled after the record is applied
  (hazard 1); UP vertices copied into staging under the same rule (hazard 2). The consumer = the CP thread's
  record loop: apply, emitDraw and drain on one
  thread, hand-off per close (the FENCE item is the batch boundary), every referenced guest range read at
  apply before the covering close (hazard 3: no lazy caches, no second thread behind the fence writes).
- **STATUS 2026-10-09 (native-a):** the trace instrument is BUILT (fb24869d: `SR_P2_TRACE`, kernel/p2_trace.cpp,
  tools/perf/p2_trace.py, TOOLKIT 1.8g; XdkScope hook table for the N0 shims, observe overrides of 820FD580 /
  82110140, CpHooks::trace for fb+0 / fb+4 / scratch, ISR, present, 82108520's answer) and P2-A is BUILT
  (kernel/gpu_ringless.cpp, `SR_NATIVE_RINGLESS` default OFF: the CP loop consumes ring index space in order and
  takes a host item where one occupies the index, so ring and ringless presents interleave with no drain; the
  per-present A/B is the identity proof). The baseline (p2_batch1) and the P2-A proof (p2_batch2) are recorded
  below; P2-A is baked. **P2-B step 1 is BUILT** (cebef9ca, `SR_NATIVE_SEGMENTLESS` default OFF: 820FD8C0 as host
  code, the segment as a part list of arena records + gap PM4, FENCE / INVALIDATE records, SEGMENT items on the
  P2-A queue, the virtual cursor = the Driver's). Its proof batch (p2_batch3) STOPPED twice and has not run since
  (ledger, 2026-10-09): first p2c_abp crashed at boot (a record at a null arena, fixed ebf40ca5); then p2c_abp and
  p2c_cppm4 wedged at present 55 / 57 (a stale segment-registry entry, fixed 1a0081dc) and graded nothing. Only its
  replay (9/9) and the ring-only control p2c_ctrl ran to the end; segmentless itself is unproven.
  **2026-10-10 (native-a):** the wedge is root-caused from the archived trace and the transport fixed - a segment's
  identity is an id handed from the close to its own publish, never a lookup by phys (the guest's 11-dword flush
  block at a reused phys was answered with the native 22-dword coherency block registered there 51 presents
  earlier); only the Z-pass bracket replay resolves by address, scoped to its bracket. Design, evidence and the
  owed proof: `docs/NATIVE_P2B_SEGMENTLESS.md`; the proof runs as `tools/perf/native_proof.py --plan segmentless`.
  **2026-10-10 later (native-a, segd):** its probe wedged again at present 25, after the buffer wrapped. The CP
  stall dump shows the kick block exactly as 820FDA80 writes it (not overwritten); the cause is the close itself:
  `coherencyBlock`'s `buildParts` over the tail block dropped every footprint of the open segment as "behind the
  range", so each non-pinned segment was published as gap bytes - the native records' positions executed as
  unwritten memory on lap 1 and as the previous lap's PM4 after the wrap (the boot init block's SCRATCH_REG4 =
  0x0BADF00D replayed from 1E820000). Evidence and the proposed fix (a footprint belongs to the segment it was
  written in, by sequence): `docs/NATIVE_P2B_SEGMENTLESS.md` 2b / 2c; ledger contradiction
  `p2.segmentless/close_dropped_feet`.
- **P2.7 item 1 RESULT - the ring-path baseline (2026-10-09, roll `archive/v1_native/p2a_trace`, exe 1a077cdf855d,
  gameplay / port uncapped, default gates + `SR_P2_TRACE`; 223 s, 29,351 presents, 131 fps; 24.4M records;
  `p2a_trace_summary.json`) [measured]:**
  - Per present: closes 7.49 (= COMPLETE = PROGRESS writes), IBs 33.5 (sizes: the 11-dword coherency block 8.5,
    the ~57,325-57,329-dword segments ~7, the 23-dword kick block 1), BS 4.0 exactly {0x15555555, 0xFFFFFFFF,
    0x2AAAAAAA, 0xFFFFFFFF}, kicks (820FD8C0) 16.8, scratch write-backs 9 (reg0 2, reg1 1, reg4 4, reg5 2), ISRs
    2 (mask 0x4 only), InsertFence 1, in-place patch decisions 300 (36% in place), ring-space waits 171 (never
    blocking: 109 of 5.0M over 50 us, max 14.9 ms), read-pointer wrap waits 37.5 (max 66 us).
  - Invariants on the ring path: S odd and +2 on every one of 219,739 close steps; C never ahead of S; every
    COMPLETE is a closed S in close order; one close open at exit. `IsFencePending` is NEVER called (0) and
    `QueryGetData` 0 (no ZPD / EXT queries in this era: the research agent counts 0 in the gp1 / gp2 / menu / lg1
    traces too; the only callers are the sun flare 82721118 and the HUD glow 82304058). So the guest's only
    fence answers are GpuBlockOnFence and the patch decision.
  - Waits (GpuBlockOnFence): swap (reason 3) once per present, p50 1 us, p99 0.83 ms, max 75 ms, 1.35 s total;
    idle (reason 4) 6,941 calls, p50 0.57 ms, max 726 ms, 5.8 s total (2.6% of the run - the one wait class
    P2-B must keep cheap); reason 10 64K calls / 76 ms; reason 14 20.8K / 5 ms.
  - Pacing: present p50 7.28 ms, p99 34.3 ms.
  - Wait attribution (the begin record's ctx.lr): every idle wait (reason 4) comes from `D3DDeviceBlockUntilIdle_820FDCB8`
    (return 820FDD10), 6,941 calls = one per 4.2 presents, each started 3 closes behind (F - C = 6 at p50 and p90,
    max 8); the swap wait from `Swap_82112050` (820FD9A8 at 0x8211271C, mean 46 us); reasons 10 / 14 (85K calls,
    82 ms total) carry a stale lr (the game's module-update vtable call 8271EE0C: the recomp updates ctx.lr only at
    indirect calls), i.e. the lock core's callers' reasons - negligible time. P2-B keeps BlockUntilIdle as the
    guest's own spin on fb+0 == S and dev+0x2A74 == 0; the 2.6% it costs today is the consumer's latency to the
    last close, so the ringless consumer must finish a close no later than the CP does now.
  - The ZPD fake roll was skipped by the batch (no queries on this path); SR_CP_ZPD_FAKE stays default OFF.
- **P2-B BUILD PLAN (native-a, 2026-10-09, written before building, after the baseline and with P2-A in proof):**
  gate `SR_NATIVE_SEGMENTLESS` (default OFF), built inside the existing Driver / CP, in this order:
  1. **The one close site.** Every close - the kick checks, the reserve 820FC910, InsertFence, BlockOnFence(S),
     BlockUntilIdle, QueryGetData(S), Swap's two, the lock core - goes through `GpuCommandBufferFlush_820FD8C0`,
     whose body holds the segment flush 820FD660, the fence tail 820FD580, the coherency block 820FD3B8, the IB
     publish 820FD218 and the segment switch 820FCF90. P2-B's transport core is therefore ONE whole-body
     override: 820FD8C0 closes the Driver's open span to the arena, appends INVALIDATE {base, size} (the
     `dev+0x2D18` range swapped to -1, as 820FD3B8 reads it) and FENCE {S, progress} (S = `dev+0x2A1C`, then
     += 2; `dev+0x3298 / 0x329C` as today), and moves the VIRTUAL cursor to the next virtual segment start.
     While pinned (`dev+0x3290`, the Z-pass bracket) it appends to the bracket list and does not move S,
     exactly as the XDK's mode-B record. Nothing is written to the ring; the ring, the doorbell and the pump
     never run.
  2. **The virtual cursor.** The Driver already advances `c` by every item's exact XDK byte length; in P2-B that
     integer is the cursor: `dev+0x28 / 0x2C / 0x30` keep the values the XDK would hold (published at each span
     close), so the XDK's own kick checks and the guest helpers that read them (the allocator 820FCDF0, the reserve
     820FC910, 82108520's fence words, the lock core) behave as on the ring path, and the S sequence is the ring
     path's exactly (P2.7 item 2's exact oracle). The 2 MB command buffer stays allocated as the STAGING arena: the
     allocator carves vertex blocks from the same tail positions as today, records reference them by phys, and
     the consumer's FENCE apply writes fb+4 (progress) then fb+0 = S when it passes the close, so the XDK's own
     reuse wait (GpuRingWaitForSpace on fb+4) keeps the staging memory alive until applied (hazard 3). No marker,
     no padding, no segment-limit abort: closeSpan writes the record only.
  3. **The bridge for what is still guest PM4.** Until P1 is 100% converted (row 9 in progress, the 8-byte
     setters, SetPredication, 82113010, the DrawVertices residue), a guest emitter still writes PM4 into the
     (virtual) segment memory between spans: `afterGuest()` knows the bytes it wrote ([cursor before, cursor
     after)) and appends them as a **PM4 record (kind 10, literal dwords, applied by execute())**. Rule 1 holds
     per entry, P2-B can start now, and the meter keeps counting those packets until each producer converts
     (the sentinel tripwire = kind-10 records per frame reaching 0).
  4. **The consumer** = the CP thread's record loop: walk the arena from the tail in order (no ring parse), apply
     each record (the apply* set + opaque handlers + kind 10 + the transport kinds), emitDraw and drain on this
     one thread, hand-off per FENCE (the batch boundary), fb+0x3C unused. New transport kinds, each
     CPPM4-renderable where a PM4 form exists: FENCE, INVALIDATE, CALLBACK {cb, ctx, cpuMask, waitIdle} (the
     scratch write-backs pc+0x10/+0x14/+0, the ISR as a DPC on each CPU of the mask - today's onInterrupt -,
     pc+0x10 = 0x0BADF00D after; ONLY the Z-pass kick block 82110C78 -> 820FDA80 {82110B00, list} reaches it in
     this title: D3DDeviceInsertCallback_820FDE38 has 0 calls (research/p2 bec93f45, four gated rolls + the P2
     traces' 8,743 interrupts all cb 82110B00 or the vblank gate 82111E38), so the kind exists only until the
     consumer owns the bracket replay and the kick disappears), GATE {arg} (pc+4 = 1 and
     the pacing callback; every later item held until pc+4 == 0; from the Swap block), SWAP {fetch, fb, w, h}
     (native Swap 82112050: the body's device writes ported, the N-1 throttle kept as the guest's own
     BlockOnFence(dev+0x34D0)), BRACKET begin / end + BS (the Z-pass list kept by the consumer and replayed
     twice with 0x15555555 / 0x2AAAAAAA, `dev+0x2A74` never moved by a native bracket - see the BRACKET build note
     for the SDK's locking; from 82110B30 / 82110C78, which no longer kick the pump), WAIT_UNTIL (BlockUntilIdle:
     no-op item), ZPD / EXT (kept as the fake until a flare scene says otherwise).
  5. **Deferred-fill pointers** (hazard 1): BeginVertices / UP already fill their allocation before the next entry
     closes it; 82113010 returns a pointer the caller fills after the call: a staging block handed out and sealed
     at the next device entry (its SET_CONSTANT record is emitted then).
     **SPEC for the research agent (native-a, 2026-10-09, from the asm of 82113010 / 827333E0; build after step 1
     lands):** `D3DDeviceEmitPm4Packet_82113010(dev, type, index, count)` = `ReserveCommandSpace_820FC910(dev,
     4*count + 5)` (0 = failure, returns 0), then at the cursor c (dev+0x28, the last written dword): pad =
     `(-(c + 12)) & 15` bytes of type-2 NOPs (0..3, so the body is 16-byte aligned for the caller's XMemCpy), the
     SET_CONSTANT header `0xC0002D00 | (4*count << 16) | dev+12440` (the predicate flag SetPredication keeps),
     the offset word `(4 * (index - (type << 8))) & 0x7FC` (VS c100 -> 0x190, PS c100 -> 0x590), the body of
     4*count dwords the CALLER fills afterwards (Lion 827333E0: XMemCpy 16 bytes per call, VS then PS per tile,
     between its SetPredication calls); dev+0x28 = c + pad + 8 + 16*count; r3 = the body VA = c + 12 + pad. The
     only caller in this title is Lion's per-tile c100 (census: 38.8 packets / frame incl. the NOPs). Host form: a
     record kind **kRkSetConstMem (14)** `{tag(kind, n = 4*count, pred), offsetWord, bodyPhys}`; the CP applies it
     by reading n big-endian dwords at bodyPhys at APPLY time (as the PM4 path reads the ring) and feeding the
     SetConst apply; CPPM4 form = header + offset + memcpy(body). Producer: `Entry::reserve(dwords)` (= the
     Driver's reserveDw, 820FC910's semantics) then `Entry::setConstMem(type, index, count, bodyVA&)`: the item's
     XDK bytes = pad + 8 + 16*count, the body left to the caller. The body lives in the segment memory at the
     XDK's own position, so its lifetime is the segment's (fb+4). RING PATH CAVEAT: the span marker (16 bytes at
     spanStart+4) would overwrite the body when pad < 8 and the item opens the span, so on the ring path the item
     forces its span LITERAL (the XDK's bytes, written by the host, body by the caller = today's output) and is a
     record only under SEGMENTLESS (no marker) - the meter drops it there, which is the target transport. Dry
     CHECK kind 7: compare the first pad + 8 bytes only (the body is filled after the call). The override takes
     the guest body under `recb::checkArmed()` and the Driver's CHECK like the row-5 leaf. Expected meter: -38.8
     per frame under segmentless (25.4 NOP pads + 13.4 SET_CONSTANT).
     **BUILT (native-a, 2026-10-10, `SR_NATIVE_SETCONSTMEM` default OFF)**, from the generated body of 82113010
     (IDA's MCP was down; `guest/ppc/ppc_recomp.2.cpp` is what runs and confirms the spec line by line: the
     reserve of 4*count + 5, three unconditional NOP stores, pad = (-(c + 12)) & 15, the header with dev+12440
     OR'd in, the offset word, r3 = c + 12 + pad, dev+40 = body + 16*count - 4; IDA cross-check 2026-10-10, the oracle
     back: `slwi r30, r6, 2` / `addi r4, r30, 5` / three `stw` of `lis -0x8000` at r3+4.. / `subfic r4, r11, -8;
     clrlwi 28` / `oris 0xC000, ori 0x2D00, slwi r30, 16, or dev+0x3098` / `clrlslwi r5, r5, 23, 2` / `addi r3, r11,
     8` / `dev+0x28 = r3 + (r30 << 2) - 4` - identical). Record kind `kRkSetConstMem`
     (14, opaque; payload {header, offset, bodyVA}; `Driver::setConstMem` / `Entry::setConstMem`): the CP handler
     reads the values from the body at apply time and feeds `applySetConstant`; the renderer writes the XDK bytes
     with the body copied from memory, so the dry CHECK (kind 7) compares the whole item without a special
     prefix rule (the three unconditional NOPs and the unfilled bytes read the same on both sides) and the ring
     path keeps the span literal (`Driver::forceLiteral`: a marker over the caller-filled body would be
     overwritten). The record form, and the meter's drop, exist only under segmentless. The `recb::checkArmed()`
     guard waits for row 9's landing (its header is not on this branch). Proof: `tools/perf/native_proof.py
     --plan setconstmem` (dry CHECK, the ring-path A/B, the segmentless A/B + CPPM4, the meter under
     segmentless, the gate), recorded into `p2.setconst_mem`.
     **The unfilled body (native-a, 2026-10-10, IDA back as the oracle): 82113010 takes its count in vec4s** -
     `slwi r30, r6, 2` makes the SET_CONSTANT's count 4*count dwords (count 4 -> 16 dwords = c100..c103, dev+40 =
     body + 64 - 4) - while Lion 827333E0 fills 16 BYTES per call (`XMemCpy(v8, v7, 0x10)`), so the 12 dwords
     after c100 (VS c101-c103, PS c101-c103) are never written by the game: the CP reads whatever the ring held at
     that spot on the previous lap - on every run, with or without STRESS, on both transports, and on the console.
     The draw-ledger roll s1l_abpl showed it as its only non-clock candidate (`stress_tail_in_constants` on
     p1.s1_flush, first read as "a fence tail written over the body"): at draws 1856-1864 of the pre-game freeze
     c101 steps through the same six stale values present after present on both transports - an earlier lap's
     fence tail (`4194:C0003B00 4195:00007FFF 4196:C0025800`; STRESS closes a segment every <= 1 KB, so tails lie
     everywhere), vertex floats, text, 82113010's own earlier pad words (`01200084 80000000 80000000`) - the ring
     lap position, not the transport; c100 itself (0x4190-0x4193) never differed, which rules out any write over
     the body. STRESS changes which stale bytes, not whether. Who could read them: of the 172 VS translations in the
     installed cache none loads rows 101-103 by a literal index and of the 117 PS two load c101-c102 inside a
     c0..c117 / c0..c142 block the game sets for them (`archive/v1_native/c101_shader_scan.txt` by `tools/shader_cb_rows.py`, dxc -dumpbin);
     52 VS index cb0 register-relative (bone palettes) - whatever a shader reads there the game set before the
     draw, or the console reads the same uninitialised ring bytes; the two shaders at the affected draws
     (b341612f5de0eaef / 9bafe30bdf04f4c5) read c100 only. Not a native-path defect and not a README speck
     candidate; nothing to fix; the port keeps the XDK's 16*count bytes.
     **P2-B per-tile constants baked and closed (2026-10-14): the live bake proof scb1 passes every step.** scb1 (exe
     fc3cfb79fb13, the lane exe; rule in v1_native/scb1/NOTES.txt written before the rolls): replay 9/9 vs the
     pre-bake exe; the gate span 0 of 358, freeze 15/15 (world 15); CPPM4 15/15 (world 14) with the CP's own
     render-back counted (cpPm4 > 0); 82113010's SET_CONSTANTs 5.32 -> 0.00 and pm4 361.3 -> 330.2 per ingame frame
     (-31.1, t2nop pads 189.1 -> 174.5) against SR_NATIVE_SETCONSTMEM=0 on the same exe.
     **BAKED (2026-10-14, `SR_NATIVE_SETCONSTMEM` default ON; branch native/setconst-bake from dev 9a367dab, merged
     after Astra's two rounds and scb1):** `setConstMemOn()` returns `!e || e[0] != '0'`; =0 keeps
     the guest body for the A/B. In the release configuration the gate is kept as the earlier bakes kept theirs (rows
     3 / 5 / 3b / 9, segmentless): SR_ORACLE=0 compiles out the in-roll A/B toggle (`sr::abt` inline no-ops) and the
     dry CHECK, not the env read, so =0 still selects the guest body there. The go rests on `p2.setconst_mem`'s closed
     record (plan `setconstmem`, scm4 on exe 42f048a64456: replay 9/9; dry CHECK kind 7 0 BAD; the ring-path and
     segmentless frozen-pass A/Bs 15/15; the records-as-PM4 freeze 15/15; the flicker gate span 0 of 358; under
     segmentless SET_CONSTANT 6.9 -> 0.0 per ingame frame and pm4 372.9 -> 357.5).
     **The CPPM4 render-back (Astra round 1 on this branch):** until 466e4ae2 the CP's CPPM4 split had no
     kRkSetConstMem case - the record took the opaque fallback and ran through `nativeRecord`, so no CPPM4 roll ever
     rendered it (scm4's `cppm4` was the producer-side `SR_NATIVE_FLUSH_PM4`). Now `cpRecordPm4` (the render-back
     loop, moved out of `cpApplyRecord`, every other kind unchanged) renders it with `setConstMemRender`, the
     renderer the producer's literal span uses: the SET_CONSTANT header, the offset word and the body the caller
     filled, read at apply (no pad NOPs); a malformed payload or a body not mapped in one piece is a rejected record,
     as `setConstMemApply` rejects them. The consumer counts it: `cpPm4=` on the periodic draws line. Offline proof
     (`tools/difftest/setconst_render_run.py`, difftest `--setconst-render-test records | cppm4 | pm4`; the worker,
     `archive/scb/setconst_render`, valid): 82113010 through production dispatch with `SR_NATIVE_SETCONSTMEM` unset,
     the SDK body against the Entry on identical fixtures, four cases (pad 0 / 4 / 8 / 12 bytes, count 1 / 2 / 4, VS /
     PS, predicated or not) - the record's payload is the SDK's packet; under CPPM4 the CP's PM4 equals the SDK's
     bytes without the pads, executes to the body's constants and is counted (36 checks); in record mode cpPm4 does
     not move (28); the producer's literal split equals the SDK's bytes (20); per case the CP's PM4 equals the
     producer's; two compiled mutants of `cpRecordPm4` (no kRkSetConstMem case; the body's length taken as bytes)
     fail their named CPPM4 assertions (12 / 8) while record mode and the split pass; the source restored and rerun.
     Offline on the worker (`archive/scb` of the lane worktree, after 466e4ae2): the default build and the release
     build (`build-rel`, SR_RELEASE_COMPOSITION=ON) build; all 77 difftest suites' exit codes identical to dev's (the
     worker's dev build 328f9c1b4d64, itself identical to dev_057863dc's summary), the new suite exit 0 bare; the
     composition audit unchanged (103 failing units, 44 markers, as dev); the forcing audit exit 0. The live bake
     proof: `python tools/perf/native_proof.py --plan setconstmem_bake --base <the pre-bake exe 328f9c1b4d64>` -
     replay vs the pre-bake exe; the new-game world-rule gate with only the strict transport witness
     (`SR_NATIVE_SEG_TILE=1`: the tile and drained reconciliation in both rolls, every record executed, 82113010's
     Entries taken); the default path under the CPPM4 split (`SR_NATIVE_FLUSH_CPPM4=1`, frozen, exact, with
     `cpPm4 > 0` required - the CP rendered the setconst records itself - the first live roll of that render-back,
     which every other plan's CPPM4 roll now runs); and `SR_NATIVE_SETCONSTMEM=0` against the default on one exe: 82113010's
     SET_CONSTANTs by kind (base >= 5.0, default <= 0.1) and the total pm4 not rising. The per-present A/B of
     segmentless is not used: it is void in both forms (`p2.segmentless` abp_toggle_drops). The plans whose
     arms assumed it OFF now say so: setconstmem's meter0, and the meters of segmentless and segmentless_bake (their
     census bound was taken with it OFF; the record form exists only under segmentless, so it would ride in the ON
     arm).
  6. **Proof (P2.7 items 3-4):** `SR_P2_SYNC=1` (every close drains the consumer) -> the per-present (call site,
     answer) sequence from the P2 trace identical ring vs segmentless; the frozen-pass per-present A/B at the
     throttle point (both transports resumable because the virtual cursor equals the real one); the invariants on
     every roll; pacing vs the golden; replay 9/9; CPPM4 for every new kind; the flicker gate; `[PM4_COV]` 0 and
     the native meter's pm4 = kind-10 records only, then 0.
  Build order: 1 + 2 + 3 + the FENCE / INVALIDATE apply (the transport with every other packet bridged as kind
  10) -> proof 6 on that -> 4's remaining kinds one at a time, each with its CPPM4 form and the same proof.
- **p2.transport BUILD PLAN (native-a, 2026-10-10, written before building; builds after the segmentless plan
  reports; from IDA 820FDA80 / 820FDBA0 / 82110B30 / 82110C78 / 82111EC8 / 82111E38 / 82112050 and the P2.4-P2.7
  contracts above):** the remaining guest PM4 of P2.4's table as host items the record consumer applies, one kind
  at a time, each behind its own gate (default OFF, each requires `SR_NATIVE_SEGMENTLESS`; `SR_AB_TOGGLE` per
  kind for the per-present A/B), rule 1 per entry (the decision at the top of the override; a decline runs the
  guest body whole). Nothing default ON changes. Where each kind is applied: register-level kinds in
  `Pm4Engine`'s record switch (FENCE / INVALIDATE already are); the kinds that wait, interrupt, present or
  replay in the CP thread's item loop (`kernel/gpu_command_processor.cpp`), which owns the wait (`cpFlushTimed`),
  the ISR DPC (`onInterrupt`: KPCR cpu = n, IRQL 2) and the present (`onSwap`).
  1. **CALLBACK {cb, ctx, cpuMask, flags}** (kind 13, exists as a reserved enum only): the 23-dword block of
     GpuEmitCpuCallbackPackets_820FDA80 natively - [flags & 0xC0FFFFFF == 0: WAIT_UNTIL 3D idle = the engine's
     drain] pc+0x10 = cb, pc+0x14 = ctx, pc+0 = mask (the SCRATCH_REG4 / 5 / 0 write-backs, written by the
     consumer directly, so the block's WAIT pc+0 == mask holds by construction), INTERRUPT(mask) = `onInterrupt`
     (the guest ISR 820FDBA0 source 1 as a DPC per CPU of the mask: calls cb(ctx), clears its bit in pc+0 under
     the real spinlock), then WAIT pc+0 == 0 (true after the DPCs; a tripwire `cbStuck=` if not), pc+0x10 =
     0x0BADF00D; register 0x45E = mask through writeReg (no effect). Producers: the Z-pass kick 82110C78 (cb
     82110B00, ctx = the record-list head phys) until BRACKET retires it, the vblank gate 82111EC8 (folded into
     GATE), InsertCallback 820FDE38 (0 calls in this title: the kind keeps it honest). **Owner question (for
     the orchestrator):** kind 13 and `recOpaque`'s exception for it were reserved in cebef9ca as "owner-applied
     (the P2 research agent)"; research/p2 (`_worktrees/p2`) holds only that enum line, no producer and no
     apply. This plan has native-a build both; the research lane keeps its Z-pass / movie findings. Confirm the
     kind moves to native-a, or name the research lane its builder (then GATE and BRACKET wait on it).
  2. **GATE {arg = vblanks << 16 | pct}**: GpuPresentEmitVblankGate_82111EC8's block natively: pc+4 = 1 (the
     SCRATCH_REG1 write-back; its WAIT pc+4 == 1 holds by construction), the CALLBACK behaviour with cb =
     GpuPresentPacingUpdate_82111E38, ctx = arg, mask 4 (CPU 2), flags 0 (the 3D-idle prefix), then the consumer
     HOLDS every later item until pc+4 == 0 - cleared by the callback itself (release now) or by the vblank
     ISR's countdown (source 0, on the vblank thread) - exactly the block's last WAIT_REG_MEM; the vblank thread
     never waits on the consumer, so the hold cannot deadlock. Skipped as the asm skips it (PresentInterval
     IMMEDIATE, no previous front buffer); `dev+0x2A39 |= 0x20` as today. **(a)** the callback stays the guest
     82111E38 reading the emulated V_COUNTER / height: pacing identical by construction, `dispreg` unchanged.
     **(b) = `display.registers`:** the pacing decision as host code - the same arithmetic on today's scan-out
     answers held by host code (dev+0x3B14 / 0x3B18 / 0x3B1C / 0x3B20 stay the guest's words; "line %" from a host
     counter the vblank tick advances by today's V_COUNTER rule - the orchestrator's pacing decision of 2026-10-14;
     the vblank clock's phase first planned here is rejected, see the build note), the guest 82111E38 never called,
     `vdScanoutRegsTick` not written under the gate: the two MMIO answers per vblank stop and the column drops by 2
     per vblank. The vblank ISR's source-0 path (820FDBA0: dev+0x3B14++, the countdown, the swap callback) stays
     guest and keeps the vsync-enable bit 0x7FC86544 (counted once per run) unless **(c)** the host vblank service
     performs those three effects itself - a second question for the orchestrator under the counting rule (a kernel
     export answered by host code is native; the ISR is guest code reading an MMIO bit).
     **DISPLAY.REGISTERS BUILD NOTE (display-registers lane, 2026-10-14, written before building; branch
     native/display-registers at dev a9546edd; gate `SR_NATIVE_VBLANK`, default OFF; decided design = (b) WITH (c),
     DECISIONS (2) below. Sources: IDA default.xex sha256 c8f243acd99d..., read-only; the generated bodies of dev's
     tree, ppc_recomp.1.cpp (820FDBA0) and ppc_recomp.2.cpp (82111E38).)**
     - **820FDBA0 source 0, every instruction (IDA).** Prologue: 820FDBA0 `mflr r12`, 820FDBA4 `bl __savegprlr_29`,
       820FDBA8 `stwu r1,-0x90(r1)` (backchain store at r1-0x90), 820FDBAC `mr r31,r4` (dev), 820FDBB0 `cmplwi
       cr6,r3,1` / 820FDBB4 `bne` 820FDC30 (source 1 is the other branch). 820FDC30 `cmplwi cr6,r3,0` / 820FDC34 `bne`
       exit. 820FDC38 `lis r11,0x7FC8` / 820FDC3C `lwz r11,0x6544(r11)` (the vsync-enable MMIO word) / 820FDC40
       `clrlwi. r11,r11,31` / 820FDC44 `beq` exit. 820FDC48 `lwz r10,0x3B14(r31)` (vblank counter) / 820FDC4C `lwz
       r11,0x3B1C(r31)` (countdown) / 820FDC50 `addi r10,r10,1` / 820FDC54 `cmpwi r11,0` / 820FDC58 `stw
       r10,0x3B14(r31)` / 820FDC5C `li r10,0` / 820FDC60 `ble` 820FDC80. 820FDC64 `addic. r11,r11,-1` / 820FDC68 `stw
       r11,0x3B1C(r31)` / 820FDC6C `bne` 820FDC80. 820FDC70 `lwz r11,0x2A14(r31)` (pc) / 820FDC74 `stw r10,4(r11)`
       (pc+4 = 0: the gate opens) / 820FDC78 `lwz r11,0x3B14(r31)` (reloaded after that store) / 820FDC7C `stw
       r11,0x3B18(r31)` (last release). 820FDC80 `lwz r11,0x3B10(r31)` / 820FDC84 `cmplwi r11,0` / 820FDC88 `beq` exit.
       820FDC8C `lwz r9,0x3B20(r31)` / 820FDC90 `addi r3,r1,0x60` / 820FDC94 `stw r10,0x68(r1)` (0) / 820FDC98 `stw
       r9,0x64(r1)` / 820FDC9C `lwz r9,0x3B14(r31)` / 820FDCA0 `stw r9,0x60(r1)` / 820FDCA4 `mtctr r11` / 820FDCA8
       `bctrl` (the swap callback, r3 = {counter, +0x3B20, 0}). Exit 820FDCAC `addi r1,r1,0x90` / 820FDCB0 `b
       __restgprlr_29`. **Exactly the three effects plus the one MMIO read: no lock, no dev+0x2A74, no Kf spinlock
       dev+0x2A78, no other call** (the KeAcquire/ReleaseSpinLockAtRaisedIrql on dev+0x2A18 are source 1's). The STOP
       condition is not met. Generated vs IDA: identical except `__savegprlr_29` / `__restgprlr_29` (elided by the
       localized codegen: no stack save slots written; the host matches the generated form, as native_display.cpp's
       pin records). dev+0x3B10 is null in this title (Appendix C.2: golden 0, no writer), so the callback never
       fires here; it is ported and tested anyway.
     - **82111E38, every instruction (IDA; a leaf).** 82111E38 `lis r11,VdGlobalDevice@ha` / E3C `srwi r10,r3,16`
       (vblanks) / E40 `clrlwi r7,r3,16` (pct) / E44 `lwz r11,0x948(r11)` (the cell 0x82000948) / E48 `lwz r11,0(r11)`
       (dev) / E4C `lwz r9,0x3B14` / E50 `lwz r8,0x3B18` / E54 `subf r9,r9,r10` / E58 `lwz r10,0x3B20` / E5C `addi
       r10,r10,1` / E60 `add. r8,r9,r8` (remaining = last + vblanks - count, signed 32-bit compare) / E64 `stw
       r10,0x3B20` / E68 `bgt` EC0. E6C `lis r10,0x7FC8` / E70 `mr r9,r10` / E74 `lwz r10,0x6530(r10)` (D1MODE_V_COUNTER)
       / E78 `lwz r9,0x6584(r9)` (D1MODE_VIEWPORT_SIZE) / E7C `mulli r6,r10,100` / E80 `clrlwi r10,r9,20` (H) / E84
       `twllei r10,0` / E88 `divwu r10,r6,r10` (the low 32 bits of V*100) / E8C `addi r10,r10,1` / E90 `cmplwi r10,100` /
       E94 `blt` E9C / E98 `li r10,100`. E9C `cmplw r10,r7` / EA0 `bgt` EBC. EA4 `lwz r10,0x2A14` / EA8 `li r9,0` / EAC
       `stw r9,4(r10)` (pc+4 = 0) / EB0 `lwz r10,0x3B14` / EB4 `stw r10,0x3B18` / EB8 `b` EC0. EBC `li r8,1`. EC0 `stw
       r8,0x3B1C` / EC4 `blr`. **IDA and the generated code disagree once:** the trap at E84 (a zero viewport height
       traps on the console; the generated body drops it and guards the divide to 0, line % 1). The host form never
       reaches it (H is a constant 720). Writers of +0x3B14..+0x3B20 by displacement in the binary: these two
       functions only (the 824DAC00 / 824E0338 / 824DDD70 / 824DE7F8 / 82507380 stores at the same offsets are the
       WMV decoder context, C.2).
     - **The host version (kernel/native_vblank.{hpp,cpp}), line by line.** (b): a strong `sub_82111E38` - every route
       to the callback enters it: the GATE record's CALLBACK apply -> applyInterrupt -> onInterrupt -> the guest ISR
       source 1 -> its `bctrl` at 820FDBF4, and the PM4 form of the same block (CPPM4, a declined GATE leaf, GATE's own
       A/B OFF window) through the CP's INTERRUPT. Gate OFF: `__imp__sub_82111E38`. ON, under the CPU-2 lock: dev =
       load(load(0x82000948)) [E44, E48]; vblanks, pct [E3C, E40]; count, last [E4C, E50]; gates = load(+0x3B20) + 1
       [E58, E5C]; remaining = vblanks - count + last mod 2^32 [E54, E60]; store +0x3B20 [E64]; remaining > 0 (signed)
       -> store +0x3B1C = remaining [EC0], outcome WAIT. Else the two MMIO loads [E6C-E80] are replaced by TODAY's
       answers held by host code (the orchestrator's pacing decision, 2026-10-14): H = 720 (`kViewportAnswer` & 0xFFF:
       the 0x050002D0 vdScanoutRegsTick writes, Xenia's ReadRegister 0x1961 constant), V = the host counter. One
       function, `nextVCounter`, computes each tick's V_COUNTER answer - V + 720 on the console path, wrapping at 2^32;
       0 under HIGHFPS - and vdScanoutRegsTick's register write uses it today; under the gate the vblank tick advances
       the host counter with it instead (0 before the first tick, as the zero-backed page reads). So the host V is, on
       every tick, the value today's path writes to the register, and the host line % equals today's by construction,
       wrap points included. line % = min(100, uint32(V * 100) / H + 1) [E7C-E98, the generated arithmetic including
       the 32-bit product]; line % > pct -> store +0x3B1C = 1 [EBC, EC0], outcome NEXT; else pc = load(+0x2A14), store
       pc+4 = 0 [EA4-EAC], store +0x3B18 = load(+0x3B14) [EB0, EB4], store +0x3B1C = remaining [EC0], outcome NOW. The
       volatile registers r6-r11 are not reproduced: dead at 820FDBF8, which reloads r10 / r11 and reads only r13 / r31.
       (c): the VD_ISR thread's tick under the gate takes the CPU-2 lock, advances the host counter, writes no scan-out
       answer (vdScanoutRegsTick skipped), records the tick time and runs the existing `sr::native::display::trySource0`
       (native_display.cpp, M2 display.source0; `--native-display-test` holds it against `__imp__sub_820FDBA0`:
       countdown 0 / 1 / 2 / 0x7FFFFFFF / 0x80000000 / ~0, scratch-device aliasing, the callback block and frame,
       reentry, fault) in place of the guest ISR: [820FDC48-820FDC7C] the stores in the SDK's order, [820FDC80-820FDCA8] the callback captured
       after them with the block {+0x3B14, +0x3B20, 0} at r1+0x60 of its 0x90 frame. The registration is native
       (registrationService's nativeSource0): VdSetGraphicsInterruptCallback leaves 0x7FC86544 alone.
     - **Threads, order, locks.** The pacing runs on the CP thread at the point (a) runs it: the GATE apply writes
       pc+4 = 1 (0x579), waits pc+4 == 1, drains (WAIT_UNTIL), writes scratch 4 / 5 / 0 = cb / arg / 4, waits pc+0 == 4,
       INTERRUPT(4) -> onInterrupt (registration lease, CPU 2 DPC) -> ISR source 1 -> sub_82111E38 -> back in the ISR:
       spinlock dev+0x2A18, pc+0 &= ~4 -> 0x45E, wait pc+0 == 0, scratch 4 = 0x0BADF00D -> the hold (pc+4 == 0) -> the
       SWAP item -> the present. The service runs on VD_ISR every 16.667 ms (HIGHFPS: its VblankClock): lease -> [lock
       -> the host counter's advance and the tick time (a toggled roll: the scan-out write, inside the lock, at the
       point it is written today) -> CPU 2 DPC -> source-0 effects -> the per-vblank check -> unlock] -> gpuCpVblank.
       The host counter advances on exactly the ticks on which today's path writes the register (after the lease and
       the callback lookup, as before). **The CPU-2 lock** (new, a host mutex): on the console both are DPCs on CPU 2 and never interleave; our two host threads do today
       (the guest 82111E38 on GPU_CP against the guest ISR on VD_ISR race on +0x3B14 / +0x3B1C under (a)). Under the
       gate both forms run under one leaf lock (the OFF arm of a toggled roll too). Nothing is taken inside it but the
       swap callback (null here); it is taken after the lease and released before the ISR's spinlock dev+0x2A18 on the
       CP thread: no lock-order cycle. pc+4's other writer, the GATE's SCRATCH_REG1 write-back, precedes its callback
       and never meets a release (the CP holds on pc+4 == 0 whenever the countdown is positive) - but the NEXT gate's
       write-back can land, outside the lock, between a source-0 release and that tick's check (Astra round 1, below:
       the check counts the CP's write-backs instead of assuming pc+4 still reads 0). The producer (render
       thread: 82111EC8 -> presentGateLeaf, dev+0x2A39 |= 0x20) touches none of these words. **BlockUntilIdle drain
       rule:** neither path reads or writes dev+0x2A74 or the Kf spinlock dev+0x2A78; the drain is untouched (the
       probe's P2-trace idle checks stay as the regression check).
     - **Gate OFF (default), byte-identical to today:** SR_NATIVE_VBLANK unset, the override's first test is a latched
       false and it calls `__imp__sub_82111E38` with the same context; the registration, vdScanoutRegsTick and the
       vblank tick take today's path (no lock, no host counter; vdScanoutRegsTick's V + 720 / HIGHFPS 0 is the same
       expression, now `nextVCounter`, and its viewport the same constant). Added: host-only counters for a dispreg
       breakdown (no guest write, no log line). Proof: replay 9/9 vs the base; every difftest suite's exit code as dev's.
     - **Requires** SR_NATIVE_GATE=1, segmentless, SR_KPCR (the host source 0 needs the CPU-2 DPC identity) and today's
       scan-out answers: SR_VD_SCANOUT_REGS on and not A/B-toggled (its OFF windows answer zeros, which the host
       counter would not reproduce). Otherwise REFUSED: one Error line, `state=refused` in every [NATIVE_VBLANK] line,
       the emulated path runs (no exit).
     - **A/B (rule R):** `SR_AB_TOGGLE=SR_NATIVE_VBLANK` (`SR_AB_PRESENT=1` per present): ON = (b) + (c), OFF = the guest
       82111E38 and the guest ISR source 0. A toggled roll keeps the OFF arm's inputs live in BOTH arms (the vsync bit
       armed at registration, the scan-out answers every tick), so OFF is (a) at every moment; its dispreg is (a)'s.
       The zero is measured untoggled. **The shadow check, every pacing call in both arms** (under the lock, on the
       call's pre-state): ON arm - the host form's outcome and effects against `decide()` on today's answers (the live
       registers in a toggled roll; untoggled, the host counter itself); OFF arm - the guest body's effects against
       `decide()` on the host counter. A difference counts `mismatch=` (must be 0; the first 8 log `[NATIVE_VBLANK]
       pacing MISMATCH`); a host release-now where today's answer says NEXT also counts `early=` (must be 0). Both
       arms agree by construction, so either counter is a defect. The per-vblank check also compares the live
       registers with the host counter and the viewport answer in a toggled roll (traceBad).
     - **Instruments** (milestone tag, with the CP census and at exit): `[NATIVE_VBLANK] <why> state= toggled= ticks=
       native= guest= released= callbacks= traceBad= late= maxGapUs= vcount= | pacing calls= host= guest= wait= now=
       next= early= mismatch= | dispreg scanout= sysbuf= vsyncBit=`. The per-vblank oracle check (traceBad), pre / post
       under the lock for either form: counter + 1; countdown - 1 iff positive; at 1 -> 0 pc+4 == 0 and +0x3B18 ==
       +0x3B14, else +0x3B18 unchanged; +0x3B20 unchanged; a callback counted iff +0x3B10 != 0; live registers (a
       toggled roll) equal to the host counter / 0x050002D0. The OFF arm runs the guest ISR under the same check: the
       oracle trace. vcount = the host counter (information).
     - **Xenia's V_COUNTER (reference/xenia-canary @0b4e6c44, src/xenia/gpu/; archive/dreg/xenia_vcounter_check.txt).**
       graphics_system.cc:101-105 maps 0x7FC80000-0x7FC8FFFF to ReadRegister. ReadRegister (:273-295) answers 0x1961
       D1MODE_VIEWPORT_SIZE with the constant 0x050002D0 (:283-286) and every other register, 0x194C D1MODE_V_COUNTER
       included (register_table.inc:1121; 0x1961 at :1134), from the register file (:293-294). The frame limiter thread
       adds GetResolution().second (:425-442; 720 at the default 1280 x 720) to V_COUNTER at the top of EVERY loop pass
       (:146-147). On Windows with vsync (default true, gpu_flags.cc:21) a pass marks a vblank and sleeps 90 % of the
       period only once the period has elapsed (:149-167), and otherwise loops straight back: V climbs once per
       frame-limiter SPIN pass, a variable number of times per vblank. On Linux every pass marks one vblank and sleeps
       a full period (:184-198): V climbs once per vblank, today's cadence. The viewport answer is today's exactly.
     - **Where Xenia, today's emulation and the host form agree on the gate outcome (quantified).** The outcome reads
       V only through line % = min(100, (V * 100 mod 2^32) / 720 + 1): 100 - NEXT for every pct < 100 - except at the
       wrap points of that 32-bit product, in all three. Today (and the host counter, the same sequence) V = 720 n at
       tick n, so V * 100 mod 2^32 = 64 * (1125 n mod 2^26) and line % <= L exactly when 1125 n mod 2^26 < 11.25 L.
       line % < 100: 1,114 ticks in every 2^26 (one per ~16.7 min at 60 Hz), first at tick 59,653 (V 42,950,160,
       line % 68; ~16.6 min into a run), then 119,305 (line % 36). line % <= 8 (a release at once at gameplay's pct
       8): 90 ticks in every 2^26 (one per ~3.45 h), first at tick 178,957 (V 128,849,040, line % 3; ~49.7 min), then
       357,914 (line % 6) and 2,207,136 (line % 3). V's own 2^32 wrap: tick 5,965,233 (V 464, line % 65; ~27.6 h).
       Xenia on Linux runs today's sequence from its own first pass; on Windows the spin passes leave V at effectively
       arbitrary residues, so a decision reads line % <= 8 with probability 5,760 / 2^32 = 1.34e-6 and line % < 100
       with 71,280 / 2^32 = 1.66e-5. So Xenia and today's emulation agree on the gate outcome everywhere except at
       those wrap points; the host form agrees with today's emulation everywhere, wrap points included, by
       construction - `--native-vblank-test` W walks the counter across each of these points and checks the host
       decision against the guest arithmetic on every tick.
     - **Rejected alternative: the phase model.** This note first built line % from the host clock's phase since the
       last tick (V = elapsed * H / period, the hardware's beam position), which releases at once whenever the
       callback lands in the first pct % of a period - one vblank earlier than today on a share of console-path
       presents (the first build's `early=`). Rejected by the orchestrator (2026-10-14): the pacing oracle is the
       all-OFF control (P2.7 item 5), not the console's beam; today's answers are what every flicker gate and pacing
       golden was measured on; and a native port answers the registers with host code without changing what the
       guest sees. The host form reproduces today's answers exactly, wrap points included, and `early=` stays as a
       must-be-0 counter.
     - **Expected pacing.** Identical to (a) per present, by construction: the same V and H at every decision, so the
       same outcome and countdown. HIGHFPS: V = 0 in both (release at once). Console path: line % 100 (NEXT at
       gameplay's pct 8) except at the wrap points above. PRE-REGISTERED: `early=0` and `mismatch=0` in every roll,
       the pacing step (pacing_cmp.py against the all-OFF control) within its margins; anything else is a defect.
     - **dispreg arithmetic** (per ingame frame, F fps, R vblank Hz; the vsync bit is armed at boot, outside the
       window): default 2R/F + 1 (the syscmdbuf fill per swap) = 28.6 at scb1 (R 1000, F 72.5: 27.6 + 1.0; measured
       28.6), 5.0 on the console path (R 60, F 30); composed (every transport kind + SR_NATIVE_DEVCREATE=1), gate OFF:
       2R/F + d (d = swaps whose SDK body ran, i.e. SWAP's declines) = 27.6 / 4.0 at d = 0; **composed + SR_NATIVE_VBLANK=1:
       d, 0 at SWAP's pre-registered decl=0**, whole run 0 + boot-time d; composed + gate, toggled: 2R/F + d (+1 bit);
       GATE + VBLANK without SWAP: 1 a frame, the syscmdbuf answer that SWAP retires. SWAP already carries
       0xBEEF0000 / 0xBEEF0001 (swapLeaf writes the descriptor at frame+0xC0 / +0x74 and never calls
       VdGetSystemCommandBuffer, whose only caller is 82112050's `bl` at 8211228C), so that answer stops exactly
       where SWAP is native.
     - **Proof.** Offline (worker): `--native-vblank-test` - A the pacing differential (the generated 82111E38 with
       the MMIO seeded against the host core on the same inputs: vblank counts, remaining > 0 / 0 / < 0 / wrap, line %
       0 / mid / today's wrap-point answers / the product's wrap, H 720 / 1 / 0xFFF / 0, pct 0 / 8 / 100 / 0xFFFF,
       aliasing of pc+4; `decide()` predicting each row's effects), B / C linePercent's and nextVCounter's tables
       (today's rule written out independently in the test, the 2^32 edge), D the event-script differential (ticks
       and gates interleaved: the guest ISR + guest 82111E38 reading registers a replica of vdScanoutRegsTick writes,
       against the production host path with the registers poisoned the other way: counter, countdown, releases, V,
       callback blocks in order), W the wrap walks (ticks n-4 .. n+4 with a line-% gate after each: 178,957 /
       357,914 / 2,207,136 at pct 8 - release-now exactly at the point; 59,653 / 119,305 at pct 8 - NEXT throughout -
       and at pct = their line % - release-now exactly at the point; 5,965,233, V's own wrap; the points computed
       by the modular inverse and by a brute-force walk), S the shadow check in both arms (clean when the forms
       agree, mismatch / early counted when the registers are made to differ), E the wiring (the guest ISR's source 1
       reaching the host form) and the refusal table, F the per-vblank check (six mutant source 0s, live registers
       that differ), G test-local mutants each failing named rows; compiled mutants of the production source
       (`native_vblank_mutation_run.py`, two of them breaking the wrap); every suite's exit code vs dev with the gate
       OFF; build-rel. Live: `native_proof.py --plan display_registers`.
     - **AS BUILT, first build (2026-10-14; c4e3f3ce, edb355e5, e6a86e41, 4165831b; the phase model, superseded by
       the revision below).** kernel/native_vblank.{hpp,cpp} as above;
       (c) reuses native_display.cpp's trySource0 unchanged (stubs.cpp: the registration native under the gate,
       `tickNative` per tick, `TickScope` around the delivery). The strong sub_82111E38 means a declined GATE leaf, the
       CPPM4 form and GATE's own A/B OFF windows also get the host pacing, so no guest body ever reads the unwritten
       MMIO under the gate. Offline on the worker: `--native-vblank-test` 41 passed / 0 failed (A: 33,656 rows equal,
       device / pc block / MMIO / cell bytes; D: seven event scripts equal after every event, callback blocks in order;
       E: the dispatch slot and the ISR's source 1 reach the host form; F: six mutant source 0s flagged; G: 13
       test-local mutants caught); `tools/difftest/native_vblank_mutation_run.py`: six edits of the production source
       each fail the suite (10 / 1 / 10 / 5 / 2 / 1 FAIL lines), restored and rebuilt 41 / 0; every difftest suite's
       exit code identical to the base a9546edd built in the same worker tree (80 suites, 62 exit 0 each, the 18 bare-run
       failures the same codes; archive/dreg/suites_compare.json); build-rel (SR_RELEASE_COMPOSITION=ON) builds; the C1
       audit classifies kernel/native_vblank.cpp native (it appears only as one more reader in the existing failing
       native_flush.hpp content item); scripts/audit_forcing.py exit 0. Live, after p2.transport closes: `python
       tools/perf/native_proof.py --plan display_registers --exe <snapshot of this exe> --base <dev exe> --stage
       J:\sr_stage --apply-ledger` (dry run clean). Its pacing step is pacing_cmp.py's once dev's native/transport-composed
       (merged 25d74b5b) is merged into this branch - checked by splicing the plan into dev f839fec1's runner; before
       that, one owed step that fails a live run. OPEN: the ledger's timing guard (>= xenia) - no roll produces a Xenia
       pacing reference.
     - **AS BUILT, REVISED (2026-10-14, the orchestrator's pacing decision: today's answers reproduced; on the merge
       with dev d71946e2).** kernel/native_vblank.{hpp,cpp} as above: `nextVCounter` / `kViewportAnswer` shared with
       stubs.cpp's vdScanoutRegsTick (gate OFF: the same expression and constant); the host counter advanced in
       TickScope, which now opens before the scan-out write (after the lease, as before); `decide()` and the shadow
       check in both arms; REFUSED without today's scan-out answers; the [NATIVE_VBLANK] line's `vcount=` and
       `mismatch=` (the first build's `noTick=` gone). Offline on the worker (this head's exe): `--native-vblank-test`
       55 passed / 0 failed (A: 39,256 rows equal, decide() exact on every unaliased row; C: 200,000 counter steps and
       the 2^32 edge; D: seven event scripts equal after every event, mismatch / early 0; W: the wrap points
       recomputed by the modular inverse and by brute force, eight walks host == guest on every tick, release-now
       exactly at the line % <= 8 points; S: five shadow cases; F: six mutant source 0s and differing live registers
       flagged; G: nine pacing-core and four counter-rule mutants caught). `native_vblank_mutation_run.py`: ten edits
       of the production source each fail the suite (20 / 1 / 21 / 16 / 2 / 9 / 2 / 1 / 1 / 1 FAIL lines;
       counter_wrap_saturates and product_wrap_lost break the wrap), restored and rebuilt 55 / 0
       (archive/dreg/mutants_counter). Every difftest suite's exit code identical to dev d71946e2 built in the same
       worker tree (82 suites, 63 exit 0; archive/dreg/suites_compare_counter.json). build-rel
       (SR_RELEASE_COMPOSITION=ON) builds; the C1 audit's counts as before (kernel/native_vblank.cpp native, only a
       reader in the existing native_flush.hpp content item; 103 failing items, the one fewer than at a9546edd from
       dev). Python on the worker (archive/dreg/py_counter): test_native_proof.py passes every display_registers check
       (its one failure, 'ledger paths start at _worktrees/', is the worker tree's location C:\sr\wt\dreg, whose
       umbrella-relative paths start wt/dreg/); the plan's --dry-run exit 0 with the pacing step pacing_cmp.py's;
       scripts/audit_forcing.py exit 0; gen_override_manifest.py leaves docs/OVERRIDES.json unchanged;
       test_flicker_gate.py / test_frame_measurements.py need PIL, absent on the worker. Live, unchanged command,
       after p2.transport closes; its vblank items now also require `early=0 mismatch=0` and forbid `pacing
       MISMATCH` lines. OPEN: the timing guard (>= xenia), as above.
     - **ASTRA ROUND 1 (2026-10-14, MERGE AFTER FIXES; archive/dreg/astra/round1.txt) - three findings, fixed.** None
       changes what the guest sees or the pacing decision in this title (its one registration is the D3D ISR's; the
       other two fixes are checks): no further review round by the orchestrator's rule.
       (P1) The tick picked the host source 0 for any leased registration whenever the gate's window was active, and
       the thunk handed trySource0 a hard-coded kD3DIsr, bypassing its identity check. Now `planTick(nativeSource0,
       callback, context)` decides from the leased registration: the host source 0 only for 820FDBA0 with a device
       context (a native registration, or that registration in a toggled roll's ON windows); any other registration
       runs its own callback and the per-vblank check never reads its context (the tick stays scoped, so the host
       counter still advances on every tick today's path writes the register); the thunk passes the leased callback,
       so trySource0's own identity check applies. The tick's body is `sr::kernel::vdVblankDeliver` (stubs.cpp), which
       the vblank thread calls and the difftest drives on a GuestCaller thread of its own: a non-D3D registration
       (gate on, toggled ON window, gate off) runs its callback with its context untouched; the D3D ISR's runs the host
       source 0, checked clean.
       (P2, the release check's race) After a release the CP can pass its hold and rearm the next gate (SCRATCH_REG1
       write-back, pc+4 = 1) outside the CPU-2 lock before the tick's check reads pc+4: a false traceBad. The CP's
       SCRATCH_REG1 write-backs are now counted (gfx/pm4_engine.cpp writeReg, `noteScratch1Write`, a generation bumped
       BEFORE the store is performed or queued); the check reads pc+4, then the generation, and a release passes when
       pc+4 reads 0 or the CP wrote it since the tick began (it passes the hold only after the clear). The lock is
       never held across the CP's wait. Tested deterministically with a real Pm4Engine in CP mode rearming between the
       release and the check (not flagged), and the negative controls: pc+4 set back by anything but the write-back,
       and a release without the clear (both flagged).
       (P2, premature clears) Neither check looked at pc+4 outside a release. Now the tick check requires pc+4
       unchanged on every tick that releases nothing (unless the CP wrote it meanwhile), and the pacing shadow
       requires pc+4 unchanged on WAIT / NEXT: a source 0 that counts 2 -> 1 right and clears pc+4, and a host core
       that clears it on WAIT / NEXT (`test::setCore`), are both caught (traceBad / mismatch), the production forms
       clean. Every fix has a compiled mutant that reverts it (tick_any_registration, release_race_blind,
       rearm_not_counted - in gfx/pm4_engine.cpp - early_clear_blind, pacing_clear_blind) and fails the suite.
       On the worker (before the dev merge): `--native-vblank-test` 71 passed / 0 failed (16 new checks: R four, F
       seven, S three, E two); `native_vblank_mutation_run.py` fifteen mutants each failing the suite (the five fixes'
       reverts 3 / 2 / 3 / 3 / 2 FAIL lines), restored 71 / 0 (archive/dreg/mutants_astra1); every difftest suite's
       exit code identical to dev d71946e2 (82 suites, 63 exit 0; archive/dreg/suites_compare_astra1.json);
       scripts/audit_forcing.py exit 0 once the delivery takes its GuestCaller by pointer (the audit reads a
       `GuestCaller&` parameter as a guest-call site; the construction in vdVblankThread stays the allowlisted one).
       Merged with dev 4aa4f963 (d8bd4f76; dev's side changes no C++ or build file since d71946e2): on the worker
       `--native-vblank-test` 71 / 0; every difftest suite's exit code identical to the d71946e2 build of this tree
       (82 suites, 63 exit 0; archive/dreg/suites_compare_merge2.json); build-rel builds, the C1 audit's counts and
       failing items exactly as before (the new include of native_vblank.hpp in gfx/pm4_engine.cpp adds none);
       Python on the worker (archive/dreg/py_merge2): every tools/perf test as before (test_native_proof.py's one
       failure the worker tree's location; PIL absent for two), the plan's --dry-run exit 0, audit_forcing exit 0,
       the override manifest unchanged.
     - **dreg2's GPU HANG, ROOT CAUSE AND FIX (native/dreg-hang, 2026-10-15).** dreg2 (exe 2bfd499ebe58, the composed
       transport + SR_NATIVE_VBLANK=1 + SR_NATIVE_SEG_TILE=1, HIGHFPS: the vblank clock 1000 Hz) aborted at its probe on
       H1 at 17.3 s (BlockOnFence 0x27 vs 0x1F, the CP in phase ring-packet 3.2 s, rptr 114 / wptr 144, packets 0), then
       hung again and again (phase isr, 3-33 s; presents 0.0-9.5 a second; the drive never reached the main menu).
       - **The tick cost.** Its [NATIVE_VBLANK] census counted ~46 ticks a second against the 1000 Hz clock (292 ticks by
         56.697, 812 by 07.634, 8,728 at exit): ~21 ms a tick. Under the gate every tick runs trySource0 inside TickScope
         (the CPU-2 lock), and trySource0's admission made five VirtualQuery walks of the guest arena a tick (readable +
         writableVirtualRange on the device words and on the ISR frame, readable on the KPCR) plus one of the host
         exception word: VirtualQuery walks the resident pages of the whole same-protection region (5-10 ms a call on
         guest VAs, astra/035; 'TRANSPORT PAIR ADMISSION A' above). Every tick being late, VblankClock re-bases and
         returns at once, so the vblank thread re-took the std::mutex microseconds after releasing it, back to back.
       - **The starved CP.** The CP takes the same lock in the GATE's pacing call (ISR source 1 -> the strong
         sub_82111E38 -> pacingEntry: ISR #2 took 900 ms, [GPU_CP_TIME] isr=900) and took it in its census
         (vblank::finalLine locked it to read the host counter): at 57.956 the census printed [GPU_CP] and
         [NATIVE_TRANSPORT], and the [NATIVE_VBLANK] line of the same census call only at 07.634, 9.7 s later
         ([GPU_CP_TIME] other=9678 ms, idle=0; the hang's phase 1 is the CP after its item loop). The P2 trace agrees:
         the render thread pushed its items at ring indices 114 / 117 at trace 15.4630, the CP consumed them at 25.1197,
         fence 0x1F flat in between. The later phase-isr hangs are the pacing call's wait. Without the gate (tcmp2 / tcmp3) neither the
         lock nor trySource0 runs (tcmp3's ISR #1-#8 within 60 ms of the first). Nothing in the ring, the doorbell,
         DEVCREATE's ring init or the ISR source order was involved: the CP was blocked on a host mutex.
       - **Fix (7b170dbc; gate-only, the default path unchanged).** trySource0 admits its guest ranges by arithmetic (the
         transport pair's ADMISSION A, kernel/guest_protect.hpp): mapped contiguously into the registered arena, no
         declared foreign range; it declines under a page-protecting diagnostic or an all-excluding declaration and
         holds an ArenaLease across every guest access (retired arena: decline). The host exception word is
         overlap-checked, not queried; only a non-null swap callback's dispatch / host-code check still queries (null in
         this title; native_display::test::osQueries counts). finalLine never takes the CPU-2 lock (the host counter is
         atomic, written under it) and its line ends `| cpu2 tickMaxUs= tickSlow= waitMaxUs= waitSlow=` (a tick's
         longest hold, a pacing call's longest wait, the over-1 ms counts). decideState refuses under a page-protecting
         diagnostic (every host source 0 would decline: fatal for a native registration); SR_NATIVE_DISPLAY_SOURCE0
         keeps the guest registration then.
       - **Offline (worker).** `--native-vblank-test` 77 / 0 with the new section T (dreg2's stall class): T1 the census
         line returns while a tick holds the lock; T2 production ticks at countdown 0 / 1 / 2 make 0 OS queries; T3 233 MB
         of the arena resident, ticks back to back against 150 CP pacing calls: mean 0.2-0.3 us a tick, the CP's longest
         wait 0.1-1.0 ms. `native_vblank_mutation_run.py` 18 mutants each fail the suite, restored 77 / 0
         (archive/dreg/mutants_hang); source0_os_query (the old admission back) reproduces dreg2 offline - mean 3,961 us a
         tick, the CP's pacing call starved 5.0 s, the whole window; census_takes_lock fails T1; protect_diag_ignored
         fails E. `--native-display-test` native 59 / 0 (its protected-page rejections are now declared ranges, as every
         production protector declares; arena-bound rows; 0 OS queries on null-callback completions), retired 2 / 0,
         excluded 2 / 0. At 7b170dbc and again at b7a90ca7: every difftest suite's exit code identical to dev 18a52975's
         build (no C++ change to 1928116d; 85 suites, 67 exit 0; archive/dreg/suites_compare.json, suites_compare2.json);
         composed_suites_run valid (the census suite's composed
         81 / 12 is its named BRACKET exception, census_no_bracket 93 / 0); build-rel builds; the C1 audit's only failing
         item touching these files is the existing native_flush.hpp content item (guest_protect.hpp classified native);
         audit_forcing exit 0; the override manifest unchanged.
       - **Live (information-only rolls, game-slot owner claude-dregdiag, archive/dreg/rolls of the native/dreg-hang
         worktree; each the plan's probe: --only probe,probe_trace).** fix1 / fix2 (exe 0ec7a4aba0d9 = 7b170dbc): both
         SUCCESS through the probe, 0 hang triggers, probe_trace PASS; ~1,000 ticks a second (246,632 / 234,196 in ~4 min);
         `| cpu2 tickMaxUs=325 / 43 tickSlow=0 waitMaxUs=10 / 14 waitSlow=0`; mismatch / early 0. diag1 (exe 7a86d537df77:
         7b170dbc with exactly the two old behaviours restored - the OS-query admission and the census lock - the witness
         kept): dreg2 again - 7 hang triggers in ~3 min (one 33 s H1 H2 H3), ~55 ticks a second, `tickMaxUs=49890
         tickSlow=8289` of 8,290 ticks, `waitMaxUs=32488017 waitSlow=239` of 239 pacing calls, [GPU_CP_TIME] isr=87,796
         ms of a 92,616 ms window (stopped by hand after the attribution; the owner saw the stall on screen).
       - **The check's own race (b7a90ca7).** fix1 / fix2 showed traceBad=3 / 4: every one a non-release tick with pc+4
         0 -> 1 and no "(CP rearm)" excuse. Astra round 1's protocol counted the CP's SCRATCH_REG1 write-back once, before
         its store, so a tick that began between the count and the store read the new generation with the old pc+4 -
         invisible at 46 ticks a second, a few a roll at 1,000. Now the write-back is bracketed (writeReg, and the
         executor's deferred perform): Begin = in-flight + 1 then generation + 1, End = generation + 1 then in-flight - 1;
         a check excuses a pc+4 change when the generation moved or a write-back was in flight at either end
         (scratch1Excuses; TickScope and the pacing shadow read generation, in-flight, then pc+4). Offline: three new F
         rows drive the production writeReg through the engine's own physToHost (after Begin) and P2-trace (after the
         store) hooks - a tick opened inside the bracket, one closed inside it, one wholly inside it - none flagged (the
         old single count flags two); `--native-vblank-test` 81 / 0; 20 mutants each fail the suite, restored 81 / 0
         (archive/dreg/mutants_bracket).
       - **Live, the final exe (aabc298d25c0 = b7a90ca7; information only, claude-dregdiag, 2026-10-15 13:58-14:11).**
         final1 / final2 / final3: each SUCCESS through the probe, 0 hang triggers, probe_trace PASS, no trace BAD /
         pacing MISMATCH / source0 failure line; ticks 237,199 / 233,572 / 235,480 in ~4 min each; `traceBad=0 early=0
         mismatch=0`; `| cpu2 tickMaxUs=283 / 92 / 937 tickSlow=0 waitMaxUs=3 / 3 / 102 waitSlow=0`. The plan's five
         pre-registered probe text items graded read-only from its own definitions (archive/dreg/grade_probe_items.py):
         dreg_probe_vblank / census / seg / tile / gate PASS on all three (fix1 / fix2 fail only dreg_probe_vblank, on the
         race lines above). The re-run is the full plan (dreg3, pre-registered by the orchestrator), from the dev checkout:
         `python tools/perf/native_proof.py --plan display_registers --exe <aabc298d25c0's exe or a snapshot of it>
         --base <native>/archive/exe_snapshots/fbefd44de8ae-devc2c5/SupermanReturns.exe --out
         <native>/archive/v1_native/dreg3 --owner claude-dreg3 --stage J:\sr_stage --apply-ledger` (dreg2's form). The
         ledger's timing guard (>= xenia) stays open as before.
       - **Astra round 2 (d2abe387; MERGE AFTER FIXES, two P2s).** (1) The bracket could hide a real early clear: a
         foreign write clearing pc+4 while the CP's write-back of 1 was in flight was excused. Now Begin(addr, value)
         records the write-back's own target and a change is excused only when a write-back moved or was in flight AND
         its target is pc+4 with exactly the value read (scratch1Explains). (2) The executor's deferred perform
         (Pm4Engine::execPerformStores) had no test: four F rows drive a real store item through it with hooks at the
         bracket's own points. Offline: `--native-vblank-test` 88 / 0; 24 mutants each fail (explain_value_blind fails the
         four negative controls, exec_begin_dropped / exec_end_dropped 5 / 6 rows); every suite identical to the base
         (85, 67 exit 0); staged exe cd706091beaf. Since release/emu-removal stage 2a the writeReg half lives in
         gfx/record_engine.cpp (RecordEngine::writeReg; native/dreg-bake's merge).
       - **DISPLAY-REGISTERS BAKED (2026-10-15, dbk1).** SR_NATIVE_VBLANK default ON (=0 keeps the guest 82111E38, the
         guest ISR's source 0, the scan-out answers and the vsync-enable bit; an explicit =1 without its requirements
         refused, the default standing down at WARN). dbk1 (exe 0360f8ed9ed7, native/dreg-bake 23adfe53's code): every
         step and all 46 items PASS - replay 9/9, the gate, the probe + P2 trace, the per-present A/B, CPPM4 exact, the
         meter dispreg per ingame frame 25.64 -> 0.00, the run-cumulative dispreg 1 <= SWAP's 1 boot decline, the CPU-2
         lock under bound in every default roll, pacing PASS (vblanks delta 0.0). default_bake and default_pacing
         resolved by the apply; dreg3's dreg_meter1_run kept as the witness of contradiction meter_run_swap_decl (a
         run-cumulative 0 needs SWAP's boot decline converted: SWAP's lane). Every default roll booted the default form
         (archive/dbk/check_default_form.py). The timing guard (>= xenia) stays open.
       - **DISPLAY-REGISTERS BAKE PLAN (native/dreg-bake, 2026-10-15; written before any bake roll; DRAFT for the
         orchestrator).** `SR_NATIVE_VBLANK` flips to default ON in its two readers (kernel/native_vblank.cpp
         computeState via `requestOf`, vblankGateRequested: `!e || e[0] != '0'`); `=0` keeps the guest 82111E38, the
         guest ISR's source 0, the scan-out answers and the vsync-enable bit (the A/B's OFF arm, SR_AB_TOGGLE unchanged).
         Its requirements (GATE, the native flush, segmentless, SR_KPCR - all default ON -, today's scan-out answers, no
         page-protecting diagnostic) decide as before for an explicit `=1` (REFUSED, an Error line); the DEFAULT stands
         down instead (one WARN line naming what is missing, state Off, no [NATIVE_VBLANK] census line), so an arm that
         turns a prerequisite off - the transport's `=0` arms set SR_NATIVE_GATE=0 - runs today's path without an error.
         The orchestrator's decisions (2026-10-15): the stand-down is accepted for the DEV build on two conditions - its
         line is at WARN (a default build silently emulating the display registers stays greppable) and the release
         configuration must refuse or be unable to stand down, since its emulated answers will be gone
         (docs/RELEASE_COMPOSITION.md, stage 4). The CPU-2 bounds stand (dreg3's maxima: probe tickMax 2,443 us, every
         other roll <= 95 us, waitMax <= 24 us - none within 2x); no stress pair; the cpu2 witness stays on pace1.
         - **What the live set must show** (`native_proof.py --plan display_registers_bake`, prefix dbk; one baked exe,
           same_exe enforced, no exe pin; every item into display.registers; the unknowns `default_bake` (every item but
           the pacing steps') and `default_pacing` (the pacing steps' items) name their resolvers):
           - (a) replay 9/9 against the pre-bake exe; (b) the world-rule gate with SR_NATIVE_SEG_TILE=1, the vblank
             witness, the CPU-2 lock witness and the drained tile witness in both rolls, the reclaim audit (xport) in
             the freeze roll;
           - (c) the traced probe: every tick native, every pacing call host, `traceBad / early / mismatch` 0, no swap
             callback, `scanout=0 vsyncBit=0`; the GATE native (composed_gate_item); SWAP at most its boot decline; the
             P2.7 invariants and BlockUntilIdle drained;
           - (d) the frozen-pass per-present A/B of the default against `=0` (SR_AB_TOGGLE=SR_NATIVE_VBLANK: the per-vblank
             check on both arms, the OFF arm's inputs live) and CPPM4 on the default;
           - (e) THE METER, `=0` against the default (the meter arms uninstrumented, as dreg's ran): the dispreg column per
             ingame frame live at `=0` (>= 2) and **exactly 0** by default - the meter bound;
           - (f) THE RUN-CUMULATIVE CRITERION (the orchestrator, 2026-10-15, after dreg3's meter1 rolled; dreg3's own
             `dispreg_run_item` - a cumulative 0 - stays as written for dreg3): the default meter roll's cumulative
             dispreg (the last [NATIVE_METER_RUN] line) **<= the run's SWAP declines d** (the swapSdk `decl=` of the
             first swapSdk line after that meter line - both counted from the same run, the right one covering at least
             the left one's interval), plus `scanout=0` and `vsyncBit=0` from the same roll's [NATIVE_VBLANK] line.
             **d is not 0 on the default path:** SWAP's boot decline (`SWAP_DECL #1 why=flags2A39`, the first present's
             refused device / global MODE; one SDK-body VdSwap whose VdGetSystemCommandBuffer call is one emulated answer,
             [NATIVE_VBLANK] `sysbuf=1`) still happens with the transport baked: tbk1 / tbk3a / tbk3b's probe, meter_on
             and census1 rolls each show `decl=1`, and the lane's final1-3 (the composed env with the gate) the run
             cumulative `dispreg=1` with 0.0 per ingame frame. So the bake claims **dispreg 0 per ingame frame, and d = 1
             emulated answer a run (at boot, SWAP's) in the whole run** - not a run-cumulative 0. swap_boot_only_item
             holds d <= 1 in the same roll; the ledger grades the comparison with the new kind `text_le`;
           - (g) pacing on the console path, the default against `=0` with a second `=0` as the calibration
             (pacing_cmp.py, PACE_LONG, the 28,672-present window), uninstrumented;
           - THE STRICT WITNESS in every roll: each default roll `vblank_item` (pure ON; toggled on the A/B) and
             `cpu2_item` (the CPU-2 lock: the longest tick hold < 20 ms and the longest pacing wait < 100 ms -
             pre-registered from final1-3's 937 us / 102 us maxima and diag1's 49,890 us / 32.5 s; the slow counts
             information); each `=0` roll `vblank_off_item` (no [NATIVE_VBLANK] line at all, the emulated answers live);
             the probe and the pixel rolls the transport's tile_drained + xport.
         - **Older plans keep the arms they ran.** Every roll and gate step of a plan before the bake that does not name
           SR_NATIVE_VBLANK runs with `SR_NATIVE_VBLANK=0` (native_proof `run_gates`, where the env is built, so the
           steps read as written and the dry run shows the pin); display_registers names it in every arm already.
         - **The timing guard** (>= xenia) stays open: no Xenia pacing reference comes from these rolls.
         - **Astra round 1 on native/dreg-bake (MERGE AFTER FIXES, no P1), fixed after dbk1 ran** (dbk1 graded the
           plan as committed; the fixes are verified by tests, no re-review). (1) P2, "default" not proven unset:
           perf_run passes the parent environment to every roll, so an exported SR_NATIVE_VBLANK=1 would have made the
           default arms explicit. Now a live run of a plan with `default_keys` (display_registers_bake:
           SR_NATIVE_VBLANK) refuses before any step while the parent environment carries the key (Runner.env_refusal;
           refused, never stripped), every default roll's vblank item forbids the explicit On line and a new
           `vblank_default_item` per default roll requires the On line's "(the baked default;" form (53 items). dbk1
           itself: archive/dbk/check_default_form.py read every roll's On line and effective env - every default roll
           the default form, every =0 roll the guest path (the orchestrator's RESULT in dbk1/NOTES.txt). (2) P2, no
           fresh_out: set, with a test that an `--only meter1` re-run into the set's --out is refused before anything
           is deleted or written. (3) P3, section E's `envReq == Explicit ||` bypass removed; new E2 runs this exe as a
           fresh child (`--native-vblank-test latch`) under seven envs - =1 with GATE / SEGMENTLESS off or a
           page-protecting diagnostic REFUSED (no native registration, the scan-out answers as today, the tick
           unscoped), the default standing down at WARN, the On line naming =1 / the baked default, =0 silent - and the
           mutant explicit_as_default (computeState mapping =1 to the default) must fail it. (4) The gate's text: the
           drained tile in both rolls, the reclaim audit in freeze only (the span roll carries no xport item).
  3. **SWAP {fetch[6], fbPhys, w, h}**: D3DDeviceSwap_82112050 as host code - the fetch-constant fixups from
     hdr+0x1C and dev+0x33B0, `memcpy(hdr -> dev+0x3470, 52)`, dev+0x4E80 / 0x4FB8 (= sysbuf+4 / +8: the SWAP
     item carries the host's own 0xBEEF0000 / 0xBEEF0001 as Xenia fills them, so VdGetSystemCommandBuffer is not
     called and the SYSCMDBUF fill retires with it), dev+0x4E0C and the D1GRPH item (type-0 0x1844 when the front
     base changed: a `run`), the scaler rect / gamma state (8210A788 / 82111CB8 emit type-0 runs: guest at first,
     recordable as runs after), the flag bits 0x2A39 / 0x2A3A / 0x2A3B as the body sets them, the SWAP record
     where VdSwap's 64-dword block sat (the consumer: the present copy of the fetch's surface, `onSwap(fbPhys, w,
     h)`, XE_SWAP's counter), the close F(N) as a FENCE, BeginNewSegment virtual, then the guest's own throttle
     `GpuBlockOnFence(dev+0x34D0 = F(N-1), 3)`, SwapEpilogStats 8210BBE8 and the re-emit 820F0048 (kind 9). The
     timestamp path (dev+0x4E58 != 0) and movie capture decline to the guest body (not taken in this title; counted if
     seen). CORRECTED 2026-10-16 (native/boot-gaps): VdPersistDisplay (byte 0x2A39 & 0x10) IS taken in this title -
     once, by the FIRST Swap (the bit is set only by D3DDeviceCreatePresentBuffers 82105B18): dcf2 census1's
     SWAP_DECL #1 why=flags2A39 and frame 2's 527 literal packets. Native since native/boot-gaps under
     SR_NATIVE_DISPLAY (default ON; "BOOT GAPS BUILD NOTE" in p2.transport below); =0 declines it as before.
     **2026-10-12 bounded implementation:** default-OFF `SR_NATIVE_SWAP_BLOCK` replaces only VdSwap's reserved
     256-byte block with kind 19, an owned physical fetch and the shared host present effect. V4 passes 402
     assertions across six actual-export/offline pin-transport configurations; protected root/cell controls
     reach periodic census and cached-readable device admission uses a fault-free snapshot. Unexpected bytes,
     deleted export wiring and both unsafe-read source mutations are detected; sources and the rebuilt test
     are restored. Astra passes this scope. Device lifetime after admission is a precondition. Full 82112050,
     live SWAP/pacing, native defaults, Native0 and shipping composition remain open; p2.transport does not close.
     **2026-10-12 normal SDK body, bounded Astra PASS:** default-OFF `SR_NATIVE_SWAP` adds the admitted
     recording-allocator path, with kind 20 for the scaler-reset suffix. V9 passes 1,133 positive assertions
     across eleven fresh processes plus the intended 81/22 fence-corruption control. Real SDK producer and
     two real closed-segment consumers agree on the device except 12 QPC bytes, returned r3 and preserved
     r1/r13/lr, list/literal bytes, all GPU registers, wait counts and flush-before-swap callbacks. Actual
     allocator callbacks exercise shared-recorder reentry and changed feedback/rectangle inputs. Guard and
     stale-input defects have executed controls; handlers register once before publication. Gamma/scaler,
     epilog and transport helpers still run, and late record admission can use the development literal
     oracle. Stable writable allocator/helper mappings are a precondition. No wait-event trace, full PPC
     ABI, arbitrary SDK modes, live pacing or whole-SWAP acceptance; milestone 2 and p2.transport stay open.
     Runtime `9ba749ab61b0` builds and historical replay is 9/9 byte-identical to `c616236dd83b` with the same
     verified shader seed. Replay bypasses this SDK producer and establishes no native wiring/Xenia fidelity.
     The failed incomplete-snapshot bank is retained; the complete runtime snapshot includes its dependencies.
  4. **BRACKET begin / end + BS**: the Z-pass list owned by the consumer. BeginRecording 82110B30 -> its close,
     then BRACKET_BEGIN (the pin dev+0x3290 / 0x3294 set as today so the reserve keeps the staging memory alive;
     the CB-state save dev+0x32F0.. as today); EndRecordingKick 82110C78 -> BRACKET_END {the flush block as
     INVALIDATE records; on the alloc-failure flag 0x2A39 & 0x40 the CB-state restore and return 1 as the asm};
     the consumer replays the bracket's records twice with BS 0x15555555 then 0x2AAAAAAA (today's pump 82110460
     -> 820FD218 and the deferred (phys, dwords) lookup disappear: `replayHit / replayMiss / expired` read 0),
     `dev+0x2A74` += 1 at the kick and -= 1 at the end-sync, moved by the consumer only (as planned here; as built,
     a native bracket never moves it - the build note below. In the SDK only EndSync's decrement and 820FD800's
     chain path run under the spinlock dev+0x2A78 (SR_KE_SPINLOCK); 820FD800 reads the count unlocked and, when 0,
     adds its increment on the direct path with no lock held); the pump thread 82110A30 is never kicked. Hazard 9:
     the bracket's records stay readable until both passes ran (the arena's history half; `torn=` checks the header).
     **BRACKET BUILD NOTE (transport-bracket lane, 2026-10-14, written before building; `SR_NATIVE_BRACKET`, default
     OFF, requires segmentless; `SR_AB_TOGGLE=SR_NATIVE_BRACKET` per present; non-tiling Z-pass only, the one form
     in this title):**
     - **BRACKET_BEGIN (82110B30).** The override decides once at the top: gate, A/B window, segmentless active,
       record mode, the async CP, no PM4-output split, no tiling (0x2A38 & 0x20), no whole-body guest mode (0x2A38 &
       0x80, 0x2A39 & 0x42, PIX 0x4DF4 / 0x4E84, HSIO). Then the SDK body runs whole - its close is the ordinary
       segmentless FENCE close; the CB-state save, the pin, the 32C8 -> 32DC descriptor copy, the three fresh lists
       and the 63-dword header stay the SDK's own CPU bookkeeping ("as today"; 82110B30 sends nothing to the GPU) -
       and a host generation G opens {id, dev, list head dev+0x32C8, header VA, thread}. No consumer item.
     - **Pinned closes** are unchanged (the native close writes the mode-B record, parts registered deferred, 2
       passes) and, while G is open on that thread, also append {SegParts, list-record VA, n, phys} to G in order. A
       guest-mode close inside the bracket poisons G.
     - **BRACKET_END (82110C78).** Rule 1: everything is decided before the first write - G open and clean for this
       dev / thread; the gates re-read; device and stack mapped writable; the list walked fault-free from head+4 to
       its cursor (software jumps followed) and parsed as `[0x80 hdr][0x84](0x82)*[0x85](0x82)*[0x86]` (any other
       marker - 0x81, 0x83, 0x87..0x8C, a second 0x84 - declines); every 0x82 record bound IN ORDER to G's closes (same
       list position, n, phys: identity by sequence, never a newest-phys search), each bound entry still unpublished.
       Then the port of the generated SDK body with three changes: (1) 820FD3B8's coherency block as kRkInvalidate
       records (the SDK's own sentinel 0x00000000FFFFFFFF, same allocation, same out-params, the SDK's 11 / 22 dwords
       projected into the block); (2) the 23-dword callback block replaced by the BRACKET_END record (kRkBracket,
       kind 21, opaque) at the same allocation, the SDK's 23 dwords projected into it for CPU observers; (3) the kick
       appended through the guest's own 820FD800 with increment 0 (the SDK passes 1). Records are written only in the
       success branch; the failure branch (0x2A39 & 0x40) is the asm's (CB-state restore, dev+0x30 = dev+0x32F4 - 160,
       return 1) and drops G and its deferred entries (the SDK never replays a failed list).
     - **What the records carry.** BRACKET_END {G lo, G hi, step count, cb 82110B00, list-head view, cpu mask, flags
       r5|1, pc|2} (the last five render the SDK's 23 dwords as its PM4 form). Its SegParts = [the record] + the
       replay STEPS: BS {value} parts and nested-segment parts (the bound closes' own immutable SegParts). The steps
       come from a port of 82110460's control flow over the walked list (flags bits 30 / 31, the 0x84 / 0x86 return
       positions, the S+0x16C cache reset by the header): BS 0x15555555, the depth records, BS 0xFFFFFFFF, the records
       between 0x85 and 0x86 ONCE, BS 0x2AAAAAAA, the depth records again, BS 0xFFFFFFFF (ENGINE_MAP's 2026-10-12
       boundaries - not "the list twice"). BS is not a separate stream item: it is a step of BRACKET_END.
     - **The consumer.** The CP takes the BRACKET_END SEGMENT item by its hand-off id like any other. cpSegment gains
       two part kinds: BS = `Pm4Engine::applyBinReg(2, v)` (the P2-A BS item's method; under SR_NATIVE_FLUSH_CPPM4
       the packet C0006200 through execute()), nested = the bound segment's parts (first application counted for
       SR_NATIVE_SEG_TILE; P2 trace cpitem aux 4). The kRkBracket handler checks the payload's G against the SegParts
       being applied (monotone, once). BRACKET_END sits where the kick sat, so every later close follows the replay,
       as the SDK's chain made it.
     - **dev+0x2A74 (the drain rule) - a deviation from the sentence above, consistent with P2.4 ("stays 0; the pump
       thread just sleeps").** Under BRACKET nothing increments it, so it stays drained. For OFF (guest) kicks the
       guest's own bodies still move it exactly as the SDK does - and only partly under the spinlock dev+0x2A78:
       820FD800 reads it WITHOUT the lock and, when 0, takes its direct path (publish through 820FD218 and `+=
       increment`, no lock held); when non-zero it takes the lock, re-checks, and either chains the segment into the
       software list and adds the increment under the lock, or (drained meanwhile) releases it and takes the direct
       path; EndSync 82110768 decrements it under the lock. (Corrected 2026-10-14 after Astra's review: this note first
       said both bodies move it under the spinlock.) A consumer-side += 1 would make 820FD800's check see non-zero and
       CHAIN producer closes into the software list, which only a pump walk publishes: the consumer would become the
       pump again (guest-list walking, the deferred lookup this kind retires, the segf race class). Mixed OFF traffic
       keeps 820FD800's locked re-check (the chain_unlocked fix). Not moving it is equivalent for every guest reader:
       820FD800's chain / direct choice only orders segments, and the in-order consumer already places post-kick
       closes after the replay; BlockUntilIdle's fence wait (its flush closes after BRACKET_END) completes only after
       the replay, so its spin exits with the guarantee the pump's park gave (pinned BlockUntilIdle, fence wait
       skipped, keeps the SDK's "all published", which direct publication satisfies). Mixed A/B: an OFF kick's +1 is
       drained by the guest pump's EndSync; a BRACKET_END published while that pump still runs is chained by 820FD800
       and published by the pump through the existing deferred lookup (its only remaining use). (Superseded on
       native/bracket-ab: BRACKET_END now declines while that count is non-zero - "Mixed per-present traffic" below.)
     - **Hazard 9.** The bound closes' parts are immutable copies captured at emission (SegRecord) plus gap bytes
       copied at their close; the BRACKET_END SegParts holds them until its one application ends (both passes ran)
       and then releases them; arena reuse cannot reach them. Hazard 5 is unchanged (the bracket's command-buffer
       memory retires with the first post-bracket FENCE, applied after BRACKET_END).
     - **Retires under pure ON:** the kick's literal coherency + callback PM4 (~10 packets a bracket), the CP
       interrupt and ISR 82110B00, the pump wake and its 82110460 -> 820FD218 / 82110140 path, the 4 BS items, and
       the deferred lookup for bracket records (`replayHit / replayMiss / expired / chained` 0). The deferred
       registry stays for OFF brackets and A/B transitions. **Declines** (guest body whole, counted by reason): each
       begin condition above; at the end no / poisoned G, a non-canonical or unreadable list, a binding mismatch, an
       already-published entry, a gate or mode re-check. A declined end runs the SDK kick and the guest pump replays
       the still-deferred entries as today.
     - **Proof.** Offline: difftest `--native-bracket-test` drives the real BeginZPass 8210DF38 / EndZPass 8210E1A0 /
       InvokeRenderPass 8210E458 through production dispatch, the per-present A/B selecting the arm: OFF = the actual
       SDK kick, the actual marker interpreter 82110460 (its ring output parsed) and the actual EndSync 82110768; ON
       = BRACKET. Compared: device / list / command buffer / stack frame / r3 (only dev+0x2A74 differs, by design),
       the replay sequence, an in-order emulated CP's effects on both arms (fence and per-pass predicated stores),
       dev+0x2A74 drained; named compiled defects each detected, gate-off and decline controls, exact restoration.
       Live: `native_proof.py --plan bracket` (probe + P2 trace invariants, frozen per-present A/B, CPPM4 and stress,
       meter, gate).
     - **Status (2026-10-14, branch native/transport-bracket): BUILT, default OFF; offline proof only.**
       `tools/difftest/native_bracket_run.py` on the cloud worker (archive/tbr/final/): `--native-bracket-test` 263/0
       over 20 fixtures (plain, native records in the closes, the neutral scissor between 0x85 and 0x86, a coherency
       range, the whole buffer, both, the all-ones range, a post-kick close, the direct 82110C78 route, a single record,
       a 540-close list across software links, the kick / hole / coherency allocation failures and a pre-set failure,
       a helper's late no-GPU flip, and four declines: poisoned, a second 0x84, a duplicated record, an entry already
       published); gate-off 82/0; CPPM4 263/0; composed with SR_NATIVE_TRANSPORT 263/0 (its gate-off 82/0);
       segmentless-off 83/0 (every begin declines); P2 trace on 263/0. 16 compiled defects (colour BS, the 0x85 jump,
       the colour pass, the increment, the coherency sentinel, the root view, retirement, the failure rollback, the
       software link, the A/B window, the consumer's BS register, the sequence binding, poisoning, the frame, the
       published check, the segmentless requirement) each fail their named assertions and pass their gate-off
       control; exact restoration and rebuild. The 74 existing difftest suites' exit codes equal dev ac0e8058's (56
       exit 0). p2_trace.py's segment_order gains the bracket class (kind 24 aux 4). Owed: the live plan above (not
       run; its meter bound -9.5 is a planning estimate to confirm against a census).
     - **Found while building (pre-existing, not changed here).** The native close's coherencyBlock tests the packed
       range dev+0x2D18 against ~0 and leaves all ones; the SDK's 820FD3B8 (and so 82110C78, guest or BRACKET) uses
       0x00000000FFFFFFFF. After a native close every kick therefore sees a "range" and emits a one-page invalidate at
       0xFFFFF000 (the `all-ones-range` fixture pins the SDK's behaviour on it), and a non-owner merge (820F4280 /
       820F3988: base = min, end = max) into all ones yields [start, 4 GB). Today only the COHER registers see it.
     - **Mixed per-present traffic (2026-10-14, native/bracket-ab; the live abp roll brk1/tbr_abp).** Under
       `SR_AB_TOGGLE=SR_NATIVE_BRACKET` the frozen A/B roll fell to one present per ~805 ms from the third intro movie
       on (dispreg ~1610 a frame is only the 2-per-vblank scan-out count over the long frame) and never reached play;
       the pure-ON probe was healthy. Cause: a native BRACKET_END while an OFF bracket's pump work was still
       outstanding (dev+0x2A74 != 0: the CP / pump a little behind the producer). 820FD800 chains every close made
       while the count is non-zero into the list it is given - after a kick the current list dev+0x32C8, i.e. the
       native bracket's own list, which no pump ever walks. Those closes (draws, fences; with a longer lag also the
       next OFF bracket's guest kick, which leaves the count stuck) were never published. Each present's fence wait then
       made no progress until GpuBlockOnFence 820FD9A8 gave up: its step GpuRingWaitProgress 820F33E8 returns 0 once
       the KTHREAD+0x58 clock has run 5000 units without fb+0 moving (82106BA0's recovery is a trap without a hang
       callback). The constant ~805 ms, under the hang watchdog's 1 s H0 threshold, is read as that limit - inferred
       from the asm and the interval, not instrumented. Fix: BRACKET_END declines (`pending=` on the bracket
       line) while dev+0x2A74 != 0; the SDK kick's +1 then makes the pump walk this list after the outstanding one.
       A 0 read stays 0 until this thread's next guest kick, so a native end never meets a chain (the chained
       BRACKET_END route no longer occurs). Offline: the suite's mixed fixture (alternating windows, the CP and the
       actual pump - 82110460 + EndSync through chained blocks - lagging 0 / 1 / 2 frames, a present between begin
       and end, the all-OFF SDK reference; per-close markers, dev+0x2A74 and fb+0 checked) lost 8 / 44 (count stuck
       at 3) / 11 of 56 markers before the fix and none after; the compiled fault pump_pending restores the loss.
  5. **The residue:** WAIT_UNTIL (BlockUntilIdle 820FDCB8 keeps its guest spin on fb+0 == S and dev+0x2A74 ==
     0; its packet = a `run` of 0x5C8 the engine drains on), InsertFence (FENCE), the lock core / VertexBuffer_Lock
     (INVALIDATE), 820FC000's closes inside the DrawIndexed flush (FENCE once the close is native: segmentless),
     RING_UNSTAMPED; ZPD / EXT stay the fake (OFF; no queries on this path).
     **RESIDUE BUILD NOTE (transport-residue lane, 2026-10-14, written before building; three producers, each behind
     its own gate, default OFF, each requiring segmentless, each with its own per-present A/B; from the generated
     bodies of 820FDCB8 / 820F4280 / 820F3988 / 820F3FA0 / 820F40F0 / 820FF468 / 820FDD30 / 820FC000 / 820FD9A8 -
     IDA's MCP was unreachable, so guest/ppc is the oracle, as for BRACKET):**
     - **Already native, nothing to build (the suite checks them, nothing is converted):** InsertFence 820FDD30 is
       `dev+0x2A2C = S`, the close 820FD8C0, return S - it writes no PM4 of its own, and under segmentless its close
       IS the INVALIDATE + FENCE records. 820FC000's (and 820FBBF8's) closes are their single `bl 820FD8C0` (no inline
       fence tail in the generated bodies), which production dispatch sends to the segmentless override: FENCE
       records since p2.segmentless. RING_UNSTAMPED is the Z-pass replay's four ring-level bin selects (P2-A host BS
       items, baked; BRACKET steps) plus the boot ME_INIT (boot.device_create). ZPD / EXT stay the fake. What still
       reaches the CP as guest PM4 from this item is three producers' bytes in the open segment (gap parts):
     - **IDLE - D3DDeviceBlockUntilIdle 820FDCB8 (`SR_NATIVE_IDLE_WAIT=1`, `SR_AB_TOGGLE=SR_NATIVE_IDLE_WAIT`).** The
       body: [kick check: dev+0x28 > dev+0x30 -> 820FD8C0], the 2-dword type-0 WAIT_UNTIL {0x5C8 = 0x00020000} at the
       cursor (dev+0x28 += 8), `if (S = dev+0x2A1C) GpuBlockOnFence(dev, S, 4)`, then `while (dev+0x2A74 != 0);`.
       Native: a port of the generated body (ctx-style, kept so by the regen) with ONLY the two stores replaced by an
       Entry span holding one register run {0x5C8: 0x20000} - an existing record form, applied as the PM4 path's
       type-0 branch applies it (a register-file store: this CP gives WAIT_UNTIL no other effect; an in-order
       consumer is idle-ordered by construction). Everything else stays the SDK's code: the kick check and its lr,
       GpuBlockOnFence(S, 4) - whose F == S flush closes the segment holding the record (INVALIDATE + the run + FENCE
       S) - and the guest's own spin on dev+0x2A74. **The drain rule:** IDLE writes none of dev+0x2A74, fb+0, S:
       S moves only in the close (+2), fb+0 only in the consumer's FENCE apply (after it applied the run, in stream
       order), dev+0x2A74 only in the guest's 820FD800 / EndSync 82110768 (OFF kicks) and never under BRACKET. So the
       call returns exactly when the SDK's does: fb+0 reached S (the wait) and dev+0x2A74 == 0 (the spin). Pinned (a
       Z-pass bracket open): GpuBlockOnFence returns at once as in the SDK; the record rides the pinned segment.
     - **INVAL - GpuInvalidateMemoryRange 820F4280 (`SR_NATIVE_INVAL_RANGE=1`).** On a non-owner thread it only
       merges the range into dev+0x2D18 (CPU, no PM4): planned as declined at the top; built (amended while building)
       as the port's own transcription of that merge, unchanged, counted `merge` (no packet, so nothing is declined
       there: the suite runs it against the SDK body so the ported code is exercised). On the owner thread
       (dev+0x2A08 == the current thread, the 82468EF8 read): [kick check], then 13 dwords - WAIT_UNTIL {0x20000},
       COHER_STATUS_HOST 0xA31 = 0x03000000, COHER_SIZE / BASE 0xA2F {size, base} (the page-rounded physical range),
       WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}. Native: the port of the owner path; one Entry span with the run
       {0x5C8} and one kRkInvalidate {base, size, whole 0} (the existing kind renders exactly those 11 dwords;
       applyCoherency writes the three registers and runs the same register-mode WAIT). No fence, no dev+0x2A74.
     - **LOCK - the lock core 820F3988 (`SR_NATIVE_LOCK_COHER=1`).** Callers: VertexBuffer_Lock 820F3FA0 (reason 10,
       COHER status 0x03000000), the texture lock 820FF468 (reason 14, status 0x02000000), IndexBuffer_Lock 820F40F0
       (reason 12: flags |= 2, never emits). Body: the resource fence wait GpuBlockOnFence(res+8 or +0xC, reason); if
       !(flags & 0x12), on the owner thread [kick check] + the 11-dword COHER block with the caller's status (the
       stack argument at sp+0x54), on another thread the merge into dev+0x2D18; then the lock bookkeeping (dirty
       range, the physical view, res+0 += 0x100) and r3. Native: the port of the generated body, the owner block as
       one Entry span - status 0x03000000 / 0x01000000 as kRkInvalidate {base, size, whole 0 / 1}, any other status
       (the texture lock's 0x02000000) as a NEW opaque kind **kRkCoherHost (22) {status, size, base}**: kRkInvalidate's
       third word is the whole-buffer flag the shipped segmentless close, BRACKET and the record adapter read as a
       bool, so it is not widened; kind 22 writes the same three registers with the status as given, runs the same
       WAIT, and renders the same 11 dwords for CPPM4. Declined at the top (the guest body whole: no PM4 to convert):
       flags & 0x12 (counted `noPk`). The non-owner thread: as INVAL (amended while building), the port's unchanged
       transcription of the dev+0x2D18 merge, counted `merge`.
     - **Rule 1 - the decision at the top of each override (all of it, else the guest body whole, counted by
       reason):** the producer's gate, its A/B window, record mode (SR_NATIVE_FLUSH on, not CHECK), segmentless active,
       no scoped SDK oracle (PresentGateOracle / ShaderConstRunsOracle), the device mapped, the close's guest-mode
       predicate false (b38 & 0x80, b39 & 0x42, PIX 0x4DF4 / 0x4E84, the HSIO failure) - a record whose close then
       went the guest way would be lost (feetLost) -, no tile auto-predication (10808 & 2), no live trace or native-dev
       projection, the owner thread. The port then runs the SDK's code, and at the one place a record is written a
       late refusal (the Entry's arena wait, or a guest mode a helper flipped inside the kick) writes the SDK's own
       literal dwords instead (counted `late`): from that point the port IS the guest body, so the output is the SDK's
       whole, never half of each.
     - **Composition.** Segmentless: required; the spans travel as the segment's record parts by footprint sequence
       like every Entry span; CPPM4 renders them back (run -> type-0, kRkInvalidate / kind 22 -> the 11 dwords).
       BRACKET: orthogonal - the three never touch the pin, the bracket list or dev+0x2A74; a span inside a pinned
       segment is a bracket part replayed twice, as the SDK's bytes there are. The coher-sentinel lane (the native
       close's dev+0x2D18 sentinel): independent - the owner blocks never read dev+0x2D18 and the non-owner merges
       stay the guest's. The default path: all three gates default OFF and latched; each override's first statement
       tests its gate and runs the guest body when it is off (820F4280's N0 shim gives way to an override that keeps
       SR_NATIVE_SCOPE; 820F3988 had no override; 820FDCB8's observe override gains one line).
     - **Proof.** Offline (this lane): difftest `--native-residue-test` - the actual SDK bodies through production
       dispatch against the native ports, the per-present A/B choosing the arm (OFF = the SDK body on the segmentless
       transport, ON = native), one in-order emulated CP applying both (synchronously at each publish - P2.7's sync
       mode - so the fence completes inside the call; one fixture drains late from another thread and one releases
       dev+0x2A74 late, to prove the exits); compared: device, command buffer outside the span (the span: the SDK's
       bytes vs untouched, the record rendering them), ring, stack, registers, the consumer's register effects and
       waits, fence stores, fb+0, dev+0x2A74; bracket cycles with the producers inside (BRACKET native, BRACKET
       declined while a pump is pending, the SDK pump); compiled defects, gate-off and segmentless-off controls,
       restoration; the 74 existing suites against dev. Live (owed, not run): `native_proof.py --plan residue`.
       Counters on the `[NATIVE_FLUSH] residue` line: taken / declined by reason / late per producer, `pk` (the PM4
       packets the taken calls replaced: IDLE 1, INVAL 4, LOCK 3) and `litPk` (packets the producers still wrote as
       PM4: a decline with a packet, or late) - the producers' own packet count, which the meter's kinds cannot
       isolate (WAIT_UNTIL and COHER are type-0 / sync among many others).
     - **Status (transport-residue, 2026-10-14): BUILT, default OFF, offline proof only** (native/transport-residue,
       merged with dev 5b482384 - bracket-ab, coher-sentinel, the oracle split; not merged into dev). Astra's review
       of the first proof (no P1 implementation defect) asked for three repairs, all made: the fixture's dev+0x2D18
       starts at the SDK's empty 0x00000000FFFFFFFF (it was all ones, which kept a merged end at 0xFFFFFFFF and hid
       a dropped end update) and every call checks the range right after it (a non-owner merge's both endpoints
       against the fixture's inputs, then the next close's INVALIDATE; otherwise the empty value); real bracket
       cycles (BeginZPass 8210DF38 -> the texture lock, an owner INVAL, BlockUntilIdle inside -> EndZPass 8210E1A0,
       the CP lagging the producer, the kick INTERRUPT answered and the actual pump 82110460 + EndSync 82110768 run);
       suites_compare.py fails closed (exit 2 on an empty / malformed summary or a duplicated suite).
       `--native-residue-test`: 34 call fixtures (12 IDLE, 6 INVAL, 14 LOCK, 2 InsertFence) and 3 bracket cycles,
       both A/B arms each; ten modes valid - native 597/0, gate-off 550/0, CPPM4 594/0, PM4 output 593/0, BRACKET
       602/0, BRACKET with the residue gates off 555/0, native transport 597/0, transport gate-off 550/0,
       segmentless-off 549/0, P2 trace 597/0. Under SR_NATIVE_BRACKET=1 (BRACKET native in both arms) BRACKET_END is
       published and consumed once per arm with the residue spans in its nested replay, kind 22 applied in both BS
       passes, 4 coherency requests per arm, no interrupt or pump; the pending cycle ends a bracket while an SDK
       bracket's pump work is outstanding (dev+0x2A74 != 0): BRACKET_END declines (kBrPending), the SDK kick chains
       behind that pump, everything drains. Without BRACKET the same cycles run through the SDK kick and the actual
       pump (kind 22 again applied twice). 24 compiled defects in kernel/native_flush.cpp - among them the merge's
       dropped end update in both ports and the residue refusing a pinned segment - each failing its named
       assertions with its gate-off control valid, sources restored, rebuilt, five modes rerun valid
       (`archive/tres/final/residue_matrix/residue_matrix.json` in the lane worktree). One defect (the IDLE cursor 4
       bytes short) is caught as the native arm never returning from its fence wait (the watchdog), not as a field
       difference; its cause inside the close was not traced. The 74 existing suites: exit codes identical to dev
       ac0e8058's baseline, 56 exit 0 (`archive/tres/final/suites_compare.json`); the BRACKET suite's seven modes
       and the coher-sentinel suite's three modes valid on this branch (`archive/tres/final/bracket_suite`,
       `coher_suite`; the coher runner detects its first fault, then refuses restore_all_ones because that anchor
       occurs twice in dev 5b482384's own native_flush.cpp - pre-existing); test_native_proof.py fails only the
       pre-existing transport_pair item. The INVAL / LOCK ports sit in a block conditioned on their gate
       (scripts/audit_forcing.py drops early-exit guards at goto labels): forcing audit 0 UNGATED. Live (owed, not
       run): `native_proof.py --plan residue` (dry-run only). Coher-sentinel: composed (its fix is merged here and
       the fixture now uses its empty value); the owner blocks never read dev+0x2D18 and the non-owner merge is the
       guest's own code.
     - **The owner census items (residue-census lane, 2026-10-14): a measurement defect, not PM4 from converted
       paths.** The live plan on dev 00865e25's exe 4292f86c1eec (`_worktrees/native/archive/v1_native/res1`) failed
       its four `[PM4_COVMAP]` owner items (census0: no owner=820FDCB8 / 820F4280 line, 820F3FA0 only `pk=1 keys=r05:1`;
       census1: owner=820F4280 `keys=3C:1,r04:1,r05:1`, owner=820F3FA0 `keys=3C:2,54:1,r05:2`). Why: (1) on the
       segmentless transport every guest PM4 byte of a segment runs from the close's host copy (cpSegment, a kind-1
       part: `execute(sp->bytes, ..., 0)`), which has no guest physical address, so pm4_coverage's stamp lookup finds
       no owner - in all 16998 / 16259 census frames every depth-0 packet is UNSTAMPED (`acc <= dN`, `unst >= d0`;
       96.8% of all packets); (2) what the owner lines do hold are the depth-N literal windows (the Z-pass kick
       blocks, `dN=10` a frame, allocated outside any bracketed cursor range) attributed by STALE stamps (the interval
       map is never cleared on consumption): census1's 820F4280 / 820F3FA0 keys are packets those functions never
       write (register page 0x04, INTERRUPT 0x54; and no page 0x0A). The base therefore could never show the producers
       by owner (the plan's expectation came from the pre-segmentless P1 census). PM4 does not flow from converted
       paths: census1's residue line reads `litPk=0`, every late and decline-with-packet 0 (`noPk=3497` = IndexBuffer
       locks, no packet), and the CP's own per-frame counts dropped by exactly what the residue converted - page 0x0A
       packets 13.975 -> 7.398 a frame (expected drop 2 x 3.383 converted blocks = 6.766: ratio 0.972), WAIT_REG_MEM
       12.988 -> 9.699 (expected 3.383: 0.972), page 0x05 9.236 -> 9.044 (expected 0.209 WAIT_UNTIL: 0.920); in steady
       gameplay windows 16.1 / 14.05 -> 8.07 / 10.04 (4.0 blocks a frame).
       **Fixed in the census itself (Astra: an aggregate cannot establish removal per producer):** (a) a segmentless
       gap part is attributed by the guest VA it was copied from (`SegPart::va`) with the stamps the close moved into
       the segment (`SegParts::stamps`, `pm4cov::setCopyWindow`), and every stamp carries the producer - the innermost
       table scope that wrote the bytes (`Stamp::inner`, `[PM4_COVMAP] ... inner=<VA>` lines beside `owner=`); (b) the
       stamp lifetime, from the code (accepted by the coordinator 2026-10-14, Astra to review): born at a piece cut on
       the device owner's thread (dev+0x2A08) and at a GpuCmdBufAllocate 820FCDF0 hand-out inside a bracket (the block
       stamped with its allocating scope: the SDK Z-pass kick blocks are EndZPass's, so no producer hides in
       UNSTAMPED - the transport plan's [PM4_COV] 0 milestone counts every producer), MOVED with a native close's copy
       (the open piece restarts past the closed range), ENDED when either XDK allocator hands the memory out again
       (BeginNewSegment 820FCF90's new segment; 820FCDF0's block, through its new N0 scope's hooks - a table internal
       row), ring dwords taken on read - so a reused block reads its new allocator's or UNSTAMPED, never an old owner;
       a chained close's 16-byte list node stays unowned but marked, its packets (if the CP ever read them) counted
       in `[PM4_COV] CUM chain=` (ENGINE_MAP "Brackets"). The pump driver 82110840's override now carries its
       SR_NATIVE_SCOPE (the generator passes; the table and OVERRIDES.json are byte-identical). **Astra (c0c4a917,
       MERGE AFTER FIXES):** ownership is now POSITIVE - a piece is stamped only when the device was the bracket's
       thread's at both of its cuts and a block only when it is at the hand-out (the released device, 0, and an
       unknown identity own nothing; a cut finding another owner drops the piece); ring stamps stay
       ownership-independent (the pump's 820FD218 ring writes are the driver's). **Astra round 2 (817ceb97):** the
       transitions between the cuts count too - an ownership epoch moves at the enter and leave of every scope that
       writes dev+0x2A08 (820F3618, 820F3658, D3DDeviceCreate 820F34C0: all its guest writers) and must be unchanged
       between the two cuts (A -> B -> A drops the piece); a cut reads epoch, owner, cursor, epoch, owner (a takeover
       inside its reads drops it); a piece never stamps over another thread's stamp (refused whole). Disclosed, not
       fixed: the pump's bin-select dwords (82110140 writes them to the ring directly) carry no ring stamp - incomplete
       attribution, counted RING_UNSTAMPED; with SR_KPCR=0 no thread has an identity, so no command-buffer piece or
       block is stamped (ring stamps remain). Offline proof
       `tools/difftest/native_census_run.py` (`--native-census-test`, bare runs supported: producers through
       production dispatch, nested scopes, both hand-outs, block scopes, a non-owner thread, the released device and
       an unknown identity, an AcquireThreadOwnership steal on a second guest thread inside an open bracket, A -> B ->
       A between two cuts (820F3618's scope hook), a takeover inside a cut's reads, a raw owner write under another
       thread's stamp, a block allocated on that thread, a lagging CP, the BeginZPass -> EndZPass kick and a close
       chained behind its pump, the kick's pump run by the production driver 82110840 on the second thread (owned,
       then released device); twenty-two compiled faults; census on / off digests equal). The plan's
       census items are producer-specific again (`producer_census_item`, pre-registered from res1's census1 residue
       line): census0 shows each converted producer's own keys (BlockUntilIdle r05; INVAL r05 / r0A / 3C;
       VertexBuffer_Lock r0A / 3C; the five texture / surface / volume lock entries r0A / 3C), census1 none of them;
       `residue_census.py` stays as a diagnostic step only (strict: a malformed or truncated census line exits 2).
       **The fresh census pair rcen1 (2026-10-14, merged dev 6528ffef, exe 6705f9e06683, rule in rcen1/NOTES.txt
       written before the rolls): every producer item passes** - census0 shows BlockUntilIdle, INVAL, VertexBuffer_Lock
       and the texture / surface / volume locks writing their own keys as guest PM4; census1 shows none of them, its
       residue counters strict (litPk=0); the kind diagnostic PASS (ratios COHER 1.047, WAIT_REG_MEM 1.047,
       WAIT_UNTIL 1.200). res1's four failing items are the witnesses of contradiction `census_v1_measurement`.
  6. **Proof** (`tools/perf/native_proof.py --plan transport`, every item into `p2.transport`): the probe roll with
     the P2 trace and the P2.7 item-4 invariants (every CALLBACK ran exactly once, pc+0x10 == 0x0BADF00D and pc+0
     == 0 and `dev+0x2A74` == 0 at every present, fb+0 monotone and never ahead of C, produced == consumed per
     kind, no `[GPU_HANG]`); `SR_P2_SYNC=1` answers identical per present ring vs transport; the frozen-pass
     per-present A/B per kind at the throttle point, graded under rule R; CPPM4 for each kind (a PM4 form exists
     for all but the hold) and its stress twin; the meter: pm4 per ingame frame -> 0 with `[PM4_COV]` 0 and kind-10
     records 0; pacing vs the golden (vblanks per present, `dev+0x3B20`, `dev+0x4E4C`) for the `timing` guard;
     the flicker gate; replay 9/9. Each kind lands gated OFF with its own A/B before the next is built.
     **BlockUntilIdle drained (orchestrator, 2026-10-10):** release/forcing deletes the 0x820FDD18 hook in
     D3DDeviceBlockUntilIdle 820FDCB8, so the call spins on `dev+0x2A74` until the ISR / pump drain it, as on the
     console (the Xenia oracle). Every kind here that replaces the ISR, the pump or the kick (CALLBACK, GATE, BRACKET,
     the host vblank service) must keep `dev+0x2A74` drained exactly as 820FDBA0 / 82110768 do. Proof check on
     every traced roll (P2 trace kinds 26 / 27, `p2_trace.py --json` -> `idle`): every BlockUntilIdle entry
     completes and the call minus its own fence wait (the drain spin) is never longer than a frame (16.7 ms).
  Build order: CALLBACK -> GATE (a) -> SWAP -> BRACKET + BS -> the residue -> `[PM4_COV]` 0 (p2.transport closes)
  -> GATE (b) (+ the (c) decision) = `display.registers` -> acceptance.zero's C1.
  **DECISIONS (orchestrator, 2026-10-10):** (1) CALLBACK kind 13 moves to native-a - the producers and the apply;
  the research/p2 reservation lapses (that lane is not running and holds only the enum line); the reservation
  note in the code and the ledger are updated in the same commit that builds it. (2) `display.registers` = GATE
  (b) WITH (c): the host vblank service performs 820FDBA0's source-0 effects itself - dev+0x3B14++, the countdown
  (dev+0x3B1C, releasing pc+4 = 0 and dev+0x3B18 = dev+0x3B14 at 0) and the swap callback (dev+0x3B10 with its
  {frame counter, dev+0x3B20, 0} block) - so the vsync-enable bit 0x7FC86544 goes to 0 as well ("once per run"
  is not zero under C1; no exception carved after the counting rule). Conditions: the host service matches the
  source-0 path call for call (IDA, plus an oracle trace of the ISR's effects per vblank: counter, countdown,
  callback order and timing); its own default-OFF gate and its own A/B under rule R, plus the pacing check
  against the golden; last in the order, after `[PM4_COV]` 0. If the source-0 path does more than those three
  effects, or something only the guest ISR can do, stop and bring it to the orchestrator with evidence - no
  workaround. (3) The rest of the plan and its order: approved. Building starts after the segmentless plan
  reports.
- **p2.transport GAP MAP (transport-composed lane, 2026-10-14; written before any composed code; branch
  native/transport-composed at dev 041d6f9e).** What still reaches the CP per present in gameplay, by producer, and
  what removes it. Source: the producer census pair rcen1 (exe 6705f9e06683 = dev 6528ffef; census0 = segmentless +
  `SR_NATIVE_BRACKETS=1`, census1 = + the three residue gates), read by `tools/perf/transport_census.py` (new; its
  test `test_transport_census.py` has 16 cases, 11 negative controls; a reconciliation mutant, a tail mutant and a
  foreign-signature mutant each fail it). Outputs:
  `archive/tcmp/rcen1_gapmap.{txt,json}` and `s1_rates_census{0,1}.txt` in this lane.
  - **Method.** The census is cumulative and periodic (a report every 256 frames or 20 s, none at exit). Each
    producer's `inner=` line is differenced between the first report at or after the ingame marker (A) and the
    last report (B): census0 covers frames (2301, 3584] (1,283 presents), census1 (3328, 5376] (2,048 presents).
    Both rolls pass every completeness check: F records 1 to the last frame are contiguous; the tail after B is
    155 / 207 frames (17.0 / 14.2 s), within one report period; for every key, the F lines over (A, B] equal the
    CUM delta and the sum of the producer lines (UNSTAMPED 0 in both intervals). A census frame is one XE_SWAP
    and a meter frame is one VdSwap, so both count per present. Totals: census0 364.43, census1 352.89 packets per
    present. The default meter roll on the setconst-baked exe reads 330.2 (scb1: t0 133.8, t2nop 174.5, sync
    17.0, bin 4.1, drawAuto 0.6, other 0.2).
  - **The map** (packets per present, census0 / census1, and what removes each):

    | Producer (inner) and its keys per present | c0 | c1 | Removed by |
    |---|---|---|---|
    | 82112050 Swap: VdSwap's 64-dword block (XE_SWAP 64, fetch run r48, r05, 52 type-2 fill) | 55.0 | 55.0 | SWAP (`SR_NATIVE_SWAP_BLOCK` kind 19 + `SR_NATIVE_SWAP`) |
    | 82112050 Swap: 820F0048's per-present re-emit (60, 61, 62, 63, r0F one each) | 5.0 | 5.0 | boot.device_create's per-frame half (`SR_NATIVE_DEVCREATE`, kinds 8 / 9): not a transport kind |
    | 82111EC8 the vblank gate (3C 4, 54 1, r04 1, r05 5 = the 39-dword block) | 11.0 | 11.0 | GATE (`SR_NATIVE_GATE`, kind 17) |
    | 8210E1A0 EndZPass + 820FCF90 BeginNewSegment: the Z-pass kick block (3C 3, 54 1, r04 1, r05 3, r0A 2 per kick; 0.99 + 0.01 kicks) | 10.0 | 10.0 | BRACKET (`SR_NATIVE_BRACKET`, kind 21) |
    | 820F3FA0 / 820FFCB0 / 820F4280: owner COHER blocks and WAIT_UNTIL | 12.03 | 0 | the residue (ON in census1) |
    | 82113010 per-tile constants (2D, type-2 pads) | 15.24 | 14.25 | p2.setconst_mem: default ON since 4f6c7972 (after this exe), 0 on dev 041d6f9e |
    | 820FC000: 821075E0's shader-constant runs, 3 calls (COHER_DEST_BASE_1 r20, COHER r0A x2, WAIT 3C, SQ_*_CONST r23) | 15.0 | 15.0 | p1.row3_construns (`SR_NATIVE_SHADER_CONST_RUNS`, kind 18; default OFF) |
    | 820FBBF8 / 820FC000 / 820FB6E8: the rest of the S1 draw flushes' scope | 238.5 | 239.0 | **nothing built** (below); p1.flush_residue since the decisions |
    | 8210C5F8 / 820F5570 / 82101A58: Resolve / RegisterBlock / ClearF declines | 2.67 | 3.69 | p1.row9's declines (no open item); p1.flush_residue since the decisions |
    | RING_UNSTAMPED / UNSTAMPED | 0 | 0 | in frame 1 only (ME_INIT 1, the device-less create bracket 22): boot.device_create |
    | ZPD sun flare 82721118 / HUD glow 82304058 | 0 | 0 | p2.zpd (never fired) |

    CALLBACK has no line: 820FDA80 is not a census scope, and IDA shows exactly three callers, 82111EC8, 82110C78
    and InsertCallback 820FDE38 (0 calls). So once GATE and BRACKET own their blocks, CALLBACK takes only their
    declines. The pair emits no PM4.
  - **With every built kind ON, the remainder is not zero.** On dev 041d6f9e (setconst baked) the census predicts
    352.89 - 55 (SWAP) - 11 (GATE) - 10 (BRACKET) - 14.25 (setconst) = **262.6 per present**: 239.0 unbuilt, 15.0
    construns, 5.0 devcreate and 3.7 row 9 declines. On the meter basis it is about 330.2 - 88 = 242. (Corrected
    after Astra's round 1: the 262.6 still counts 820F0048's 5.0 re-emit packets. The composed env the orchestrator
    chose has `SR_NATIVE_DEVCREATE=1`, so about **257.6 remain** there: 239.0 + 15.0 + 3.7.)
  - **The unbuilt 239 are the S1 Driver's guest leaves.** They are both the segment-limit section fallbacks AND the
    count > 0xFFFF split-draw declines (both write inside the flush scope; see the last item here).
    `Driver::constRuns` / `regRuns` / `fetchRuns`
    (kernel/native_flush.cpp) hand a whole section to the guest leaf (821078C0 / 82107520 / 82107760) when it does
    not fit before the segment limit (`cc + ... < lim()`). The leaf writes type-0 runs with type-2 alignment pads
    and kicks through 82107058's chunked path. Whole-call S1 declines of draws with count > 0xFFFF add the guest
    body (its split DRAW_INDX and every section).
    - The S1 counters per present in census1's interval: 25.0 section fallbacks (21.2 of them constRuns aborts),
      1.07 count > 0xFFFF declines, 6.26 kicks, 2,301 draws taken. census0 reads the same.
    - Keys per present: type-2 pads 131.6, ALU constants r40-r47 68.4, fetch r48 19.9, the fetch trailer r50 7.6,
      bool r49 0.4, register runs r21 / r22 8.5, DRAW_INDX 2.5, shader binds 0.06.
    - p2.transport's own title names this residue: "the DrawVertices flush residue (markers, segment-limit
      fallbacks)". The BUILD PLAN's items 1-6 have no kind for it. The RE-PLAN expected P2 to make the
      segment-limit aborts go away; under segmentless they did not, because the segment limit and the kick stay
      guest-visible (dev+0x2C / 0x30, S, the kick check).
    - What would close it: the sections written across the kick as records (82107058's chunking natively, with
      records on both sides of the segment switch and the SDK's cursor and kick unchanged), and count > 0xFFFF
      draws split natively (820FC000's own loop over 820B3668 / 820B366C). Neither is built.
    - The census cannot split the 239 between the two causes, because both write inside the flush's own scope.
      The counters split the calls (25.0 against 1.07 a present); splitting the packets needs a sub-scope stamp.
  - **What [PM4_COV] 0 includes (proposed rule for the composed plan).**
    - Counted: every packet the Pm4Engine interprets: type-0 / 1 / 2 / 3 headers at every depth, type-2 pads
      included, chain tails and ring-level IB descriptors. The span runs from the first census report at or
      after the ingame marker to the run's last F record. The producers over (A, B] must each read 0, and the
      tail's F lines (unattributed) must read pk=0 too.
    - Not counted: every frame before the ingame marker. Frame 1's ME_INIT (RING_UNSTAMPED, 1 packet) and the
      device-less D3DDeviceCreate bracket (UNSTAMPED, 22 packets) belong to boot.device_create and to
      acceptance.zero's whole-run C1. The movies, title and front end are acceptance.zero's eras.
    - RING_UNSTAMPED in gameplay is 0 on a plain roll. Under ringless the CP parses one ring packet per run (the
      first doorbell, at boot). The SDK pump's 8 bin-select ring dwords (82110140, 4 BS items per replayed
      bracket) are host BS items, applied through applyBinReg, so the census never counts them, even while an SDK
      pump runs. They exist only while an SDK kick wakes the pump (BRACKET off, declined, or `literal=`), so the
      composed plan grades them on the bracket line, not on [PM4_COV]. CPPM4 rolls render them as packets
      (C0006200); those are not meter rolls.
    - "Kind-10 records 0" is vacuous: kRkPm4 is a reserved kind with no producer ("the segment parts carry gap
      bytes directly"). The gap parts' bytes are exactly the packets the census counts, attributed by SegPart::va.
      The composed plan uses the census and the periodic line's `spans ... pm4=0` instead.
  - **GATE and SWAP compose; they are not alternatives** (IDA). Every present caller (D3DPresentSwapOnly
    82611FB8, D3DPresentResolveThenSwap 82612090, D3DDeviceBlitAndSwap 82112750) calls InsertFence 820FDD30, then
    the gate 82111EC8, then Swap 82112050. They are different producers (11 and 55 a present), and zero needs both.
    The swap_sdk plan kept GATE off over its unresolved v1 freeze; pg2 resolved that (`gate_v1_superseded`). In
    code, `swapLeaf` neither calls 82111EC8 nor tests the 0x2A39 bit 0x20 that the gate sets, and it clears that
    bit as the SDK does. No roll or suite has run the two together yet.
  - **Orchestrator decision needed before the composed plan is pre-registered:** p2.transport's observation is
    "[PM4_COV] 0 in gameplay", and the built kinds cannot reach it: 262.6 a present remain (257.6 with devcreate
    ON), and 239 of them are an unbuilt item of the record's own title (the section fallbacks and the split-draw
    declines). Either (a) build that item (and split draws) as p2.transport items 7 / 8
    and keep the global zero, which also needs construns and devcreate in the composed env (both default OFF and
    in progress) and an owner for row 9's declines; or (b) close p2.transport on a producer-scoped zero (every
    transport producer above at 0 over the census interval, plus the composed A/B, CPPM4 / stress, trace and
    pacing) and move the global [PM4_COV] 0 to acceptance.zero, with new records for the segment-limit fallbacks,
    split draws and row 9's declines. Also to decide: whether `SR_NATIVE_DEVCREATE=1` and
    `SR_NATIVE_SHADER_CONST_RUNS=1` belong in the composed env.
  - **DECISIONS (orchestrator, 2026-10-14, on the gap map):**
    1. **Option (b).** p2.transport closes on the transport-producer zero, not on the global [PM4_COV] 0. Every
       transport producer must read 0 in the composed census over the census-covered interval: VdSwap 82112050's
       block (820F0048's per-present re-emit included, with devcreate ON), the vblank gate 82111EC8, the Z-pass
       kick, the residue's lock / invalidate / idle blocks, the pair, and CALLBACK's declines. Pacing is the
       second owed part. The global [PM4_COV] 0 stays acceptance.zero's: its definition already says 0 emulated
       work across whole runs. The ledger follows: p2.transport's observation and its `composed_zero` unknown
       name the producer set, how the census selects it (inner= VAs and their packet keys,
       `tools/perf/transport_census.py --transport-zero`) and what is excluded and why.
    2. **FLUSH SPLIT BAKED (2026-10-15, fsb1 + fsb2).** SR_NATIVE_FLUSH_SPLIT default ON (native/flush-residue-2, merged
       309c00e4; =0 keeps today's path). Live on the baked exes: replay 9/9, the new-game gate (freeze 15/15, world 15),
       every snapshot pre=0 / calls == taken (row 9's GPU-wait stop modelled), the default-path meter (t0 141.9 -> 35.0,
       t2nop 181.1 -> 52.0 per frame), CPPM4 15/15 with the CP-consumed checks, and the census flush_residue group 0 in
       the interval and from boot (construns' pads witnessed as o6). The frs1 / frs1b failures on the pre-fix exe are
       kept as contradiction split_v1_superseded's witnesses.
       **A new obligation, p1.flush_residue: the draw path's PM4 leftovers.** It covers the S1 flushes'
       segment-limit section fallbacks (constRuns / regRuns / fetchRuns handed back to the guest emitters), the
       count > 0xFFFF draws declined whole, and row 9's Resolve / ClearF declines. Its observation is 0 of these
       packets per present in gameplay. acceptance.zero depends on it. A separate orchestrator lane builds its
       code (native splitting of the fallback sections and of > 0xFFFF draws); this lane keeps the ledger records.
    3. **The composed env** is every transport kind (CALLBACK, GATE, SWAP_BLOCK + SWAP, BRACKET, the three residue
       gates, the pair) plus `SR_NATIVE_DEVCREATE=1`. `SR_NATIVE_SHADER_CONST_RUNS` stays OFF: its packets are
       p1.row3_construns'. GATE and SWAP are both ON; they compose per the IDA read above, and the composed proof
       must show it.
    The build order's "[PM4_COV] 0 (p2.transport closes)" therefore reads "the transport-producer zero + pacing
    (p2.transport closes)"; the global zero follows p1.flush_residue and p1.row3_construns under acceptance.zero.
- **COMPOSED TRANSPORT (transport-composed lane, 2026-10-14; branch native/transport-composed): the offline
  composition, the `transport_composed` plan (pre-registered, dry run only), the pacing instrument, the bake order.**
  The composed env is the decision's: every transport kind + `SR_NATIVE_DEVCREATE=1`, `SR_NATIVE_SHADER_CONST_RUNS=0`.
  - **Offline composition (cloud worker, no game).** Outputs in `archive/tcmp/` of the lane worktree.
    - *The composed present* (`tools/difftest/present_composed_test.cpp`, runner `present_composed_run.py`;
      `present_matrix3/present_matrix.json` valid; the first run, before the rows 5 / 3b decision, is `present_matrix/`). One present is InsertFence 820FDD30, the gate 82111EC8, Swap
      82112050 (its 820F0048 re-emit included), then the close. The fixture is the union of swap_sdk_test's and
      present_gate_test's recording-pinned devices. It runs 8 cases (plain, intervals 0 / 2 / 4 with three fetch
      formats, a gate that kicks first, the immediate and no-front skips, scaler + gamma) at cursor offsets 0 and 4.
      Each case checks: the scoped SDK oracle moved no leaf counter; each producer call was converted exactly once
      (the gate, its callback, the swap block, the Swap body and the re-emit, each to the count the env asks); the
      device, list, ABI and return equal the oracle's (and every command byte where nothing went native); the CP's
      effects equal the oracle's when replayed twice. Both oracle scopes are also checked to decline the gate, its
      callback, the re-emit and the baked rows 5 / 3b; outside any scope rows 5 / 3b must take their calls.
    - The matrix has 15 modes, all valid, 69 assertions each (70 indirect, 0 failed): all OFF; GATE, SWAP_BLOCK +
      SWAP, DEVCREATE and CALLBACK alone; GATE + SWAP; GATE + SWAP + DEVCREATE; the composed env by strong symbols,
      by the production dispatch table, under CPPM4 and under the PM4 output; composed without GATE (CALLBACK takes
      the SDK gate body's callback: 12 of 16 cases, the 4 skips emit none); composed with segmentless off (every leaf
      declines; rows 5 / 3b held off there, their ring-path records not modelled); composed and all OFF with rows 5 /
      3b pinned off (the earlier configuration). In the composed env every present converts the gate, the swap block,
      the Swap body and the re-emit once each, and never the callback: **GATE and SWAP compose, with no double
      application.**
    - Eight compiled faults in kernel/native_flush.cpp each fail their named assertions in their mode and leave their
      control mode valid (all OFF; for the rows 5 / 3b faults, all OFF with rows 5 / 3b off, where they are inert).
      devcreate_oracle (the defect fixed below) fails 34. swap_after_gate (SWAP refusing a device the gate flagged)
      fails 16. gate_double (the gate records, then reports a decline) fails 36. reemit_bypass fails 16.
      gate_oracle_scope and callback_oracle_scope fail 1 each. tilerects_oracle_scope fails 34: the oracle's own Swap
      re-emit goes native in every case, and both scope checks fail. tilepred_oracle_scope fails 2 (the scope checks).
      The source was restored byte for byte and rebuilt, and all OFF, composed and GATE + SWAP were rerun valid.
    - *Every transport suite under the composed env* (`composed_suites_run.py`, `suites_final3/composed_suites.json`):
      every suite that reaches a transport kind ran twice, once in its runner's own env and once with the composed
      env added. Each row keeps its SR_AB_TOGGLE marker, so no suite relaunched bare, and the census rows show the
      env took effect. 26 rows, all valid: 3,753 assertions in the reference rows and 3,756 composed. The residue,
      census and bracket runners' own positive modes rerun valid on the rows 5 / 3b build (10, 6 and 7 modes). The
      suites:
      bracket ±CPPM4; residue ±CPPM4 and with the P2 trace; the owner census; the coherency sentinel; setconst
      render; the gate SDK suite leaf / direct / indirect; present_gate; the Swap SDK suite direct / leaf / indirect;
      the swap block; callback; the pair; the integrated pump driver; recording; the record adapter and its failure
      cases; segment ownership; seg tile; the composed present.
      - The one named exception: the census suite's zpass-kick and pump-driver cases (12 assertions) attribute the
        SDK Z-pass kick's INTERRUPT packets and the pump it leaves. With BRACKET native the kick is a record and
        leaves neither, so their preconditions fail by construction (the suite pins `SR_NATIVE_BRACKET=0` itself).
        The composed row allows exactly those, and `census_no_bracket` (composed, BRACKET at the suite's pin) passes
        83 / 83.
      - The suites that run with segmentless or flush off (the pair, the pump driver, recording) keep that off: the
        segmentless leaves are inert there, and the row records it.
  - **Defects found and fixed (each behind its gate, default OFF).**
    - `devCreateOn()` honoured neither scoped SDK oracle. With `SR_NATIVE_DEVCREATE=1`, the composed oracle's Swap
      ran a native 820F0048 inside a guest-closed segment, and the swap_sdk oracle would have done the same. It now
      declines under PresentGateOracle and ShaderConstRunsOracle. Mutant devcreate_oracle above.
    - `presentGateLeaf` and `callbackLeaf` ignored ShaderConstRunsOracle, whose Entry owns the arena spinlock (`g_mu`
      is not re-entrant). Neither path reaches them today, but both now decline there too, for a uniform oracle
      contract. Mutants gate_oracle_scope and callback_oracle_scope.
    - The baked rows 5 / 3b leaves (820F0208 `emitScissorTileRects`, 8210D400 `setPredicationLeaf`; not transport
      kinds) honoured neither scope. By the orchestrator's decision they now decline inside both, which keeps every
      oracle pure: a scoped oracle Swap's re-emit 820F0048 reaches 820F0208 unconditionally (IDA), and the
      residue suite's SDK BeginZPass under PresentGateOracle reaches both.
      - The decline triggers only inside a scope. A no-scope check calls both leaves outside any scope, and each
        takes its call (tile 1 of 1, pred 1 of 1).
      - PresentGateOracle is entered in production only by the gate's dry CHECK (`SR_ORACLE` + `SR_NATIVE_FLUSH_CHECK`),
        where a leaf going native inside the guest body would make records the CHECK cannot compare. Declining is what
        that CHECK needs.
      - ShaderConstRunsOracle is entered in production where an Entry already holds `g_mu`: `Entry::guest`, the
        gate's segment allocation, Swap's kick, 821075E0's SDK branch. There, opening the leaf's own Entry would spin
        forever on the non-re-entrant lock. So any run that completes today never reached these leaves inside that
        scope, and the default path is unchanged.
      - The present tests' pins of rows 5 / 3b are gone. The route now takes row 5 once per present (Swap's re-emit,
        also through DEVCREATE's native 820F0048), and its records equal the oracle's SDK PM4 at the CP. The pinned
        configuration is kept as two modes.
      - The swap_sdk runner keeps its own pins. Its direct, leaf and indirect cases pass unpinned: the composed
        suite rows run them that way, 125 / 125. But its SWAP-off cases compare the literal bytes of the unscoped
        guest Swap, where row 5 takes the re-emit: 22 failures unpinned (`archive/tcmp/swap_sdk_unpinned/`).
        Superseded 2026-10-14: that was a test defect (the bare suite failed the same 22 since 5dc52749), fixed on
        the transport-fix lane - see "tcmp1 DIAGNOSIS AND FIXES" below.
  - **Lock order and double application (read from the code, consistent with the matrix).** `g_mu` (the arena
    spinlock an Entry holds) is the outermost lock. Taken inside it: the segment and tile locks, the pair's Kf
    spinlock dev+0x2A78, the record-list lock and the guest critical section. Kf is innermost: inside it only the
    KEVENT set lock. No path takes `g_mu` with Kf held, and the CP thread takes only leaf locks, so the graph is
    acyclic. Every producer leaf owns its call whole or declines before any write, and every override runs the SDK
    body only on a decline. CALLBACK sees only the SDK bodies' 820FDA80 calls, so a converted gate or bracket never
    reaches it. The composed census gives the live check (no transport signature key on any other producer), and
    the P2 trace's drained reconciliation (`idle.a74_nonzero_exit` = 0) checks dev+0x2A74 at every BlockUntilIdle
    return.
  - **The pacing instrument (new; observe-only).** P2 trace kind 29 writes one record per present: dev+0x3B14
    vblanks, dev+0x3B20 gate count (82111E38 increments it), dev+0x4E4C throttle wait. `p2_trace.py` summarises a
    window bounded by present IDS: the ids up to the last present the trace saw. That is 2048 by default; the plan's
    pacing rolls use 28,672 (below). Since Astra's round 2 the window's end comes from the kind-13 present records,
    so a pacing stream that stops early leaves its trailing ids missing; it never substitutes an earlier window.
    Ids with no record are counted as missing presents (100,000 missing ids between two blocks are 100,000, not
    one gap). An id recorded twice is counted apart, and deltas are never taken across a missing id. The summary
    keeps the per-present samples. `tools/perf/pacing_cmp.py` grades a control against a target
    (`test_pacing.py`: 28 cases, 17 negative controls).
    - The oracle is P2.7 item 5's "must match between ring and ringless": the composed roll against the all-OFF
      roll, both on the console path (`SR_HIGHFPS=0`; the uncapped port mode releases the gate at once), both
      uninstrumented (no `SR_NATIVE_SEG_TILE`: they measure timing).
    - Pre-registered (revised after Astra's rounds 1-3, still before any roll). Every statistical check has THREE
      outcomes, and uncertainty is never acceptance. The bound is a fixed-seed bootstrap of the difference
      target - control: 1000 resamples of each roll, drawn as multinomial counts over the distinct values, giving
      one-sided 95% bounds.
      - PASS only when the upper bound of the regression is inside the practical margin. A two-sided check needs
        both bounds inside ± the margin.
      - FAIL when the lower bound is beyond the margin (a clear exceedance).
      - Otherwise, when the bounds straddle the margin, INCONCLUSIVE.
    - The practical margins:
      - vblanks: the mean ±0.05; the share above the control's p99 +0.5 points; the hitch RATE (presents at >= 6
        vblanks, ~100 ms at 60 Hz, over the pairs) +0.2 points;
      - gates: the doubled-present proportion +0.5 points;
      - the throttle wait: p50 ±max(15%, 64); p95 / p99 by their exceedance dual, +1 / +0.5 points (the share of
        presents slower than the control's p95 / p99 + max(25%, 64): a quantile of a two-valued distribution flips at
        its mixing fraction, the share does not); the share above 2x the control's p99 + 64 +0.5 points.
    - Severity bounds are fixed, since a single hitch is a regression however rare. The max vblanks per present may
      be at most max(the control's max, 6) + 2, and the max wait at most 4x + 64. The hitch count with its one-event
      slack is gone: Astra round 3 showed it false-failing on unequal windows; the rate check above replaces it.
    - Sanity bounds are fixed too:
      - coverage: >= 1000 pairs, <= 1% missing ids, no duplicate id;
      - gates per present within 0.02 of 1;
      - the vblank distributions' total variation distance <= 0.05.
    - VERDICT. Any FAIL is the verdict: a hard failure stays a FAIL whatever the calibration says, and its item is
      written to the ledger. Otherwise any INCONCLUSIVE check makes the verdict INCONCLUSIVE. Otherwise PASS.
    - CALIBRATION: the plan rolls a second all-OFF control in the same session (pace0, the composed pace1, pace0b),
      and the comparator grades pace0 against pace0b under the same rules. The calibration only gates a PASS: a
      would-be PASS whose calibration is not a PASS becomes INCONCLUSIVE. An INCONCLUSIVE step writes no ledger item
      and never counts toward closing pacing.
    - SAMPLE SIZE. For each statistical check the comparator states the presents a roll needs for that check to PASS
      with 95% probability when nothing changed: n * (2 * 1.645 * se / margin)^2, from the bootstrap's own standard
      error. On the calibration fixtures the doubled-gate proportion at a 2.5% base rate needs the most, about 21,000
      to 24,000. At 2,048 presents most same-distribution pairs are INCONCLUSIVE.
      - So the plan's pacing window is 28,672 present ids (`pacing_cmp.PLAN_WINDOW` = native_proof's `PACE_WINDOW`).
      - The three pacing rolls run the soak tape with a 1,200 s gameplay dwell: 36,000 presents at 30 a second, and
        the window still fits down to 23.9 a second.
      - At that window, independent same-distribution pairs pass 197 of 200 and never FAIL. The fixtures mix
        vblank jitter WITH 6+ vblank hitches, ~2.5% delayed gate ISRs, and a 99% / 1% or lognormal wait.
    - Astra's round-3 examples (50 -> 75 delayed gates, 20 -> 35 slow waits, in 2,048) are INCONCLUSIVE, not PASS. At
      the plan's window the same rates FAIL. Astra's round-2 sampling differences PASS at the plan's window, and the
      hitch rates of 10 in 1,024 against 20 in 2,048 are never a FAIL.
    - The negative controls keep the mean and the median and must FAIL:
      - steady 2 vblanks against alternating 0 / 4;
      - a 2% vblank tail;
      - a 1% long-frame tail;
      - one 100-vblank frame in 2,048;
      - a 200-vblank stall with 99 zero-vblank catch-up frames;
      - a 100x wait spike on 25% of presents;
      - 10x wait spikes on 2%;
      - delayed gate pairs tripled;
      - the same doubled count in half the window;
      - 100,000 missing ids;
      - pacing records stopping 1,500 presents before the last present;
      - a duplicate id;
      - a 100-vblank stall with an unrelated calibration failure (still FAIL, its item written).
    - Mutants, each failing its test:
      - "uncertainty becomes acceptance" (not-FAIL counted as PASS) passes Astra's round-3 examples;
      - a calibration that overrides every verdict erases the stall's FAIL;
      - removing the severity bound passes the two stalls.
    - The Xenia golden (~2.48 vblanks per frame in gameplay) is Xenia's own speed and is reported, never graded.
  - **The plan `transport_composed`** (`tools/perf/native_proof.py`, prefix tcmp; resolves `composed_zero` and
    `pacing`; dry run `archive/tcmp/plan_dry.txt` with `--base`: 21 steps, 45 ledger items). The criteria are the items' checks,
    written as code before any composed roll:
    1. replay vs the base exe (9/9).
    2. probe (composed + `SR_NATIVE_SEG_TILE=1`, P2 trace): the segment counters, the tile and drained
       reconciliation, NEVER-EXECUTED=0, `spans rec>0 pm4=0`, ring0, devcreate's line, every gate call native, the
       swap block and Swap with 0 declines, the bracket with no decline and no literal, the residue strict, the pair
       with decline 0 and abOff 0.
    3. probe_trace: the P2.7 invariants, the close order with the bracket class, BlockUntilIdle drained, and no
       BlockUntilIdle return with dev+0x2A74 non-zero.
    4. abp: the composed frozen-pass per-present A/B. It toggles every kind but SWAP_BLOCK, as the swap_sdk plan
       did (the reserved block in both arms), and checks each kind's line for both arms.
    5. cppm4 and stress: the frozen CPPM4 rolls, every gate / Swap / block call native. stress_off is the same roll
       with every kind OFF, the pre-registered control (a stress failure the control shares is not the composition's).
    6. census0 (every kind OFF) / census1 (composed), both `SR_NATIVE_BRACKETS=1`. The step `census` runs
       `transport_census.py --transport-zero` and requires: the base showing the transport group; the target's
       transport producers at 0 packets (`target.transport.pk`, always written); nothing unattributed or unmapped;
       no transport signature key under another producer; both censuses complete, reconciled, covering the run and
       mapped.
       **native/composed-census (2026-10-15): tcmp2's census1 printed no census at all.** On exe 403bd798be42 (dev
       b6a806c7) census1 reached gameplay with 5,640 CP presents (`[GPU_CP] census ... swaps=5640`) and zero
       `[PM4_COV]` / `[PM4_COVMAP]` / `[NATIVE_BRACKETS]` lines, while census0's 4,944 F lines equalled the CP's
       swap count at every one of its 45 `[GPU_CP]` census lines (the last 4,920 / 4,920). The counter was on (census1's `[NATIVE_CPTIME] slow
       packet ... entry D3DDeviceSwap_sub_82112050`: a classified packet), the tags are in kMilestoneTags. Cause:
       gfx/pm4_coverage ended a frame only at the `XE_SWAP` packet (packet3, op 0x64), and every report hangs off
       the frame end; under `SR_NATIVE_SWAP_BLOCK=1` VdSwap's block is a kRkSwap record that `swapBlockApply`
       presents through `Pm4Engine::applySwap` with no 0x64 packet, so no frame ever ended. rcen1 never ran
       SWAP_BLOCK (census1 = segmentless + the residue gates), and tcmp1 stopped at census0, so this was its first
       composed census. Fix: `pm4cov::presented()` from `applySwap` ends the frame of any present with no packet
       before it (the packet's own present does not end a second; an unsigned XE_SWAP clears it); the counter prints
       `[PM4_COV] armed` at its first hook and `[PM4_COV] ENDS <why> frames= xeSwap= present= presents=` with every
       CUM; the producer lines (internalOrigin included) print with every report even before a first classified
       packet. transport_census.py checks a COVERAGE WITNESS first (the F lines must track every `[GPU_CP]` present
       count; DARK_CENSUS / NO_CENSUS, exit 2, never a zero), the census item requires `coverage.state` live, and
       census0 / census1 carry `composed_census{0,1}_armed` and `_ends` (census0 every frame at an XE_SWAP,
       `present=0`; census1 frames at the records, `present>0`). Offline: the census suite's `swap-frames` scenario
       (both record executors, the CPPM4 rendering, the unsigned XE_SWAP) with three compiled faults in
       native_census_run.py.
    7. meter0 / meter1: pm4 delta <= -88.4 (the rcen1 transport group, 93.0, less 5%; information beside the
       census).
    8. pace0 / pace1 / pace0b: the all-OFF control, the composed roll and a second identical all-OFF control, on
       the console path with the P2 trace, uninstrumented. Each is a long roll: the soak tape with a 1,200 s gameplay
       dwell, its summary over a 28,672-present-id window. Every trace gets the same item: the P2.7 invariants,
       BlockUntilIdle drained, the pacing records present. The forced-drain / terminal reconciliation stays the
       instrumented probe's, since only `SR_NATIVE_SEG_TILE` writes those points (Astra round 1: asking for them
       here could never pass). Then `pacing_cmp --calibration`:
       - FAIL on any hard failure, and its item is written;
       - PASS only when every check passes and the control-vs-control calibration passes;
       - otherwise INCONCLUSIVE: no item, and pacing is not closed.
    9. The world-rule flicker gate on the composed env.
    Since the merge of dev a9546edd, the two comparison steps `census` (tcensus) and `pacing` follow dev's
    comparison-step discipline (devcreate-boot, Astra rounds 4-6), as owner / census do:
    - a RUN-UNIQUE output `<tag>_tcensus|_pacing.<UTC stamp>.json`, created exclusively behind a pending FAIL row;
    - the body guarded, so any exception is the step's FAIL;
    - the step's verdict stamped into the JSON (`runner.verdict`), then `committed: true` written last;
    - the item names that exact file and requires both, and `--recompare` re-runs both steps over stored rolls;
    - an INCONCLUSIVE pacing row writes no item.
    `test_native_proof.py` checks the run-unique name, the stamp and the commit marker; a mutant without the commit
    marker fails it.
    Every compared pair (census, meter, pacing, stress) differs only in the ten transport gates
    (`test_native_proof.py` checks it, and it grades the tcensus and pacing steps on synthetic data: a passing pair
    PASSes, a transport producer left, an unknown producer, a doubled gate or a trace without kind 29 FAILs). One
    defect was found by that test before any roll: the census item first read `target.group_pk.transport`, which is
    absent when the group is zero. That check could never pass; it now reads `target.transport.pk`, and the mutant
    fails the test.
  - **BAKE ORDER (one kind per bake, each with its bake proof, as rows 5 / 3b / 9, segmentless and setconst had;
    the orchestrator's decisions below applied).**
    0. First, `transport_composed` on the pre-bake exe with the env. That closes p2.transport if it passes, and it
       proves the end state before any default changes.
    1. The residue (IDLE_WAIT, INVAL_RANGE, LOCK_COHER): ONE bake for the three gates (decided). It is the least
       coupled, and it was proven as one unit (one A/B toggle, one plan).
    2. BRACKET (the Z-pass kick; no present-path coupling).
    3. CALLBACK. With GATE and BRACKET baked it carries only their declines. Baked before GATE, it backs up GATE's
       declines from GATE's first default day.
    4. GATE.
    5. SWAP_BLOCK + SWAP (one bake: SWAP requires the block).
    6. The pair (`SR_NATIVE_TRANSPORT`) last: it changes the CPU submission (EndSync's chain / park) and is the one
       kind whose live risk is pacing.
    - Not in this order: DEVCREATE. Its bake belongs to boot.device_create, after that record's pooled meter
      re-measure and the boot-delay work (decided). Until it bakes, the composed plan's arms keep pinning
      `SR_NATIVE_DEVCREATE` explicitly, as they pin every kind.
    - Each bake's proof follows the `setconstmem_bake` template on the baked exe with no env: replay vs the previous
      exe; the world-rule gate with only `SR_NATIVE_SEG_TILE=1`; CPPM4 on the default; the kind `=0` against the
      default on one exe (its producers' census keys from base > 0 to default 0, the total not rising).
    - **The composed plan doubles as the LAST bake's proof, not the intermediate ones.** It pins every kind
      explicitly, so on the exe where the pair is baked its composed arm is the default and its all-OFF arms are
      explicit controls. Its census, pacing and A/B then grade the default. On an intermediate exe it never runs
      the default with no env, which is the thing a bake proof grades. The explicit pins also keep it valid across
      the bakes, which is not true of plans whose OFF arms rely on defaults: each bake must pin its kind `=0` in
      those, as the setconst bake did.
  - **DECISIONS (orchestrator, 2026-10-14, on this section).**
    1. The pacing oracle: the composed roll is graded against the all-OFF roll (P2.7 item 5, ring vs ringless
       must match). The Xenia golden is reported only.
    2. SWAP_BLOCK is not toggled in the composed A/B: the swap_sdk precedent, the reserved block active in both arms.
    3. The residue's three gates: one bake.
    4. The DEVCREATE bake is boot.device_create's (after its pooled meter re-measure and the boot-delay work), not
       this lane's.
    5. Rows 5 / 3b decline inside both scoped SDK oracles, as devCreateOn / callbackLeaf / presentGateLeaf do. Done
       above, with tests, mutants and the default path shown unchanged.
    6. The census suite's named exception (its zpass-kick / pump-driver cases need `SR_NATIVE_BRACKET=0`): accepted.
       It is documented in the suite (tools/difftest/native_census_test.inc) and allowed only there in
       `composed_suites_run.py`.
    - Interaction with the p1.flush_residue lane (merged after this branch): `transport_census.py` maps the S1
      scopes 820FBBF8 / 820FC000 / 820FB6E8 to p1.flush_residue (821075E0's keys to construns). A new guest producer
      VA left by that work reads UNMAPPED, and that fails the composed census until the map names it. The meter
      bound is relative to the same exe, so it is unaffected.
  - **BOOT GAPS BUILD NOTE (native/boot-gaps, 2026-10-16; written before any roll; branch off dev 03946a5f, dev
    ddfc6be0 merged in - the display-registers bake and native/zpd - before the live proof).** The
    default build's LAST literal-PM4 producers, from dcf2's census1 (exe 864eb1c17db4, the devcreate-baked default, a
    whole-run census from frame 1; the stage-1 era census s1c1's five rolls of the default build found the same two in
    every boot block and nothing else but a ZPD query, the ZPD lane's): frame 1's pk=2 UNSTAMPED (r05 WAIT_UNTIL + r0F RB_BC_CONTROL) and frame 2's pk=527
    on Swap's line (ops 21:1, 3C:2, 45:256; regs 18:5, 19:263), every later frame 0; gapApplied=2 gapKB=9 whole run; SWAP
    decl=1 (flags2A39=1); the meter's run-cumulative pm4=529 and, on display.registers' meter rolls, dispreg=1 (the
    declined Swap's SDK body calls VdGetSystemCommandBuffer once: contradiction meter_run_swap_decl on native/dreg-bake).
    release/emu-stage1 (f372f963) makes any literal-PM4 gap fatal, so both need a record form first. IDA read-only
    throughout; the asm cited by VA.
    1. **Frame 1's pair = GpuRingEmitModeStateReg 82103238** (dev+0x2A24 = (old & ~0x3000) | (mode & 3) << 12, stored
       before the kick check; then WAIT_UNTIL 0x5C8 = 0x20000 and RB_BC_CONTROL 0xF01 = dev+0x2A24). One code caller,
       D3DDeviceInitDefaultRenderStates 821058B0 (0x821059D4), called by D3DDeviceCreate 82106010 (0x8210616C) between
       the ResetState 8210E520 and the create's flush (0x821061D4) - the residual owner_compare_item already named.
       UNSTAMPED because the D3DDeviceCreate 820F34C0 bracket opens before the device exists (noDevice). **Record form**
       (`SR_NATIVE_MODE_STATE`, default ON, `!e || e[0] != '0'`; =0 the guest body and the A/B arm): the override of
       82103238 (kernel/native_flush.cpp) writes the device field, then one Entry with the two runs
       (Driver::modeStatePackets: the XDK kick check, then run 0x5C8, run 0xF01 = the field re-read); r3 = the cursor,
       r9 / r10 as the asm leaves them (r11 is local to the generated body: non_argument_as_local). Declines to the
       guest body (nothing written) without a command buffer or when the Entry declines; the dry CHECK has kind 10.
    2. **Frame 2's 527 = the first Swap's persisted display.** Byte 0x2A39 bit 0x10 is set only by
       D3DDeviceCreatePresentBuffers 82105B18 and consumed by the first Swap (the SWAP item above declined it - its
       "not taken in this title" was wrong; corrected there). Swap's persist path: at 0x8211236C
       D3DDeviceSetDisplayModeAndGamma 82105DD0(dev, dev+0x3370, 1) - a linear ramp built in its frame (0xCD0, the
       builders 82105278 / 82105458 0x8F0 more), compared with dev+0x3510 and, when different, copied there and emitted
       by GpuRingEmitGammaRamp 821056E0 (table: 2309 dwords, the 256 COND_WRITEs) or ...Wide 821057C0 (PWL: 1413; display
       formats 7 / 54), then VdSetDisplayMode and VdGetCurrentDisplayInformation into dev+0x4DFC..0x4E08; the scaler block
       (GpuScalerEmitCommandBuffer 8210A788, 0x2A3A bit 0x10 - also set by the device create); at 0x8211265C the kick
       check, REG_RMW {0x1E4E, ~1, 0}, VdPersistDisplay(sp+0x98 = 12 zero bytes, sp+0xA8) and, when it returns non-zero,
       MmFreePhysicalMemory(1, *(sp+0xA8)), the bit cleared. The first Swap's modes: 2A38..3B = 00 B4 34 00. **Record
       form** (`SR_NATIVE_DISPLAY`, default ON; =0 the decline as before): swapLeaf admits the persist bit (its admission
       mask 0x52 -> 0x42 when the gate is on; VdPersistDisplay / MmFreePhysicalMemory exports and 0x3000 bytes of stack
       below the SDK frame required) and runs the path in the SDK's order: 82105DD0's guest body with its emitter calls
       taken by the new leaves, the scaler leaf, REG_RMW 0x1E4E as a kRkRegRmw item (the SDK's literal 4 dwords counted
       as fallbackPersist when no Entry takes it), the two kernel calls with the SDK's frame offsets, the bit cleared. No
       SDK-body Swap, so no VdGetSystemCommandBuffer (display.registers' run-cumulative dispreg 0).
       The emitters (kernel/native_display_emit.cpp, each gated by SR_NATIVE_DISPLAY, deciding at the top - the gate,
       record mode, the transport, outside both scoped SDK oracles, a fault-free read, an Entry - and running the guest
       body whole when it declines, counted): 821056E0 / 821057C0 as the reserve at the asm's site (820FC910: the XDK's
       flush / new segment) and one **kRkGammaRamp** record {form, values} (table 257 dwords, PWL 385), applied by the
       record engine's register writes (writeReg 0x1921 / 0x1922 / 0x1927 / 0x1925 or 0x1924: gfx/gamma_ramp.hpp's DC_LUT
       side effects, the presenter's ramp) and rendered back to the SDK's exact dwords for CPPM4; GpuSetGammaRamp 82111CB8
       (reached only under 0x2A3B bit 0x40, never set on this port) as one Entry {kick, REG_RMW 0x1841, run 0x1930}, the
       asm's default ramp in its frame and the emitter leaf; 8210A788 from the asm in its own frame (0x9E0: 8210A660 /
       8210A0F8 guest bodies, RtlFillMemoryUlong, the 220-dword reserve, VdInitializeScalerCommandBuffer with the asm's
       arguments) and, with no program from the platform (this port: 0 dwords), one **kRkScaler** record of the SDK's 80
       bytes (0x1973 / 0x1964 / WAIT_REG_MEM {3, 0x1973, 0, 1, 256} / 0x1852 / 0x1838); a platform program (never here)
       is written as the SDK's bytes - a literal gap, counted (scalerProgram). Each Entry that can switch segments holds
       a ShaderConstRunsOracle across it (the Swap front write's rule): a reentrant allocator's nested emitters stay
       guest bodies instead of waiting on the arena lock (the swap suite's reentry row hung without it). The report line
       `[NATIVE_FLUSH] <why> display ...` counts every leaf, decline, program, reserve failure and CP application, and
       prints the presenter ramp's FNV-1a hash (lut=).
    3. **The settings emitters.** No guest display emitter is reachable after boot: 82105DD0 only from the first Swap;
       82111CB8 only under 0x2A3B bit 0x40, whose one setter is Swap's system-command-buffer branch (sysbuf+8 != 0 -
       never: the host descriptor is Xenia's zero fill); the scaler only under 0x2A3A bit 0x10 (the device create, or a
       Swap rectangle that differs from dev+0x33D0 - the four Swap callers pass 0, 0, 0 and dev+0x33D0 itself). The XDK's
       SetGammaRamp is not linked (dev+0x3510 is written only by 82105DD0); "Brightness" in the strings is audio reverb;
       the retail options are gameplay / audio / controller. The port's DISPLAY OPTIONS (brightness / gamma / upscale)
       are host present-chain stages (gfx/d3d12_present_chain.cpp) calling no guest code: they write no PM4 on any
       build. A census that wants their era runs the faithful path - tools/scenarios/gfxset_settings.json through the
       custom front end's menu (harness keys) - not the SR_GFX_BRIGHTNESS / SR_GFX_GAMMA env shortcut (a dev tool that
       skips the menu).
    4. **Proof offline** (the worker; archive/bgaps in the lane): `difftest --boot-gaps-test` - each of the five against
       its SDK body on fixture devices (the mode for every mode and kick; the ramps for two ramps, fitting and not; set
       gamma in three modes with a ramp and the default; the scaler at two sizes, display information zero and set, no
       program and a 4-dword program), comparing device, r3..r10, list, the display exports argument for argument, the
       registers / waits / presenter ramp the consumer applies from both streams (the native one twice), the leaf's
       counter and no literal gap part - in six gate arms (tools/difftest/boot_gaps_run.py); `--swap-sdk-test` gains five
       persisted-display Swaps (alone, with the scaler, PWL, everything, 82111CB8's wide mode) at two offsets, every
       suite row on the swap suite's own pins (208 / 0; flush / segmentless off 149 / 0; the fence control 176 / 32);
       tools/difftest/boot_gaps_mutation_run.py's 20 compiled defects each failing its row with its control passing
       (18 in the record forms, gate-off controls; 2 in VdGetCurrentDisplayGamma, item 7); every difftest suite's exit
       code as dev's; scripts/audit_forcing.py exit 0 (each leaf tests its own
       default-ON gate first: its guest writes are DEFAULT-ON own-gate sites, as the other bakes'), the five overrides in
       docs/OVERRIDES.json (platform-fix, with evidence). 8210A0F8 never builds its filter table on this platform (it
       divides VdGpuClockInMHz by VdQueryVideoMode's word +0x1C, which kernel/stubs.cpp leaves 0; Xenia answers 0x4A),
       live or in the fixtures: the export receives the frame's stale stack, compared by pointer only. The difftest loads
       no guest image, so the ramp builders 82105278 / 82105458 build a flat transfer curve there (their constants are
       .rdata) and their output does not show their input: 82111CB8's default ramp is compared by the planes each
       builder call is handed (difftest observers of the two builders, forwarded unchanged; mutant gamma_default_ramp).
    5. **Live: BAKED by bgb1** (2026-10-16, native/archive/v1_native/bgb1, exe 97be88395971, every step PASS):
       tools/perf/native_proof.py --plan boot_gaps_bake - replay 9/9; the census pair graded by era_census.py (vendored
       byte-identical from release/emu-stage1 a2aefccf): the =0 arm FAILs in boot alone with dcf2's 529 packets / 2 gaps
       exactly, the default PASSes every era from boot (boot pk 0, gapApplied 0, the presenter LUT equal to the =0
       arm's); the meter pair on display.registers' baked default (the branch merged dev ddfc6be0: SR_NATIVE_VBLANK
       default ON; the plan declares "vblank_default", so no roll is pinned to SR_NATIVE_VBLANK=0, and the meter rolls
       check the default form): run-cumulative dispreg 1 -> 0; the dry CHECK kind 10; the per-present A/B, CPPM4 and the
       flicker gate PASS; the strict witness in every default roll; one exe, a fresh --out, the parent environment
       refused when it carries either gate or SR_NATIVE_VBLANK. Step 3 applied after Astra round 1's P2 fix (eb15681c:
       mode_state_bake's resolvers also take census1's display line and F records and era1's zero): boot_gaps_bake and
       mode_state_bake resolved, p2.transport and boot.device_create baked again (closed), display.registers'
       contradiction meter_run_swap_decl also resolved by the run-cumulative 0 (bgb_meter1_dispreg / _vblank). The
       default build writes no literal PM4 from boot in the new game; the other eras are the stage-1 census's.
    6. **Merge note for release/emu-stage1:** there the opaque handlers take `sr::gfx::CpEngine&` (f372f963) - the four
       apply / render functions of native_display_emit.cpp change type with it, and native_flush.cpp's cpRecordPm4 cases
       for kRkGammaRamp / kRkScaler sit inside its `#if SR_ORACLE` CPPM4 block; the record forms themselves need no walker.
       The C1 composition audit of this branch's release configuration (SR_RELEASE_COMPOSITION=ON) against dev
       ddfc6be0's: one failing unit more, native_display_emit.cpp - class mixed by its docs/release_composition.json
       rule, the emulation part being those Pm4Engine handlers, as native_flush.cpp's (archive/bgaps/comp_lane vs
       comp_base in the lane); its rule follows native_flush.cpp's when the handlers become CpEngine's.
    7. **VdGetCurrentDisplayGamma (kernel/stubs.cpp), the ramp builders' export.** The contract (xboxkrnl; Xenia
       xboxkrnl_video.cc:134-143): VOID, `DWORD* type` (r3) and `float* power` (r4), 4 bytes each - type 2 (BT.709,
       Xenia's default) and power 2.22222233 (read only for type 3). Its two callers, 82105278 / 82105458, pass their
       frame's sp+0x58 (type: lwz, compared with 1 sRGB / 2 BT.709 / 3 power - anything else takes the BT.709 branch in
       this title) and sp+0x50 (power: lfs, type 3 only); nothing reads r3 after the call. The stub wrote a 512-byte
       linear ramp through EACH pointer: sp+0x50..sp+0x257 of the builder's 0x8F0-byte frame - every word there the
       builder reads it writes first, and the type it read back (0x04040505) took the BT.709 branch as 2 does, so no
       output changed, but it was a stack overrun. It now writes exactly the two words; `--boot-gaps-test` checks both
       values and that every other word of a 0x300-byte canary span around them is untouched; mutants
       gamma_stub_overrun (a word past the type) and gamma_stub_type (type 1) fail it, while the swap suite on the same
       build - its persisted-display Swaps run the builders through the export in both arms - cannot see either (their
       control).
  - **TRANSPORT BAKED (2026-10-15, tbk1 + tbk3).** The nine kinds default ON. On ONE exe d92f94aef9cf (tbk3a + tbk3b):
    replay 9/9, gate, probe trace, the A/B / CPPM4 / stress / stress_off with the drained witness and reclaim items, the
    meter 92.96 -> 6.00 PM4 per ingame frame, the census by the review-fixed grader with the graceful quit's terminal report
    (both snapshots complete): the default build's gameplay holds NO PM4 but 820F0048's per-present re-emit (6 a present,
    DEVCREATE's). Pacing (default_pacing) on tbk1's exe b7cca6575e37: vblanks delta 0.0. tbk1's run-time census and tbk2's
    graceful census (its terminal snapshots failed verify on the armed line's order, fixed in 03dd5a5e) are kept as
    superseded evidence; default_bake's resolvers were re-pointed by hand to tbk3's items as tbk3's NOTES pre-registered.
  - **TRANSPORT BAKE PLAN (native/transport-bake, 2026-10-15; written before any bake roll).** p2.transport closed on
    the composed proof tcmp3 (exe 2bfd499ebe58). The bake is ONE flip of nine kinds (the orchestrator's call; it replaces the
    one-kind-per-bake order above, since the composed proof graded them together): CALLBACK, GATE, SWAP_BLOCK + SWAP,
    BRACKET, IDLE_WAIT / INVAL_RANGE / LOCK_COHER and the pair. Every reader is now `!e || e[0] != '0'`: unset = ON, `=0` =
    the guest body (the opt-out, and the A/B arm; SR_AB_TOGGLE unchanged).
    - **DEVCREATE stays OFF** (in this bake; baked default ON by native/devcreate-fix on 2026-10-15 - boot.device_create
      "ROOT CAUSE + BAKE PLAN": the ON-boot wait was the shader cache's load on the attach path, in both arms, not
      devcreate's). As of this plan: boot.device_create is in progress: its meter item fails (dc3 -2.28 against -3), and its
      `boot_order_gap` unknown is OPEN under its own pre-registered rule (dcc4: the backend attached 20-36 s late in 2 of
      3 ON boots). The set that would close it (devcreate_close: BOOT_EQUIV + BOOT_STALL, 42 rolls) has not run.
      render/boot-attach's async load fits that stall being gone (every async roll attached within 0.85 s, tcmp's
      DEVCREATE=1 rolls included), but those rolls are information, not the pre-registered set. So the default path keeps
      820F0048's per-present re-emit under Swap: **6 PM4 a present**. Its a2 == 0 branch writes the four bin packets
      60 / 61 / 62 / 63, then the type-0 WAIT_UNTIL 0x5C8 = 0x20000 (r05) and RB_BC_CONTROL 0xF01 (r0F) (IDA
      0x820F0098-0x820F0110). The gap map's table counted 5.0 for the re-emit and put the r05 under VdSwap's block, but
      the block (kernel/stubs.cpp VdSwap: fetch run, XE_SWAP, type-2 fill) writes no WAIT_UNTIL and neither does Swap's
      own body on its normal path (IDA 82112050). transport_census.py's KEY_SPLIT now moves r05 with the other five (the
      transport group is unchanged). Expected default meter: 93.0 -> about 6.0 per ingame frame, not 0.
    - **What the live set must show** (`native_proof.py --plan transport_bake`, prefix tbk; one baked exe, no exe pin;
      every item into p2.transport; the new unknowns `default_bake` and `default_pacing` name their resolvers):
      - (a) static: a `default_on_*` item per reader (eleven: BRACKET and IDLE_WAIT have two each) and
        `devcreate_not_baked`. The build is what the rolls grade.
      - (b) replay 9/9 against the pre-bake exe: no regression on the Xenia traces. They bypass the producers, so this
        says nothing about the transport itself.
      - (c) the census pair, =0 against the default (SR_NATIVE_BRACKETS=1), graded by the new `transport_census.py
        --pm4-zero --allow-devcreate-reemit`. Both sides must have live coverage (the native/composed-census rule) and
        the base must show the transport group. The default arm must hold NO packet in gameplay: none in any F line from
        the ingame marker to the run's end, and none on any producer line over the interval, UNSTAMPED included. The
        one exception is the re-emit's six keys on 82112050's line, at most one of each a frame. This closes the
        default-path PM4 claim (`default_bake`).
      - (d) the meter, =0 against the default, bound -82.6 per ingame frame (the transport group 93.0, less the 6.0
        re-emit, less 5%). Information beside the census.
      - (e) pixels: the frozen per-present A/B (default against =0; every baked kind but SWAP_BLOCK, the swap_sdk
        precedent), CPPM4 on the default, and STRESS with its =0 control.
      - (f) the traced default probe. Every kind's line is pure ON: no decline, literal 0, the residue strict, the pair's
        decline 0. The P2.7 invariants, the close order with the bracket class and its binding records, and BlockUntilIdle
        drained (`a74_nonzero_exit` 0). The strict tile runs in every pixel roll and in the probe.
      - (g) pacing, default against =0 (`pacing_cmp.py`, the PACE_LONG soaks, pace0b as the calibration). tcmp2's
        composed PASS ran construns OFF; the default runs it ON. This closes `default_pacing`; INCONCLUSIVE writes nothing.
      - (h) the world-rule flicker gate on the default exe, with SR_NATIVE_SEG_TILE=1 in both rolls. Mandatory for the
        merge.
    - **acceptance.zero** (blocked on p2.zpd; it depends on boot.device_create) gets no item from this plan. Its pm4 item
      is a whole-run meter at 0 in every era, which a default with DEVCREATE off cannot meet. This census is its gameplay
      half: only the re-emit left.
    - **Older plans keep the arms they ran.** The per-kind transport plans now pin the other kinds =0 (native_proof
      `genv` / `TRANSPORT_OFF`): present_gate, swap_sdk, transport_pair (its chain / park items need BRACKET off),
      residue, bracket and callback. So do the segmentless plans, whose census bound and A/B predate the bake.
      transport_composed and display_registers already pin every kind. devcreate_close is left as written: both of its
      arms now carry the baked kinds, which is the default DEVCREATE would bake onto. boot.device_create's lane decides,
      before it runs, whether to pin them back to dc3's exact arms.
    - **SR_NATIVE_SEGMENTLESS=0 now runs the pair on the ring path.** That is proven offline only (the pair's own suites
      run with segmentless off). An A/B with segmentless off that is meant to be the pre-bake path pins the nine kinds =0
      (`gs_pre`).
    - **Offline proof (cloud worker, 4401d43f against a dev baseline built from a146f731).** difftest's 85 bare suites:
      the exit codes match except `--present-gate-sdk-test`, 1 -> 0. That suite exits 1 unless the gate reads ON, so 1
      -> 0 is the flip itself (28/0). `--log-async-test` failed once in each tree and passed on rerun, so it is flaky and
      unrelated. native-transport-test exits 1 in both trees: 151/14 bare on the base and 159/6 here, and the 6 are a
      subset of the 14. The eight transport runners (composed_suites, present_composed, native_census, native_residue,
      tracker_admission, swap_sdk, swap_block, present_gate_sdk) are all valid with the same rows. The 105 Python suites
      are identical. The forcing audit reads 0. The release build (SR_RELEASE_COMPOSITION=ON) passes, and
      gen_override_manifest reproduces OVERRIDES.json. One expectation assumed a kind OFF: present_composed_test read
      CALLBACK with `envOn`, and now reads it with `defaultOn`. The plan's dry run gives 21 steps and 59 items. Staged
      exe sha256 B7CCA6575E37D71B7D20C588985A358626F773D329CC06443771B9146B50C7DA.
    - **Astra round 1 fixes (native/transport-bake-r1; made while tbk1 ran on the plan as committed).** Three P2s, all in
      the graders; no game code changed and no rebuild is needed.
      - (1) The census allowance now holds only for 820F0048's whole re-emit: each gameplay frame has none of the six
        keys or exactly one of each (`target_reemit_whole`). It must also be attributed to 82112050 in every report
        segment from the ingame marker to the final report, not only in (A, B] (`target_reemit_attributed`). Allowed
        packets that no producer line attributes make the verdict INCONCLUSIVE (exit 3), and the item grades that as a
        FAIL. This covers the frames after the final report of a killed roll. A pair run with `--graceful-quit` has
        its tail attributed by the terminal report.
      - (2) The A/B, CPPM4, stress and stress_off witness items keep their ids but use the full drained predicate
        (idreorder / drainMiss / forcedMiss), and each of those rolls gets the reclaim item. All eight now resolve
        `default_bake`. The census and meter arms keep no SR_NATIVE_SEG_TILE, as tbk1 ran them and as rcen1 / dmb1
        measured the bound: the forced drained point stalls the producer thread up to 200 ms every 10 s. Pacing stays
        uninstrumented on purpose.
      - (3) "One exe" is now enforced. Every roll, the gate folder and every p2trace summary is stamped with the exe's
        sha256. A comparison step refuses an input from another exe (or with no stamp): it is a FAIL with no item. A
        live run refuses an --out that holds another exe's roll. --reapply and --recompare refuse an --exe other than
        the stored proof's. Rolls from before the stamp count only through the stored proof's own rows.
  - **tcmp1 DIAGNOSIS AND FIXES (transport-fix lane, 2026-10-14; branch native/transport-fix from dev d71946e2;
    every change behind a default-OFF gate or observe-only, no default changed).** The diagnosis of the first
    `transport_composed` run (tcmp1) found four things. The log-lock stall behind census0's hang is another lane's
    (app/log.cpp, untouched here).
    1. **The swap_sdk suite's 22 bare failures were a test defect.** Bisected on the worker: ecc753f6 125/0,
       5dc52749 103/22. 5dc52749 made row 5 (820F0208, default ON) decline inside the scoped oracles. With SWAP off,
       the test's oracle ran the SDK Swap inside those scopes (literal tile rects) while the route ran it unscoped
       (row 5's record), so the literal-bytes check failed in all 22 cases. Production was not affected. The runner
       pinned `SR_NATIVE_TILERECTS=0`, and no lane ran the bare suite list.
       - Fix (tools/difftest/swap_sdk_test.cpp): SWAP on, the oracle stays scoped; SWAP off, the oracle is the
         unscoped SDK body, the production route itself. The `leaf` mode's fallback is unscoped, as the production
         override's is. A mismatch now prints the first differing list / buffer word.
       - The runner's pins are gone except one: `segmentless_off` keeps `SR_NATIVE_TILERECTS=0`. With
         SEGMENTLESS=0 row 5's record is a marker-path span. Its 16-byte NOP marker carries the record's arena
         position (the marker's third word), which advances 0x28 per record, so two sequential executions cannot
         write equal bytes. The sweep: unpinned and 3b-pinned 103/22, first difference at 90642038 (oracle 00000000 /
         actual 00000028); row 5 pinned 125/0.
       - `composed_suites_run.py` gains `swap_sdk_bare`: the suite with no SR_ variable, a reference-only row. It
         fails at d71946e2 (no fix; 5dc52749 is in its history) and passes with the fix.
    2. **The composed SWAP and BRACKET memory declines: `gateWritable` refused pages the GPU write tracker owns.**
       tcmp1's swapSdk declines (probe 25, cppm4 9, abp 4, stress 22 in gameplay, plus the one boot decline SWAP
       alone shows too) were whole-call admission declines (`entryDecl=0`). The leading candidate was the reserved
       block's writability: the command buffer sits in the tracker's 0x8 / 0xA windows, which the tracker makes
       PAGE_READONLY after validation and releases on the first CPU store. A segmentless span writes no guest bytes,
       so with GATE native no store released the page before Swap's admission. BRACKET's `memory` declines (probe 1,
       stress 34; BRACKET alone under stress 10) were by elimination the recording lists' prefix / cursor check, the
       same family. CALLBACK's declines equalled them: a declined end runs the SDK body.
       - Counters (kernel/native_flush.hpp `SwapDeclWhy` / `BracketMemorySite`): every SWAP admission refusal is
         counted by its reason, in admission order, on a new `[NATIVE_FLUSH] <why> swapSdkWhy` line. The two post-swap
         literal fallbacks are counted there too; those calls still count as taken. Every BRACKET_END memory decline
         is counted by its site (frame, device, stack, the list read, each list's alignment / prefix / cursor, the
         Entry) on `[NATIVE_FLUSH] <why> bracketMemory`. The existing swapSdk and bracket lines are unchanged, since
         their patterns are graded as written.
       - Samples: each decline writes a capped line (per reason: the first 8, then every power of two).
         `[NATIVE_FLUSH] SWAP_DECL #k why=...` gives the cursor and its reserved block page by page (VirtualQuery
         protection, `/own` where the tracker holds the page), `writable=` (the plain query), `tracker=`
         (gww::cpuWritableRange) and the device's mode fields. `[NATIVE_FLUSH] BRACKET_MEMORY #k site=... list=...`
         gives the same for the refused region.
       - The fix: `gateWritable` = `writableRange || gww::cpuWritableRange`, the rule native_pump, native_recording
         and native_transport already use (12343e71). A tracker-owned page is as writable as the SDK body finds it: the
         first store faults into the tracker's VEH, which releases the block. Its callers are all default-OFF
         (SR_NATIVE_SWAP, SR_NATIVE_BRACKET, SR_NATIVE_REPLAY_CALLBACK).
       - Fixtures on the real tracker (both physical copies protected and validated, the guest arena registered as
         native_transport_test does): `--swap-sdk-test tracker` (26 assertions: the reserved block on tracked pages at
         offsets 0 / 4 / 0xF00, the last crossing a page; admitted, equal to the scoped SDK oracle; every page a store
         changed released by the handler, not a host-write scope; a block mixing an owned page with an unowned
         read-only one declines as cursorWrite before any store). `--native-bracket-test` case `tracked-lists` (the
         three lists' prefix / cursor pages tracker-owned before the end: published natively, no memory decline,
         released in both arms; a decline counts once, at a list site). The admission fixtures name their reason.
       - `tools/difftest/tracker_admission_run.py` (worker) runs those positives and four compiled faults:
         gateWritable without the tracker (the six swap admits and the tracked-lists end fail; the samples read
         `why=cursorWrite ... writable=0 tracker=1` and `site=prefix|cursor ... writable=0 tracker=1`), a reason
         counted under another, the list sites counted elsewhere, and the binding record split into one-id runs (item
         4). Each fails exactly its named assertions with controls whole, followed by byte restoration and a rebuild.
    3. **segment_order's `skipped` 11 was the trace's end, not a skip.** The harness killed the game while the CP was
       inside the last BRACKET_END: its first bound segment applied, the other eleven never recorded. The grader could
       not tell bound from skipped.
       - The binding record: a BRACKET_END about to be published traces its bound closes as kind 24 aux 5
         {f = first id, a = count, b = generation}, one record per contiguous id run (kernel/p2_trace.hpp; only with
         SR_P2_TRACE). The bracket suite checks it names exactly the bound closes.
       - `p2_trace.py` segment_order reports `bracket_binds` and a separate `tail_bound_unapplied`. That applies only
         when the trace's last CP item is an aux-4 apply of an id x, a binding record names x, the applied ids of x's
         generation are a prefix of its bound set, and the id is not taken by any aux-3 record. Everything else stays
         `skipped`, fail-closed (no binding record, the CP continued, a hole, an earlier bracket, an ordinary gap).
         `tools/perf/test_segment_order.py` covers both directions.
       - The graceful-quit probe (another lane's harness feature) removes the cut altogether.
    4. **The follow-up live plan `transport_fix`** (tools/perf/native_proof.py, prefix tfix; resolves nothing on its
       own): replay; the composed probe (+ SEG_TILE, P2 trace) and the stress roll, both with the new items. Pre-
       registered: no SWAP memory decline (`swapSdkWhy` devRead .. stack, cursorAlign, cursorWrite, entry and both
       fallbacks 0), at most the boot decline (`decl=0|1`), BRACKET `bracketMemory` 0 at every site; the probe trace
       with `bracket_binds > 0` and skipped 0. The probe ended through graceful quit is an `owed` step: recorded SKIP,
       no item, until that lane merges. Dry run: 5 steps, 14 items (`archive/tfix/plan_dry.txt`).
    5. **Offline results (cloud worker, the committed tree; `archive/tfix/` in the lane).**
       - The full default build (SupermanReturns.exe and difftest) exits 0.
       - Suites against d71946e2, run bare on the worker: 82 difftest suites with equal exit codes except
         `--swap-sdk-test` (1 -> 0). The 18 other nonzero suites are nonzero on both sides (the worker's environment).
         The Python tests are equal, plus the new `test_segment_order.py` (0). `test_native_proof.py`'s one failure,
         "ledger paths start at _worktrees/", is the worker's path on both sides.
       - The bare row: d71946e2 103/22 (FAIL, no fix); the branch 125/0.
       - `composed_suites_run.py`: 28 rows valid (swap_sdk_tracker 151 / 151, swap_sdk_bare 125).
         `present_composed_run.py`: 15 modes valid and 8 faults detected, restored.
       - `tracker_admission_run.py`: 6 positives valid and 4 faults detected, restored.
       - `swap_sdk_test_run.py`: 13 cases valid. `swap_sdk_mutation_run.py` on that matrix: 5 faults detected, gate-off
         controls whole, restored matrix valid.
       - `test_segment_order.py`: 13 / 13. Four grader faults (late-taken ids counted, no prefix rule, no last-apply
         rule, the tail not subtracted) each fail their named tests.
       - Not done in this pass (the coordinator's priority change: a lean proof first): the release build and audit,
         and a 5dc52749 build of the bare row (d71946e2 contains 5dc52749 and shows the same 22).
    6. **Astra round 1 (MERGE AFTER FIXES: two P2 grading defects, one P3; no production memory-coherency defect) and
       the merge of dev 95e221f5 (graceful quit).** Items 3 and 4 above are as first built; this supersedes their rule
       and their owed step.
       - P2, a skipped suffix excused as tail: the last aux-4 record was taken as proof the bracket was still in flight,
         so a bracket that went on (its next bin select) or completed after omitting applications read as cut.
         Now the trace carries the bracket's lifecycle and plan:
         - the CP traces kind 24 aux 6 when a BRACKET_END's replay begins and aux 7 once every step ran {f = segment
           id, a = steps, b = generation} (cpSegment);
         - the producer traces the replay plan at the publish, aux 8 a bin select / aux 9 an application {f = step
           index, a = value / id, b = generation} (traceBracketBinding);
         - the CP's own steps were already traced (kind 3 aux 1 bin selects with the generation, aux 4 applications).
         `tail_bound_unapplied` now needs the last begun generation without a completion, nothing but its steps after
         the begin, and those exactly its plan's first steps. Anything else stays `skipped`, as does a trace without
         these records (8648af78's exe).
         - New whole-trace fields for a drained run: `brackets_open`, `bound_unapplied`, `published_untaken` (-1 when the
           trace lacks the records).
         - Tests: Astra's three counterexamples, the old exe's records, a plan not whole, another generation's step,
           the terminal fields.
         - Faults: `tracker_admission_run.py` adds lifecycle_end and plan_kind (the bracket suite checks the
           lifecycle and plan records against the segment). Six grader faults (`archive/tfix/w_py_faults.py`, run on the
           worker): the tail rule without its prefix, completion or other-work check, the open count zeroed, the
           8648af78 wildcard pattern back in the swapSdkWhy item, and the graceful items not keyed on the step's gate.
       - P2, "no memory decline" accepted memory failures: the item's `.*` hid `rect` (a failed rectangle read), and
         hsio / movieGlobal / root mixed failed reads with refused states. The reasons are split (hsioRead / hsio,
         movieGlobalRead / movieGlobal, rootRead / root), and the item names every reason with no wildcard:
         - the read / writability / admission reasons must be 0;
         - the refused modes and states are free;
         - on an exe before the split, the combined hsio / movieGlobal / root must be 0 (it cannot tell a read
           failure from a state).
         The swap fixture's root case now points the root at VA 0 (rootRead). Faults: root_read (compiled) and the
         wildcard pattern (a grader fault).
       - P3, the two post-swap fallbacks counted without their sample: they go through swapDecline (total and reason
         once, the capped SWAP_DECL sample), the call still taken. Tracker mode adds a case where VdSwap's export sets
         the bit every Entry declines on (dev+0x2A38 0x02): both fallbacks counted once and sampled
         (`swapSdkDeclSampled`). Fault: fallback_sample.
       - The graceful probe is real. `probe_graceful` is the probe's own roll with `SR_DRIVE_GRACEFUL_QUIT=1` in its
         gates (sr_drive reads it from -Gates).
         - It gets the graceful quit's items without the plan-wide `--graceful-quit`: `<prefix>.probe_graceful.terminal`,
           `.quit_receipt`, and every text item's terminal twin.
         - Its trace (`tfix_probe_graceful_trace`) keeps the probe trace's checks and adds the terminal segment_order:
           tail_bound_unapplied 0, brackets_open 0, bound_unapplied 0, published_untaken 0.
         - No game-side change: the graceful quit is dev's. The exe for it is this lane's C++ (the lifecycle / plan
           records and the reason split), so it needs a new build: tfx2.
       - tfx1 (exe 74744198a07d = 8648af78; the coordinator's live run, `_worktrees/native/archive/v1_native/tfx1`,
         items applied to this branch's ledger) predates these records.
         - replay 9/9; probe: swapSdk calls=19783 decl=1, swapSdkWhy flags2A39=1 only (the boot decline; tcmp1
           decl=26), bracket memory=0, bracketMemory 0 at every site; stress: freeze 15/15, the same counters.
         - Its lines read hsio / movieGlobal / root / rect 0, so they also pass the corrected swapSdkWhy item. Its
           probe trace was not cut inside a bracket (skipped 0, tail 0).
         - The probe trace was summarised by this lane's in-progress `p2_trace.py` (rule "no CP lifecycle / plan
           records"): tfx1 ran from this worktree.
       - **Results (cloud worker, the committed code; `archive/tfix/a1`, `a2`).**
         - The full default build (SupermanReturns.exe, difftest) exits 0.
         - Suites against dev 95e221f5, run bare: 82 difftest suites with equal exit codes except `--swap-sdk-test`
           (1 -> 0, the item-1 fix; dev still fails it). The 18 other nonzero suites are nonzero on both sides. The
           Python tests are equal plus `test_segment_order.py` (23 / 23); `test_native_proof.py`'s one failure (the
           worker's path) is on both sides.
         - `composed_suites_run.py`: 28 rows valid (swap_sdk_tracker 153, swap_sdk_bare 125).
           `present_composed_run.py`: 15 modes valid, 8 faults detected, restored.
         - `swap_sdk_test_run.py`: 13 cases valid. `swap_sdk_mutation_run.py`: 5 faults detected, gate-off controls
           whole, the restored matrix valid.
         - `tracker_admission_run.py`: 6 positives (swap tracker 153, bracket p2trace 346) and 8 faults detected,
           restored.
         - The six grader faults are detected, restored.
         - The plan's dry run: 6 steps, 33 items (`archive/tfix/plan_dry_r1.txt`).
    7. **tfx2 (exe 32cdac2c6fcf = 91d02caa): the graceful quit's ring cut against the composed BRACKET path.**
       - Results: replay 9/9, the probe and its trace under the new tail rule, stress 15/15 - all PASS, the counters as
         in tfx1. probe_graceful crashed in its quit: acknowledged at 8 ms, the process gone at 8.06 s with exit
         0xC0000409. The run.log ends 2 ms after `[QUIT] stage ring-cut` with `[NATIVE_FLUSH_BAD] bracket: coherency block
         published without its hand-off`. The trace's published_untaken was 12: the CP never finished its drain.
       - Cause: once the ring is cut (kernel/quit.hpp `g_ringCut`, set under the CP's publish mutex), the 820FD218
         override (kernel/gpu_ringless.cpp updateWritePointer) queues nothing. It calls `doorbellCut()`: a lock holder
         or exempt thread has its publish dropped and runs on; any other thread parks. BRACKET_END publishes from inside
         820FD800, which holds the pair's Kf spinlock. Its thread was not parked (the BAD line came 2 ms later), so its
         coherency block was dropped and the hand-off was never taken.
         bracketPublish then returned route 2, and bracketEnd called bracketFatal, whose `std::abort()` is the fail-fast
         (the MSVC CRT's abort -> __fastfail -> 0xC0000409).
         - The same drop hit the other two hand-off sites, more quietly: an ordinary close's publishSegment counted a
           mismatch and logged "publish not taken" (that would fail the terminal twin `mismatch=0`), and the replay kick
           callback counted a literal route.
       - Fix: a hand-off not taken while the ring is cut is an orderly stop. The guest's own publishes are dropped the
         same way, and the CP drains up to the final write index.
         - At all three sites (publishSegment, the kick callback, bracketEnd's coherency block and END): the parts are
           erased, counted apart on a new `[NATIVE_FLUSH] <why> quitCut segs= kicks= coherency= ends=` line, never a
           mismatch, a literal route or a fatal.
         - A dropped END retires its generation (bound closes off the deferred registry) and is traced as P2 kind 24
           aux 10 {END segment id, 0, generation}.
         - `p2_trace.py`'s bound_unapplied leaves the cut generations out, and reports them as `brackets_cut`.
         - Without the cut, route 2 is still bracketFatal.
       - Test: the bracket suite's `ring-cut` run (native arm, last, gate on) sets `g_ringCut` before the direct route's
         last close, which is bound to the bracket, and keeps it set through an ordinary close after the bracket.
         - The difftest publish observers model the override: no hand-off after the cut.
         - It checks: the end returns as the SDK body (r3 0, no failure branch, no fatal); the coherency block, the END
           and the later close each counted once on quitCut; no END published, no mismatch, no literal route; the
           generation retired; under SR_P2_TRACE, the cut record naming the bound generation.
         - Faults in `tracker_admission_run.py`: cut_close (the drop counted as a mismatch: named failures), and
           cut_coherency / cut_end (route 2 back to bracketFatal). The last two must crash - exit not 0 / 1, no summary,
           the BAD line in stderr - which is tfx2's failure, reproduced offline.
         - `test_segment_order.py` adds the cut bracket (brackets_cut 1, bound_unapplied 0; without the record, 6);
           grader fault terminal_cut.
       - Results (cloud worker, the committed code; `archive/tfix/c4`):
         - The full default build exits 0.
         - The bracket suite: native 314/0, SR_P2_TRACE 351/0. The ring-cut run shows quitCut +1 / +1 / +1 (close,
           coherency, END), END +0, mismatch +0.
         - `tracker_admission_run.py`: 6 positives and 11 faults detected. cut_coherency and cut_end exit 0xC0000409
           with the BAD line, tfx2's exit code.
         - Grader faults 7 / 7. `test_segment_order.py` 24 / 24.
         - `swap_sdk_test_run.py` 13 cases valid. `composed_suites_run.py` 28 rows valid. `present_composed_run.py`
           15 modes valid, 8 faults detected.
         - Suites against dev 95e221f5: equal exit codes except `--swap-sdk-test` (1 -> 0); the Python tests equal plus
           `test_segment_order.py`.
       - Known residue: a producer that passes the override's relaxed cut check just before the cut can push its item and
         then find the doorbell cut. The item stays queued (the quit's `rl_pending`), and its hand-off counts as one
         published_untaken. That window is microseconds per publish; the graceful trace item does not excuse it.
- **TRANSPORT PAIR DIAGNOSIS (transport-pair lane, 2026-10-14; branch native/transport-pair; `SR_NATIVE_TRANSPORT`,
  default OFF, unchanged).**
  - **What the pair is.** The CPU half of the SDK's software submission list, not a PM4 kind. 820FD800
    `GpuRingAppendJumpSegment` (with the inline 820FCA80) publishes a closed segment straight to the ring through
    820FD218 when `dev+0x2A74` (outstanding pump work) is 0, or chains it as a 16-byte node onto the software list
    for the pump to publish. 82110768 `GpuCommandSegmentEndSync`, the pump's end-of-list step, follows a link
    written meanwhile, or parks: nesting -1, at 0 it drains `dev+0x2A74` by one and sets the completion event, then
    records the park position and waits. Both run under the shared Kf spinlock `dev+0x2A78`. The native pair
    (kernel/native_transport.cpp) does the same work with the same imports. It emits no PM4 of its own, so it has
    no meter item. It replaces the EndSync that drains `dev+0x2A74`, so the BlockUntilIdle drain rule (item 6)
    applies to it. In this title chain and park happen only while an SDK Z-pass kick leaves pump work outstanding.
    Increment 1 comes only from that kick, by three routes: the guest 82110C78 itself; `SR_NATIVE_REPLAY_CALLBACK`'s
    append, which forwards the same kick call; and BRACKET's literal SDK kick route (kernel/native_flush.cpp, the
    kick step's late-PIX / no-GPU branch, counted `literal=`). The guest closes (820FD8C0 / 820FD660), the native
    segmentless close and a fully native BRACKET_END all pass 0.
  - **What each failing item shows.** Read from the run logs under
    `_worktrees/dev/archive/v1_release_codex_20261012/native_transport_v1*`. From V14 on, no counter stays at zero
    in any roll. Only `decline=0` fails, and at V13 the line was missing.
    - V13 (exe 5213a4f1c662), `transport_pair_{probe,cppm4}_activity_v13`: the probe and CPPM4 logs have **0**
      `[NATIVE_TRANSPORT]` lines. The player log profile dropped the tag. The instrument was fixed in V14, when the
      tag joined `app/log.cpp` kMilestoneTags (it is on dev).
    - V14 (04da82028b00): every route was taken but declines were nonzero. Probe: append 516647, chain 11521,
      direct 505126, park 29615, resume 663, decline 842. CPPM4: 526742 / 36016 / 490726 / 27685 / 444, decline
      761. V15 (3ee20858b161; readable EndSync markers): decline 465 = appendInput 347 + tailInput 118, with owner,
      endState and endMarker all 0. V16 (1487f73837ad; reject samples): decline 503 = 384 + 119. All 16 samples
      (8 `node`, 8 `tail-memory`) are committed pages in the 0xA physical window with Protect 2 (PAGE_READONLY).
      That is the GPU write tracker's validation protection (`SR_SHM_WRITE_TRACK`, default ON). A guest store
      faults through the tracker's VEH, which releases the page, but the native admission refused it. These
      declines were an **admission property (a design gap), not a scenario property.** Tying all of them to the
      tracker gap is an **inference**. V14 has no reason counters. V15 has only the input / tail categories. V16's
      permission snapshots were taken after the rejection, so they do not prove ownership at that moment. 12343e71
      (V10 host bank, 169/0) admits tracker-owned read-only pages, and V17 (7257e324f3f5) passes all 7 probe items:
      append 254618, chain 186004, direct 68614, park 14399, resume 5983, decline 0, every reason 0 in all 124
      samples. That supports the attribution without proving every historical decline. Nothing in the pair's
      sources has changed on dev since.
    - `transport_pair_cppm4_v13` and `_v14` read "WORLD NOT DRAWN: 0 graded freezes" today. They graded **PASS**
      when run (V13: 15 counted, world 12; V14: world 15; `proof_progress.txt`). The J: scratch GC deleted the raw
      frames on 2026-10-14 at 00:45 and 00:50 (`frames_deleted.txt`). This is the `evidence_deleted` class, not a
      rendering result. Both attempts carry a recovered `frames_measured.json`
      (tools/perf/recover_frame_measurements.py, on tools/frame-evidence), which dev's ledger tool does not read
      yet. That is the frame-evidence lane's to land.
  - **No bound was wrong for this composition.** Chain, direct and park are all taken in this gameplay scenario with
    BRACKET OFF (park about 1 per present, chain at least 11521, from V14 to V17), so `chain/direct/park=[1-9]` and
    `decline=0` stand. **Under fully native BRACKET execution (`SR_NATIVE_BRACKET=1`, every end native) chain and
    park are unreachable by design.** A native BRACKET_END never moves `dev+0x2A74` (BRACKET build note), so every
    close takes the direct route and the pump never wakes. Under BRACKET as it actually runs, pump work can still
    come from two places. One is a declined end (`beginDecl` / pending and the other reasons), which runs the SDK
    kick. The other is the literal SDK kick route inside a native end (`literal=`, increment 1). So chain and park
    there are rare and depend on the declines, not zero. The pair's route proof therefore stays on a BRACKET-OFF
    base, as the plan is. A later BRACKET-composed roll must pre-register its own item: `decline=0` and
    `abOffAppend=0 abOffEnd=0`, with chain and park only as information, read against the bracket line's declined
    and literal counts. It must not reuse `chain/park=[1-9]`.
  - **What blocked an honest live pass, now changed (default OFF unchanged).**
    1. *There was no per-present A/B.* `gateOn()` is a startup latch, so `SR_AB_TOGGLE=SR_NATIVE_TRANSPORT` would
       have been ignored and an "A/B" roll would have graded a pure-ON roll: a vacuous draw-path guard. Both entries
       now read the A/B window once per call, before any read, import or write (`abWindowOn`). An OFF window runs
       the SDK body whole and is counted as `abOffAppend` / `abOffEnd` on the census line. These counts are not
       declines. Each call is wholly native or wholly the SDK body, and both leave the same guest-visible state and
       import sequence. So a mask flip inside VdSwap, mid-emission, is harmless here, unlike segmentless's (that
       mode-per-call toggle dropped records).
    2. *The plan.* `native_proof.py --plan transport_pair` gains the frozen per-present A/B
       (`transport_pair_abp`, `SR_AB_TOGGLE=SR_NATIVE_TRANSPORT,SR_AB_PRESENT=1`, freeze, `ab_present_min` 1000),
       with seg, tile and xport witnesses. Its activity item requires both arms (`abOffAppend` / `abOffEnd` > 0) and
       the native arm's chain, direct and park with decline 0. The probe now carries the P2 trace, with step
       `probe_trace` and item `transport_pair_probe_trace`. That item is `trace_order_item` plus
       `idle.a74_nonzero_exit == 0`: every BlockUntilIdle completes, none spins over a frame, and each exits with
       `dev+0x2A74` drained, which is the item-6 rule for a pump replacement. The probe's tile is the drained
       reconciliation (`tile_drained_item`), and the probe adds xport. The pure-ON activity items also require
       `abOffAppend=0 abOffEnd=0`, so every gated call was native: a tightening. Every item id starts
       `transport_pair_`. test_native_proof.py: 0 failed (it was 1, the missing draw-path guard). Dry-run only.
  - **Offline proof (cloud worker, commit e9ca93e3 = built snapshot eb19fa668d54; `_worktrees/transport-pair/
    archive/tpair/`).** `--native-transport-test ab` drives the real kernel/ab_toggle per present. In each arm,
    append (count 0/1/3, increment 0/1) and EndSync (link and terminal) are dispatched through production and equal
    the actual SDK. The census counters show which arm ran. In an OFF window the native entries fall back before any
    import or write. An 11-call software list (direct, chain, follow, park at nesting 2->1, a follow after a park,
    park with `dev+0x2A74` drained, direct, chain), with calls in alternating presents at both phases, equals the
    all-SDK reference: device, list, ring, import sequence and every full-width return.
    - `matrix/native_transport_matrix.json` is valid. Native 170/0, gate-off 170/0 and ab 47/0. Thirteen compiled
      defects each fail only their named checks and pass their gate-off control: the ten V10 defects plus three new
      A/B ones. `ab_append_window` fails 9 checks and `ab_endsync_window` 5; in each, an entry ignores its window,
      and only the arm counters and the entry fallback see it, because both bodies agree. `ab_off_skips` fails 14
      checks: an OFF window skips the call, and the SDK differential and the mixed list see it. Sources are restored
      exactly, and the restored build gives 170/0, 170/0 and 47/0.
    - Suites, against a build of dev c86bf251 on the same worker. Exit codes are equal in all 77 runs: the 74 bare
      (the changed `--native-transport-test` apart: its same 9 gate-off bare failures, plus the new witness, 161/9),
      the bracket and coher-sentinel suites bare, and the bracket suite composed with SR_NATIVE_TRANSPORT. Every
      pass/fail summary is equal except `--timebase-test`'s K3 wall-clock check, which flips run to run on both
      builds (`timebase_rerun`: 27/3, 27/3, 28/2 each). The bracket suite's seven configured modes (native 290/0)
      and the coher-sentinel suite's three are identical (`configured_compare.json`). The forcing audit and the
      override manifest are clean.
    - **Re-run after merging dev 53ee0128** (transport-residue, receipts fixes; commit 5ba7b050 = worker snapshot
      59e51bdb8a72), which is what the ledger items now cite:
      - `matrix2/` is valid with the same numbers: 170/0, 170/0, 47/0, and 13 of 13 defects detected and restored.
      - Suites against a build of dev 53ee0128 on the same worker: 78 runs, where the bare set now includes
        `--native-residue-test`. Exit codes and every summary are identical (`suites_compare2.json`; timebase
        agreed this time).
      - The residue suite's ten configured modes (SR_NATIVE_TRANSPORT composed 597/0), the bracket suite's seven
        and coher-sentinel's three are identical (`configured_compare2.json`, 20 runs).
      - test_native_proof.py is 0 failed, the forcing audit is clean and the manifest is unchanged.

      Ledger items `transport_pair_unit_{native,gate_off,ab,matrix}` and `transport_pair_suites{,_configured}`
      pass.
  - **Owed live (not run; the game slot was not used):** `native_proof.py --plan transport_pair` on this branch's
    exe: replay, traced probe plus trace, abp, CPPM4 plus its `SR_NATIVE_TRANSPORT=0` control, stress, gate.
    *What it should show if the pair is right:*
    - Replay is 9/9. It bypasses the producer, so it says nothing about the pair.
    - The probe matches V17: chain, direct and park > 0, decline 0, abOff 0, and the census, seg, drained tile,
      xport and swap items pass.
    - The trace has idle entries > 0, none open at the end, no spin over a frame, `a74_nonzero_exit` 0 and the
      segment order intact.
    - abp: at least 8 world freezes, exact between alternating presents, with both arms counted.
    - CPPM4 and stress are exact.
    - The gate passes.

    A pixel failure in CPPM4 that `cppm4_off` shares belongs to the composition (the swap SDK), not to the pair. A
    failure only in abp or CPPM4 with the pair on is the pair's, and it must be read before anything else.
  - **Composition (decided, orchestrator 2026-10-14).** The plan keeps the SDK swap base (`SR_NATIVE_SWAP=1`,
    `SR_NATIVE_SWAP_BLOCK=1`), comparable with V13-V17. That composition no longer carries open freeze failures: the
    swap_sdk plan's re-run on the merged dev exe passed every step (`_worktrees/native/archive/v1_native/sw1`:
    A/B, CPPM4, SDK-off control and stress all world 15, gate PASS), and the v1 FAILs are superseded
    (`swap_v1_superseded`).
  - **Open before any default flip (orchestrator, 2026-10-14): the admission cost.** Throughput is unattributed.
    V17's probe ran 63-84 presents/s (p95 28-32 ms), V13/V14's 125-150 (p95 7-9 ms) and V15/V16's 104-127. V17 ran as
    instance native-a, the others as native, with no matched scene.
    - The offline bound (`--native-transport-test bench`; worker, one uncontended thread, real imports, n = 200000;
      `archive/tpair/smoke1`): a chained append costs 104 ns in the SDK and 2617 ns native, and an EndSync follow
      83 ns and 13896 ns.
    - One admission query costs about 0.5 us (QueryWorkingSetEx 487 ns, the tracker query 470 ns). A chained append
      makes five and a direct one four. A follow makes one QueryWorkingSetEx plus `readableMarker`'s VirtualQuery,
      so by subtraction the VirtualQuery takes about 13 us.
    - At V17's call rates (about 1150 appends/s and 90 EndSyncs/s) that is about 4-5 ms per wall second, under 1%
      of one thread. **The pair cannot by itself explain V17's drop** unless live contention (the working-set lock
      against the tracker's faults) multiplies it about 100x.
    - Still, the native pair costs 25-170x the SDK body per call, all of it in the fresh OS permission queries.

    Owed before the pair can be default ON: a matched ON/OFF throughput comparison, and a cheaper admission (for
    example `readableMarker` through QueryWorkingSetEx first, as `writableRange` does) with its own differential
    and defects. That changes the reviewed admission and is not done here. (Superseded by tpl1 and
    'TRANSPORT PAIR ADMISSION A' below: the admission is now arithmetic. The matched ON/OFF throughput comparison
    is still owed.)
  - **TRANSPORT PAIR LIVE RUN tpl1 (tpair-fix lane, 2026-10-14; exe 328f9c1b4d64; `_worktrees/native/archive/
    v1_native/tpl1`).** Replay, A/B (world 15), CPPM4 and its OFF control (world 13 each) and the gate passed. The
    probe's drain check and the stress roll failed. Analysis: `tools/perf/p2_idle_spins.py` on the probe's P2 trace
    against the residue probe res1 (the same guest pump without the pair).
    - **The late drain is the pair's own admission stalling, not the protocol.**
      - BlockUntilIdle's post-fence spin: mean 3.9 ms, 436 of 1367 entries over 2 ms, max 19.3 ms, against res1's
        0.014 ms (none over 1 ms).
      - In every long spin the pump published its last list node through 820FD218 about 0.24 ms before the CP
        completed the fence. No thread recorded anything until the drain: the CP was idle and the pump silent.
        The pump's next activity is the next frame's bracket (BS 0x15555555), 0.9-24 ms after the drain.
      - So the pump spent up to 19 ms inside its end-of-list step, the native EndSync, before the dev+0x2A74
        decrement. Its only operations the SDK body lacks are the fresh OS permission queries: QueryWorkingSetEx
        on the state block and VirtualQuery on the marker.
      - The render thread shows the same signature in the native append, which runs inside GpuCommandBufferFlush.
        Flush p50 is 0.014 ms against 0.013 ms in res1, but the mean is 0.239 ms against 0.076 ms and p99 9.1 ms
        against 2.3 ms: about +2.9 ms per present (17.9 flushes per present), all in the tail.
      - The swap-throttle fence wait follows: p99 19.4 ms against 4.3 ms. The probe ran 53.5 fps against res1's 88.
      - The protocol itself is exact: decline 0, a74_nonzero_exit 0, segment order intact, and the native bodies
        match the SDK step for step (no wait, sleep or extra lock).
      - The offline bench (uncontended, 2.6 us per append, 14 us per follow) was a floor taken on a near-empty
        arena. The `cost` mode (below) reproduces the live scale offline: VirtualQuery walks the resident pages of
        the marker's whole same-protection region (1786 MB of the arena), so its cost grows with the touched working
        set. Protection churn on another thread adds multi-ms tails. The chain path also ran one query (on the
        previous tail) while holding the guest spinlock dev+0x2A78, which EndSync must take.
    - **Stress freezes 4 and 19 are not out-of-order records.** Every strict transport witness in tp_stress is
      clean: tile gap / overlap / order 0, first dup / reorder 0, drainMiss 0, idMiss 0, NEVER-EXECUTED 0, no
      NATIVE_FLUSH_BAD / DROP, coherency top / page 0, decline 0.
      - Freeze 4 is a permanent step at its first dumped pair: p4462 has no Superman shadow, and p4463 onwards all
        have it (438,843 px).
      - Freeze 19 is a one-present glitch at k=1: p10008 alone differs (149,851 px on a background building's
        facade); p10007 and p10009 onwards are identical.
      - Both lie about 400 ms into a hold (k=0/1). sw1 and brk2 stress (no pair) show no such pair.
      - With the pair under stress, only 18% of closes chain (2.40M of 13.15M) against about 90% without the
        pair: the slower producer lets the pump drain.
      - The cause is unattributed: a timing-exposed renderer race is the leading candidate, with no transport
        evidence. The plan now carries `stress_off` (the SR_NATIVE_TRANSPORT=0 control, pre-registered) to
        attribute the next stress result.
    - **The live re-run tpl2 (2026-10-14, merged dev 3ea0c19d, exe bf1246b89b0f, rule in v1_native/tpl2/NOTES.txt
      written before the rolls): every step PASS.** Replay 9/9; the probe's P2 trace invariants hold (tpl1:
      idle.spin_over_frame=6); the per-present A/B, CPPM4 and its OFF control 15/15; stress 15/15 and stress_off
      15/15 (tpl1's freezes 4 / 19 did not reproduce, cause not attributed); the gate PASS on the rule's single void
      re-run (the first gate stalled at boot: CP TIMED OUT at 60 s under an orphan recursive grep saturating D:).
      Appends 358,246 (chain 246,690, direct 111,556, park 23,400), worst 620.9 us, none over 1 ms; EndSync worst
      32.4 us. Ledger: tpl1's two failures -> `pair_admission_stalls`, the v13-v16 activity failures ->
      `pair_v13_v16_activity`, the void gate -> `tpl2_gate_void`. p2.transport stays in progress on two residual
      unknowns: `composed_zero` (every kind is proven alone, default OFF; the record closes on the composed meter,
      [PM4_COV] 0 with every kind ON) and `pacing`.
    - **TRANSPORT PAIR ADMISSION A (the orchestrator's decision, 2026-10-14): arithmetic admission, no OS query.**
      Option B (the same queries made cheaper) was declined.
      - **Offline confirmation first** (`--native-transport-test cost`, ungraded; worker, real imports, 4000 calls
        per cell).
        - The working set is touched around the fixture (0 / 16 / 64 / 256 MB). Then a second thread churns
          protection 32 MB away: raw VirtualProtect, or the tracker's protect / store-fault / release cycle.
        - The table is one run (`archive/tpf/modes2/cost.out.txt`) on the fixed build. It measures, on the same memory,
          the native entries with the arithmetic admission and the replaced admission's queries alone ("old
          queries": a follow's QueryWorkingSetEx-or-tracker query of the state block plus its VirtualQuery walk of the
          marker; a chain's five QueryWorkingSetEx-or-tracker queries).
        - Means and maxima in us:

        | row | VirtualQuery | follow SDK | follow, arithmetic | follow, old queries | chain SDK | chain, arithmetic | chain, old queries |
        |---|---|---|---|---|---|---|---|
        | 0 MB quiet | 13 / 162 | 0.10 / 4.2 | 0.19 / 0.7 | 14 / 154 | 0.12 / 7.4 | 0.28 / 6.0 | 2.5 / 141 |
        | 64 MB quiet | 100 / 275 | 0.14 / 162 | 0.19 / 7.1 | 101 / 268 | 0.12 / 3.8 | 0.26 / 5.8 | 2.4 / 164 |
        | 256 MB quiet | 367 / 519 | 0.10 / 5.8 | 0.20 / 6.2 | 369 / 524 | 0.12 / 4.0 | 0.30 / 158 | 2.5 / 161 |
        | 256 MB + VirtualProtect churn | 214 / 526 | 0.10 / 0.4 | 0.19 / 3.2 | 113 / 303 | 0.12 / 7.2 | 0.26 / 5.8 | **194 / 4871** |
        | 256 MB + tracker fault churn | 233 / 511 | 0.10 / 3.4 | 0.21 / 4.9 | 93 / 250 | 0.17 / 159 | 0.29 / 4.4 | 14 / 203 |

        - The old follow queries grow with the touched span (14 -> 369 us mean): VirtualQuery walks the resident
          pages of the marker's 1786 MB same-protection region.
        - Under raw VirtualProtect churn the old chain queries reach 4.9 ms (5.8 ms in the first run,
          `archive/tpf/cost1.txt`, which measured the old native bodies).
        - The tracker row now churns for real (Astra round A). It rotates 32 blocks and advances 64 frames per cycle
          past each block's fault cooldown: 77,302 cycles, 77,302 protect runs, 77,302 faults during the row. Under
          it, QueryWorkingSetEx contends (old chain queries 14 us mean against 2.5 quiet). The fragmented protection
          shortens the VirtualQuery walk (93 us against 369).
        - The arithmetic admission stays at 0.19-0.30 us mean in every row. Its rare 130-160 us maxima also appear in
          the SDK body's own (0.17 / 159 under tracker churn): host preemption, not admission.
        - The live arena's resident span is larger than 256 MB, which fits tpl1's ms-scale pump stalls.
        - The first run's tracker row (`cost1.txt`) churned only once: the first fault's cooldown made every later
          protection skip its block. Its numbers are withdrawn.
      - **The admission** (`kernel/native_transport.cpp admit`): a range is admitted when it is non-empty, does not
        wrap, maps contiguously (a range across the 0xC0000000 alias fold declines) and lies inside the registered
        guest arena (getGlobalGuestMemory). It must also touch no declared foreign-protected range. The arena is one
        committed read-write allocation in both memory models, and nothing decommits inside it (the census below
        classifies every free, decommit and protection site). The only protection left on an admitted page is the
        GPU write tracker's PAGE_READONLY. The pair's stores are plain host stores, so the first one into a tracked
        page faults into the tracker's
        handler exactly as a guest store does. The handler releases the 64-page block in both copies and publishes
        the dirty-block invalidation.
      - **Foreign page protection is declared** (`kernel/guest_protect.hpp`).
        - Every other host protector declares before protecting:
          - the image watchdog's `wdProtect`: its always-on boot page (guest 0x8209A000) is a declared *range*, and
            its opt-in modes (SR_WRITE_WATCH_VA, SR_IMAGE_RO and the runtime arm) exclude everything;
          - the audio null-page `protectRange` (SR_PROTECT_ZERO) excludes everything.
        - The pair declines every entry while a page-protecting diagnostic is requested in the environment (read at
          its first call, so it holds before any protector arms) or once an exclusion is declared. These declines
          are counted `exclusion=`. A range declaration refuses only the ranges that touch it (`foreignRange=`).
        - Astra accepted the boot-page range decline as sound for the current initialisation order: the page is
          registered before guest code runs, the range covers the whole protected page, and it stays declared after
          the page is unprotected.
        - **The rule for a runtime protector** (Astra round A). A declaration publishes; it does not quiesce. A call
          can pass `excluded()` / `intersects()` and only then reach its store, while another thread declares and
          protects in between. The protectors above are safe:
          - their opt-in gates are read at the pair's first call, before any of them can run;
          - the boot page is protected before guest code runs.
          A future protector that can start while the pair runs needs pre-exclusion through a gate listed in
          `kDiagnosticGates`, or a quiescence protocol (an ArenaLease-style drain of the calls in flight) before it
          protects. The same rule is written in `kernel/guest_protect.hpp`.
      - **The arena's lifetime** (Astra round A, P1). Arithmetic admission trusts the registered arena to stay mapped.
        - Before the fix, `freeGuestMemory` released the arena at teardown with guest threads still running and the
          registration in place. A later native call would have passed admission against freed memory where the old
          queries declined.
        - Every native entry now holds a `guest_protect::ArenaLease` across all its guest accesses: its own loads
          and stores, `sub_820FD218`, the lock imports and the park's `KeWaitForSingleObject`.
        - The release goes through `releaseGuestArena` (kernel/guest_context.cpp). It calls `retireArena` first,
          which refuses every later lease (those entries decline, counted `retired=`) and then waits up to 250 ms
          for the leases in flight. Dekker order, seq_cst on both sides.
        - When a call still holds the arena (a pump parked for good), the release is skipped: the arena stays mapped,
          a leak at exit, with a `[NATIVE_TRANSPORT] guest arena release skipped` line. With the gate off no lease is
          ever taken and the release is unchanged.
        - **Scope (Astra round B).** A retired decline is an ordinary decline: the production dispatch then runs the
          SDK body, which still touches guest memory. This fixes the *native arithmetic admission*: no native entry
          admits against, or touches, a released arena. It does not fix the pre-existing teardown race in which guest
          threads that survive the release (SDK bodies included) touch freed memory. No whole-process teardown safety
          is claimed. The retire fixture calls the native entries directly.
        - Other release paths: none of a registered arena.
          - allocGuestMemoryUnified frees only during setup, before registration.
          - The replay arena (gpu_trace_replay.cpp) is never freed.
          - NtFreeVirtualMemory recycles or zeroes guest memory and never releases or decommits host memory.
          - The census below classifies every such site.
      - **The census** (`tools/difftest/guest_protect_census.py`, rewritten after Astra round A, tightened after
        rounds B and C; in the runner, with `test_guest_protect_census.py`). It is a textual check, not a compiler:
        it does not preprocess or parse C++. It enforces the rules below and rejects the forms named. What it cannot
        see is listed under "Not covered".
        - Text inputs: the exe's actual build inputs. `ninja -t inputs SupermanReturns.exe` gives every source,
          object and library, recursively: kernel, gfx, app, the generated guest runtime guest/ppc, imgui and the
          in-tree FFmpeg. For every item with a deps entry, `ninja -t deps` adds every compiler-recorded dependency,
          whatever its suffix.
          - The runner builds the exe first; an object without a valid deps entry is a problem.
          - Toolchain and SDK files (Program Files / Windows Kits) only declare the APIs and are skipped (counted).
        - Binary inputs: these must be classified in `BINARIES` with a reason:
          - every input that is not text or does not exist;
          - every library, delay-load DLL and module-definition file named by a link-affecting form the census reads.
          - Forms read:
            - on link statements, the link variables token by token: `-l`, library paths, `-Wl,`, `-Xlinker`,
              `-Xclang --dependent-lib`, and `/` flags;
            - on every other statement the exe is built from (compiles, custom commands), in every rule command and
              in every file-level variable: `--dependent-lib`, each `-Xclang` value, `-Wl,`, `-Xlinker` and the
              `/DEFAULTLIB`-style flags.
          - Rejected forms:
            - a linker flag other than `/DEFAULTLIB`, `/DELAYLOAD`, `/DEF` and a short benign list (subsystem,
              manifest, stack, debug, ...);
            - a link-line driver flag outside a short list;
            - an `-Xclang` value other than `--dependent-lib`, `-include` / `-include-pch` with their path, and
              `-emit-pch`;
            - `--linker-option`, `-fms-runtime-lib` and `/link`;
            - a statement whose rule has no command in the rule files.
          - IN_TREE entries must be outputs of this build graph (`ninja -t query`): ffmpeg_xenia.dll and its import
            library, SupermanReturnsGuest.lib, sr_imgui.lib, xenos_xlat.lib and the custom-command stamps.
          - The others are external, each with its reason: SDL2.lib, SDL2main.lib, the CRT (msvcrt / oldnames), the
            static delay-load helper, uuid, 21 Windows system import libraries, and the export list CMake writes for
            ffmpeg_xenia.dll (symbol names, no code).
        - Pragmas:
          - `#pragma comment`, `__pragma(comment(..))` and `_Pragma("comment(..)")` must be `comment(lib|linker, ...)`
            whose argument is a plain sequence of string literals, or a user / compiler / exestr comment. The
            literals are joined before classification, so `"kernel32" "evil.lib"` is `kernel32evil.lib`, which is
            unclassified. A macro argument is rejected.
          - A `_Pragma` whose argument is not a literal sequence is rejected. So is a `__pragma` whose first word is
            not a known non-linking pragma (warning, pack, GCC, clang, omp, ...).
          - The exception is the body of three classified wrapper macros: FFmpeg `AV_PRAGMA`, hedley
            `HEDLEY_PRAGMA` and fmt `FMT_PRAGMA_IMPL`. Every use of one of them must pass a known non-linking
            pragma. An alias of a wrapper, or a redefinition of one in another file, is rejected.
        - Sites: every reference to
          - the protection, allocation and release APIs: VirtualProtect / Free / Alloc*, Nt/Zw Protect-, Free- and
            AllocateVirtualMemory*;
          - the section APIs: Nt/Zw Create-, Map- and UnmapViewOfSection*, CreateFileMapping*, (Un)MapViewOfFile*;
          - the symbol resolvers, whatever their argument: GetProcAddress*, LdrGetProcedureAddress*, dlsym;
          - the watch arm / disarm / install / restore entry points, the null-page protectRange and the arena release.
          A call, a declaration, an address taken, a pointer type and a string all count. Line splices are joined
          and comments removed first. Then adjacent string literals are joined, as the compiler joins them, so
          `"Virtual" /* x */ "Protect"` counts. Every file that holds a string literal or an API name is scanned.
        - Each (file, outermost function, token) needs an allowlist entry with an exact count and a reason. Generated
          guest files are counted per file.
        - Funnels: `wdProtect` and `protectRange` must declare, and `releaseGuestArena` must retire, as their first
          effective statement, unconditionally.
          - Definition detection is textual, in the funnel's own file. A definition is the name, a balanced parameter
            list, the qualifiers a definition may carry and a brace. The qualifiers are const, volatile, `&`, `&&`,
            noexcept, `noexcept(..)`, `throw(..)`, override, final and a trailing return type.
          - Exactly one such definition is allowed.
          - These are rejected:
            - the name inside any preprocessor directive (a macro body);
            - the name at file scope in any other form, such as an alias, an attribute after the parameters or a
              macro call;
            - a definition inside a preprocessor conditional other than a `_WIN32` guard;
            - a directive before the required statement.
          - Only void casts, namespace aliases and guard returns may precede the required statement.
          - A same-named function in another file is not checked as a funnel. Its API references are ordinary sites
            and must be classified.
        - Result (rounds C and D): 1114 text inputs scanned (519 toolchain / SDK files skipped), 39 binary inputs
          classified, 156 sites in 105 classified keys, 0 problems.
        - Every false negative Astra found is a regression test that fails by name:
          - round A: a pointer call; VirtualFree with 0x4000 or MEM_RELEASE; VirtualAlloc(MEM_COMMIT,
            PAGE_NOACCESS); an armRuntimeWriteWatch alias; an `if (false)` declaration;
          - round B: an unclassified protector.lib input, a recorded `guest_memory.def` dependency, a `#pragma
            comment(lib)`, `GetProcAddress(.., "Virtual" "Protect")`, NtUnmapViewOfSection and the other section APIs,
            a compliant `#if 0` copy of wdProtect before a conditional real one, a funnel inside a non-platform
            `#if`, and a directive before the declaration;
          - round C:
            - a compliant wdProtect in an unused `#define`, next to a `noexcept(true)` real one whose declaration is
              conditional;
            - `#pragma comment(lib, MACRO)`, `__pragma(comment(lib, "x"))` and `"kernel32" "evil.lib"`;
            - an object compiled with `-Xclang --dependent-lib=protector`, and `LINK_LIBRARIES =
              -Wl,/DELAYLOAD:protector.dll`;
            - flags in the other places: `/DEF`, `--linker-option`, `-fms-runtime-lib`, an unknown `-Xclang` value,
              a file-level variable, and a rule with no command;
            - `"Virtual" /* x */ "Protect"`, and a name split by a line splice;
            - an unclassified `_Pragma` wrapper, a wrapper alias, and a wrapper use that passes `comment`;
          - round D: a digit separator (`4'096`, `1'000`) read as a character literal, which hid the literals and
            the `__pragma` after it on the line. Both scanners now take a whole pp-number first.
        - Not covered:
          - the implementation inside the binaries classified as external (each reason says why the guest arena never
            reaches it);
          - toolchain / SDK headers (declarations, skipped by path);
          - a raw system call that names none of the APIs or resolvers above;
          - a name assembled by token pasting (`##`) or at run time;
          - macro expansion in general. Inactive `#if` branches are scanned like active ones, which can only add sites.
          - linker directives that a prebuilt object or library carries, or that the compiler adds by means other than
            the flags above.
        - Pre-existing on dev, outside this lane: `tools/test_hrr_invalidation_surface.py` reports four unwrapped
          sites (gpu_write_watch.cpp protectRun x2, app/main.cpp's pid-map UnmapViewOfFile x2).
      - **Host evidence** (`archive/tpf/matrix2`, worker, `native_transport_test_run.py`; valid).
        - native and gate-off 165/0, ab 47/0, permissions 18/0;
        - exclusion 21/0, and exclusion-env 6/0 under each of the six diagnostics;
        - retire 11/0;
        - the census over the freshly built exe clean;
        - 21 compiled defects, each failing only its named checks with a passing SDK control, exact restoration
          and a valid restored rerun of every mode.
        - The retire mode pauses a chained append inside its chain-lock acquire, with its lease held and its stores
          still ahead. It then requires, in order:
          - retirement does not report the arena drained;
          - new native calls decline before any guest access;
          - the real release keeps the arena mapped;
          - the append completes into it;
          - the drained release frees it.
          The process then ends without touching the freed arena.
        - Defects `arena_drain` (retirement does not wait), `arena_lease` (the pair takes no lease) and
          `release_retire` (the release ignores the retirement) each free the arena under the paused append. They
          fail by name, and the append never resumes.
        - The queried admission's read-only / guard / no-access decline fixtures are replaced. In their place:
          - ranges across the alias fold, and above / below / without the registered arena;
          - the arena's exact last byte;
          - a chain tail outside the arena (declined under the lock);
          - declared ranges, including a read-only declared page that is never written;
          - the real watchdog boot page, its runtime arm and the audio protection.
        - The tracker path:
          - Append (node, tail and crossing pages) and EndSync park / follow run on pages protected and validated
            as the CP does: the actual SDK, production dispatch and the direct entry.
          - Each leaves the SDK's tracker view: fault count, released pages, published blocks, per-page ownership,
            validation and protection.
          - Defects `pair_bypass` (the pair's stores go around the handler) and `handler_release` (the handler
            unprotects without releasing) fail by name.
          - The EndSync fixture puts its lock word and event on an untracked page, so only the entry's own stores
            can release the state page.
        - New defects: `contiguity`, `arena_bound`, `pair_bypass`, `handler_release`, `declared_range`, `exclusion`,
          `exclusion_env`, `arena_drain`, `arena_lease`, `release_retire`. Retired with the queries:
          `readable_marker`, `tracked_admission`.
      - **The live census line** keeps the host-time counters with their tails: `appendUs / appendMaxUs /
        appendOver100us / appendOver1ms`, the same for `end`, and `lockedAdm`. A re-run's drain check and these
        tails show whether the pump stall is gone. It also counts `exclusion= foreignRange= retired=`.
      - Astra round A (MERGE AFTER FIXES, `archive/tpf/astra/round_a.txt`): P1 arena lifetime, P2 census
        completeness, and P2 the tracker-churn row are fixed as above. Rounds B and C (MERGE AFTER FIXES,
        `round_b.txt`, `round_c.txt`): the teardown scope wording and seven P2 census gaps are fixed as above, and
        the census claims are reduced to what it enforces. Round D (MERGE AFTER FIXES, `round_d.txt`): the
        digit-separator P2 is fixed as above.
      - Open: the live plan re-run (drain check, frame rate and `stress_off`).
- **GATE(a) no-kick engineering checkpoint (2026-10-11, dev; default OFF):**
  `SR_NATIVE_GATE` emits kind 17 with the pacing argument and separately translated
  scratch-root / scratch-gate addresses. The existing consumer performs the two
  unpredicated waits around the callback's predicated block. Eight actual SDK goldens
  and simulated completion checks pass; the full six-process unit matrix is 139/0,
  with Astra engineering PASS. Admission requires active segmentless transport, a
  supported interval, readable mapped state and the complete no-kick footprint.
  Performance capture, kick-required, undefined-interval and invalid-address cases
  decline before mutation. The scoped SDK oracle also suppresses nested callback
  interception. This is not full GATE(a): live pacing, kick handling, real CHECK
  coverage, whole-world comparison and transport zero remain owed. The existing
  PM4 engine and register emulation remain linked; shipping composition is open.
  Repro: `build/difftest.exe --present-gate-test`; private exact snapshot
  `077016d096d1`, `archive/v1_release_codex_20261011/present_gate_unit_matrix_final.json`
  and `native_gate_checkpoint.md`. Ledger statuses are unchanged.
  - **Bounded live witness (2026-10-12):** quiet private-save OFF/ON spans both
    complete SUCCESS, 1/358 flagged, 360/360 in play, health PASS / zero hang triggers.
    ON last sampled calls = applications = 23,119, skips 1, declines 0. Astra PASS
    for producer/consumer exercise with a drawn world only. Flagged wall shading,
    input readback and camera timing differences remain unresolved; no matched
    fidelity, causal performance, full freeze, live CHECK or pacing verdict.
    `native_gate_live_checkpoint.md` and exact receipts retain the scope. Gate OFF
    remains the default; no ledger status changes or transport-zero claim.
  - **Dry SDK CHECK / calling-ABI follow-up (2026-10-12):** snapshot
    `2552638c16e3` preserves incoming full-width `r3` on emission and skip, with
    actual SDK upper-word goldens and a cached-readable/PAGE_NOACCESS rejection
    fixture. Six-process matrix 277/0, relevant SDK regressions, forcing 0,
    unchanged strict ledger and quiet historical renderer replay 9/9 exact pass.
    Astra PASS for engineering and the bounded quiet live comparison after blind
    image review. Gate OFF / CHECK ON disables record execution: admitted calls
    compare SDK 39DW/cursor/full-r3/required flag bit, not asynchronous full-device
    equivalence or pacing. One periodic sample records tested16,693/skipped1/bad0/
    declined1; skipped is a subset, decline unclassified, totals not final. Full
    retained log has zero BAD lines. Private-save span SUCCESS/PASS 1/358,
    in-play360/360, health PASS / zero hangs. p10977's wall-shading flag remains
    unexplained; no flicker-free or matched-reference claim. Selected D proof
    retained, J GC 31 leaves/no eligible removals/no warnings. Initial overlapping
    source-query runs remain diagnostic. `native_gate_check_checkpoint.md` and
    exact quiet receipts record the scope. Execution CHECK, kicks, pacing,
    full GATE(a), transport/display zero and shipping composition remain open.
- **GATE(a) complete emission engineering checkpoint (2026-10-12, dev; default OFF):**
  This supersedes the no-kick/noncapture implementation subset above. The native
  entry implements the defined performance-begin / gate / performance-end
  sequence, including recording-pin flushes and the final-kick return cursor.
  Undefined intervals, unsupported segmentless modes and malformed allocation
  state still decline before mutation. Slot offset precedes a timestamp kick;
  its base is loaded after the kick, as the SDK assembly requires.
  Leaf, direct, actual production-table indirect and scoped SDK-oracle routes
  compare 28 fixtures each in record, literal and reconstructed modes. Full
  device/segment state, ABI, emitted stores and two native transport replays
  match; V5 has 594 passing assertions across 17 positive processes. An actual
  source mutation fails all four allocator-mutated timestamp-base fixtures;
  a separate wrong-return control fails all four final-kick cases. Astra passes
  this bounded SDK/native recording-pin wording. Corrected snapshot
  `c616236dd83b` also passes historical renderer replay 9/9 byte-identical; that
  replay bypasses the producer and does not prove this gate or Xenia fidelity.
  Repro: `python tools/difftest/present_gate_sdk_test_run.py --out archive/<bank>`.
  Private receipts: `archive/v1_release_codex_20261012/present_gate_full_v5_*`,
  `present_gate_base_mutation.json` and `present_gate_full_fixed_replay_compare.txt`.
  The corrected pure-ON live gate fails freeze 1 (67 pixels at delta 2, above
  the unchanged tolerance), despite span PASS 0/358 and 360/360 in play. Both
  canonical rolls succeed, health passes, all eight strict transport items and
  the complete retained-log integrity supplement pass. This remains a failed
  gate with unclassified cause; selected D proof and the exact failing pair are
  retained. The corrected same-run frozen per-present SDK/native comparison
  passes all 15 graded freezes (14 gameplay plus one menu over world), with 567
  static pairs graded, 18 unsettled pairs excluded and 2,740 logged toggle
  witnesses. All five declared evidence items pass independent evaluation; the
  complete retained-log named integrity supplement reports no faults and detects
  20 negative controls. The periodic gate sample is 13,426 calls/applications,
  zero declines, not a final total. Astra passes this bounded wording. The A/B
  pass does not supersede the failed pure-ON gate or classify its cause. Xenia
  differences and blind visual concerns remain, including dark facades,
  limited close-wall coverage, blur and unreadable overlay text.
  Live pacing, arbitrary SDK modes / GPU backpressure, full GATE(a), transport /
  display zero, defaults and shipping composition remain open. The PM4 consumer
  and display-register emulation remain linked; release milestone 2 is incomplete.
- **P2-A RESULTS (2026-10-09, native-a; `SR_NATIVE_RINGLESS`, commit a33d0f92 + 8f367bb1; exe 55bc1197b96c;
  archive/v1_native/p2b_*) [measured]:**
  - Replay 9/9 byte-identical vs 1b27b69dd6ab.
  - Frozen-pass per-present A/B (`SR_AB_TOGGLE=SR_NATIVE_RINGLESS,SR_AB_PRESENT=1`, dense freeze, the P2 trace on):
    grade **PASS 12/12** (pixels exact with the transport alternating every present); the CP took 458,051 queue IBs
    and 53,657 BS items for the ON presents (`abOff=512,216` ring-path calls for the OFF ones), `desync=0
    badIB=0`; trace invariants: 206,255 closes all S+2, C never backwards (the lock-order fix) nor ahead of S, every
    COMPLETE a closed S in order, ISRs 2 / scratch 9 per present as the baseline.
  - Whole run ON (`p2b_on`): ring packets parsed by the CP **1** (the first doorbell), `ringless ib=970,530
    bs=109,440 desync=0`; 215,879 closes all S+2, C monotone; meter vs the ring baseline p2a_trace, per ingame
    frame: **pm4 3,276.6 -> 3,227.3 (-49.3)** with every draw-side kind unchanged (t0 2,241 -> 2,242, shader 107 ->
    107, const 35.5 -> 35.8): exactly the transport (the IB descriptors 35.5 + the 4 bin selects + the kick blocks'
    share) left the count. Waits: swap p99 0.79 ms (baseline 0.83), ring-space waits never block (87 of 4.8M over
    50 us), idle total 5.68 s (baseline 5.82 s).
  - The `{IB dwords}` per-present identity (P2.7 item 2 as written) is NOT a usable criterion: on the ring path
    itself consecutive FROZEN presents differ in byte length (menu freezes 37-39 of 39 pairs identical, gameplay
    freezes 0-5 of 39) because P1's section decisions depend on the cursor position (leaf aborts at the segment
    limit, alignment pads, kick splits) while the pixels stay exact; the BS pattern and the IB counts are stable.
    `archive/v1_native/ab_frozen.py` measures it per frozen window. The ring-only control (p2c_ctrl, the same
    freeze, no gates): 508 frozen pairs, 144 identical, `ib_dwords_same=150` - the same ~27% as the A/B roll's
    150 of 585, so the byte-length variation is the ring path's own. The P2-A identity rests on the pixel-exact
    alternation, desync 0 and the invariants.
  - Flicker gate with the gate ON (p2b_gate): **FLICKER_GATE PASS** (span 0 of 358, freeze counted=11 bad=[]).
  - Speed (p2b_ab, in-roll A/B, 59 windows): tick -0.047 +-0.382 ms (paired -0.053 +-0.197), frame p50 +0.024
    +-0.320, fps +0.18 +-5.1: neutral.
  - **Baked default ON** (27235c5d, kernel/gpu_ringless.cpp). The bake proof (p2_bake_ringless.sh, p2d_*, exe
    3d53ccbf0a56, no gates) [measured]: replay 9/9 byte-identical vs 1b27b69dd6ab; flicker gate **PASS** (span 0 of
    358 presents, freeze counted=11 bad=[]) - **ledger correction (2026-10-09):** the bake gate's FREEZE roll drew a
    black world (10 of its 11 graded freezes are black frames, the eleventh the pause menu over black), so only its
    span (scene luma 23.1, all 360 presents in play) holds; P2-A's frozen evidence is the gated proof's (p2b_abp and
    p2b_gate, the world drawn in every graded freeze); default meter roll `[GPU_CP] census ... packets=1 ... ringless
    ib=916,334 bs=105,120 desync=0`, `NEVER-EXECUTED=0`, 0 ICALL_FAIL / NATIVE_FLUSH_BAD; `meter_delta.py p1e_meter
    p2d_meter`: **pm4 1,825.9 -> 1,768.6 per ingame frame (-57.3)**, bin 57.6 -> 52.7, t2 pads 211 -> 209, the rest
    unchanged. One `[GPU_HANG]` H3 diagnostic episode at boot (37 lines): the shader cache loaded at +7.9 s (usual
    +0.6 s; +3.6 / +4.1 s in two row-4 rolls without ringless) and the CP sat in its first IB until then, first
    present +9.6 s, recovered, the run normal; the other ringless rolls booted normally (p2c_ctrl: cache +0.63 s,
    first present +3.93 s) - boot I/O variance, not the transport. PROGRESS.md entry; dev fast-forwarded.
- **Proof chain (P2.7):** the trace baseline; P2-A sequence identity; P2-B `SR_P2_SYNC` answer equivalence +
  the frozen-pass A/B at the throttle point (or two-roll dense freezes if the toggle cannot resume the
  command-buffer state); the invariants on every roll (fb+0 monotone and <= C, produced == consumed per kind,
  every CALLBACK once, pc+0x10 == 0x0BADF00D and pc+0 == 0 outside callbacks, `dev+0x2A74` == 0, no
  `[GPU_HANG]`); pacing vs the golden (~2.48 vblanks per frame, the gate count = frames - 1, the throttle wait);
  replay 9/9, the whole-run sheets, the flicker gate, CPPM4 for every new kind, forcing audit 2, and
  `[PM4_COV]` 0 packets per frame as the acceptance.

- **Next (rows 4 + 6 + 7 as one entry, from the asm of 820FB6E8 / 820FBBB0 / 820FBBA0):**
  `D3DDeviceEmitDrawIndexedGen_820FB6E8(dev, prim, count, stride)` IS the third draw flush: the
  same five dirty qwords as 820FC000 (constant runs, the shader bind 82108668 - which is why 107
  shader loads and 37 constant loads per frame remain after rows 1+2 -, the window scissor, the
  register runs, the fetch constants, the 0x2180 run, 821075E0), with the device's stride byte
  dev+0x2FE0 set to stride/4 for the bind (`|= 1<<51` when it differs from the stride the bound VS
  was PATCHED with, dev+0x2D20 - not from the previous stream stride: batch 3's dry CHECK caught that
  misread, 897,670 BAD draws = a spurious pointer IM_LOAD + INVALIDATE, and the execution CHECK failed
  3 freezes of 12 - and a VS is bound) and restored after; then a kick check, `GpuCmdBufAllocate_820FCDF0(dev, count*stride/4, 16)` (the
  ring's own allocator: carves the vertex space from the segment's TAIL, moving dev+44/48 down),
  then type-0 0x2102 {0}, type-0 0x48BA x6 {1, 0, 1, 0, phys(alloc)|3, (4n & 0x3FFFFFC) |
  0x10000002} (vertex fetch slot 31), type-0 0x5000 x3 {0, 0, 0}, and DRAW_INDX 0xC0012201 {viz,
  prim | count<<16 | 0x80} (the tile-predication branch, byte 0x2A38 & 2, never taken here); dev+0x10
  |= 0x1000, dev+0x3314 = cursor, +0x3318 = alloc VA, +0x3320 = dwords. `DrawVerticesUP_820FBBB0`
  = that + memcpy(alloc, caller's vertices) + `dev+40 = dev+0x3314`; `EndVertices_820FBBA0` = the
  publish alone. So: the S1 Driver gains a third entry (flushState with bindState, the three runs
  and the predicated auto-index draw as records; the allocator stays the XDK's call between spans);
  the vertex bytes stay in the ring allocation the CP already reads through physToHost - no copy
  into the arena until P2 moves the allocation to a host buffer. Expected: shader 107 -> ~0, const
  37 -> ~0, and the row-4/6/7 register runs and pads.

**Order:**
1. P0, one roll.
2. P1 entries in census order, each with its CHECK, frozen-pass A/B and the flicker gate.
3. P2 behind `SR_NATIVE_RINGLESS` until frozen-pass A/B, sheets and the gate pass; then baked.
4. P3.

Strand B runs in parallel (resolves, EDRAM). Its resolve entries are P1 record types behind one
interface: the rule that resolves are stream items stands.

**Risks:**
- P2's frozen-pass A/B needs both transports live at a frame boundary. The ring must be drained
  completely before a switch, and in host-queue mode the native ring functions must keep the
  guest-visible ring pointers consistent. If that is not feasible, the fallback oracle is the
  generate-both CHECK with the execution ledger.
- Inlined writers that P0 finds outside any bracket (the five bypass classes, section 3.2) each need
  a native replacement before P2.

## P2 CONTRACTS (research, 2026-10-09, branch `research/p2`)

**What this is.** It maps everything in the XDK D3D runtime (0x820F0048..0x821162E8) that manages the ring or
depends on GPU progress, for the ring cutover (P2 above). The map itself is research only. The platform bugs it
found are fixed separately (P2.9).

> **Occlusion queries: the ZPD fake is NOT done.** The fake ZPD / EXT write-back (P2.9 item 3, `SR_CP_ZPD_FAKE`)
> is the Xenia-parity interim only. The native end state is real host D3D12 occlusion queries on strand B's target
> path, so occluded effects answer as they do on hardware - in retail that is the HUD threat-marker glow alone: the
> sun flare's query (82721118) never runs (0 calls live, P2.12 item 1).

**Evidence ranks (CLAUDE.md oracle-first):**
- **[golden]**: the Xenia golden dumps `sound_gameplay` / `sound_menu` (`tools/golden.py`). The device is at
  0x40002880 in both, and the dumps are rank (2) oracle memory.
- **[xenia]**: `reference/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h`, `command_processor.cc`,
  `graphics_system.cc` and `kernel/xboxkrnl/xboxkrnl_video.cc`, also rank (2).
- **[IDA]**: asm or decompile, with the arguments read from the asm at the call site (ENGINE_MAP 7.3a), rank (4).
- **[ours]**: our code, rank (3).
- **[inferred]**: anything not checked directly.

IDA renames and comments are listed in P2.11. The verified contracts are also in ENGINE_MAP 6d, "THE RING / FENCE /
INTERRUPT CONTRACT".

### P2.1 The whole GPU write-back surface (what the guest reads back)

`D3DDeviceInitRingBuffer_820FDE90` [IDA] allocates two small physical blocks. Together with the display MMIO
registers, they are everything the D3D runtime ever reads back from the GPU:

| Word | Written by (console / Xenia) | Read by (guest) | Golden gameplay |
|---|---|---|---|
| `fb+0x00`, fb = `*(dev+0x2A10)`: **completed fence** | 2nd EVENT_WRITE_SHD of each segment's fence tail | `GpuBlockOnFence_820FD9A8`, `D3DDeviceIsFencePending_820FDD90`, `GpuRingWaitProgress_820F33E8` (hang timer), `D3DQueryGetData_82102DC8`, `D3DVsPatchFetchesInPlaceIfIdle_82108520` | 0x0010F8F3 |
| `fb+0x04`: **progress word** `(gen & 3) \| cursor` of the closed segment | 1st EVENT_WRITE_SHD of the tail | `GpuRingWaitForSpace_820FCB30` (command-buffer reuse) | 0xBFA8C403 |
| `fb+0x08` | `VdSetSystemCommandBufferGpuIdentifierAddress` target; passed to VdSwap as r5. Xenia never writes it | no reader found | 0 |
| `fb+0x3C`: **ring read index** (dwords) | `VdEnableRingBufferRPtrWriteBack(phys(fb+0x3C), r4 = min(log2(ringBytes >> 9), 19))` | `GpuRingWaitReadPtrWrap_820FD150` (ring space only) | 0x3380 (wptr 0x3413) |
| `pc+0x00`, pc = `*(dev+0x2A14)` = SCRATCH_ADDR: **SCRATCH_REG0**, the pending CPU mask | CP scratch write-back; the ISR clears its bit | the GPU's own WAIT_REG_MEM | 0 |
| `pc+0x04`: **SCRATCH_REG1**, the vblank gate | CP write-back (= 1); the vblank ISR / the pacing callback clear it | the GPU's own WAIT_REG_MEM | 1 |
| `pc+0x10` / `pc+0x14`: **SCRATCH_REG4 / 5**, callback and argument | CP write-back; `0x0BADF00D` when idle | ISR source 1 (`GpuInterruptCallback_820FDBA0`), which traps on 0x0BADF00D | 0x0BADF00D / 0x00010008 |
| MMIO `0x7FC86544` bit 0 | display | vblank ISR (source 0) | Xenia returns 1 |
| MMIO `0x7FC86530` (D1MODE_V_COUNTER) / `0x7FC86584 & 0xFFF` (viewport height) | display | `GpuPresentPacingUpdate_82111E38` | Xenia: a growing counter / 0x2D0 |

Notes on the table:
- SCRATCH_UMSK = 0x00020033, so REG0, REG1, REG4 and REG5 are written back.
- SCRATCH_ADDR is set by guest type-0 PM4 in the init state block, not by a Vd* call [IDA 0x820FE29C].
- There is NO other GPU write-back on the steady-state path. Two inactive blocks: the swap-timestamp slots
  `fb+0x40..0x5F` (only when `dev+0x4E58 != 0`; golden 0) and the perf buffer `dev+0x4E6C` (only when
  `dev+0x4E70 == 2`; golden 0) [IDA + golden].
- Query slots (ZPD / EVENT_WRITE_EXT) are the one exception. They are data, read by `D3DQueryGetData` (P2.6 hazard 7).
- Xenia's ordering semantics [xenia `pm4_command_processor_implement.h:884`, `command_processor.cc:541-552`]:
  EVENT_WRITE_SHD writes its value when the command processor REACHES it. That is translation order, not host-GPU
  completion. Scratch write-backs happen at the register write, in packet order.

### P2.2 The protocol in one gameplay frame [IDA + golden]

**Segments and fences.**
- The 2 MB command buffer (`dev+13484..13488`) is a ring of generations (`dev+13500`). Segments are carved by
  `GpuCmdBufReserveSpace_820FCBE8`.
- Every close (`GpuCommandSegmentFlush_820FD660` mode C) appends the 10-dword fence tail
  (`GpuBuildFenceWritePacket_820FD580`):
  - `INVALIDATE_STATE 0xC0003B00 {0x7FFF}`
  - `EVENT_WRITE_SHD 0xC0025800 {3, phys(fb+4)|2, progress}`
  - `EVENT_WRITE_SHD {3, phys(fb+0)|2, S}`
  - then `S = dev+0x2A1C += 2`.

  So fences are odd and step by 2, and `dev+0x2A1C` is always the open submission's id.
- The segment then goes to the ring as `INDIRECT_BUFFER 0xC0013F00 {phys, dwords}` through
  `GpuRingUpdateWritePointer_820FD218`, the only doorbell (MMIO 0x7FC80714).
- Every `GpuCommandBufferFlush_820FD8C0` first appends an 11-dword coherency block as its own IB
  (`GpuBuildFlushMemoryPackets_820FD3B8`):
  - `COHER_STATUS_HOST = 0x03000000`
  - `COHER_SIZE/BASE` for the deferred CPU-write range `dev+0x2D18`
  - `WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}`

  In the golden ring, almost every segment IB has an `IB 11` in front of it.

**Closes.** `GpuCommandBufferFlush` has 97 call sites in 56 functions. Closes happen at:
- the kick checks, where cursor `dev+0x28` > threshold `dev+0x30` (most of those call sites);
- `D3DDeviceReserveCommandSpace_820FC910`;
- the explicit flushes:
  - InsertFence;
  - `IsFencePending` / `GpuBlockOnFence` on the open id;
  - BlockUntilIdle;
  - `D3DQueryGetData` on the open id;
  - Swap (twice);
  - the lock core and InvalidateGpuCache on the owner thread, when over the threshold.

**The Z-pass bracket is recorded, then replayed by the pump thread.** A bracket runs from `BeginZPass_8210DF38` to
`EndZPass_8210E1A0`.
- `GpuReplayListBeginRecording_82110B30` pins the command buffer (`dev+0x3290 = dev+0x3298`,
  `dev+0x3294 = dev+0x329C + 1`).
- From then on, every close is a mode-B record `{0x82000000 | dwords, phys}` in the software list `dev+0x32C8`.
  It carries **no fence tail**, so S does not move inside a bracket. `GpuBlockOnFence(S)` returns at once while
  pinned, without flushing.
- Markers 0x80 (header), 0x84, 0x85 and 0x86 frame the bracket.
- `D3DDeviceInvokeRenderPass_8210E458` -> `GpuReplayListEndRecordingKick_82110C78`(dev r3, tiles r4,
  cpumask r5 = 0x04000000) then does three things:
  - it unpins the buffer;
  - it terminates the list with 0xC0000000;
  - it appends two IBs directly to the ring: a flush block, and the 23-dword kick block.

  The kick block is `GpuEmitCpuCallbackPackets_820FDA80` with callback `GpuIsrKickReplayList_82110B00` and
  ctx = the list head. Its append passes `a5 = 1`, so `dev+0x2A74` (the chain count) goes to 1. After that, later
  segments that frame are chained into the software list (4-dword blocks from `GpuCmdBufWriteJumpPacket_820FCA80`)
  instead of going to the ring.
- The CP runs the kick block:
  - SCRATCH4/5 = {cb, list}, SCRATCH0 = 4;
  - `WAIT_REG_MEM` (predicated) until pc+0 == 4;
  - `INTERRUPT` (predicated) mask 4, so the ISR runs on CPU 2;
  - `CP_CPU_INT_ACK`;
  - `WAIT_REG_MEM` until pc+0 == 0;
  - SCRATCH4 = 0x0BADF00D.
- The ISR calls `82110B00(list)`. That stores `dev+0x2A94` and signals the pump event chosen by the CURRENT CPU
  (`dev+0x2BDC + 0x38 * KPCR.cpu`), then clears pc+0 bit 2.
- The pump thread (`VdInterruptPumpWait_82110A30`, one thread, CPU 2) runs `GpuCommandBufferReplayDriver_82110840`
  -> `GpuReplayMarkerInterpreter_82110460`:
  - 0x84 -> `GpuRingEmitBinSelectLo_82110140` writes `SET_BIN_SELECT_LO 0x15555555` straight into the ring;
    then the 13 bracket IBs go through `820FD218`.
  - 0x85 -> BS 0xFFFFFFFF.
  - 0x86 -> jump back to 0x84 with bit 31: BS 0x2AAAAAAA, the same 13 IBs, BS 0xFFFFFFFF.
  - Then it forwards each chained segment once.
  - At the empty terminator, `GpuCommandSegmentEndSync_82110768` (under spinlock `dev+0x2A78`) brings
    `dev+0x2A74` back to 0 and the pump parks.
- These bin selects are P0's 4 UNSTAMPED packets per frame.
- Inside the IBs, the bind's predicated packets carry the per-pass difference (`GpuEmitDrawState_82108668`):
  - 0x15555555 selects the depth-only `RB_MODECONTROL` and the Z-pass IM_LOAD;
  - 0x2AAAAAAA selects colour.

**Golden ring, decoded by two independent passes.**
- The 16K-dword ring holds only IB descriptors (5182) and bin selects (419 = 104 x 0x15555555,
  105 x 0x2AAAAAAA, 210 x 0xFFFFFFFF).
- One frame reads: `[IB11, IB seg] x n, IB11, IB23 (kick), BS 15555555, 13 x IB, BS FFFFFFFF, BS 2AAAAAAA, the same
  13 x IB, BS FFFFFFFF, [IB11, IB seg] x 11`.
- Bracket IBs are 33K-57K dwords each.

**Present and throttle.** `D3DPresentResolveThenSwap_82612090` runs four steps:

1. **`GpuPresentEmitVblankGate_82111EC8`.** It emits:
   - SCRATCH1 = 1;
   - `WAIT_REG_MEM` pc+4 == 1;
   - a callback block, cb `GpuPresentPacingUpdate_82111E38` with arg `(vblanks << 16) | pct` (golden 0x00010008);
   - `WAIT_REG_MEM` pc+4 == 0. **The GPU stalls here until the vblank ISR opens the gate.**
2. The resolve to the front buffer.
3. `D3DDeviceInsertFence_820FDD30`. Its value is only stored at rwdrv+0xA4/0xA8 (0x8299AE94/98), never waited on.
4. **`D3DDeviceSwap_82112050`(dev r3, front hdr r4, scaler rect r5).** It:
   - reserves 64 dwords;
   - calls VdSwap (r3 = the block, r4 = the fetch constant, r5 = fb+8, r6 = the system command buffer struct,
     r8 = &fbVA);
   - saves `r30 = S` = F(N);
   - flushes, which emits F(N) right after the swap block;
   - runs `GpuCmdBufBeginNewSegment`;
   - then **`GpuBlockOnFence(dev, F(N-1) = dev+0x34D0, 3)` at 0x8211271C**, and `dev+0x34D0 = F(N)`.

**So the throttle is N-1.** Swap(N) returns only when the GPU has passed frame N-1's swap. The CPU builds N+1 while
the GPU runs N.
- The copy of the throttle at the end of `820FCF90` is skipped because the behavior flags `dev+0x4FC0 = 0x04000001`
  have bit 0 set [golden].
- Golden caught the render thread in that very wait: `dev+0x34D0 = 0x10F8F7` > fb+0 = 0x10F8F3, gate pc+4 = 1,
  countdown `dev+0x3B1C = 1`.

**Pacing (vblank ISR source 0, `820FDBA0`).**
- `dev+0x3B14 += 1` per vblank (gated on MMIO 0x7FC86544 bit 0).
- The countdown `dev+0x3B1C` reaching 0 writes pc+4 = 0 and `dev+0x3B18 = dev+0x3B14`.
- `82111E38` (source-1 callback, CPU 2) computes `remaining = dev+0x3B18 + vblanks - dev+0x3B14`. It increments
  `dev+0x3B20`.
- If `remaining > 0` it sets the countdown. Otherwise it reads V_COUNTER / height: `line% = V*100/H + 1`, capped
  at 100. It releases now if `line% <= pct`, else it sets countdown = 1.
- PresentInterval `dev+0x33A4 = 1` [golden].

### P2.3 Function contracts

How to read the tables:
- dev = r3 unless stated. "RT" = render thread, "pump" = `VdInterruptPumpWait` thread (CPU 2), "ISR" = interrupt
  context (Xenia: the CP thread as a DPC).
- Every row is [IDA] verified, from the decompile plus the asm where registers or masks matter, unless marked.
  Field offsets are given in hex (decimal).

**A. Command buffer, segments, ring**

| VA, name | Inputs (asm) | Reads | Writes | PM4 | Waits / thread |
|---|---|---|---|---|---|
| 820FC910 `D3DDeviceReserveCommandSpace` | r3 dev, r4 dwords | +0x28, +0x2C | via callees | none | flush, then `820FCF90`; returns the cursor or 0 / RT |
| 820FCBE8 `GpuCmdBufReserveSpace` | r3, r4 mode (2 segment / 1 tail carve), r5 &bytes, r6 align | +13484..13508, +13492, +12944/12948 (pin) | +13496/13500/13504, byte +0x2A3A \|= 4 on wrap | none | `GpuRingWaitForSpace(dev, end, gen)` / RT |
| 820FCDF0 `GpuCmdBufAllocate` | r3, r4 dwords, r5 align | +13496, +0x28, +13512/13516 | +13496, limit +0x2C, threshold +0x30 (moved down), byte +0x2A39 \|= 0x40 on failure | none | via 820FCBE8 / RT. Callers: UP / Begin vertices 820FB6E8, flush blocks, kick blocks |
| 820FCF90 `GpuCmdBufBeginNewSegment` | r3, r4 minDwords | +0x34B4 seg size, +0x4FC0 flags, +0x34D0 | +13508 seg, +0x28 = seg-4, +0x2C = cursor + size - 56 (14 dwords kept for tail + jump), +0x30 = limit - 160 | none | 820FCBE8. The swap block here is skipped (flags bit 0) / RT |
| 820FD660 `GpuCommandSegmentFlush` | r3 | +13508, +0x28, bytes +0x2A38/39, +12944 | C: fence tail + ring / chain. B (pinned, the Z-pass bracket): `{0x82000000\|n, phys}` record at +13008. A (command-buffer device): table +13160. Next segment at align32(end) or 820FCF90 | fence tail (C) | / RT |
| 820FD580 `GpuBuildFenceWritePacket` | r3, r4 dst | +13500, +0x28, +0x2A1C, +0x2A10 | +0x3298 = cursor, +0x329C = gen, **+0x2A1C += 2** | `C0003B00 7FFF`, `C0025800 3 fb+4\|2 prog`, `C0025800 3 fb+0\|2 S` | No-GPU mode (byte +0x2A39 & 2, PIX off): the CPU writes fb+0/fb+4 itself |
| 820FD8C0 `GpuCommandBufferFlush` | r3 | +12944, byte +0x2A38 & 0x80 | via callees | 11-dword coherency IB, then the segment | HSIO-fail branch only: block on S-2 / RT. 97 call sites |
| 820FD3B8 `GpuBuildFlushMemoryPackets` | r3, r4 &phys, r5 &dwords | +0x2D18 (swapped to -1 atomically), byte +0x2A3A & 4, +13484/13488 | none | `00000A31 03000000`, `00010A2F size base`, `C0043C00 3 A31 0 80000000 8` (x2 with the wrap flag) | / RT |
| 820FCA80 `GpuCmdBufWriteJumpPacket` | r3 list, r4 hole, r5 dwords, r6 phys | none | 4-dword chain block `{cont, n\|0x82000000, phys, 0xC0000000}` | none (a software-list record) | / RT |
| 820FD800 `GpuRingAppendJumpSegment` | r3, r4 hole, r5 phys, r6 dwords, r7 inc, r8 list | +0x2A74 (read unlocked) | +0x2A74 += r7: == 0 direct, no lock; != 0 spinlock +0x2A78, re-check, chain + add under the lock | via 820FD218 when +0x2A74 == 0 | / RT |
| 820FD218 `GpuRingUpdateWritePointer` | r3, r4 &{dwords, phys} | +13476 base, +13480 mask, +10820 wptr | ring[wptr..+3), +10820, MMIO 0x7FC80714 | `C0013F00 phys dwords` (ring) | `GpuRingWaitReadPtrWrap(dev, wptr, 3)` / RT or pump |
| 82110140 `GpuRingEmitBinSelectLo` | r3 = the replay state S (dev+0x2A3C) | S+0x28 flags, S+0x2C tile, S+0x16C last | ring, +10820, S+0x16C | `C0006200 value` (ring) | `820FD150(n=2)` / pump. No doorbell |
| 820FD150 `GpuRingWaitReadPtrWrap` | r3, r4 wptr, r5 n | fb+0x3C, +13480 | none | none | spins on 820F33E8 |
| 820FCB30 `GpuRingWaitForSpace` | r3, r4 end, r5 gen | fb+4 | none | none | spins until progress is past (gen-1, end) |
| 820F33E8 `GpuRingWaitProgress` | r3 &{dev, reason, lastFb0, t0} | byte +0x2A39 & 2, fb+0, +0x2A08, +0x2A70 | waitctx | none | 4-loop delay; hang trap after 0x1388 ticks of KTHREAD+0x58 (never advances, so it never fires) |

**B. Fences and waits**

| VA, name | Inputs | Reads | Writes | PM4 | Waits |
|---|---|---|---|---|---|
| 820FDD30 `D3DDeviceInsertFence` | r3 dev | +0x2A1C | +0x2A2C = S | via Flush | Returns S (the id about to close) |
| 820FDD68 `D3DDeviceBlockOnFence` | r3 F (dev from VdGlobalDevice) | | | | `GpuBlockOnFence(F, 6)` |
| 820FDD90 `D3DDeviceIsFencePending` | r3 F | +0x2A1C, fb+0 | | Flush if F == S | TRUE = pending: `(S-F) < (S-fb0)` |
| 820FD9A8 `GpuBlockOnFence` | r3 dev, r4 F, r5 reason | +0x2A1C, fb+0, +0x3290 pin | timing +0x4E38 / +0x4E40 | Flush if F == S (not while pinned: then it returns at once) | Spin. Reasons: 0 HSIO, 3 swap, 4 idle, 5 NotBusy, 6 BlockOnFence / PIX, the lock core passes its caller's, destroy 7/8/9/0xB/0xD/0xF/0x11 |
| 820FDCB8 `D3DDeviceBlockUntilIdle` | r3 dev | +0x2A1C, +0x2A74 | +0x28 | `000005C8 00020000` | `GpuBlockOnFence(S, 4)`, then spin while `dev+0x2A74 != 0` |
| 820FDE08 / 820FDE30 `BlockUntilNotBusy` / `IsBusy` | r3 res | res+8 | | | `GpuBlockOnFence(res+8, 5)` / `IsFencePending(res+8)` |
| 820F3988 lock core (IDA `GpuRingEmitResourceUse`) | r3 res, r4 reason (VB 0xA / IB 0xC / tex 0xE), r6 base, r8 addr, r9 size, r10 flags, sp+0x54 COHER value (VertexBuffer_Lock 820F3FA0 0x03000000, the texture lock 820FF468 0x02000000, IndexBuffer_Lock 820F40F0 0 with flags \| 2: no packet; generated bodies, transport-residue 2026-10-14) | res+8 or +0xC (flags & 0x1010), +0x2A08 owner | +0x2D18 deferred range (other threads), res+0 lock count += 0x100 | owner thread: `00000A31 v, 00010A2F size base, C0043C00 3 A31 0 80000000 8` (11 dwords) | `GpuBlockOnFence(fence, r4)` unless flags & 0x12 skip the packet |
| 820F4280 `GpuInvalidateMemoryRange` (InvalidateGpuCache) | r3 base, r4 size (dev from VdGlobalDevice) | +0x2A08 | +0x2D18 (non-owner) | owner: WAIT_UNTIL + the same 11-dword COHER block | |
| 820F3788 `D3DResourceDestroyDispatch` | r3 res | res+8 | frees backing (XMemFree 0xB1800000) / EDRAM bitmap | | `GpuBlockOnFence(res+8, 7..0x11)` before freeing |
| 82102DC8 `D3DQueryGetData` | r3 q, r4 out, r5 size | q+0x14 fence, slots (0xFFFFFEED = not written) | out | Flush if the fence is open | S_FALSE while pending or while a slot still holds the sentinel |
| 82108520 `D3DVsPatchFetchesInPlaceIfIdle` | (ENGINE_MAP "The shader bind") | vs cache+0x40 fence, fb+0 | | | Patches in place only if the ucode's last-bind fence passed |
| Binds 820F2A50 / 820F2C08 / 820F4E88 / 820F5218 / 82100310 | | +0x2A1C | **the OUTGOING resource's hdr+8 = S** (stamp at unbind) | | |
| Game: `GpuFenceReaper_82616208`, `GpuFenceReaperQueueEntry_82616180`, `GpuFenceReaperDrainAll_826160B8` | | entry fence (= res hdr+8 at rename) | | | Frees the old VB / texture block when `IsFencePending` is false (RW `LockDiscardRename` 8261BCF0 / 8261BDC8 / 8261BEE8). DrainAll blocks (shutdown) |

**C. Interrupt, callback, pump**

| VA, name | Inputs | Contract |
|---|---|---|
| 820FDA80 `GpuEmitCpuCallbackPackets` | r3 dev, r4 cursor, r5 flags, r6 cb, r7 ctx | id = `(flags >> 24) & 0x3F` or 4. Emits `[000005C8 00020000 if (flags & 0xC0FFFFFF) == 0]`, `0001057C cb ctx`, `00000578 id`, `C0043C01 13 pc\|2 id ~0 100`, `C0005401 id`, `0000045E id`, `C0043C01 13 pc\|2 0 ~0 100`, `0000057C 0BADF00D` |
| 820FDE38 `D3DDeviceInsertCallback` | r3 dev, r4 flags (cpu mask << 24), r5 cb, r6 ctx | Flush if needed, then 820FDA80. Callers: EndTiling (dead), EndZPass 0x8210E2A4 (tiling path only), MoviePlayerPresentFrame 0x824735D0. **0 calls in this title's flows [measured iter-467 p2]**: the intro movies draw through `RenderYuvVideoFrameTec0_8235CA48`, and cb 82471890 is in no trace of ours or Xenia's |
| 820FDBA0 `GpuInterruptCallback` (ISR) | r3 source, r4 dev | Source 1: `cb = pc+0x10` (traps on 0x0BADF00D), `cb(pc+0x14)`, then clears `1 << KPCR.cpu` in pc+0 under spinlock +0x2A18. Source 0 (if MMIO 0x7FC86544 & 1): +0x3B14++, countdown +0x3B1C -> pc+4 = 0 and +0x3B18 = +0x3B14, optional +0x3B10 callback |
| 82110B00 `GpuIsrKickReplayList` | r3 list | +0x2A94 = list; KeSetEvent(+0x2BDC + 0x38 * KPCR.cpu). It **must run as CPU 2** |
| 82110A30 `VdInterruptPumpWait` | r3 pump ctx (dev+0x2BBC + 0x38n) | Waits on ctx+0x20 (30 ms timeout on the D3D CPU: the present-pending poll, byte +0x2A3A & 2, clear here). Signalled: the replay driver. Created by 82110EB8 at device create (one per CPU bit of the flags, here CPU 2, priority +17) |
| 82110840 `GpuCommandBufferReplayDriver` / 82110460 `GpuReplayMarkerInterpreter` / 82110768 `GpuCommandSegmentEndSync` | r3 ctx / r3 record, r4 S / | Walk the list. Marker 0x82 -> 820FD218; 0x84/0x85/0x86 -> 82110140 bin selects + replay; 0xC0000000 -> EndSync (`dev+0x2A74 -= 1` under +0x2A78, or continue if a link appeared). 0x87-0x8C tiling: dead |
| 82110EB8 `GpuCreateBlockInterruptThreads` | r3 dev | Pump threads; one EVENT_WRITE_SHD per pump into the command buffer at init |
| 821102B8 `GpuWaitTilesComplete` | | Tiling only (marker 0x8A); never reached |

**D. Present**

| VA, name | Contract |
|---|---|
| 82112050 `D3DDeviceSwap` | As P2.2. Also: `memcpy(dev+0x3470, hdr, 52)`; D1GRPH_PRIMARY_SURFACE_ADDRESS (type-0 0x1844) when the front base changed (CPU MMIO 0x7FC86110 too in no-GPU mode); scaler / gamma emission gated on the system command buffer struct (P2.9 item 1); `dev+0x4E80 = sysbuf+4`, `dev+0x4FB8 = sysbuf+8`; the throttle; `D3DDeviceSwapEpilogStats_8210BBE8` (frame count +0x4E20, wait accumulators); `D3DDeviceCmdBufSegmentBeginReemitState_820F0048`. Callers: `82612080`, `82612220`, BlitAndSwap `82112864`, the pump `82110AA8` (re-swap, inactive) |
| 82111EC8 `GpuPresentEmitVblankGate` | Skipped for IMMEDIATE or the first present (`dev+0x3490 & ~0xFFF == 0`). Emits `00000579 1`, `C0043C00 13 pc+4\|2 1 ~0 100`, the callback block (cb 82111E38, flags 0: WAIT_UNTIL + CPU 2), `C0043C00 13 pc+4\|2 0 ~0 100`; byte +0x2A39 \|= 0x20 |
| 82111E38 `GpuPresentPacingUpdate` | ISR source-1 callback: P2.2 "Pacing". Reads MMIO 0x7FC86530 / 0x7FC86584; writes pc+4 = 0, +0x3B18, +0x3B1C, +0x3B20++ |
| 82112750 `D3DDeviceBlitAndSwap` | Movie thread (MoviePresentLoop_SelfPresent 0x824822EC): SetRenderTarget(+0x3464) -> gate -> Resolve to +0x3460 -> Flush -> restore viewport / scissor -> Swap |
| 82611FB8 / 82612090 RW present wrappers | rwdrv+0xA8 = rwdrv+0xA4; rwdrv+0xA4 = InsertFence(). Never waited on |
| VdSwap (kernel) | Xenia: type-0 `00054800` + 6 fetch dwords, `C0036400 'SWAP' fbPhys w h`, NOP fill; no write-back. XE_SWAP -> `counter_++`, present |
| 8210BB08 `GpuEmitPerfTimestamp`, 8210AB30 `PixPerfCounterQueueAdvance`, 82103B10, 8210B1A8 | PIX / perf only; dead in normal play [golden flags] |

**E. Z-pass, predication, tiling**

| VA, name | Contract |
|---|---|
| 8210DF38 `D3DDeviceBeginZPass` (r3 dev, r4 flags = 0 from 0x8277C650) | `+0x309C = 1`; `82110B30` (pin + record header + 0x84); byte +0x2A38 \|= 0x40; `+0x308C = (+0x28B4 >> 4) & 7`; byte +0x2A39 bit 0x80 = !(flags & 1); `+0x10 \|= 8`; `+0x3088 = -1`; dirty `+0x00..0x27 = ~0`; scissor tile rects. No PM4 of its own |
| 8210E1A0 `D3DDeviceEndZPass` (r3) | Flush (the last bracket record), 0x85, byte +0x2A38 = (x & 0xBE) \| 1, `+0x10 \|= 8`; with bit 0x80: `D3DDeviceInvokeRenderPass_8210E458` (Flush, 0x86, kick with cpumask 0x04000000; returns 0x8007000E if the kick failed) |
| 82110B30 / 82110C78 `GpuReplayListBeginRecording` / `GpuReplayListEndRecordingKick` | P2.2. EndRecordingKick restores the command buffer state from +0x32F0.. if an allocation failed (the bracket is dropped) |
| 8210D400 `D3DDeviceSetPredication`, 82102010 / 82102228 / 82102448 / 82102608 (conditional survey / rendering) | Command buffer only: `SET_BIN_MASK_LO/HI` (+ `VIZ_QUERY`, reg 0x2293 for surveys); `+0x3090/94/98`, `+0x2A30/34`, byte +0x2A38 bit 1. No ring, no wait |
| 8210D588 / 8210DA98 Begin / EndTiling | Gated on the tile count `dword_8299AE10` (golden 0). **Dead in this game** |
| 82113010 `GpuBeginShaderConstantF4` | `ReserveCommandSpace(4*count + 5)`, NOP pad, `SET_CONSTANT` header with the `+0x3098` predicate, and **returns a pointer into command memory that the caller (Lion `827333E0`) fills afterwards** |

**F. Init.**
- `820F34C0 D3DDeviceCreate` stores the behavior flags (game: 0x04000001; `|= 0x04000000` when no CPU bit is set) at
  `+0x4FC0`.
- `82106010` registers the ISR (`VdSetGraphicsInterruptCallback(820FDBA0, dev)`) and the VdGlobalDevice cell.
- `820FDE90` sets up the ring and the init state block (ME_INIT is written straight into the ring at wptr 0..19).
- `82106BA0` is the hang trap. Unreachable: KTHREAD+0x58 never advances, in Xenia too.

**G. Movie.**
- `MoviePlayerPresentFrame_82473208`:
  - returns 0x8000000A when `player+0x170` >= 3, otherwise increments it;
  - copies Y/U/V into set `player+0x174` (of 3);
  - draws;
  - calls `InsertCallback(dev, 0x01000000 << curCPU, thunk 0x82471890, player)`. The thunk -> vtable slot 23
    `82472EB8` decrements `player+0x170`.
- `MoviePlayerWaitGpuDrain_82472D90` sleeps 50 ms until that count is 0.
- `MoviePlayerRenderStateTick_8235F488` calls **BlockUntilIdle on every playing tick** (0x8235F82C), because the
  single Y/U/V set is rewritten next tick by `RenderYuvVideoFrameTec0_8235CA48`.
- Shutdown-only BlockUntilIdle callers: `EngineRenderShutdown_822EB348` and `D3DReleaseDisplaySurfaces_8261FAE0`.

### P2.4 (a) What a native replacement of each must do

**The rule for every replacement: give the guest the same answers from the same words.**
- The guest never reads ring contents.
- It reads only the eight write-back words of P2.1, the MMIO registers and the device fields its own code writes.
  So a ringless path must:
  1. keep `dev+0x2A1C` exactly as the XDK moves it (+2 per close, odd);
  2. write `fb+0` (big-endian, monotone) when the consumer passes the close, never earlier than allowed (P2.5);
  3. run the guest ISR with the scratch words in place, at the point where the CP would reach the INTERRUPT;
  4. leave every CPU-side device field to guest code, or port it bit for bit.

**Per function.**

| Function | Ringless obligation |
|---|---|
| Kick check / Flush / SegmentFlush / FenceWritePacket (820FD8C0 / 820FD660 / 820FD580) | A close = a host FENCE item `{S}` appended to the record queue, then `dev+0x2A1C += 2` and `dev+0x3298/0x329C` as today. Keep the coherency item (the `dev+0x2D18` range, swapped to -1 atomically) as an INVALIDATE item before it. While pinned (inside a bracket) a close appends to the bracket list and does NOT move S. `GpuBlockOnFence(S)` must keep returning at once while pinned |
| Reserve / Allocate / BeginNewSegment / ReserveSpace / WaitForSpace (820FC910 / 820FCDF0 / 820FCF90 / 820FCBE8 / 820FCB30) | No guest-visible output except the cursor triple and fb+4. Ringless: no caller may remain (every emitter is a record after P1). The cursor stays on the sentinel of NATIVE_XDK_SPEC §3 item 2 and any remaining caller trips it. Deferred-fill callers (BeginVertices, `82113010`) need guest-writable staging (P2.6 hazard 1) |
| UpdateWritePointer / EmitBinSelectLo / WaitReadPtrWrap (820FD218 / 82110140 / 820FD150) | The ring writers. In P2-A (P2.8) they enqueue `{IB phys, dwords}` / `{BS value}` and keep `dev+10820` advancing mod mask; fb+0x3C reported consumed so no writer waits. In P2-B they are unreachable |
| IsFencePending / BlockOnFence / GpuBlockOnFence / BlockUntilNotBusy / IsBusy | Keep the guest bodies; they only read `dev+0x2A1C` and fb+0. The one native duty is that a Flush on the open id really closes it and that fb+0 then progresses on its own (no consumer stall behind a waiting producer) |
| BlockUntilIdle 820FDCB8 | Keep the guest body (WAIT_UNTIL becomes a no-op item). It needs fb+0 to reach S and `dev+0x2A74` to return to 0. In P2-B nothing increments `dev+0x2A74`, so the spin exits at once |
| Lock core / InvalidateGpuCache / DestroyDispatch | Keep the guest bodies. The COHER block becomes an INVALIDATE `{base, size}` item (host: drop mirrors over the range, roadmap 4.5); the deferred range rides the next close. The waits are already on resource fences |
| InsertCallback 820FDE38 / EmitCpuCallbackPackets 820FDA80 | A CALLBACK item `{cb, ctx, cpuMask, waitIdle}`. At the item the consumer writes pc+0x10 = cb, pc+0x14 = ctx, pc+0 = mask (the write-backs), runs `820FDBA0(1, dev)` as a DPC on each CPU in the mask (KPCR cpu = n, IRQL 2: today's `onInterrupt`), checks that pc+0 came back to 0, then writes pc+0x10 = 0x0BADF00D. Order: after every earlier item is consumed |
| VblankGate 82111EC8 / PacingUpdate 82111E38 | A GATE item `{arg}`: pc+4 = 1, then the CALLBACK item behaviour with cb = 82111E38 on CPU 2, then the consumer **holds every later item** until pc+4 == 0 (the vblank ISR or the callback clears it). This reproduces pacing exactly; it needs vblank interrupts at the refresh rate and MMIO V_COUNTER / height as the oracle has them (P2.9 item 2) |
| Swap 82112050 / VdSwap | Native Swap (P1 row 10): every CPU-side device write of the body (the +0x3470 copy, +0x4E0C, +0x4E80/0x4FB8, the scaler / gamma state, +0x34D0, the flags), a SWAP item `{fetch constant, fb, w, h}`, the display-register items, the close (F(N)), then **the guest's own throttle call `GpuBlockOnFence(F(N-1), 3)`**. No extra wait; F(N) must not complete before the consumer has issued the present of N |
| BeginZPass / EndZPass / InvokeRenderPass / BeginRecording / EndRecordingKick / the pump (8210DF38 / 8210E1A0 / 8210E458 / 82110B30 / 82110C78 / 82110A30..) | In P2-A all guest; the pump re-emits through the two ring writers. In P2-B: BeginZPass's device writes as today, the pin (`dev+0x3290/94`, read by GpuBlockOnFence and 820FCBE8) set and cleared at the same points, a host BRACKET list that the consumer runs twice (`binSelect = 0x15555555`, then 0x2AAAAAAA, then 0xFFFFFFFF), re-testing each predicated record (native/n0 already does this). `dev+0x2A74` stays 0; the pump thread just sleeps (its 30 ms poll is guest code and harmless) |
| QueryGetData / Issue (82102DC8 / 82102BC8) | ZPD / EXT items that write the slot words (Xenia's values) before the fence that covers them completes (P2.6 hazard 7) |
| Movie (82473208, 8235F488) | CALLBACK items as above (CPU = the issuing CPU, `0x01000000 << curCPU`); BlockUntilIdle per tick (above). 82473208 is unused in this title's flows (row 820FDE38), so no CALLBACK item is needed today |

### P2.5 (b) The progress model a native consumer must expose

| Counter | Definition | Who moves it |
|---|---|---|
| **P (produced)** | The last closed submission id (= `dev+0x2A1C - 2` after a close) | The producer (guest thread), at a close |
| **C (consumed)** | The last submission whose every item the consumer has applied: guest memory read or snapshotted, host commands recorded | The consumer thread |
| **R (retired)** | The last submission whose host GPU work has finished (`ID3D12Fence`) | Host GPU completion |

**Guest-visible mapping.**
- `fb+0 = C`, except that a submission carrying a CPU-readback item is held until R covers it. CPU-readback items
  are: query slots, a resolve destination the CPU reads (strand B's readback list), and CPU-visible readbacks of the
  front buffer. That is Xenia's semantics: EVENT_WRITE_SHD at translation [xenia].
- `fb+4` = the progress word of the last consumed close, written in P2-A only.
- `fb+0x3C`: P2-A writes the queue's consumed index; P2-B never writes it.
- `pc+0 / +4 / +0x10 / +0x14` follow the CALLBACK and GATE items exactly as the scratch write-backs would.

**Ordering.**
1. `fb+0` only grows, and only in close order.
2. `fb+0 >= S` implies every CALLBACK / GATE item before that close has run.
3. A FENCE is never written while a GATE before it is still closed. This is the throttle: Swap(N+1) waits for
   F(N), which follows frame N's gate and swap.
4. Every close must complete eventually without the producer doing anything else. The guest spins on fb+0
   (`820F33E8` is a pure spin), so a consumer that waits for more producer work deadlocks.

**Batching.** The executor finding above still holds: hand off per close, not per draw. The FENCE item is the
natural batch boundary.

**What each guest-visible value corresponds to:**

| Guest fence | Meaning on the native side |
|---|---|
| `dev+0x2A1C` (S) | The id the next close will publish; stamped into outgoing resources (hdr+8), queries (q+0x14), resolve destinations (+8 / +0xC) and the VS patch cache (+0x40) |
| InsertFence's return (rwdrv+0xA4) | S at that point; never waited on |
| `dev+0x34D0` (swap fence) | F(N) = the close right after the SWAP item. Its completion is the throttle |
| Resource hdr+8 | The close after the resource's last bind. Completion means the consumer has read that resource's memory |
| q+0x14 | The close after the query's End. Completion means the slot words are written |

### P2.6 (c) Hazards: guest data reused only behind fences

The rule: before the consumer publishes `fb+0 >= F`, every guest byte that an item before F references must be read
or snapshotted, and every guest-visible GPU result before F must be written. Hazards, each with what the guest
reuses and when:

1. **Deferred-fill pointers.** BeginVertices returns a payload pointer the caller fills before EndVertices (L01).
   `GpuBeginShaderConstantF4_82113010` returns a pointer the caller fills after the call (L21). Today both point
   into the segment tail. Ringless:
   - give out a guest-writable staging block from a host-owned guest region;
   - seal the record at EndVertices / at the next device entry;
   - recycle the block only after the consumer has applied the record.
2. **Data the XDK copies into the ring.** IM_LOAD_IMMEDIATE ucode (P1 rows 1+2: the record already carries the
   patched bytes), DrawVerticesUP vertices (row 6: today a tail allocation; P2-B must copy them into the record or
   into staging under rule 1), and the resolve rectangle written inline behind the resolve packet (strand B's
   register snapshot carries it).
3. **VB / IB / texture memory freed behind fences.** The RW reaper frees the old block once `IsFencePending(hdr+8)`
   is false. DestroyDispatch frees physical backing after `GpuBlockOnFence(hdr+8)`. The lock core lets the CPU
   rewrite a non-DISCARD buffer after hdr+8 passes. Ringless keeps this safe only if the consumer reads (Xenia:
   uploads into shared memory) every range an item references while applying it, before the covering close.
   **Forbidden:** a lazy host texture / buffer cache that reads guest memory after that point, or a second thread
   (the `SR_NATIVE_EXEC` executor) running behind the fence writes. Native/n0's executor already defers fences as
   queue items; keep that.
4. **VS ucode patched in place.** `82108520` rewrites the shader object's ucode once its last-bind fence passed.
   Pointer IM_LOAD records read the ucode at apply time, so they are covered by rule 3.
5. **Command-buffer memory (P2-A only).** The 2 MB buffer is reused through fb+4. The Z-pass pin protects bracket
   segments until the replay, and bracket segments have no fence tail, so their memory retires with the first
   post-bracket close. A P2-A queue must not report fb+4 past a segment the consumer has not applied.
6. **Movie planes.**
   - In-world FMV: one Y/U/V set rewritten every tick behind BlockUntilIdle.
   - The XMV player: a ring of 3 sets behind the InsertCallback count, cap 3.

   The consumer must have uploaded a set before BlockUntilIdle's fence completes, or before running the callback.
   If player+0x170 never falls, PresentFrame returns 0x8000000A forever and `MoviePlayerWaitGpuDrain` spins.
7. **Query slots.** `D3DQueryGetData` treats 0xFFFFFEED as "not written" and returns S_FALSE. Our CP writes no
   ZPD / EXT reports today (P2.9 item 3), so any query the game issues never completes. P2 must not make this worse:
   the ZPD / EXT items write the slots before the covering fence.
8. **The front buffer.** One front buffer; the throttle orders frame N+1's resolve after frame N's swap. A single
   in-order consumer keeps the present copy ahead of the next resolve automatically.
9. **The Z-pass replay.** The bracket list must stay readable until both passes ran. Native/n0's arena keeps half
   its size as history for exactly this. In P2-B the bracket list belongs to the consumer and is freed after the
   colour pass.
10. **Spinlock and chain count (P2-A).** `dev+0x2A74` is only partly guarded by the real spinlock `dev+0x2A78`
    (SR_KE_SPINLOCK, default ON): 820FD800 reads it WITHOUT the lock and, when 0, publishes through 820FD218 and
    adds its increment with no lock held; when non-zero it takes the lock, re-checks, and chains the segment and adds
    the increment under the lock (or, drained meanwhile, releases it and goes direct); EndSync 82110768 decrements it
    under the lock. (Corrected 2026-10-14, as the BRACKET build note: this item said the count was guarded by the
    lock.) The IDA comments at 0x820FD840 / 0x82110788 calling the lock a no-op are stale. A host writer must never
    touch `dev+0x2A74` itself.

### P2.7 (d) Oracle plan: prove the ringless path gives the guest identical answers

The fence values are timing-dependent on the console too. What must be identical is (i) the guest-visible answers
under a deterministic schedule, and (ii) frames and the per-draw ledger. The invariants must hold under any
schedule.

1. **Instrument first (observe-only, gate `SR_P2_TRACE=1`):** a `[P2_FENCE]` trace. Per event it records:
   - kind (close, complete, query, wait-begin / end, callback, gate, swap, bin select);
   - S, fb+0, the queried F, the result, the reason, the thread, the present number;
   - for closes, the item count and the XDK byte count.

   Hooks:
   - 820FD580 (close);
   - the consumer's fb+0 write;
   - 820FDD90 / 820FD9A8 / 82102DC8 / 82108520 (answers);
   - the ISR calls;
   - 820FD218 / 82110140 (ring items).

   Record the counters "items produced" and "items consumed" per kind per present. **Take a baseline on the current
   ring path before any P2 code exists.**
2. **P2-A proof (host IB queue, segments unchanged).** The ordered `{IB phys, dwords}` / `{BS value}` sequence per
   frame must be **identical** between the ring path and the queue path in the same roll:
   - `SR_AB_TOGGLE=SR_NATIVE_RINGLESS,SR_AB_PRESENT=1`;
   - switch at the present after Swap's throttle, with the CP drained (rptr == wptr);
   - `dev+10820` keeps advancing in both modes, so the switch is seamless.

   Fence values are identical too: same closes, same S.
3. **P2-B proof (segmentless).** Closes now happen only at the explicit flush points, not at the threshold kick checks,
   so S values differ from the ring path. Compare relations, not values:
   - **(a) Sync mode `SR_P2_SYNC=1` (diagnostic):** every close drains the consumer before the producer continues.
     Every answer is then deterministic in both paths: `IsFencePending(F)` is FALSE for every closed F, the open id
     flushes, `QueryGetData` is ready at the first call after its close. Compare the per-present sequence of
     (call site, answer) pairs from `[P2_FENCE]` within a frozen pass: they must be identical ring vs ringless.
   - **(b) The frozen-pass A/B** (the roadmap's P1 method) with the transport toggled per present at the throttle
     point. The per-draw ledger (`SR_DRAW_LEDGER`) and the pixels of consecutive presents must be identical. The
     toggle needs both transports resumable: in ringless presents the cursor triple stays on the fresh segment
     Swap's BeginNewSegment left. If that proves infeasible, use per-roll A/B with the dense freeze (`flicker_gate`
     freeze mode) across two rolls.
4. **Invariants, checked on every P2 roll:**
   - fb+0 monotone and never ahead of C;
   - produced == consumed per kind at each present;
   - every CALLBACK ran exactly once;
   - pc+0x10 == 0x0BADF00D at every present;
   - pc+0 == 0 outside a callback;
   - `dev+0x2A74` == 0 at every present (P2-B);
   - no `[GPU_HANG]`.
5. **Pacing, against the oracle.** Vblanks per present (`dev+0x3B14` deltas), the gate count (`dev+0x3B20` =
   frames - 1) and the throttle wait time (`dev+0x4E4C`, which SwapEpilogStats snapshots) must match between ring
   and ringless. Their distribution should match Xenia (golden gameplay: ~2.48 vblanks per frame; menu ~2.0).
6. **Unchanged gates:**
   - replay 9/9: the replay fixtures run no guest XDK and must not move;
   - whole-run sheets every era;
   - the flicker gate (dense freeze, exact);
   - CPPM4 split for every new item kind;
   - forcing audit 2: P2 overrides are platform-fix rows in OVERRIDES.json.
7. **Records produced = consumed** is also the 0-PM4 acceptance: `[PM4_COV]` must read 0 packets per frame in
   P2-B, except the replay fixtures.

### P2.8 Recommended staging

- **P2-A, ringless primary ring.** Override only `GpuRingUpdateWritePointer_820FD218` and
  `GpuRingEmitBinSelectLo_82110140`:
  - they enqueue `{IB}` / `{BS}` items to a host queue the CP thread consumes;
  - `dev+10820` and fb+0x3C stay consistent;
  - everything else stays guest: segments, fence tails, kick blocks, the pump, the bracket replay.

  Small, and the oracle is exact (P2.7 item 2). It removes the ring, the doorbell, RPTR and the 4 unstamped packets
  per frame. It does NOT remove the PM4 inside segments: the fence tails, coherency blocks, kick / gate blocks, the
  VdSwap block and the remaining P1 markers.
- **P2-B, segmentless.** The producer appends records straight to host lists, and the effect kinds of P2.4
  (FENCE, INVALIDATE, CALLBACK, GATE, SWAP, BS / BRACKET, ZPD / EXT) replace the last PM4. The pump and the
  command-buffer machinery fall idle. This is where `[PM4_COV]` reaches 0. Prerequisites:
  - P1 complete (no guest emitter left, sentinel tripwire at 0 hits);
  - native Swap (row 10);
  - native bracket;
  - the P2.9 divergences fixed so that pacing matches the oracle first.
- **One P2-B design choice: close granularity.** Without segments there is no threshold kick, so closes happen only
  at the explicit flush points (at least two per frame: InsertFence and Swap). This is correct: every guest answer
  stays valid, because resources just read busy until the next close. But it changes behaviour that the game can
  see:
  - `IsBusy` and `LockDiscardRename` see more resources busy, so they rename more;
  - every S value differs from the ring path.

  The alternative is a virtual cursor: add each record's XDK byte length to an integer and close where the XDK
  would. That keeps the ring path's S sequence exactly, and with it the exact fence-trace oracle (P2.7 item 2). It
  needs only the length bookkeeping, not the padding. Recommendation: build the virtual cursor first, as the proof
  mode, then measure whether the coarse mode changes anything (renames per frame, pixels) before choosing.

### P2.9 Divergences from the oracle found on the way (platform bugs, outside P2)

Status of each item (iter-467). The proof rolls ran from the snapshot of exe 98054cc00c58 (commit 20e613b7) in one
slot hold:
- the replay check (9 traces, gates on vs off): 9 of 9 SAME;
- R1, a gameplay perf roll on the console path (`SR_HIGHFPS=0`) with the in-roll A/B of item 2;
- R2, the same roll under `SR_HIGHFPS=uncapped`;
- R3, a whole-run `check_roll` with all three gates, the S1 dry CHECK and `--flicker`.

Paths: `archive/v1_checkroll/p2fix1` in the main tree; `archive/v1_p2/{r1_scan_ab,r2_scan_ab_hf}` in the
`research/p2` worktree.

**Bake proof on dev.** Exe abf16a2e24b2 is `research/p2` e91ed18c: the bake of items 1 and 4 merged with dev
4e0f4375, which carries strand B's surface table. It was proven in one slot hold:
- **Replay.** 9 of 9 SAME against the dev exe b5ddf7226ad0 (built at 264c7fe8).
- **Flicker gate, DEFAULT env.** PASS: span 0 of 358, freeze counted 12, bad [].
  - Path: `archive/v1_checkroll/p2merged/flicker`.
  - The span roll logged two observe-only `[GPU_HANG]` H1 episodes: 3 s flat in the CP's swap phase, each inside
    the synchronous 360-BMP present-dump window. That window took 57 s here, against 12-13 s in fg42 / p2fix1, and
    an archive-wide `find` of mine on D: was running at the same time. Host I/O, not the guest. The freeze roll had
    no episode.
- **r6.** The natural title roll below. With no env set for items 1 and 4, it logged
  `[VD_RING] ... VA 0xBEF2E000 (from the argument; no claim)`, no legacy ring line, and `[VD_SYSBUF] ... xenia=1`
  with nz8 0/8192.

1. **`stub_VdGetSystemCommandBuffer` wrote nothing.** FIXED, `SR_VD_SYSCMDBUF`, baked default ON (`=0` is the A/B).
   - **Defect.** Xenia zeroes 0x94 bytes at r3 and stores 0xBEEF0000 / 0xBEEF0001 at r3 / r4 [xenia
     `xboxkrnl_video.cc:353-357`]. Without that, Swap copied stack garbage into `dev+0x4E80` / `dev+0x4FB8`, and the
     scaler / gamma emission is gated on those words [IDA].
   - **Witness.** `SR_VD_SYSBUF_LOG=1` logs `[VD_SYSBUF]` from VdSwap.
   - **Off (R1).** Descriptor +4 = 0x500 and +8 = 0x300 early, then +8 = 0xBF800000; `dev+0x4FB8` = 0xBF800000.
     +8 was non-zero in 6111 of 6144 sampled swaps. Swap emitted display-register PM4 from that garbage about every
     frame: `[PM4_COV]` pages 0x18 / 0x19 had 7721 / 13793 packets over about 6400 swaps.
   - **On (R3).** +0 = BEEF0000, +4 = +8 = 0, r7 = BEEF0001, non-zero 0 of 14336. Pages 0x18 / 0x19 had 5 / 263
     packets over 15872 frames.
2. **MMIO V_COUNTER (0x7FC86530) and the viewport-size register (0x7FC86584) read 0.** FIXED, `SR_VD_SCANOUT_REGS`,
   baked default ON (`=0` is the A/B). `vdScanoutRegsTick` runs in the vblank thread before the source-0 ISR.
   - **Defect.** `82111E38` released the vblank gate at once (line% = 1). Xenia (a growing V_COUNTER, height 0x2D0)
     always waits one more vblank [xenia `graphics_system.cc:273-295`, frame limiter `:143-144`].
   - **Console path.** V_COUNTER += 720 per vblank, and the viewport reads 0x050002D0.
   - **`SR_HIGHFPS` port feature.** Scanline 0, so the gate still releases at once and the feature's pacing is
     unchanged.
   - **Pacing.** Gameplay on the console path (R1, paired in-roll A/B): fps 30.00 both ways, delta +0.015 ± 0.017;
     frame_p50 delta -0.033 ± 0.051 ms. That is 2.0 vblanks per frame both ways. The golden Xenia capture runs at
     about 2.48 vblanks per frame, which is Xenia's own speed, not this gate.
   - **`SR_HIGHFPS=uncapped` (R2).** 123.65 fps, delta -1.1 ± 2.6 (noise; inert as designed).
   - **Intro movies and title (r6, the bake proof).** Natural console path (`perf_run --scenario title --config
     player`, `SR_HIGHFPS=0`, 300 s), merged exe abf16a2e24b2, in-roll A/B over 145 windows / 70 pairs:
     - fps +0.089 ± 0.560 (paired); frame_p50 +0.025 ± 0.037 ms; neutral.
     - Intro-movie windows: fps 28.28 on / 28.01 off. Title windows: 29.85 on / 29.53 off.
     - Path: `archive/v1_p2/r6_title_ab` in the `research/p2` worktree.
   - **Native note.** Answering display-register reads is emulation (section 2, "not native"). This is oracle
     parity for the emulation path. The P2-B GATE item (P2.4) replaces the guest's reads in 82111E38. The audit lists
     the writes as ALLOW (`fn:vdScanoutRegsTick`, next to `fn:vdVblankThread`).
3. **No EVENT_WRITE_ZPD sample-count reports and no EVENT_WRITE_EXT (0x5A) case** in `gfx/pm4_engine.cpp`. Query
   slots keep 0xFFFFFEED [xenia `pm4_command_processor_implement.h:921-1047`].
   - **Draft.** Xenia's fake mode as `SR_CP_ZPD_FAKE`, default OFF: the 0x5B fallback plus a new 0x5A case, with a
     census `zpd=N (fake N) ext=N`. It compiles clean.
   - **Routing.** Not committed here. The diff (`archive/v1_p2/zpd/p2_zpd_ext.diff`, against dev 74dc5e16) went
     to strand B for sign-off and to Fable to land.
   - **INTERIM ONLY (see the note under the P2 CONTRACTS heading).** The native end state is real host D3D12
     occlusion queries on strand B's target path.
4. **U1, the forcing-audit open defect: `gpu_ring_alloc_fix` `sub_82467F40`'s pre-claim heap route.** RETIRED to a
   master-gated diagnostic; `SR_VD_RING_ARG` is baked default ON.
   - **Fix.** `VdInitializeRingBuffer` takes the ring from its argument, as Xenia's
     `GraphicsSystem::InitializeRingBuffer` does: VA = 0xA0000000 + (r3 & 0x1FFFFFFF), size 1 << (r4 + 3). This uses
     the real `MmGetPhysicalAddress` (iter-463, `SR_MM_PHYSADDR`).
   - **Effect.** `Hr1Call` counts every bit-31 request as past the claim, so `SR_GPU_PHYS_ROUTE` runs the guest's
     own `sub_82468008 -> MmAllocatePhysicalMemoryEx` path from boot.
   - **The legacy heap route** is now `&& forcingAllowed()`. `SR_VD_RING_ARG=0 SR_ALLOW_FORCING=1` restores it.
   - **Proof (R3).**
     - `[VD_RING] ring phys=0x1EF2E000 bytes=0x10000 -> VA 0xBEF2E000` matches the first doorbell's ring, and no
       `[GPU_RING_ALLOC]` / `[RING_BASE] Deferred` line appears.
     - The whole run passes. Generation 0 (`run.log`) covers boot, intro movies, title, main menu, MODS and
       RESTART NOW. Generation 1 (`run_r1.log`, the relaunched child) covers title, NEW GAME, the HUD at +59 s
       and 240 s of tutorial gameplay. (Corrected 2026-10-09: an earlier version of this line said the run did
       not reach gameplay; the two logs had been read the wrong way round.)
     - The S1 dry CHECK in generation 1, gameplay included: `draws=8388608 ... | BAD bytes=0 len=0 dev=0`.
     - The flicker gate (gameplay) passes: span 0 of 358, freeze bad [].
   - **Audit.** `audit_forcing.py` UNGATED 2 -> 0. The two sites (the memset and the `ctx.r3` return) are now
     MASTER, and nothing new appears.
   - **Registry.** `docs/OVERRIDES.json` `sub_82467F40`. Retire: delete the route after a clean roll set on the
     baked default.
5. Minor (open):
   - our `writeReg` never sets bit 31 of COHER_STATUS_HOST (the match result is the same: the game's mask is
     0x80000000);
   - rptr is written back per descriptor, not per batch;
   - `VdInitializeScalerCommandBuffer` returns 0 copied dwords (Xenia: the NOP count);
   - the legacy (non-async) VdSwap path writes fb+8 and sets byte `dev+0x2A3A` bit 1 (golden: 0 and clear). That
     bit enables the pump's 30 ms re-swap.

### P2.10 Open questions

- Does the game issue occlusion / extent queries in gameplay (the `zpd=` field of the `[GPU_CP]` census line;
  flare / HUD glow)? Item 3 of P2.9 decides how urgent ZPD is.
  - **Partly answered (iter-467).** No EVENT_WRITE_ZPD (0x5B) and no EVENT_WRITE_EXT (0x5A) packets appeared in
    any of the four frame traces checked (`archive/v1_p2/xtr_opcodes.py`):
    - the Xenia gameplay frames 25023 (gp1) and 28050 (gp2);
    - the menu frame 9415;
    - our live gameplay frame `lg1`.

    Each frame has EVENT_WRITE_SHD 20-28 and EVENT_WRITE 31.
  - Our R3 whole run (boot to menus) logged `zpd=0`.
  - The only game callers of `QueryGetData` are `OpticalFX_DrawOcclusionFadedFlare_sub_82721118` and
    `HudGlowOcclusionQuery_sub_82304058`, through `sub_82779A48`. Queries are therefore scene-specific: a sun or
    flare in view. Testing them needs a flare scene (Q11), not an arbitrary gameplay frame.
  - The whole-run census (RE-PLAN, "Whole-run census (2026-10-09)") counted 0 ZPD and 0 EXT in every era. That
    covers the intro, title, menus, opening FMV, 210 s of tutorial gameplay and 125 s of Warworld.
- The relative cost of P2-A's IB queue against today's ring parse is unmeasured. It is the first in-roll A/B.
- Whether the frozen-pass A/B toggle (P2.7 item 3b) can resume the command-buffer state cleanly in P2-B needs a
  prototype.

- **vpkd3d128 type 5 (FLOAT16_4) rounding (opened iter-467 p2, scoped per the coordinator's three-step method).**
  XenonRecomp's lowering truncates the mantissa, keeps half denormals and saturates above 65504; Xenia's
  `EmulateFLOAT16_4_RoundToEven` rounds to nearest even, flushes denormals and saturates from 0x47FFE000. On the
  44 synthetic edge vectors of `gen_vpk_f16_cases.py` they agree 18/44 (fmt 7 of `sub_8210C090`). Whether that is
  LIVE depends on the production inputs, which the observe-only census `kernel/observe_vpk_f16.cpp`
  (`[VPK_F16_4]`, player-profile tag) records at both sites of the binary:
  - `sub_8210C090` fmt 7 (`k_16_16_16_16_FLOAT` clear colour): **0 calls** in the part-3 rolls (intro, title, custom
    menu, opening FMV, 60 s tutorial, ON and =0) - no trace of the 44 binds a fmt-7 RT, the fast clear skips 64-bpp
    formats, and every resolve clear's source is fmt 0 / 4;
  - `sub_823AE810` (the float -> half loop under `TexEncode_Float16x1/x2`, reached only through the D3DX surface
    codec: `TextureResHandler_Load_822EE690` -> `TextureCreateFromImageMemory_822EE4C0` ->
    `D3DXCreateTexture2DFromImage` -> `Texture2DFillTiledFromMemory` -> `D3DXFilterSurfaceDispatch` ->
    `CreateSurfaceFormatCodec_823AD940`): **0 calls** in the same rolls and in the Warworld arena roll
    (`archive/v1_p2/vpk/c0_arena`: the dev-tool level skip through the tutorial and the level loads to WARobj01,
    60 s in the arena, 3,604 player-profile log lines, no `[VPK_F16_4]` line; the same tag printed 44 fmt-7
    calls in the golden-call harness run, so the path is live). A float16 destination needs a float16 image file.
  - **Verdict: latent (step 3)** - 0 production inputs at either site over boot, intro, title, menus, the opening
    FMV, the tutorial, the level loads and the arena. The fix goes into the emitter's type-5 case at the next full regen; strand
    B's `rs_colorPack` case 7 reproduces XenonRecomp's form today and moves in the same commit.

- **The XDK's own GPU-hang detection never fires here (latent; found by native/quit-drain, 2026-10-16; NOT fixed -
  a fidelity question).** Every XDK GPU wait - GpuBlockOnFence 820FD9A8, GpuRingWaitForSpace 820FCB30,
  GpuRingWaitReadPtrWrap 820FD150, GpuWaitTilesComplete 821102B8 - spins on `GpuRingWaitProgress_sub_820F33E8`, a
  pure spin (a 4-pass delay loop, then a progress test). Its timeout: `v5 = *(*(r13+0x100)+0x58)` - KTHREAD+0x58 of
  the current thread, read as a tick count - and `a1[3]` reset to it whenever the read-pointer write-back
  `*(*(dev+0x2A10))` changes (or `dev+0x2A70` is set while the device owner `dev+0x2A08` is this thread, per
  `sub_82468EF8` = KTHREAD+0x14C); after `v5 - a1[3] >= 0x1388` ticks without progress it calls
  `D3DGpuHangHandlerTrap_sub_82106BA0` (IDA: "fatal path, clears dev+10868") and returns 0, which ends the wait
  loop. In our build KTHREAD+0x58 never advances (IDA note at 820FDA28: "Today r13=0, so this reads [[0x100]+0x58],
  the same word for every thread"; with SR_KPCR the per-thread KPCR's KTHREAD is not ticked either), so `v5 - a1[3]`
  stays 0 and **the trap can never fire: a wait on a fence or read pointer the GPU never writes spins forever**
  (native/quit-drain's stranded render thread after the quit's ring cut; GPU_HANG_WD H0 is the only detector today).
  What the trap does (IDA, 82106BA0, read 2026-10-16): it prints a banner and calls `GpuHangAnalyzeReport_sub_821068C0`
  with the hang callback `dev+0x3308` (13064); **with a callback** it RECOVERS - writes `next - 2` into the fence
  block (`*(dev+0x2A10) = *(dev+0x2A1C) - 2`: every issued fence reads complete), sets `dev+0x2A39 |= 3` (from then on
  GpuRingWaitProgress returns 0 at once: no wait spins again), clears the pump-work count `dev+0x2A74` and sets
  `dword_829AC0F0 = 15` when it was 0; **without one** it prints "The GPU is hung and can't be recovered without doing
  a cold boot" and executes `trap` (a debugger break / a crash on retail). Open: (1) does this game register the hang
  callback (`dev+0x3308` non-zero on the console - which path, and does our build set it)? - that decides between
  "recovers after ~5000 ticks" and "crashes"; (2) whether the console's KTHREAD+0x58 is a tick counter the scheduler
  advances (0x1388 = 5000 of what unit: ~805 ms per native_flush's note on the 5000-tick limit); (3) whether any of our
  paths depends on the trap firing - a wait that on the console times out and recovers (every fence then complete,
  the waits disabled) but here spins: the mode-C fences of a dropped or never-kicked segment (native_flush's "fence
  waits time out at 820F33E8's 5000-tick limit"), the swap throttle, BlockUntilIdle. Until answered, a lost fence is a
  silent infinite spin here and a bounded stall then a forced recovery (or a crash) on the console. Ledger draft
  (residual unknown): `_worktrees/quit-drain/archive/qd/ledger_unknown_hang_trap_draft.json`.

### P2.11 IDA changes made by this research

Renames, each keeping `sub_XXXXXXXX`:

| VA | New name | Old name |
|---|---|---|
| 820FDD90 | `D3DDeviceIsFencePending` | the inverted "GpuCommandBufferFencePassed" |
| 82110140 | `GpuRingEmitBinSelectLo` | "GpuEmitColorWriteMaskReg" |
| 8210E458 | `D3DDeviceInvokeRenderPass` | "ZPassFinishResolve" |
| 82110B30 | `GpuReplayListBeginRecording` | "GpuCommandBufferBeginFrame" |
| 82110C78 | `GpuReplayListEndRecordingKick` | "GpuRingKickCommandBuffer" |
| 82110B00 | `GpuIsrKickReplayList` | |
| 82110460 | `GpuReplayMarkerInterpreter` | "GpuTilingCommandInterpreter" |
| 82111EC8 | `GpuPresentEmitVblankGate` | "GpuPresentEmitCommandBlock" |
| 8210BB08 | `GpuEmitPerfTimestamp` | "GpuRingEmitFenceWrite" |
| 8210BBE8 | `D3DDeviceSwapEpilogStats` | "D3DDevicePresent" |
| 8210AB30 | `PixPerfCounterQueueAdvance` | "GpuScalerSwapQueueAdvance" |
| 820FDE38 | `D3DDeviceInsertCallback` | "GpuEmitKickWithCallback" |
| 820FDA80 | `GpuEmitCpuCallbackPackets` | "GpuEmitCommandBlock" |
| 822EB348 | `EngineRenderShutdown` | |
| 8261FAE0 | `D3DReleaseDisplaySurfaces` | |
| 8261FBD8 | `RenderDeviceShutdown` | |
| 82472D90 | `MoviePlayerWaitGpuDrain` | |
| 82616180 | `GpuFenceReaperQueueEntry` | |
| 826160B8 | `GpuFenceReaperDrainAll` | |

Contract comments were set at every function in P2.3 A-E. The comment at `820FCBE8` was corrected: the pin
`dev+0x3290` is set every gameplay Z-pass frame, not dead.

Older docs that name the renamed functions:
- `GpuRingKickCommandBuffer_82110C78`, `GpuEmitCommandBlock_820FDA80`, `GpuPresentEmitCommandBlock_82111EC8`,
  `GpuEmitKickWithCallback_820FDE38` and `GpuCommandBufferBeginFrame_82110B30` appear in earlier docs and in
  `kernel/gpu_command_processor.cpp`'s header comment. The VAs are unchanged.
- ENGINE_MAP 6d's "segment flush continuation" bullet called mode B (dev+12944) inactive "at the menu and in FMV".
  It is the Z-pass recording in gameplay (corrected there).

### P2.12 ZPD: occlusion queries as host D3D12 queries (design, ledger `p2.zpd`, 2026-10-14, branch `native/zpd`)

**Scope.** EVENT_WRITE_ZPD (0x5B) only. EVENT_WRITE_EXT (0x5A, the extent query, type 10) fired 0 times in every
measured era (below) and keeps `SR_CP_ZPD_FAKE`. Gate: `SR_NATIVE_ZPD` (default OFF): `1` = strict, `2` = latency
model (both below). The interim fake (`SR_CP_ZPD_FAKE`, P2.9 item 3) is skipped for a ZPD that the native path took.

**BUILT (native/zpd, 2026-10-14, `SR_NATIVE_ZPD` default OFF).** `gfx/native_zpd.{hpp,cpp}` (the logical queries,
segments, write-back, counters), `Pm4Engine::applyZpd` + the covering-fence hook in `applyEventWriteShd` and the
executor's `kOpEvShd` store, the marker `DrawBatch` in the drain, `zpd*` / `pm4ZpdFlushPartial` /
`pm4ZpdPartialFrame` in `gfx/d3d12_pm4_edram.cpp`, the `[NATIVE_ZPD]` line at the `[GPU_CP]` census.

**Astra round 1 (2026-10-15, MERGE AFTER FIXES; `archive/zpd/astra/round1.txt`) - every P1 / P2 fixed:**
- P1 the synchronous partial replayed its draws at present: `pm4ZpdFlushPartial` now CONSUMES the set (swapped out of
  `m_drawBatches` before it renders, as the render thread's handoff already did), so a swap with no draw after the
  covering fence renders an empty set, not the partial's batches a second time (item 5 "The covering fence").
- P1 the query counted helper draws: a query no longer stays open from its BEGIN marker; every GUEST draw between the
  BEGIN and END markers gets its own host segment, `BeginQuery` immediately before the draw call and `EndQuery` +
  `ResolveQueryData` immediately after it (`zpdDrawBegin` / `zpdDrawEnd` around `DrawInstanced` /
  `DrawIndexedInstanced` in `renderPm4Batches`). `pm4LoadTiles`' transfer triangles, resolves, clears and the B1b fill
  run outside every segment, and no segment can span a list close (the `clClose` / `clReset` split is gone) (item 5
  "Host queries").
- P1 strict released the fence after a failed flush: `cpZpdFence` now writes Xenia's strict fallback (the slot's last
  count, else 1; `PumpPendingRetire`, `command_processor.cc:1301-1365`) into every drained-but-unwritten query BEFORE
  the caller's store whenever the partial frame did not write it (failed, timed out, or no backend); the late real
  count is dropped (`superseded`). A strict fence is never published over the sentinel (item 5).
- P2 the publication could be read inconsistently: the slot write is now a single-word protocol - END+0 is the only
  "written" signal (END+4 / +8 / +C stay the sentinel), written last after every other word, and the BEGIN record
  repeats the sentinel words so they cancel; GetData reads S_FALSE or exactly `{n, 0, 0, n}` at every point (item 5
  "Write-back").
- P2 the MSAA justification was wrong: host targets are single-sampled at the guest PIXEL size times the scale (no
  supersampling of guest samples; `d3d12_pm4_edram.cpp` host RT creation), so counts are now converted to the
  console's samples from the draw target's MSAA mode: `guest = host * samplesPerPixel / area` (item 5 "Resolution
  scale and MSAA"). The HUD query's mode is recorded live (`spp1/2/4` on the census, `spp=` per query); equivalence
  is claimed only per mode the live check shows.
- P2 the difftest missed the production paths: `--native-zpd-test` now runs null_renderer's production `cpZpdFence`
  (the hook `gpu_command_processor` installs) against an attached `D3D12Backend` - the CP's `applyEventWriteShd`, the
  executor's `execPerformStores` store item, the synchronous partial, the render thread's partial handshake with 60 ms
  of GPU time and with a timeout - through the backend's own `pm4ZpdFlushPartial` / `pm4SubmitFrame` /
  `pm4RenderThreadMain` / `pm4PresentFrame` / `zpdMarker` / `zpdDrawBegin` / `zpdDrawEnd` / `zpdCollectNow`; only the
  device calls are a seam (`SR_DIFFTEST` `ZpdTestSeam`: the worker has no GPU). `tools/difftest/native_zpd_run.py`
  rebuilds the suite with each defect put back (item 10).
- Round 1 results (the worker, tree `zpd`, archive `_worktrees/zpd/archive/zpd`): `--native-zpd-test` 51 passed, 0
  failed (`native_zpd_test.txt`); `native_zpd_run.py` valid - all 8 defects detected (each failing its named
  assertions, 3 to 17 failures, the gate-off control and the checker's control passing), sources restored, rerun 51 / 0
  (`mutants/zpd_mutants.json`); the 82 other suites' exit codes identical to dev 4aa4f963's (63 exit 0 in both,
  `suites_compare_r1.json`); `build-rel` (SR_RELEASE_COMPOSITION=ON) builds; the Python checks as before
  (`py_r1/`: the PIL-less worker's two image tests and test_native_proof's umbrella `_worktrees/` path check only);
  `native_proof --plan zpd --dry-run` clean. Staged exe e8d6e21ed7a1 (`archive/zpd/binary_r1_e8d6e21ed7a1/`).
- zpd3 (2026-10-15, exe e8d6e21ed7a1): replay 9/9, freeze 26/26, CPPM4 26/26 and the gate PASS (the partial-replay
  fix holds), but no `[NATIVE_ZPD]` line in any roll: the player log profile dropped both tags (item 8). Fixed (both
  in `kMilestoneTags`, `viaPartial` / `viaPresent` added); worker: `--native-zpd-test` 54 / 0 (Z14 the player
  profile), `native_zpd_run.py` valid with 10 defects (the two tag removals added), the 82 suites identical to dev
  4aa4f963's, `build-rel` builds, test_native_proof's new tag check passes for every plan (only zpd failed it
  before). Staged exe 113e258b5fb2 (`archive/zpd/binary/`).

Offline proof (the worker, tree `zpd`, archive `_worktrees/zpd/archive/zpd`; round 1 numbers: item 10), before
round 1: `difftest --native-zpd-test` 28 passed, 0 failed (`native_zpd_test.txt`); all 82 other difftest
suites' exit codes identical to dev 4aa4f963's built in the same tree (63 exit 0 in both; `suites_compare_final.json`);
the release configuration (`build-rel`, SR_RELEASE_COMPOSITION=ON) builds; tools/perf/test_*.py as on every lane's
worker run (test_flicker_gate / test_frame_measurements need PIL there, test_native_proof's `_worktrees/` path check
needs the umbrella layout), audit_forcing 0, the override manifest unchanged; `native_proof --plan zpd --dry-run`
clean. The live proof is native_proof plan `zpd` (not run here).

**1. The guest lifecycle [IDA].**
- The game's only callers are `HudGlowOcclusionQuery_sub_82304058` (through `HudDrawOnScreenThreatMarker_8230DD28`:
  the glow of a distant on-screen threat marker) and `OpticalFX_DrawOcclusionFadedFlare_sub_82721118` (the sun
  flare). **The sun flare never runs (confirmed live, OUT OF SCOPE):** the saves lane (2026-10-14 23:58, exe
  2087ca4acdb4, the post-game seed `archive/saves/19_ch19_post_game`, port features on, 81 s of flight in sunny
  Metropolis) counted **0 calls of 82721118 against 4812 calls of its only caller**
  `VfxDeferredList_DrawAndClear_sub_828DA650` (about one per frame), which calls it only for an effect definition
  whose first word is 2 (an "optical" geoEffect); the VFX manager held 109 definitions (101 + 1 particle, 7 geo, every
  geo definition a contrail, first word 0), the chapter-10 roll 105 (98 particle + 7 contrail geo), and the only
  geoEffect list in the retail data is baseworl's GEOFX_DEFINITIONS (7 contrails; no AST defines `type="optical"`).
  So no proof can provoke the flare's query: every occlusion query the game issues is the HUD glow's. Evidence:
  `_worktrees/dev/archive/saves/19_ch19_post_game/flare_check.txt` (Frida call counters + the live definition
  census). Both reach
  the XDK through three 12-byte wrappers on `{?, query}` objects: `sub_82779A28` = `Issue(q, 2)` (D3DISSUE_BEGIN),
  `sub_82779A38` = `Issue(q, 1)` (D3DISSUE_END), `sub_82779A48` = `GetData(q, out, 16)`, returning `out[0]` on S_OK
  and -1 otherwise.
- The HUD (per marker slot `hud + 16 * (slot + 30)`: {key, query, pending byte, last result}): when the key matches
  and a query is pending it calls GetData; a result other than -1 is stored and `pending` cleared (no new query in
  that call). Otherwise it stores the key, Begins, draws ONE point (`D3DEmitDrawIndexed16_8231ADF8(1)`, the
  marker's world position, colour 0xFF000000, render states 0x8297B3AC = 3 then 7), Ends and sets `pending`. So a
  marker cycles query -> read -> query: one query per two calls. The return is 127 when the stored result is 0,
  else 255, or a distance ramp `255 * (1 - (far - d) / (far - near))` inside the per-type band; the caller uses
  `(int)(0.3 * that) << 24 | 0xFFFFFF` as the icon colour (ida_contracts `HudDrawOnScreenThreatMarker`).
- `D3DQueryIssueBeginEnd_82102BC8`, type 9 (occlusion), `q[36]` tiles (1, or `dev+12472` under predicated tiling):
  - BEGIN: for each tile slot `q[6 + i]`, the CPU writes 0xFFFFFEED to the slot's first four dwords (big-endian
    stores) + dcbf, then `GpuEmitTileSurfaceConfig_82101E08(dev, q, 1)`; `q[4] = 2`.
  - END: `82101E08(dev, q, 0)`; `q[4] = 1`; `q[5] = dev+0x2A1C` - the OPEN submission's id S, the query's fence.
  - `82101E08(dev, q, half)` writes, one tile: `00002208 00000006` (RB_MODECONTROL = 6), `00012000 00020000
    00000000` (RB_SURFACE_INFO / RB_COLOR_INFO), `00002325 <phys(slot) + 0x20 * half>` (RB_SAMPLE_COUNT_ADDR),
    `C0005B00 00000015` (EVENT_WRITE_ZPD, initiator 21), and dirties dev+0x10 / +0x18 / +0x20 so the next draw
    re-emits the render-target state. Several tiles: per tile a SET_BIN_MASK_LO (3 << 2i) + a predicated
    SET_CONSTANT of 0x2325, then the device's bin mask / select restored and one EVENT_WRITE_ZPD.
- `D3DQueryGetData_82102DC8`, type 9: S_FALSE (1) while `(S - q[5]) < (S - *fb)` (the fence has not passed; on
  `q[5] == S` it flushes first); then per tile, if the slot's first four dwords all still read 0xFFFFFEED it dcbf's
  and returns S_FALSE; else `out[0] += END.ZPass_A + END.ZPass_B - BEGIN.ZPass_A - BEGIN.ZPass_B` (each dword
  byte-swapped: the counts are little-endian), plus three more sums for the 16-byte form. `dev+0x2A39 & 2` answers 1
  at once (not this title's state).

**2. The slot [IDA + xenia].** 64 bytes per tile: the END record at `slot + 0x00`, the BEGIN record at
`slot + 0x20`, each `xe_gpu_depth_sample_counts` {Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B,
StencilFail_A, StencilFail_B}, little-endian (xenia `xenos.h:1570-1582`, `xenos_zpd_report.h:25-58`). The guest
presets only the END record's first four dwords; "written" = any of them changed. GetData reads the slot through
VA `0xC0000000 + RB_SAMPLE_COUNT_ADDR` (its `(((va >> 20) + 0x200) & 0x1000) + (va & 0x1FFFFFFF) - 0x40000000`); the
native path writes exactly that view (`guestVAToHost(0xC0000000 + slot)`, the guest loads' own window fold).

**3. What Xenia answers [xenia @0b4e6c44].** `ExecutePacketType3_EVENT_WRITE_ZPD`
(`pm4_command_processor_implement.h:958-1047`) reads RB_SAMPLE_COUNT_ADDR, classifies BEGIN / END by the record's
place in the 64-byte slot, and in the default mode `occlusion_query=fast` (`command_processor.cc:53-66`) runs REAL
host queries: BEGIN zeroes the BEGIN record and opens a host query segment (`BeginZPDReport`, `:919`; D3D12
`OpenZPDQuery` / `CloseZPDQuery`, `d3d12_command_processor.cc:5301 / 5369`; segments split at submission and render
pass boundaries and are summed); END closes it and, as the host result is not back yet, writes a SPECULATIVE value
at once - the last resolved non-zero delta of that END record, else 1 (`EndZPDReport`, `:1063`, `:1148-1164`); the
real count is committed when the resolve lands if the slot was not re-begun (`OnZPDQueryResolved` `:1267`,
`IsZPDReportCurrent` `:1401`). Counts are divided by the draw resolution scale with rounding
(`NormalizeSampleCount`, `:1407-1416`). `strict` waits for the resolve at the guest's next wait
(`PrepareForWait` -> `PumpPendingRetire`, `:840-849`, `:1301`), giving up after 2 ms / 16 polls with the cached delta
(`command_processor.h:70-74`). `fake` writes 100, 99 .. 81, 100 (the interim we ported). So Xenia's default shows the
HUD glow as visible (>= 1) whenever the real count is not back before the guest's GetData.

**4. What we answer today [ours, dev 4aa4f963].** `cpPacket 0x5B` writes the initiator only; the slot keeps the
sentinel (SR_CP_ZPD_FAKE default OFF). GetData returns S_FALSE forever, the HUD slot stays `pending` and keeps its
first result value, so a marker that has started a query never updates its glow again (0.3 * 127 = 38/255 alpha if
the stored result is 0, the "occluded" look, whatever the scene). Each such marker issues exactly one query in its
life, which is why the census counts are small: the chapter-saves lane (2026-10-14) saw `[GPU_CP] zpd=` 0 -> 2 in
MSTobj03 (the mega-meteor threat), -> 8 in metro event ME06 (chapter 3), -> 24 in ME10 (chapter 4); ext 0; idle loads
0. With real answers the HUD issues one query per marker per two frames while the marker is on screen.

**5. The native design.**
- **Transport (stream order, every CP mode).** `cpPacket 0x5B`, when the gate is on, classifies the record
  (`addr & ~0x1F`; END = `addr & ~0x3F`, BEGIN = END + 0x20, as Xenia) in `gfx/native_zpd` and pushes a MARKER
  command into the draw queue at its stream position (a `GuestDrawCommand` with `zpdOp` BEGIN / END + the logical
  query id, `hasDraw` false - the `clearOp` pattern). The drain turns it into a marker `DrawBatch`, in order between
  the query's draws, on whichever thread drains (the CP synchronously, or the `SR_NATIVE_EXEC` executor).
- **Host queries (per guest draw; Astra round 1 P1).** The BEGIN marker only opens the logical query on the list
  owner (`zpdMarker`: `m_zpdOpenId`); nothing is recorded there. Every GUEST draw the render loop (`renderPm4Batches`,
  render thread or CP) issues while a query is open gets its own segment: `BeginQuery(OCCLUSION, seg)` immediately
  before the `DrawInstanced` / `DrawIndexedInstanced` call and `EndQuery` + `ResolveQueryData(seg -> readback + 8 *
  seg)` immediately after it (`zpdDrawBegin` / `zpdDrawEnd`), against the draw's own depth target. So only the guest's
  rasterization is counted - the target preparation before a draw (`pm4LoadTiles`' transfer triangles), resolves,
  clears and the B1b fill pass all run outside every segment - and no segment ever spans a list close. The END marker
  closes the logical query (`endRecorded`); its count is the sum of its segments (Xenia sums segments too). A guest
  draw our renderer skips (no target, no pipeline) has no segment and counts 0. Heap: 4096 occlusion queries, one
  persistently mapped readback buffer; segment indices are a ring owned by `native_zpd` (a full ring = `poolFull`,
  more than 32 draws in one query = `overSegs`, no heap = `missed`: the query answers "visible" 1 when nothing else
  counted, as Xenia's exhausted pool does).
- **The covering fence (strict, `SR_NATIVE_ZPD=1`).** The query's fence is the close of the segment holding its END
  (InsertFence's close in the present sequence at the latest: P2.2 "Present and throttle" step 3, BEFORE the XE_SWAP
  of that frame). Our frame is recorded on the render thread only after XE_SWAP, so the host count cannot exist when
  the CP reaches that fence. Strict therefore does what the console's in-order GPU does implicitly: at the first
  EVENT_WRITE_SHD performed after a drained END (sync: `applyEventWriteShd` after its drain; executor: the
  `kOpEvShd` store item), the drain owner hands the set drained so far to the render thread as a PARTIAL frame (no
  scan-out, no present, no frame-epoch bump), waits until the GPU finished it (`waitForGpu`), reads the readback,
  writes the slot words, and only then performs the fence store. The words are therefore written strictly before the
  covering fence completes, by construction. The partial CONSUMES the set it renders on both paths (the render
  thread's handoff swaps it out; the synchronous path - the first frame, `SR_PM4_RENDER_THREAD=0` - swaps it out of
  `m_drawBatches` before rendering, Astra round 1 P1): the rest of the guest frame starts a new set, and a swap with
  no draw after the fence presents without rendering anything twice. When the wait fails (not ready, the render
  thread stopping, or `m_zpdFlushTimeoutMs` = 5 s without the partial finishing) or no backend is attached, strict
  still never publishes the fence over the sentinel: `fenceFallback` writes Xenia's strict fallback - the slot's last
  written count, else 1 (`PumpPendingRetire`, `command_processor.cc:1353-1360`: the cached delta, else 1) - into every
  drained END still unwritten, before the caller's store; the real count is dropped when it arrives (`superseded`),
  as Xenia erases the report. `fallback` counts them (0 in a healthy strict run). When no set is open since the last
  render (the END's set already went to the render thread or was presented), the partial is empty: only the GPU
  drain. Cost: one CPU-GPU sync per frame that ends a query (the CP waits for the GPU to finish the frame so far);
  measured by `flushes` / `waitMs` / `maxWaitMs`. The native meter's per-frame EDRAM counts see the partial as a frame
  of its own.
- **The latency model (`SR_NATIVE_ZPD=2`).** No partial frame: the fence store goes ahead, the queries it covers are
  marked `covered`, and the slot is written when the frame's GPU fence completes (checked after every present). The
  guest's GetData sees the fence passed with the sentinel still in place and returns S_FALSE - its own not-ready path
  (dcbf + S_FALSE), the "not ready yet" model this roadmap asked for in N5 - and gets the real count 1-2 frames later.
  Every such write counts as `late`. Zero sync cost; the A/B against strict prices the sync.
- **Write-back: a single-word publication (Astra round 1 P2).** GetData's "written" test is "any of END+0..+C is not
  the sentinel", and it keeps its FIRST load of END+0 for the 16-byte result's Total (82102DC8: `lwz r10, 0(r11)` at
  82102EC0, byte-reversed into out[3] at 82102FC4). Round 1 changed END+4 before END+0, so a GetData polling in
  between returned S_OK with a sentinel-derived Total. Now: BEGIN record = `{0, S, S, S, 0, 0, 0, 0}`, END record =
  `{n, S, S, S, n, 0, 0, 0}` (S = 0xFFFFFEED as the guest's own big-endian preset stores it; n little-endian in
  Total_A and ZPass_A, as Xenia's `WriteSampleCount`), END+0 stored LAST after a release fence. END+4 / +8 / +C never
  change, so END+0 is the only signal; every other word is final before it (x64 keeps stores in order, the generated
  loads are volatile and in program order); the S words cancel between END and BEGIN, so GetData answers exactly
  `{ZPass n, ZFail 0, StencilFail 0, Total n}`. A reader whose first load sees the sentinel reads S at +4 / +8 / +C
  in every state (S_FALSE); one whose first load sees n reads only final words. Counts saturate at 0xEDFEFFFE so the
  big-endian load of END+0 can never read 0xFFFFFEED. A query whose slot was re-begun since (a newer BEGIN on the same
  slot, the HUD's key change) is dropped (`stale`, Xenia's `IsZPDReportCurrent`). An END with no open BEGIN is
  `orphan` (nothing written, as Xenia strict). A BEGIN while another query is open abandons the old one (`abandoned`,
  Xenia: a new BEGIN implicitly ends the prior report).
- **Resolution scale and MSAA: the console's samples, with a non-zero floor (Astra round 1 P2).** The host render
  targets are single-sampled at the guest PIXEL size times `res_scale` per axis (`d3d12_pm4_edram.cpp`'s host RT
  creation: `RtKey::width()` / `height()` - guest pixels - times the scale; "existing targets remain single-sample"
  in the native image store); the `shX / shY` transfer remap moves EDRAM samples
  between layouts but rasterizes no guest sample. Round 1's "guest MSAA being host supersampling" was wrong. The
  console counts SAMPLES: a 2X / 4X target holds 2 / 4 per pixel (Xenia keeps the guest's sample count on its host
  targets - `d3d12_render_target_cache.cc:1844` - so its counts are samples, and `NormalizeSampleCount` divides only
  by the scale). So each segment carries its draw target's samples per pixel `spp` (RB_SURFACE_INFO bits 16-17: 1X
  1, 2X 2, 4X 4) and `n = S ? max(1, (S + area / 2) / area) : 0` with `S = sum(host_seg * spp_seg)`, `area = scale.x
  * scale.y`: a fully covered guest pixel answers `spp`, as on the console. Dividing keeps the console's units for any
  effect that compares counts. The floor differs from Xenia's plain rounding on purpose: 1-4 host samples at 3x round
  to 0, and a point or a thin sliver does not scale with area (a host point may cover one host pixel), so rounding
  would turn the HUD's visible marker into "occluded" - a verdict the console never gives a visible primitive. The
  floor moves no count by more than one guest sample. What is NOT claimed: sample-exact equality per guest sample
  position (a 4X pixel partly covered counts host coverage x 4 / area, not the console's per-sample coverage), and
  any mode the live check does not show: the HUD query's mode is recorded live (`spp1= spp2= spp4=` on the census,
  `spp=` on each `[NATIVE_ZPD_Q]` line); the HUD itself only tests zero / non-zero.

**6. The record / item form on the native path.** Today 82101E08's packets are PM4 residue the guest writes into the
segment (the transport census lists `82304058 HUD glow ZPD` under p2.zpd), and the CP parses them on every path:
the PM4 oracle path, S1 records (literal spans), segmentless, CPPM4. The native form is ONE implementation, as for
every kind: `Pm4Engine::applyZpd(record)` (this lane) is what `cpPacket 0x5B` calls; a native QueryIssue (N5,
`resource-sync-replace`) will produce a `kRkZpd {RB_SAMPLE_COUNT_ADDR value}` record applied through the same
method, CPPM4 form = `00002325 <addr>` + `C0005B00 00000015`. In P2-B's in-order consumer the FENCE item calls the
same covering-fence check before its stores (the consumer is then the drain owner).

**7. Behaviour under the A/B, the executor and CPPM4.**
- `SR_NATIVE_ZPD` changes guest-visible answers, so by the ab_toggle rule it is NOT an in-roll A/B feature: it is
  latched at startup; strict vs latency vs off are separate rolls.
- Other features' A/B windows are safe: the marker rides the draw queue (FIFO = stream order) and the covering-fence
  check runs where the store is performed - the CP in sync mode or an `SR_NATIVE_EXEC` OFF window, the executor when
  deferring - after the markers ahead of it are drained, whatever the window.
- CPPM4 (`SR_NATIVE_FLUSH_CPPM4`): the ZPD packets are literal PM4 there too; same path. The PM4 oracle path
  (`SR_NATIVE_FLUSH=0`) and the replay fixture: same CP code; the replay instance (`submitDraws` false) and an HLE-only
  run never start a logical query.

**8. Counters and log lines.** `[NATIVE_ZPD] <why> mode=strict|latency issued= ended= drained= resolved= written=
late= stale= orphan= abandoned= evicted= dropped= lost= badAddr= covered= fallback= superseded= poolFull= missed=
overSegs= flushes= flushFail= waitMs= maxWaitMs= segs= spp1= spp2= spp4= zero= nonzero= lastHost= lastGuest= live=
inflight= scale=XxY viaPartial= viaPresent= probes= depthKilled= psKilled= noRaster= lagAvgMs= lagMaxMs=` at every
`[GPU_CP]` census and the final one (cumulative values: count-safe by value, never by line count; the probe and lag
fields: item 11), plus `[NATIVE_ZPD]` once when the host heap is created, `[NATIVE_ZPD] draw #n` for the first 16
query draws (item 11), and `[NATIVE_ZPD_Q]` for the first 64 written queries (`id slot host spp guest area segs
via=partial|present lagMs` and `ref= raster=` when probed, `LATE` when late, `overflow` when a draw was unmeasured;
`FALLBACK` lines for the strict fallback). Both tags are in
`app/log.cpp` `kMilestoneTags`: every live roll runs `SR_LOG=player`, which drops an Info line whose tag is not
there (zpd3, 2026-10-15: no `[NATIVE_ZPD]` line reached any roll's run.log, five items failed with the queries
firing; `--native-zpd-test` Z14 now logs through the real Log in the player profile, and test_native_proof checks
every plan item's selected tag against the list). `segs` counts measured guest draws, `spp1/2/4` the segments by
their target's samples per pixel; `viaPartial` / `viaPresent` say who wrote each slot - the strict partial frame
(before the covering fence's store) or a present's collect (after its frame's GPU fence; under strict, a covering
fence no EVENT_WRITE_SHD hook saw). The live criterion, on the final census: `issued > 0`, `resolved + live >= ended` with `lost == 0` (every
query that ended came back), `written == resolved - stale - superseded`, and under strict `late == 0`, `flushFail ==
0`, `fallback == 0`, `segs > 0`, `missed == 0`, `overSegs == 0` (native_proof plan `zpd`, item `zpd_strict`).

**9. Known limits.** Multi-tile queries (predicated tiling) write only the tile the last RB_SAMPLE_COUNT_ADDR names,
as Xenia; the other tiles keep the sentinel (GetData S_FALSE) - not observed for the HUD. A list closed without
execution (`GuardDiscard`, an error path) leaves its segments' readback stale. The latency model can still hand a
re-issued slot the previous query's count in the window between the guest's Issue(BEGIN) preset and the CP parsing
that BEGIN (the slot sequence is bumped at the parse); strict cannot (the count is written before its own fence).
Sample counts follow host coverage times the target's samples per pixel, not per-sample coverage (item 5). EXT stays
fake. The sun flare's query (82721118) never runs in retail (item 1): out of scope.

**10. Proof.** Offline (the worker): `difftest --native-zpd-test` drives the generated guest bodies - Issue
82102BC8 through the wrappers 82779A28 / 82779A38 and GetData through 82779A48 and directly - against `native_zpd`
and the backend: the PM4 the guest emits (the record addresses); the slot words before (sentinel, GetData S_FALSE with
the fence passed = the hazard) and after (S_OK, `{n, S, S, S, n, 0, 0, 0}` / `{0, S, S, S, 0, 0, 0, 0}`, GetData
`{n, 0, 0, n}`); strict through the PRODUCTION covering fence (null_renderer `cpZpdFence` on a backend attached with
`setPm4LiveBackend`) on the CP path (`applyEventWriteShd`) and the executor path (`execPerformStores`' `kOpEvShd`
item), synchronously and through the render thread's partial-frame handshake with 60 ms of GPU time - each time the
partial frame's queries collected while the fence word has not moved, the store after; the swap right after the fence
with no draw between rendering an empty set; the query commands recorded in the order helper, BeginQuery, EndQuery +
resolve, helper; the publication checked after every one of its 16 stores with the generated GetData, and a model of
82102DC8's load order (validated against the generated body at every state) under every two-phase interleaving of
loads with stores, with the round-1 order as the checker's own control; MSAA (a 1X + a 2X draw, a 4X draw) against
`host * spp / area`; the strict fallback (a partial frame outliving an 80 ms timeout: the slot's last count before the
store, the late real count superseded; no backend: 1); the latency model, stale / orphan / abandoned, an unseen BEGIN,
no heap, over 32 draws, normalization and the saturation below the sentinel. The device calls are the one seam
(`D3D12Backend::ZpdTestSeam`, SR_DIFFTEST only: the worker has no GPU), so the placement of `zpdDrawBegin` /
`zpdDrawEnd` around the real `DrawInstanced` calls is by inspection plus the live census (`segs`, `spp`), not offline.
`tools/difftest/native_zpd_run.py` rebuilds the suite with each round-1 defect put back and requires its named
assertions to fail with the gate-off control (Z0) and the checker's control still passing: the CP hook removed, the
executor hook removed, the render-thread wait skipped, the fallback removed, the synchronous partial retaining its
set, a segment opened at the BEGIN marker, the round-1 publication order, the MSAA factor ignored, the zpd3 log tags
dropped, and (the zpd4 round, item 11: Z15 the probe on the seam, Z16 the point draw on WARP) a probe count leaking
into the answer, a probe on every draw, probes taking answer segments, the 0-answer classification swapped, the
sprite radius halved; then the sources restored byte for byte, rebuilt and rerun clean. Live (the orchestrator, native_proof plan `zpd`): the chapter-4 seed's
flight with threat markers in view; `resolved == ended`, `late == 0`, `fallback == 0`, the HUD query's `spp`; a
screenshot pair vs Xenia `occlusion_query=strict` at the same scene (the marker in view, then behind a building).

**11. The zpd4 round (2026-10-15, exe 113e258b5fb2): every answer 0, the strict cost, the freeze roll.**

*(a) Every query answered 0.* The censuses: `zero` = `written` in every roll (strict `on` 2393, `lat` 6518, `freeze`
3111, `cppm4` 3394), `nonzero=0`, `segs` = `issued` (exactly one measured draw per query), `spp1` only, `host=0` on
every `[NATIVE_ZPD_Q]` line, `area=1`. Ruled out by that evidence: the sample scale (the raw readback is already 0;
`normalize` maps every n >= 1 to >= 1, Z7); the bracket on the wrong draw (one segment per query, opened at the guest
draw call between the query's own markers - Z4c - and 82304058 draws nothing else between its Issue calls); a draw
pm4PrepareDraw skipped (it would have no segment; such a draw inside a query now counts `missed`, "visible").
- The draw (IDA): `HudThreatRenderObj_Render_823149B8` walks the frame's 64-entry threat snapshot;
  `HudDrawOnScreenThreatMarker_8230DD28` calls `HudGlowOcclusionQuery_82304058` for a FAR threat (view distance over
  the per-type threshold). 82304058: Issue(BEGIN) (82779A28); engine z state 3 (Xenos zfunc 6, GEQUAL against the
  reversed-Z depth); `RwCompiledStateApply(hud+76)` = "HUD Threat Occlusion Compiled State" (primitive 1, point list);
  ONE vertex = the threat entry's WORLD position + `0xFF000000` (`D3DEmitDrawIndexed16(1)`); Issue(END) (82779A38);
  z state back to 7 (ALWAYS, the HUD's). The answer only picks the icon's alpha: 0 -> 127 (dim, "occluded"), else
  255 / distance-faded. Today every far marker draws dim.
- The console's answer (Xenia trace 454107ED_25023, `archive/zpd/xtr_clear_z.py`): right before the HUD the XDK
  clears depth - clear #1711, surface 0A020280 (4X 640x360, the same EDRAM as the scene's 1X 1280x720 depth
  0x102D0), RB_DEPTHCONTROL 0x8777, z = 0 at all three vertices: reversed-Z FAR. Drawn in the HUD phase (where the
  z state is 7, the state 82304058 restores) the query tests against far and an on-screen point passes; drawn
  before that clear it sees the scene, and a far threat's position passes unless a building or its own mesh covers
  it. Neither gives 0 for every marker of four flights with markers in view.
- The host side is proven to count (Z16, a WARP device on the worker): ONE vertex as a point list through the real
  point-sprite GS source, c19 from `edram::pointSpriteNdcRadius` (now shared with tryXlatDraw) with PA_SU_POINT_SIZE
  0x00080008 (the value the traces hold: 1 pixel), the viewport `edram::computeViewport` gives a D24FS8 surface (the
  float24 range 0..0.5), zfunc 6 mapped as getPm4Pso maps it, a stored scene depth, the occlusion query resolved into
  a readback buffer that `zpd::collect` writes to the slot: in front 1 sample (slot 1), at the stored depth 1, behind
  0, a 2-pixel point on a pixel corner 4, the point at w = 2 still 1, size 0 and off screen 0, a discarding pixel shader
  0. So the 0 is the game draw's state on our path (where it lands, what it is tested against, what its pixel shader
  does), not the query plumbing.
- The probe (`SR_NATIVE_ZPD_PROBE`, default on with the gate): every query draw is drawn again right after its
  measured draw - the answer's segment closed - on two segments that never reach the answer: `ref` (depth ALWAYS,
  stencil off, nothing written, its own pixel shader) and `raster` (the same without a pixel shader; one of each per
  query, on its first draw; they never take an answer segment). Each 0 answer is classified on the census:
  `depthKilled` (ref > 0: the depth / stencil test removed every sample), `psKilled` (ref 0, raster > 0: the pixel
  shader discarded them - alpha test, texkill, the vertex colour's alpha), `noRaster` (both 0: off screen, clipped,
  a zero-size point, scissored); `[NATIVE_ZPD_Q]` lines carry `ref=` / `raster=` and `lagMs=`. `[NATIVE_ZPD] draw #n`
  lines log the first 16 query draws' state (topology, vertex count, the viewport and its depth range, the scissor,
  surface / MSAA / colour and depth keys, z enable / write / func, stencil, RB_DEPTHCONTROL, RB_COLORCONTROL, VTE,
  CLIP, SC_MODE, PA_SU_POINT_SIZE, c19, the CPU-decoded vertex, and the vertex as the guest wrote it - the VS's first
  stream address and its first 16 bytes, read at drain time, since the in-shader-fetch path decodes none on the CPU)
  and the first four the VS constants c0..c7, so the point's clip position can be computed offline. zpd5 names the class; the next step follows from it.

*(b) Strict's cost, and the default.* Flushes are already one per covering fence, not per query (queries per flush:
`on` 1.00, `freeze` 1.66, `cppm4` 1.48): the game ends its queries and fences once a frame. Each flush drains the GPU
(the render thread's previous frame plus the partial set, on one in-order queue): average 16.8 ms (`on`: 2393 flushes,
40.3 s of a 360 s run, max 44 ms), 19.1 ms (`freeze`), 24.7 ms (`cppm4`, max 82 ms). In play: off 98.4 / 97.6 / 95.5
fps (zpd4 / zpd3 / zpd2), strict 84.1 / 85.6 (zpd4 / zpd2, -12 to -14 %; p95 20.4 ms vs 14.6, frames over 33 ms 48 vs
5), latency 94.4 / 94.5 / 99.4 - within off's spread. Cheaper strict needs the fence itself to move: strict's rule is
"no covering fence published over an unwritten slot", and our CP parses the fence ahead of the GPU; resolving at the
frame boundary means holding the EVENT_WRITE_SHD store (and every later one) until the frame's GPU fence - the
console's own order (the fence is written at the end of the pipe), but every frame with a query would then publish
its fence on the GPU's clock, and the guest's fence waits (resource reuse, the present throttle) would wait on it: a
CP-wide change, not this lane's. The default when the gate is on should be the latency model (`SR_NATIVE_ZPD=2`): the
CP never waits, the answer comes a frame or two later (the census's `lagAvgMs` / `lagMaxMs` measure it from zpd5), and
the game is built for it - GetData's S_FALSE keeps the marker's pending flag and its previous answer (82304058:
`if (GetData != -1) { result = it; pending = 0; }`, no re-issue until answered). Strict stays the oracle.

*(c) The freeze roll's FAIL (bad 5..11 of 26).* Not the partial frame: an FMV. At 05:11:44.7 the game started the
65 s Mxyzptlk cutscene (`[MOVIE_PACE] movie#7 EOM wall=65.64s frames=1914`, ended 05:12:50.4); the sim freezes 5-11
(05:11:49.7 .. 05:12:49.8) fall inside it and the movie kept playing on its media clock (`freeze_exact_007.png`:
the cutscene's frames), while the other 19 in-game freezes - with queries and partial frames running - were exact,
as `cppm4`'s 26. zpd3 and the other zpd4 rolls never reached that cutscene. The grader (`flicker_gate.
grade_freeze_table`, also tools/native_ledger.py's) now leaves an in-game freeze that overlaps a movie's EOM window
ungraded (a note `movie#N plays through it`, `movie_excluded` on the result; a GAP or one-frame record is no window);
zpd4's roll re-grades PASS 19 with 5-11 excluded, cppm4 unchanged (test_flicker_gate `--pil-free` 1c; the archived
roll where present).

**12. zpd5 (2026-10-15, exe 062f32cc3d49): the probe's answer, rolls without queries.**

*(a) Two strict rolls issued nothing (`on`, `cppm4`: `issued=0`).* Not the lane's code: their `[GPU_CP]` census
reads `zpd=0` - the CP parsed no EVENT_WRITE_ZPD, i.e. the game never called D3DQuery Issue - and nothing of
SR_NATIVE_ZPD runs before the guest's first Issue (the probe, the missed-draw path and the draw lines act only
inside a query; `m_zpdDrainQuery` is 0). Strict itself issued 1038 in the same set (`freeze`). Issuance is the
game's: 82304058 runs only for an on-screen threat farther than its type's distance, and the tape flies blind
(fixed mouse moves; where Superman ends up depends on the frame pacing). Rolls with the same seed and tape across
sets (`[GPU_CP] zpd=`, two per query): off 8 / 20 / 8 / 8 (a few queries, then pending forever - off never answers);
on 4454 / 340 / 4786 / 0; lat 2426 / 23316 / 13036 / 4812; freeze 5252 / 2754 / 6222 / 2076; cppm4 0 / 778 / 6788 /
0 (zpd2 / zpd3 / zpd4 / zpd5) - zpd2's cppm4 issued none on an exe without the probe. A strict item graded on a roll
whose game issued nothing reads no query, not a broken one: such a roll is NO DATA for the census items (re-roll),
which the plan's items cannot express today (they require `issued > 0`).

*(b) The depth test kills ~91 % (`lat` 2186 of 2406, `freeze` 744 of 1038; psKilled 0, noRaster 2 / 11).* The
viewport's z 0.000..0.500 is not the guest's range: the guest's is 0..1 (PA_CL_VPORT_ZSCALE 1, ZOFFSET 0,
PA_CL_CLIP_CNTL dx_clip_space_def), and 0..0.5 is the D24FS8 host convention (Xenia draw_util.cc:517-537:
`edram::computeViewport` halves the range on a float24 depth surface, `edram::packDepth` / `unpackDepth` store /
load host = guest x 0.5) applied to every draw, clear, load and store on that surface - a monotonic scale, so it
changes no GEQUAL outcome, and against a far-cleared depth (0) every point passes whatever the scale. The point: the
first draw line's VS constants c0..c3 times its vertex (-1833.25, 1179.98, -409.52) give clip (-201.6, 51.4, 0.493,
250.3): on screen (NDC -0.81, 0.21), 250 units ahead, z/w 0.00197 (a reversed-Z projection with near ~0.49), host
depth 0.00099 - it fails only where something nearer than 250 units covers its pixel. The freeze roll's dumped
frames match that: freeze 7 (an alley; 5 markers, all answering 0: the markers over the walls), freezes 12 / 15 (the
marker over a wall, 0; when the non-zero answers grew, freeze 15 shows a marker over the open street); the pattern is
occlusion by the scene, not a constant depth. Which depth the console tests depends on where 82304058 runs: its
colour target is 8888 at EDRAM 0 (rt0 00008000) - after the scene's HDR passes (the Xenia frame's scene targets are
formats 3 / 12) - but the Xenia frame has 8888 at 0 both in the tonemap passes before the XDK depth clear and in the
HUD after it; before the clear the console tests the scene's depth (our answers), after it the far clear (every
on-screen marker visible, and our EDRAM model would be failing to move the cleared tiles in). The draw lines now log
it: `[NATIVE_ZPD] draw #n depth frame= batch= moves:` (the claim's moves into the tested depth: the previous owner's
key and tiles, `none` = that surface already held them) `lastDepthClear=` (the frame's last draw that rewrote a whole
depth binding - an XDK clear - its key, how many draws before, whether uniform zero). zpd6 decides: no clear since
the scene's depth (moves none, the last clear the frame's first) = the console's answer is ours; the HUD's 4X clear
just before with no move from it = our bug in the clear's propagation.

**13. zpd6's plan (2026-10-15, exe 70b6773e6c41 - no C++ change): no query, no data; a seed that asks.**

*(a) The precondition.* Every zpd roll and item carries `require` (`native_proof.ZPD_FIRED`: the roll's last
`[GPU_CP] census` line reads `zpd > 0`). A roll that misses it is VOID: kept as `<roll>_noquery1` (`voids`, rule
`require`) and repeated once with the same tape and save, the gate's VOID rule. A second miss records the step NO
DATA: never PASS or DONE (the plan exits 1), even when its freeze pixels pass. The ledger (`tools/native_ledger.py`
`require`) grades any item of such a roll FAIL `NO DATA: ...` whatever its own check says. As a witness (expect fail)
it never "fails as expected", and `apply_ledger` never lets it retire, bind or resolve (`never_fired` included).
Every roll's row shows the precondition: `precondition [GPU_CP] zpd>0 met: zpd=N (fake 0)` or `NOT MET twice - NO
DATA`, and a passing item's ledger detail ends with the same.

*(b) The seed and the tape.* Seed 12 (`archive/saves/12_ch12_after_bizarro1`, Metropolis after Bizarro 1) replaces
seed 04. The seed library's load checks (the verify's ~45 s play, exe 2087ca4acdb4) read `[GPU_CP] zpd` +30 on
seed 12, with a far marker ahead of the spawn (the seed's screenshot), and +0 on seed 04 in both checks. 06 read
+24 and 03 / 14 +10. `tools/perf/zpd_play.txt` is now 5 blocks of that verify play (look around, take off, fly with
turns, hover, punch, fly, land; the turns alternate), about 3.8 min. It replaces the 16 blind flight cycles that
carried Superman away from the markers. A dump-stall VOID of a long roll is now repeated with its own tape and save.
Before, the re-roll was the gate's new-game roll.

**14. zpd6's depth read (2026-10-15, exe 70b6773e6c41) and the Xenia oracle (zpd7).**

*(a) `lastDepthClear=00304658` is neither the scene's clear nor the HUD's.* Decoded (`gfx/edram.hpp` RtKey), it is a
D24S8 depth, 4X, 320 px pitch, at EDRAM tile 1624. That is the half-resolution post pass's depth: in both Xenia
gameplay frames (`archive/zpd/xtr_passes.py` over `454107ED_25023` / `_28050`, `archive/zpd/xtr_passes_*.txt`) the
pass clears 003045A0 and then 00304658 (25023 draws 3803-3809, 28050 draws 3744-3750). Seven half-resolution draws
follow, then the post chain and the tonemap to the 8888 target at base 0: 21 draws, resolves not counted, as our draw
serial does not count them. Then the HUD starts: its 8888 base-0 target with the scene depth 006082D0 bound. That is
exactly the query draw's state (`rt0=00008000 depth=006082D0`). The HUD's own depth clear follows: the XDK 4X clear of
007082D0 (tiles 720.., the scene depth's, z = 0 = far), 25023 draw 3847 (the 22nd draw), 28050 draw 3789 (the 23rd:
a HUD sprite, draw 3788, comes first). Our queries run before that clear:
- Every zpd6 query sits 22 to 78 draws after the 00304658 clear (freeze 22..47, lat 27..39, cppm4 52..78; 14 queries
  in one cppm4 frame). The earliest is the 22nd draw, the HUD's first position.
- None names the HUD clear. Its registers (RB_DEPTHCONTROL 00008777, RB_STENCILREFMASK 00FF0000, back face off, z
  exactly 0, the XDK clear shaders, prim 8 n 3) meet `nbClearCoverage`'s whole-rewrite rule exactly as the 00304658
  clear's do. Had it come first, the line would name 007082D0.
- `moves: none`: the query's tiles were last written under the scene depth's own key.

So the game draws its threat markers, queries included, at the start of the HUD, before the HUD's depth clear, and
they test the scene's depth. On the console they test the same depth: our answers (depthKilled = a nearer wall over
the marker's pixel; the freeze frames' markers over walls) are the console's. This is not the clear-propagation bug
that item 13's reading would have named. The diagnostic's one blind spot stands: it names only the frame's last
whole-depth rewrite. The Xenia item below checks the answers themselves.

*(b) The Xenia oracle (the ledger's guard `pixels >= xenia`; no other obligation has a pixels item at that
authority: audio.xma's `xenia_parity` is audio, display.registers' timing guard is open).* Native_proof plan
`zpd_xenia` (prefix zpdx):
- **Roll `xtrace`.** Seed 12, the zpd tape and the `[GPU_CP] zpd>0` precondition. Gates `SR_NATIVE_ZPD=1`,
  `SR_PM4_TRACE_HOTKEY=1` and `SR_PM4_TRACE_ZPD=4`: the first 4 query frames, at least 600 swaps apart, are traced to
  `<roll>_xtr` (`gfx/pm4_live_trace`, `requestOnQuery`). Every query of a traced frame logs
  `[NATIVE_ZPD_T] swap= id= slot= written guest= host=` (native_zpd, uncapped; `PM4_LIVE_TRACE` and `NATIVE_ZPD_T`
  are kept by the player profile). The armed trace keeps the render thread and the native executor off. The query's
  EDRAM and depth path is the production one.
- **Step `xenia`.** Xenia's own `xenia-gpu-d3d12-trace-dump`, built by `tools/xenia-trace/build_zpd_trace_dump.ps1`
  on the worker (xenia-canary 0bd090dbe, the oracle build's commit, plus the headless patch and
  `xenia_zpd_oracle.patch`: one `SR_ZPD_REPORT addr= zpass=` line per EVENT_WRITE_ZPD retired, and a drain before
  exit). It plays each trace with `--occlusion_query=strict`: real host queries over Xenia's own EDRAM model.
  `tools/perf/zpd_xenia.py` pairs every END report with our answer for the same query (frame, order, slot).
- **The item `zpd_xenia`** (json, authority xenia, observes pixels: the counts are the pixels that passed the depth
  test). PASS needs a calibrated dump, at least 20 pairs, none unpaired, and at least 98 % agreement on visible (> 0)
  / occluded (0), the one distinction the glow reads. A frame without a query is NO DATA.
- **Calibration and device.** Before grading, the dump renders Xenia's own golden gameplay trace
  `454107ED_25023` and must land within 1 % of its golden PNG (pixels more than 16 levels off): the device must draw
  the scene the queries test. On the worker's WARP (`--d3d12_adapter=-2`) the build runs (15 s, the drain logs
  `left=0 mode=strict`) but draws that frame without Superman or the street: 39 % of pixels off. So the xenia step
  runs on the PC's GPU. The binary is `archive/zpd/xenia_trace_dump/` (sha256 1e0de573..., built on the worker). The
  worker needed Xenia's first-run disclaimer acknowledged (`HKCU\SOFTWARE\Xenia XEFLAGS = 1`, as on the PC): the
  message box blocks forever with no desktop. The dump takes only its own flags: `--mute` / `--apu` / `--hid` are
  xenia_main's, and an unknown flag exits at once.
- **The Xenia half, end to end (worker, WARP).** `archive/zpd/xtr_inject_zpd.py` put queries around five draws of
  25023. The dump logged one `SR_ZPD_REPORT` per event in stream order, then `SR_ZPD_DRAIN left=0`. The END deltas
  were 1779, 0, 386205, 0 and 0: the two HUD draws read 0 because WARP did not draw them, which is the calibration's
  point. `zpd_xenia.py` read the trace's events and paired every END by slot.
- **Tests.** `test_zpd_xenia.py` (pairing, grade and calibration on synthetic traces) and difftest Z17 (the
  traced-frame lines). `native_zpd_run.py` mutants: the tag dropped, the trace swap ignored, every query logged.

**15. Astra's merge review (2026-10-15, MERGE AFTER FIXES, `archive/astra/merge_review.txt`).** zpdx1 was withdrawn
before any roll; the Xenia check reruns as zpdx2 on the fixed exe with the fixed grader.

*(a) P1, the answer the guest reads (`gfx/native_zpd.cpp`).* The generation advanced only when the CP parsed BEGIN,
and BEGIN did not restore the sentinel. In latency mode the HUD can replace a marker key and reissue its pending
query:
1. The guest's Issue (D3DQueryIssue 82102BC8) resets the slot's sentinel.
2. Query A's collector writes A's count into the slot before the CP parses query B's BEGIN.
3. B's BEGIN advances the generation over A's count.
4. Once B's fence passed, GetData returned A's count while B was unresolved.

BEGIN now re-arms the slot (END+0..+C = the sentinel) under the lifecycle lock, with the new generation. Difftest Z18
runs the guest Issue and the CP's execution as separate steps; the reuse test Z6 runs them together. A's count landing
between B's Issue and B's BEGIN is cleared, GetData after B's fence answers S_FALSE, then B's own count. The mutant
`begin_rearm_removed` fails Z18.

*(b) P2s, the graders.*
- `zpd_xenia.py` grades the answer the guest read (`delivered`). The computed count is kept as a diagnostic, and a
  query whose answer never reached the guest (superseded, stale, dead) is unpaired. The old grader passed 20/20 with
  strict's fallback 1 everywhere.
- Every run writes to a fresh output directory (the old one moved to `.prev<k>`) and records each dump's exit code in
  `exec.json`. A frame is graded only after exit 0 and `SR_ZPD_DRAIN left=0`, and the calibration needs its own run.
- Every report is checked against its own event: BEGIN and END keep their record bit (`record_of`), and an END pairs
  only with an END report of its own address.
- flicker_gate's movie rule: a movie covering a whole hold excludes it. A movie covering part of a hold excuses only
  the differing pairs inside its window (freeze_diff prints the per-pair rows) and the rest are graded; without pair
  rows the hold is graded whole. The archived zpd4 `zpd_freeze` now FAILS on freeze 11.
- `zpd_balance_item` selects the final strict census, then asserts balance, lost=0 and inflight=0 on it. Before, the
  select carried lost=0 / inflight=0, so an earlier balanced census stood in for an unbalanced final one.
- The zpd gate step plays the zpd tape (flicker_gate `--tape`; the tape has a `mark dump` for the span) and requires
  `[GPU_CP] zpd>0` on both its rolls (`do_gate`: VOID, run again once, then NO DATA). The ledger reads a flicker_gate
  item's `require` on both rolls (`require_rolls`). zpd6's gate freeze roll issued no query (zpd=0; span 9234), so
  its `zpd_gate` is NO DATA until the gate step runs again.

*(c) Tests that fail on the code before the fixes.* `archive/zpd/old_code_check.py` runs the new cases against the
committed tools: 12 failures in `test_native_proof` (balance, gate), 7 in `test_zpd_xenia` and 7 in
`test_flicker_gate` (movie rule, `--tape`). The P1's is the mutant above.

**16. zpd7 and zpdx2 (2026-10-15, exe cfee1101f206).** zpd7 passed every item; zpdx2 FAILED: Xenia's trace-dump
exited 0xC0000005 on all four traced frames before its drain, and the fixed grader graded nothing (its calibration
was 0.0 % off).

*(a) The crash is in our trace writer (`gfx/pm4_live_trace`), not in Xenia or the oracle patch.*
`archive/zpd/xtr_check.py` replays a trace's records the way Xenia's TracePlayer does: the packet is copied to its
address at PacketStart and executed from memory at PacketEnd. Frame 7173 has 34149 of 34159 packets recorded at
physical address 0. The CP copies every primary-ring packet out of the ring (`kernel/gpu_command_processor.cpp`
`pkt`), and the writer cannot place a host copy, so `hostToPhys` gave 0. The packets alone would still run.
But `IM_LOAD_IMMEDIATE`'s inline ucode was recorded as a MemoryRead at physical 0 (`Pm4Engine::onShaderLoad` with
address 0). 264 such reads overwrote the packet waiting at 0 between its PacketStart and PacketEnd, so Xenia parsed
garbage: "ExecutePacketType0 overflow", invalid fetch constants, then the access violation. The worker's WARP showed
the same crash (`archive/zpd/zpdx2_dump.ps1`). Repaired offline (`archive/zpd/xtr_repair.py`: packets moved to a
free address, the reads at 0 dropped), the same trace plays to `SR_ZPD_DRAIN left=0` with no error. Frame 8488
reports its two queries; Xenia's END answers are 0 and 0, the same as ours.

*(b) Four single frames held 4 queries (0 / 0 / 2 / 2).* The HUD issues its glow queries in short bursts. zpd7's
strict roll shows 8 markers' queries in 3 consecutive frames, then about 4 frames without. That is never the 20
pairs the item needs.

*(c) The fixes.*
- A host-copy packet is recorded at `kHostPacketPhys` (0x1FF00000, the top MiB of physical memory, which no guest
  allocation reaches).
- `IM_LOAD_IMMEDIATE` records no read.
- `SR_PM4_TRACE_FRAMES=<n>` records n consecutive frames per trace. A swap event follows each frame's swap packet,
  where Xenia's own writer puts it, and each frame's loose reads come first in that frame. `tracedSwap` is the
  trace's first frame.
- The oracle patch adds `TracePlayer::PlayAll`: a multi-frame trace plays every frame in one pass. A single-frame
  trace takes the old path, so the calibration is unchanged. The new dump is sha256 860f7824edc9; zpd7's dump stays
  as `archive/zpd/xenia_trace_dump_r7_1e0de573`.
- Plan `zpd_xenia` traces 4 x 16 frames.
- On WARP, the repaired 8488 + 9088 joined into one 2-frame trace report 8 times with the new dump and 4 times with
  the old one; the single frame 8488 is unchanged.

*(d) Tests.*
- Difftest Z19: the writer driven through its own taps and the engine, the file read back the way Xenia reads it.
- Mutants `host_packet_at_zero`, `imload_immediate_read`, `single_frame_trace`, `loose_reads_file_first` and
  `traced_swap_current_frame`.
- `test_zpd_xenia`: a multi-frame trace pairs.
- `test_native_proof`: the plan's gates.

*(e) Ledger.*
- zpd2's freeze and cppm4 witnesses FAILED under the rule of their time (bad=[5..10]) and PASS under the amended
  movie rule. They move, expect pass, into contradiction `zpd2_freeze_rule`, resolved by zpd7's rolls.
- zpd6's gate witness grades NO DATA. The ledger gains `"expect": "no_data"`, and `apply_ledger` writes it for a
  superseded NO DATA item (`ledger_nodata`).
- never_fired's resolvers are re-pointed to zpd7's `zpd_off_fires` / `zpd_strict`; zpd6's stay as its witnesses.
- p2.zpd now fails only `zpd_xenia`.

*(f) The dev merge (c93a3a0e: the transport and devcreate bakes, emu-removal stages 0 + 2a).* Stage 2a split the record
half of `gfx/pm4_engine.cpp` into `gfx/record_engine.cpp` (`RecordEngine`, `Pm4Engine`'s base). The ZPD hooks moved
with their functions:
- `RecordEngine`: `CpHooks::zpdFence`, `applyEventWriteShd`'s covering-fence check, `onShaderLoad(inlineUcode)`.
- `Pm4Engine`: `applyZpd`, the executor store's check, `cpPacket` 0x5B.

Behaviour is unchanged by construction; the lane test and the mutants (two re-anchored in `record_engine.cpp`) check
it on the merged build. zpdx3 runs on the merged exe, and so does a short zpd re-roll, since the hooks moved.

**17. zpd8 and zpdx3 (2026-10-16, exe 9e97d2c10419); zpdx4's design.** zpd8 passed every item with data. zpdx3
FAILED on `paired 14 < 20`; every pair agreed and none was unpaired.

*(a) The trace path worked.*
- Each of the four traces recorded 16 frames. The writer logged "16 frame(s)" for swaps 8690, 10985, 11771 and 12373,
  at 180-230 MB each.
- Xenia played every frame. Its reports per trace (4 / 8 / 10 / 6) equal our EVENT_WRITE_ZPDs in the same 16 frames,
  and those queries are spread over the frames (trace 11771: ids 132-136 over 6 s of traced frames).
- The calibration was 0.0 % off, and every dump drained.

*(b) The sizing was wrong.* The plan assumed zpdx2's rate of about one query a frame. A 16-frame trace held 2 / 4 / 5 /
3 pairs, about 3.5 on average.
- One marker was in view, so every query used one slot (1E3CE000). The next query comes only after GetData answered
  the last one, about one every 3 traced frames.
- The four traces took the first queries, at the roll's sparse start (swaps 8690-12373). Most of the roll's 1,209
  queries came after swap 15,960 (zpd= 1032 -> 2418 by swap 18,840).

*(c) zpdx4 (plan `zpd_xenia`, the same exe 9e97d2c10419, no code change).*
- 12 traces of 16 frames (`SR_PM4_TRACE_ZPD=12`): 42 pairs expected at zpdx3's mean, 24 at its worst trace.
- At least 300 swaps apart (`SR_PM4_TRACE_ZPD_GAP=300`; the default 600), so the quota fits the query window even
  though tracing stalls the game about 10 s a trace while the wall-clock tape plays on.
- 420 s of play after the HUD (was 260), so the roll keeps playing after the tape's 260 s; the drive timeout is 1,200 s.
- `test_native_proof` holds the counts to zpdx3's measured yield (twice the 20 pairs at the mean, 20 at the worst
  trace) and the quota to half of zpdx3's query window.

## STATUS (resume here) - 2026-10-04, iter-465, branch native/n0

| Phase | State | Where |
|---|---|---|
| N0 | entry sort, packet counter, brackets, report, census, conformance skeleton DONE (2026-10-01); B4 liveness answered for boot -> early Metropolis (§11.1); taps (N0.4) NOT built (needs a full regen) | §7 N0 |
| N0 speed gate (E5) | answered by the perf lane's profiler instead of [NATIVE_FRAMESPLIT]: the gameplay render thread is 94.7% guest code, the XDK range 35.9% self of it, GPU waits < 1% (archive/v1_perf/p1_prof, PERFORMANCE.md 2.3) | ENGINE_MAP "NATIVE N0" |
| Minimal hook set | 60 replace rows vs UnleashedRecomp's 42 hooks, table in §3.3 | §3.3 |
| N1 mode A | BUILT; projection v7 (2026-10-04, roll a7b): 8,454,358 joined draws, every compared field exact on all but the by-design UP vertex slot (fetch slot 31, 413,132 draws): registers, fetch, ALU constants (incl. Lion's B2 c96-c111, now applied from the ring by an observe override of D3DDeviceEmitPm4Packet_sub_82113010), bool/loop, VS/PS identity. VGT_INDX_OFFSET's residue was DrawVertices' 0xFFFF-vertex chunking, now modelled. Mode A' DRIVEN (SR_NATIVE_DRIVE=1, roll a8b): 8,388,608 draws built from the projection with 0 register/fetch/bool values differing from the CP's | §7 N1 |
| Native leaves (fps now) | DONE, default ON: SetVertex/PixelShaderConstantF + GpuEmitType0RegPacketsVec as host code (`kernel/native_leaves.cpp`). Differential 0 bad in 134M + 6.29M calls; ingame 17.1/17.8 -> 20.0/21.0 fps (PERFORMANCE.md §5) | ENGINE_MAP "Native XDK leaves" |
| N1 phase 1: native constant upload | DONE (iter-467), default ON (SR_NATIVE_CONST_UPLOAD=0 A/B): ShaderPs/Vs_UploadAllConstants 8260B660/8260B2A0 (89% of the ~45K SetShaderConstantF calls per frame, sizing SR_NATIVE_LEAF_CALLERS) run their F list as host code; I/B lists + semantic callbacks stay guest calls in guest order. Check (SR_NATIVE_CONST_UPLOAD_CHECK: native pass on a copy, guest body for real, compare its F calls + the shadow after them) 25.4M calls 0 bad, every era; projection (SR_NATIVE_RECORD) regs/ALU/bool 0 in OFF and ON rolls; replay 9/9 byte-identical; uncapped ingame +12 fps, tick -1.1 ms (PERFORMANCE.md §2.10). Phase 2 (skip redundant uploads, a PM4 change) being sized | ENGINE_MAP "Per-binding constant upload" |
| N4a | replay: native surfaces (=2) + native resolves, colour AND depth (SR_NATIVE_RESOLVE=1, straight from the source host RT) + native present (SR_NATIVE_PRESENT=1, from a linear front-buffer copy) give byte-identical frames to the EDRAM model on all 9 fixtures. Mode A' LIVE (gameplay/port roll): every era renders, fps equal to the EDRAM model (ingame 21.5 -> 22.2, CPU-bound), 91% of presents native; resolves whose span another key owns (~11%, depth key 006082D0) still use the EDRAM buffer | below |

**N1 live result (roll c2, `archive/v1_native/c2_check`, gameplay/port scenario: boot, intro,
title, custom menu, NEW GAME, tutorial; exe d93c7d3bf5a9, 2026-10-04) [measured].**
Repro: `python tools/perf/perf_run.py --scenario gameplay --config port --exe <snapshot> --out
<dir> --hz 0 --gates "SR_INSTANCE_ID=native,SR_LOG=dev,SR_NATIVE_RECORD=1"`, then the last
`[NATIVE_REC]` lines of `<dir>/drive/attempt1/run.log` (cumulative counters).

- **Join:** 7,731,381 CP draws; 7,141,013 (92.4%) joined to the native record of their own draw
  bracket; 571,346 (7.4%) sit in brackets of non-draw entries; 24 in no bracket; 18,998 evicted
  (record ring), 0 torn, 0 missing.
- **Draws inside non-draw entries (the N4a/N2 emitter list):** `GpuRingEmitRegisterBlock_820F5570`
  417,612 (the XDK SetShaderGPRAllocation, which ends in `D3DDeviceClearFullSurface_82100488`:
  ~80 draws per frame), `D3DDeviceResolve` 113,654, `ClearF` 34,021, `Clear` 4,389,
  `GpuCommandBufferReplayDriver_82110840` 1,380, a few creates/locks.
- **Registers (124 compared):** all equal except: SQ_PROGRAM_CNTL/CONTEXT_MISC/INTERPOLATOR_CNTL
  `0x2180-0x2182` (663K/196K/332K draws) and RB_HIZCONTROL `0x2203` (14.7K) - all
  **flush-derived** (the XDK computes them at the draw; native computes them from the shader pair
  and the HiZ owner); RB_MODECONTROL `0x2208` 1,612,642 draws, CP = 5 (depth-only) vs shadow 4,
  **not in the shadow** = the Z-pass (BeginZPass writes it; N5); VGT_INDX_OFFSET `0x2102` 411
  (draw-argument derived).
- **Fetch constants:** only slot 31 words 4-5 (353,564 draws = the DrawVerticesUP/Begin-End
  inline vertex buffer the XDK builds at the draw).
- **ALU constants:** VS c240-255 (6.9M draws), PS c240-255 (7.1M), PS c224-239 (4.7M), VS/PS
  c96-111 (22.5K / 12.4K) differ. The c96 blocks are written by `EmitPm4Packet_82113010` (Lion's
  per-tile c100, B2, as predicted); the c224-255 blocks are written inside the draw brackets
  themselves, so the XDK draw writes them from somewhere other than the pre-draw shadow. Next:
  post-draw ALU hashes (flush-derived or not) and the shaders' constant-use walk (§4.3: a block
  no shader reads does not matter).
- **Bool/loop constants:** 0 mismatches. **PS identity:** 0 mismatches.
- **VS identity:** 2,198,458 draws (28%) run a VS whose IM_LOAD hash differs from the bound
  object's ucode hash, before AND after the XDK call. The object's bytes are re-patched between
  draws (stride changes) and the CP hash equals an EARLIER patch state of the same object
  (`vsObj 40099CE0`: E4C1704E825A22E9 is native at call 2689944 and CP at call 3203399). Open:
  the XDK skips the reload on the console too, or our CP's IM_LOAD hash/bytes are stale (an
  oracle-path defect that would draw with the wrong vertex fetch). Settle against a Xenia trace
  of the same scene (IM_LOAD sequence per draw) before native shader binding.

**N1 projection v5 (rolls c3-c5, same scenario, `archive/v1_native/c5_check`, exe bc102d9838bc,
2026-10-04) [measured].** The native projection now carries what the XDK computes at the draw:

| Item | Native rule (all static IDA + recomp, then measured) | Mismatching draws c2 -> c5 |
|---|---|---|
| SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC / SQ_INTERPOLATOR_CNTL | `GpuEmitDrawState_82108668`: vsHdr[+8] \| psHdr[+8], vsHdr[+12] \| psHdr[+12], psHdr[+24]; vsHdr = vs + *(vs+896) + 872 (no PS and vs[872]&0x20: *(vs+904)), psHdr = ps + *(ps+64) + 40 | 663K / 196K / 332K -> **0** |
| RB_MODECONTROL Z-pass | draws inside BeginZPass..EndZPass (3.80M) run depth-only (5) first, then the XDK replays them in colour | 1.61M -> 49K (open: the remaining 0.6%; the GpuEmitDrawState depth-only rule as ported mispredicted 1.80M and is withdrawn) |
| RB_HIZCONTROL | bit 0 = (dev.byte[10810] >> 5) & ZENABLE, bit 1 on the simple branch of `GpuCmdUpdateSurfaceState_82107110` | 14.7K -> 10K (bit 1's complex branch open) |
| ALU constants | a TIMELINE like the CP's file: per draw the dirty shadow vec4 groups (dev+0 / dev+8, bit 63 = c0-c3) first, then the LITERAL constants of a rebound shader (dev+0x10 bit 51 VS / 52 PS) from the shader object's table (`GpuRingEmitFetchConstants_82108470` LOAD_ALU_CONSTANT) | VS c240-255 7.0M, PS c224-255 4.7M/7.1M -> **0**; only Lion's per-tile c96 (B2, `EmitPm4Packet_82113010`) remains: 24K VS / 12K PS |
| ALU blocks counted | only blocks an active shader may read (`ucode::constantBlocksRead`, all sources of every ALU instruction; relative addressing = whole stage: 4.41M draws) | - |
| VS identity | **settled, XDK behaviour:** every mismatch is an IMMEDIATE load. `D3DVsPatchFetchesInPlaceIfIdle_82108520` patches the object in place only when its fence passed, else `GpuLoadShaderImmediate_82108300` loads a patched ring copy and the object keeps its old patch. Native needs the patched ucode for (VS, declaration, strides dev+12256/12264): port `ShaderAssembleVertexFetches_82107A30` (pairing rule in its IDA comment; fetch sort and mini-fetch merge) | 2.55M (29%), unchanged by design |
| Fetch constants | only slot 31 words 4-5 = the DrawVerticesUP/BeginVertices inline vertex buffer in the ring (native uploads its own) | 382K, by design |
| VGT_INDX_OFFSET | draw-argument derived | 639 |
| PS identity, bool/loop, the other 116 registers | equal | 0 |

c5: 8.78M CP draws, 8.14M joined, **5.15M (63%) clean on every compared field**; the rest is the
VS patch (29%), the UP slot, Lion c96 and the small register residue above. Record mode costs fps
(16.2-17.0 ingame vs 20-21 without it). Q5 hint from the same roll: the GPU EDRAM path does 24
ownership transfers / 8,256 tiles and 26 resolves per gameplay frame (`[PM4_FRAME]`).

**N1 projection v6 (rolls c6-c10, same scenario, `archive/v1_native/c10_check`, exe snapshot of
native/n0 at the c10 commit, 2026-10-04) [measured].** On top of v5 (table above):

| Item | Native rule | Mismatching draws v5 -> v6 |
|---|---|---|
| VS identity | **host port of the XDK patcher** `gfx/native_vs_patch` (ShaderAssembleVertexFetches_82107A30, 422 instructions ported 1:1) on a host copy of the IM_LOAD range `[a2, a2 + *(hdr+4))`, a2 = `*(vs+32) + *(hdr+0)`; DrawVerticesUP / BeginVertices (and the EndVertices that closes it) put their stride/4 in stream 0 | 2.55M -> **0** (8.36M joined draws, 3.02M of them the XDK's immediate patched copies) |
| patcher differential | every in-place patch the XDK made is redone by the port on a copy of the guest's result: **1,819 / 1,819 identical** (c9); the uninitialised stack halfword (`lhz r8,80(r1)`) is irrelevant - statically (only bit 0 survives `r9|14` and it only ANDs the all-even table at 0x820BA0C0) and live (garbage 0 vs 0xFFFF: 0 differences) | - |
| RB_HIZCONTROL | bit 1's complex branch ported (notPs, HiZ-testable depth func, KEEP stencil ops) | 10K -> **0** |
| RB_MODECONTROL | BeginZPass sets dev.byte[10808] \|= 0x40, so the FIRST execution of a Z-pass draw takes GpuEmitDrawState's per-draw rule (depth-only unless the PS writes colour or alpha-tests); the replay runs the shadow | 49K -> **0** |
| VGT_INDX_OFFSET | draw-argument derived, second executions | 544 (open, 0.007%) |
| Lion c96 (B2), UP fetch slot 31 | by design (native uploads its own UP data; Lion's per-tile packet is a B2 emitter) | 33K, 384K |

c10: 8.58M CP draws, 7.95M joined, **7,569,852 (95.2%) clean on every compared field**; excluding
the by-design UP fetch slot, 99.6%. Shader-control registers, all ALU constants a shader reads (but
Lion's), PS and VS identity, bool/loop, fetch constants and 121 of 124 registers match exactly.

**N4a first experiment - native surfaces on the Xenia fixtures [measured, replay, deterministic].**
`SR_NATIVE_RT` (replay diagnostic in `gfx/d3d12_pm4_edram.cpp`): every RtKey keeps its own host
render target (a PC renderer has surfaces, not EDRAM); `=1` drops every EDRAM transfer between
keys, `=2` keeps only those between keys at the SAME EDRAM base (a surface re-created over the same
memory with another format or MSAA mode inherits its pixels). `tools/pm4_replay_score.py`
(`archive/pm4_score/n4a_*` in the worktree):

| Fixture | EDRAM model (default) | native surfaces, no transfers (=1) | native surfaces + same-base inheritance (=2) |
|---|---|---|---|
| menu PRESS START | 2.06 | 4.72 | **2.06** |
| FMV eahd / t16 / t21 / t25 | 0.00 | 0.00 | (not run; =1 already 0.00) |

So the menu frame needs surface reinterpretation (its transfers: depth 4x -> colour fmt 4 -> fmt 3 ->
1x, all at EDRAM base 0), but ONLY between surfaces at the same base: a native renderer models EDRAM
as "a surface created over the same base as the previous one inherits its pixels, format-converted"
(Xenia's transfer shaders, RT to RT) and needs no tile ownership map. This answers roadmap Q5 for
the fixtures. Resolves, clears and present still go through the EDRAM-buffer path; that is N4a's
next part.

**N4a native resolve + present [measured, replay, deterministic, 2026-10-04].**
`SR_NATIVE_RESOLVE=1` (`gfx/native_resolve.inc`, `pm4NativeResolve` in `gfx/d3d12_pm4_shmem.cpp`):
a colour resolve whose span the source key owns runs Xenia's own resolve compute shader with ONE
change - the EDRAM-word load becomes a host RT texel read packed with the EDRAM store's PackColor -
so there is no store into the EDRAM buffer. Depth and resolution-scaled resolves keep the EDRAM path.
`SR_NATIVE_PRESENT=1`: a fast 32bpp resolve from the origin also writes its destination dwords into a
linear host buffer (4 slots keyed by destination); the scan-out reads the copy the frame's own resolve
made instead of the tiled shared memory (`[NATIVE_PRESENT]`). Scored with `SR_REPLAY_DUMP_OUTPUT=1`
(the GPU scan-out is the dumped frame) on all nine fixtures:

| Fixture | =2 | =2 + native resolve | + native present | resolves native / EDRAM |
|---|---|---|---|---|
| menu | 2.06 | 2.06 | 2.06 | 24 / 2 depth |
| eahd, t16, t21, t25 | 0.00 | 0.00 | 0.00 | - |
| gp1 / gp2 | 4.59 / 2.87 | same | same | 24 / 2 depth |
| lg1 / lg2 | 5.48 / 4.67 | same | same | 24 / 2 depth |

Every frame PNG is byte-identical across the three columns (`archive/pm4_score/n4a_res_out` vs
`n4a_present`, and `n4a_gp_rt2` vs `n4a_gp_res`). All 9 presents took the front-buffer copy.
The one bug on the way: RB_COLOR_INFO carries format 12 (k_2_10_10_10_FLOAT_AS_16_16_16_16) on the
second pass of the menu's 3D resolve, while the host RT holds the base format 3 (Xenia
GetBaseColorFormat); packing with 12 returned zeros (MAD 109.99). Found with two gated diagnostics
kept for the next one: `SR_NATIVE_RESOLVE_HASH=1` (each resolve's destination hash) and
`SR_NATIVE_RESOLVE_DUAL=1` (native, then the EDRAM path again on the same state).
Clears already clear the host RT (their tile claims are the same-base inheritance above).

**Native depth resolves + mode A' live [2026-10-04].** Depth sources now resolve natively too
(the depth surface's two copy planes into a buffer, packed exactly as the EDRAM store packs them;
`SR_NATIVE_RESOLVE_DEPTH=0` keeps them on the EDRAM path): all 9 fixtures stay byte-identical
(`archive/pm4_score/n4a_depth`) and no fixture resolve takes the EDRAM path. The Xenia xesl include
closure is embedded (`third_party/xenia-shaders/xesl_embed.inc`, `tools/gen_xesl_embed.py`), and the
7 pipelines build on a worker thread (the first live roll stalled 19.9 s in the intro on an inline
D3DCompile; resolves take the EDRAM path until the build finishes, ~18 s; the replay sets
`SR_NATIVE_RESOLVE_SYNC=1`). Live, gameplay/port scenario, `archive/v1_native` (one roll each):

| Roll | gates | intro / title / gameplay / ingame fps | worst frame | notes |
|---|---|---|---|---|
| a1 | default (EDRAM model) | 31.9 / 30.5 / 22.2 / 21.5 | 179 ms | baseline |
| a2 | =2 + resolve + present, inline compile | 20.6 / 28.5 / 22.0 / 21.8 | 19,871 ms | the compile stall |
| a3 | =2 + resolve (+depth) + present, async | 32.4 / 29.2 / 22.1 / 22.2 | 204 ms | frames: every era renders (`a1_a3_sheet.png`) |
| a5 | as a3 + SR_LOG=dev | - / - / - / 21.9 | - | counters below |

a5 counters: at the 65,536th native resolve, 4,020 were depth, 6,906 took the EDRAM path while the
pipelines built, and >= 8,192 took it because another key owned the span (all logged ones: depth key
006082D0); presents 3,737 from the front-buffer copy, 359 from the shared memory (the build window).
fps does not move: the gameplay render thread is guest-CPU-bound (PERFORMANCE.md 2.3), so the native
path's value here is structural (no EDRAM buffer on the resolve/present path), not speed.

**Owner-remapped resolves [2026-10-04].** A span ONE other surface owns at the same base, pitch
and bpp resolves natively from that owner (owner's format packs the words; source pixel -> owner
pixel under the same sample; depth <-> colour swaps the tile's column halves). In-process verify
(`SR_NATIVE_RESOLVE_VERIFY=1`, the EDRAM path re-resolves and the hashes are compared): rolls a6 +
a9b, 0 mismatches in 12,288 checked resolves, 6,168 of them via an owner (depth 006082D0 <- 4x
007082D0; colour 01108000 <- depth 00308000; 00D08000). Still on the EDRAM path: spans owned
partly by the source and partly by a depth surface (000045A0 / 003045A0, 00004658 / 00304658),
about 3% of resolves, plus the ~18 s pipeline-build window.

**Mixed owners, ALU carry, step-3 sizing [2026-10-04, rolls b1/b2/c1/c2/d1/d2].**
- Mixed-owner spans (one colour + one depth owner, owner picked per tile in the shader): verify roll
  b1, 0 mismatches in 8,192 checked resolves (5,494 via an owner); the EDRAM path is left with spans
  of two COLOUR owners (1 seen) and the pipeline-build window.
- Driven mode now carries the ALU file too (render -> CP delta ring + per-block version history, so
  the Z-pass colour replays get the state AT their callId): 0 ALU values differ over 4.19M driven
  draws; carried on 87% (stale 549,578 in d1, mostly still Z-pass replays deeper than the history).
  DrawVerticesUP: the caller's bytes at entry == the CP's slot-31 bytes on 65,078 / 65,078 draws
  (native upload equivalence; Begin/End not yet checked).
- Step 3 (`SR_NATIVE_SKIP=1`: state packets of driven draw brackets not executed, the projection IS
  the register file): state correct - every 16th bracket parsed and compared, 2.98M compared draws,
  registers / ALU / bool exact, fetch only the UP slot. But it saves NO CP time: [NATIVE_CPTIME]
  draw-bracket state share 17.0% (d1) vs 21.7% (d2), GPU_CP 97.7% vs 97.6%, ingame 14.8 vs 15.7 fps
  (record mode, one roll each). The type-0 stores were never the cost (bulk stores); the per-packet
  walk is, and skipping execution keeps the walk. CP packet time is Swap 33% + BlockUntilIdle 23%
  (frame submission and the CP's wait-for-idle) + DRAW batch building 21%; and the render thread
  waits < 1% on the CP, so CP savings would not move fps anyway (PERFORMANCE.md 2.3: the render
  thread is guest-CPU-bound, the XDK range 35.9% self of it).

**Mode B (native draw entries) PARKED [2026-10-04, roll e1_split].** Sized first with
`[NATIVE_FRAMESPLIT] ENTRIES` (outermost XDK time per entry, gameplay/port, no record mode): the
render thread spends 49.1% of its wall in XDK entries, but 75.7 s of those 104 s are WAITS (Swap
55.7% of all XDK time, BlockUntilIdle 10.9%); XDK work proper is ~13.5% of the render thread, and
DrawVertices (9.7 s) + DrawIndexedVertices (4.2 s) are 6.6% of it. Native draw entries could save at
most that, on a thread that idles ~36% waiting for the frame pipeline, so fps would not move. The
design is kept for the structural step (in-scope: draws without a shader rebind; the native entry
clears the dirty masks and emits only VGT_INDX_OFFSET + DRAW; the CP applies the projection as in
step 3, which is proven correct; every Nth draw verified through the XDK).

## FULL NATIVE AS TWO PARALLEL STRANDS (proposal 2026-10-07, owner direction: native is the main track)

The PM4 path stays the oracle in both strands (every stage gated, the same .xtr fixtures replayed
P vs N, whole-run frames, 0 ICALL, forcing 2). Estimates below are marked; measured numbers are
cited by roll.

### Strand A - native draws (this lane, `native/n0`)
Goal: a guest render thread with no XDK encode; the CP runs records, not draw packets.

| Stage | What | Oracle |
|---|---|---|
| 1a | XDK body unchanged; a plain draw bracket becomes ONE same-length NOP {'NDRW', slot}; the CP writes the bracket's registers from the projection (`ndExecute`) and runs the DRAW. Shader loads / bin-mask packets pass through as PM4 in stream order | per-draw replay hash P == N on paired `.xtr`/`.nrt` (SR_REPLAY_DRAW_HASH, SR_REPLAY_NRT); live checkDraw on every 16th bracket (SR_NATIVE_DRAW_VERIFY_EVERY) |
| 1b | the XDK encode replaced: native side effects (dirty masks, stride bytes, derived regs, cursor / pad length) | SR_NATIVE_DRAW_CHECK: 1a and 1b on the same entry, cursor / kicks / every device dword / written pages equal; the 5 hot pages pinned first |
| 2 | VS fetch patch native (45% of draws load a patched VS copy, z_cls) - the shader bind leaves the pass-through list | per-draw hash; VS identity (vsNative) |
| 3 | Z-pass (30%): RB_MODECONTROL / replay semantics native | per-draw hash incl. the Z-pass executions |
| 4 | UP / Begin-End (6%), chunking, predication | per-draw hash; UP bytes == CP slot-31 bytes (already proven at entry) |
| 5 | the state setters native (SetTexture/FetchConstant, SetStreamSource, sampler words, shader bind) and the draw entries with no XDK call at all; records carry targets as a struct (D1) | per-draw hash; the guest-visible output ledger |

**1b plan, revised by measurement (2026-10-07).** Where the 0.83 ms "PM4 / D3D device emit" of
the player-profile tick goes (gc3 p0, ingame, samples inside the draw entries): the constant leaf
(GpuEmitType0RegPacketsVec + its memcpy) 7.5% of the tick (~0.67 ms), the fetch emit 0.76%, the
dword runs 0.26%, the entry bodies ~1.0%, the observe hook's declaration scan ~1%. And 83% of the
constant groups it copies (6.4 MB per gameplay frame) are already in the GPU constant file. So 1b
runs in this order:
- **1b.1 stream-resident constant dedup** (`kernel/native_const_dedup.*`, 0bfd149f / 15119d97,
  SR_NATIVE_CONST_DEDUP + _MODES): a core (the ALU constant file as the stream last wrote it; no PM4
  knowledge) + a PM4 adapter (segment epoch, header scan of other writers) that retires with the
  ring. PROVEN (CHECK 0 BAD over 1.6B checks incl. Z-pass replays and SR_NATIVE_DRAW; mutation
  caught; 9/9; sheets) but NO GAIN (tick and CP unchanged: the per-group decision costs what the
  copy did) - merged gated OFF; its value is the record path (a record carries only changed
  groups). PERFORMANCE.md 2.13.
- **The draw path writes no XDK globals** outside the device and the ring (the 5 hot pages pinned:
  <= 0.2% of sampled draw calls, other threads' writes) - the 1b side-effect set is the ledger's
  device dwords.
- **1b.2 the native flush:** 820FC000 / 820FBBF8's flush + draw packet as host code producing the
  record directly (structure from the recomp: d0/d1 constants through the dedup core; d2 shader
  bind 82108668 and the 821072F8 tiled branch stay guest calls until stage 2, the 0x2203 simple
  branch, 0x2200 / fetch / 0x2180 runs; d3 the 0x4900 bool word + 0x2100 / 0x2000 / 0x2280 runs;
  d4 0x2300 / 0x2380 runs + 821075E0 as a guest call; then 0x2102 + DRAW). The core takes its
  invalidations from native knowledge at their entry points (B2 direct packets
  D3DDeviceEmitPm4Packet_82113010; LOAD_ALU_CONSTANT of the ALU file, if any emitter uses type 0 -
  GpuRingEmitFetchConstants_82108470 loads FETCH constants) instead of the ring scan.
- Records must outlive their first execution while a replay can reach their segment (Z-pass
  segments, predicated-tiling IB ranges): retire by ring generation + segment fence, not on first
  consume (1a restores the PM4 bytes for that reason).
- **Hard per-draw budget for 1b.2 (before building it).** 1a as built is an oracle, not a perf path
  (SR_NATIVE_DRAW ingame ~33-35 fps vs ~101-105 off: it builds the full N1 projection record per draw
  - shader hashes, the ALU carry image, brackets, coverage). A lean record must beat BOTH bounds or
  native draw is a regression shipped for purity:
  - guest side: the XDK encode it replaces - measured 0.84-1.02 ms/frame ingame (pr_off / pr_dd,
    the "emit" category) over ~3,700 draws = ~0.23-0.28 us/draw;
  - CP side: the PM4 parse + register-file writes per draw - Pm4Engine::execute self 0.42 +
    writeReg 0.33 = 0.75 ms/frame (pr_off, tools/perf/thread_ms.py) = ~0.20 us/draw. (1a today:
    ndExecute 13.9 + checkDraw 5.7 + aluDrain 4.8 ms/frame on the CP, pr_nd - the oracle.)
  Measured per draw, ingame window only: (1) profile shares (tools/perf/ingame_profile.py, render
  thread and GPU_CP) divided by draws per frame; (2) a gated rdtsc counter (SR_NATIVE_DRAW_TIMING)
  around the record build on the render thread and around execRecord on the CP, reported as
  us/draw next to the XDK body's own us/draw on the same roll. 1b.2 is built only if a prototype on
  the plain class beats both, and merged only with an interleaved A/B tick and CP-thread gain.
- **Pipelined fence model (the mode-B executor's structure; decided 2026-10-07).** The drain / parse
  split cannot live on the PM4 CP: every segment the XDK kicks ends in a fence write, and the CP must
  drain before writing it (SR_CP_DRAIN_ASYNC measured no overlap plus hitches, PERFORMANCE 2.13), and
  the PM4 path is the oracle - its guest-visible timing stays as proven. In mode B the host executor
  owns it, UnleashedRecomp-style (one render thread fed by a queue): the guest thread's native draw /
  state entries append records to a queue and return; the executor thread consumes them in stream
  order - resolves the draw's resources, records D3D12 work - and implements the guest's
  synchronisation itself, AFTER the work ahead of it: fence writes (EVENT_WRITE_SHD / MEM_WRITE
  equivalents of GpuBuildFenceWritePacket, the per-segment fences), interrupts (VdSwap / the
  graphics ISR), REG_TO_MEM. The guest-side waits are satisfied from the executor's progress:
  GpuBlockOnFence / BlockUntilIdle / GpuRingWaitForSpace / WAIT_REG_MEM-style polls read fence values
  the executor wrote, so the guest simply sees a GPU that completes later. What it needs: (1) every
  fence / interrupt emitter of the XDK covered natively (GpuBuildFenceWritePacket 820FD580, the
  segment flush 820FD660, InsertFence / the swap path); (2) guest memory a record references
  captured or pinned before its fence is written (write-tracked pages are already the model);
  (3) the oracle comparison per stage (PM4 path vs executor: fence values and order, per-draw hash).
  Gains: the guest thread stops encoding PM4 and the CP stops parsing it; parse and execution overlap
  on two threads instead of serialising at every fence.
- **CP line:** the CP's per-draw cost executing 1a records vs parsing PM4, from the same profile
  (pr_nd vs pr_off, GPU_CP thread) - see the result below once measured; at 240 fps the CP's draw
  drain (drainDrawQueue 44% incl., pm4StageVsf 21%, ~22% in waits of a CP at 83% of a core, gc3 p0)
  is a co-bottleneck with the guest tick, so 1b.2 is judged on both threads.

Expected (estimates): guest render tick -1.0 to -1.5 ms (draw entries ~6% of the render thread in
z_split + setters ~5-9% of XDK time + the record cost avoided by 1b); CP -2 to -3 ms per frame (the
draw brackets' packet walk, ~40% of CP packet time in d1/d2); GPU 0.

Files A owns: `gfx/native_device.*`, `gfx/native_vs_patch.*`, `kernel/native_leaves.*`,
`kernel/native_brackets.*`, `kernel/native_draw_ledger.*`, `gfx/pm4_coverage.*`, the draw-entry and
setter hooks in `kernel/hle_api_observe.cpp`, the `.nrt` substitution in `gfx/gpu_trace_replay.cpp`,
the sidecar in `gfx/pm4_live_trace.*`, and in `gfx/pm4_engine.cpp` ONLY the 'NDRW' NOP block and
the per-draw hash block in `emitDraw`.

**Native meter (2026-10-07, `kernel/native_meter.hpp`).** The acceptance number shown to the owner:
PM4 packets interpreted, EDRAM-model resolves / transfers / loads (strand B feeds) and XMA register
accesses (the audio census) per frame in the gameplay window - goal all zeros. `[NATIVE_METER]`
every 2 s (with SR_HIGHFPS_PACE=1), `[NATIVE_METER_RUN]` every 30 s and at exit;
`tools/perf/native_meter.py <roll>` gives the ingame per-frame average, the worst frame and a verdict.
Unwired counters print `-`, never 0.
**`dispreg` (2026-10-09, native-a; the fourth C1 class, display-register answers).** COUNTING RULE, drawn before any
number was taken and confirmed by the orchestrator with the ledger's `display.registers` record: the column counts
only the emulated answers - the scan-out register writes that answer the guest's vblank-gate reads (D1MODE_V_COUNTER
+ VIEWPORT_SIZE at 0x7FC86530 / 0x7FC86584, two per vblank tick, `SR_VD_SCANOUT_REGS`), the system command buffer
descriptor fill (one per `VdGetSystemCommandBuffer` call, `SR_VD_SYSCMDBUF`) and the kernel vsync-enable MMIO bit
0x7FC86544 (once, at `VdSetGraphicsInterruptCallback`). Kernel exports a native port implements anyway
(`VdQueryVideoMode`, `VdGetCurrentDisplayInformation`, `VdQueryVideoFlags`, the gamma ramp) are NOT counted: a
kernel export answered by host code is the native implementation (the UnleashedRecomp standard), not
hardware-register emulation. The rule is never moved to reach zero; the P2-B GATE item retires the scan-out answers.
**`shm` (2026-10-15, release/mem-meter; ledger `mem.resources` step 1, docs/RELEASE_COMPOSITION.md section 7: the
shared-memory page model's work).** ONE COUNTING RULE, drawn before any number was taken (made one after Astra round 1
on the branch): a count per 4 KiB guest page per DECISION EVENT in which the model decides whether its copy of the page
is current - (a) a validation before a draw's vertex / index / texture fetch or a scan-out, decided by a hash
(`pm4ShmValidate`), by the write watch's clean state (`pm4ShmRequest`, `SR_SHM_WRITE_TRACK`) or by the range cache's
proof that the range stayed clean since its last validation (`SR_NB_SHM_RANGE_CACHE`, one per page of the range);
(b) a re-baseline after a GPU write, decided by a hash (`pm4ShmRangeWrittenByGpu` locked or unlocked, the pre-hash
`pm4ShmRebaseHash`, the hash a readback takes of the bytes it wrote back) or by the write watch's kept generation
(`SR_SHM_WT_REBASE`). A page validated and re-baselined in one frame counts twice (two decisions). NOT counted: same-epoch
dedup hits (the range memo, `SR_CP_SHM_LEAN`, the epoch stamps), a prepass page the resolve mask covers, the oracles'
re-asking (`SR_SHM_WT_CHECK` / `SR_NB_SHM_RANGE_CHECK` hashes, the live-trace recorder, the resolve-hash debug), and
hashes that are not page decisions (fetch-constant keys, the zero page, shader text). Fed by `gfx/d3d12_pm4_shmem.cpp` at
every site `tools/perf/test_shm_meter_wiring.py` enumerates (a hashing site the test does not classify fails it); wired
from the first frame; part of the 'all zeros' verdict; printed between `dispreg` and `xma`. The rule is never moved to
reach zero: 0 means the GPU's memory reads reached no page-model decision that frame, which is what mem.resources'
resource registry (invalidated by the guest's own coherency records) must achieve - the write-back of resolved bytes
into guest memory stays, no hash of them does.

First reading, the baseline driven to 0: **60,776 PM4 packets per ingame frame**, worst frame
127,643. Run bc1b, exe 37a914fd4b23 (dev 690ef745 + native/n0), gameplay/port, uncapped,
ingame window 116.4 s at 111.9 fps (12,962 frames). The whole-run average is 40,390/frame.
- edram_* print `-` (strand B feeds them).
- xma prints `-`: the exe compiled native_meter.cpp before the audio census header existed. The
  next exe reads it.

**p1.flush_residue BUILD NOTE (native/flush-residue, 2026-10-14, written before building; base dev 041d6f9e; gate
`SR_NATIVE_FLUSH_SPLIT`, default OFF - default ON since the 2026-10-14 bake, item 8 -, `SR_AB_TOGGLE=SR_NATIVE_FLUSH_SPLIT` per present; IDA read-only on the retail
XEX).** The S1 draw flush's remaining guest PM4 has three causes (native/transport-composed's GAP MAP, roll pair rcen1:
238.95 packets a present in census1 inside 820FBBF8 / 820FC000 / 820FB6E8 after its construns key split, plus row 9's
declines, 3.69). Each is converted whole. Rule 1 holds per entry: a case the native path cannot take declines before
anything is written, and the guest body runs.
1. **Segment-limit section fallbacks (25.0 a present: 21.2 constRuns aborts, 3.8 regRuns / fetchRuns).**
   - *Mechanism.* In kernel/native_flush.cpp (041d6f9e), `Driver::constRuns` (1006-1089), `regRuns` (1092-1113) and
     `fetchRuns` (1118-1146) plan a whole section. They test each run against the segment limit exactly as the XDK's
     fast paths do: 821078C0 `cursor + 4*(16n+3) < dev+0x2C`, 82107520 `cursor + 4n < limit`, 82107760
     `cursor + 4*(6n+5) < limit`. A run that does not fit sends the WHOLE section to `guestLeaf` (1150-1163, called at
     1027 / 1104 / 1130). For constants, the guest leaf 821078C0 is the dedup leaf (kernel/native_leaves.cpp
     `emitType0Dedup` 439). It writes the S2a-deduped runs as PM4 type-0 runs with type-2 alignment pads (census1: 131.6
     pads a present). When even a deduped run does not fit (native_leaves.cpp 517), the plain XDK body runs. 82107520 and
     82107760 are always the XDK bodies.
   - *The limit, and who sets it.* dev+0x2C belongs to the SDK's own segment accounting. GpuCmdBufBeginNewSegment
     820FCF90 sets limit = segment start - 4 + size - 56 (14 dwords are kept for the 10-dword fence tail and the 4-dword
     jump block) and sets the kick threshold dev+0x30 = limit - 160. GpuCmdBufAllocate 820FCDF0 can close a segment early:
     when the allocation total dev+13512 exceeds dev+13516 and no recording pin is set, it sets threshold = cursor and
     limit = cursor + 160. The user-buffer carve lowers both values as well. So a run longer than about 40 dwords fails to
     fit when it comes after a passed threshold check, or right after an allocation (UP and Begin/End vertices). Under
     segmentless the limit means nothing physical for records. It is still guest-visible SDK state, because the cursor
     dev+0x28, each segment's S (+2 per close), the tail allocator and the kick sites all follow it (P2-B kept them).
   - *What the XDK does at the limit* (GpuCmdEmitRegisterRunChunked 82107058, IDA). It never rewinds. For each run that
     does not fit:
     - avail = ((limit - cursor) >> 2, signed) - 1 dwords;
     - n = (avail / elem) * elem registers (elem 16 / 1 / 6; signed divw), capped at the run's length;
     - if n > 0: a header and n values, with NO alignment pad. The values are read word by word from the exact source
       address, not from the lvx-aligned group;
     - if registers remain: the cursor goes to dev+0x28, GpuCommandBufferFlush 820FD8C0 runs UNCONDITIONALLY, the
       cursor is re-read from dev+0x28, and the loop repeats.
     The leaf then continues with its next run on the fast path. In 82107760 the trailer depends on the last run: after a
     chunked last run it is {0x00025000, 0, 0, 0}, preceded by a kick check (cursor > dev+0x30 -> 820FD8C0); after a
     fast-path last run it is {0x00025000, 0, 0x00025000, 0}, with no check.
   - *Design: records across the boundary; the limit is NOT lifted.* Under the gate the three sections port 82107058 in
     place of the guest leaf. At a run that does not fit:
     - the planned prefix is committed (S2a: the mirror stores at commit, rule 2);
     - chunk items follow: plain runs, pad 0, read from the exact source;
     - the Driver's unconditional `kick()` runs between chunks. It is the same kick the bind's reserve and the construns
       chunker use: closeSpan, then 820FD8C0, which is the native segmentless close by default and the guest close on
       the ring path;
     - the section then continues.
     Records sit on both sides of the segment switch. The cursor, the kick count, S, dev+0x2C / 0x30 and the tail
     allocations equal the SDK's, byte for byte. Lifting the limit for record-backed segments was rejected: it would move
     every later cursor, S value and tail carve away from the SDK's, and the oracles (the dry CHECK, the CPPM4 / PM4
     splits, the difftest's byte equality) could no longer compare them.
   - *Dedup.* A chunk emits every group: no skip and no CHECK flag. Its groups are stored at its commit. A chunk written
     before the kick belongs to the old epoch, and the kick's ddSync drops every group (the epoch rule). The S2a skips stay
     on the fast-path runs, as today.
   - *Census.* Under the gate `sectionFallbacks` and `sectionAborts` stop moving. New counters (chunked sections, chunks,
     chunk kicks per leaf kind) print on a `[NATIVE_FLUSH] <why> split` line.
2. **Draws with more than 0xFFFF indices (1.07 a present).**
   - *Mechanism.* `declined()` (2450-2453) refuses every kind except 5 when count > 0xFFFF (`count>0xFFFF=`). So
     `draw()` (2511) returns false, and the guest 820FC000 / 820FBBF8 runs whole: every dirty section and the split draw
     go out as PM4. This produces census1's 2.46 DRAW_INDX a present, plus these draws' share of the section packets.
   - *The XDK* (IDA: 820FC000's loop 820FC27C..820FC468, 820FBBF8's 820FBE68..820FBFF0). Per chunk:
     - the kick check (dev+0x28 > dev+0x30 -> 820FD8C0);
     - {0x2102 = r5}: baseVertex for indexed draws, startVertex for auto-indexed ones;
     - chunk = count when count <= 0xFFFF, else ((0xFFFF / ipp) & ~1) * ipp. Here ipp = dword 820B3668[8*prim] (divwu;
       the XDK traps when it is 0);
     - the DRAW_INDX with the chunk. For indexed draws, dmaBase comes from startIndex * 2 (or * 4) + ib[6], and size =
       chunk (or 2 * chunk for 32-bit indices);
     - then count = overlap + count - chunk and start += chunk - overlap, with overlap = dword 820B366C[8*prim].
     The table (retail .rdata, as (ipp, overlap)): prim 1 (1,0), 2 (2,0), 3 (1,1), 4 (3,0), 5 (1,2), 6 (1,2), 8 (3,0),
     13 (4,0); every other prim 0. Only these two flushes read it; BeginVertices 820FB6E8 has no split.
   - *Design.* Under the gate `declined()` admits kinds 0 and 1 with count > 0xFFFF when the table's ipp is non-zero (read
     from guest memory, as the XDK reads it). `drawPacket` loops as above: `kickPoint()` per chunk, then one kRkDraw record
     with its 0x2102 run. No new record kind is needed, since each chunk's count fits the 16-bit initiator field. Still
     declined: ipp == 0 (the XDK traps, and the guest body does the same) and BeginVertices (kind 2). New counters cover
     split draws, their parts, kicks and declines.
3. **Row 9's declines (3.69 a present in census1).**
   - *Every decline reason seen in gameplay.* The source is the `[NB_REC] rec` lines in rcen1's run logs. Over census1's
     window 11:34:40 -> 11:36:40, resolve declined 98 times, clearF 41 and gpr 35 (about 0.05 / 0.02 / 0.02 a present).
     Over census0's whole run, resolve declined 95, clearF 32 and gpr 34. Every one has the reason `kick`, with `open=0`
     and `live-stop=0`. No tiling, unmapped, format, list or allocator stop occurred.
   - *Mechanism.* kernel/native_rec_b.cpp `runRecord` (961-1027) runs a pre-pass, the port dry on a device copy, which
     must predict a stop-free live pass before anything is written. It cannot model GpuCommandBufferFlush. So `Prod::kick`
     (398-411) stops at any kick site that would kick (`c > dev+0x30`), and `regRuns` (550-569) stops at a run that crosses
     the limit. The whole entry then declines, and the guest body writes PM4: resolve 8210C5F8, ClearF 82101A58, or
     SetShaderGPRAllocation 820F5570 with ClearFullSurface 82100488.
   - *Design: a kick-transparent pre-pass (record mode only, under the gate).* A would-kick no longer stops the pre-pass;
     it marks `kicked` and continues on the copy. That is exact for every stop the live pass can still meet. Per IDA, the
     kick family (820FD8C0, 820FD3B8, 820FD660, 820FD580, 820FD800, 820FCF90, 820FCDF0, and the native close
     flushSegmentless / coherencyBlock / segmentClose) writes only:
     - the cursor, limit and threshold (0x28 / 0x2C / 0x30);
     - the segment and allocator fields (13000.., 13160.., 13496 / 13504 / 13508 / 13512 / 13520, 0x34C4);
     - the fence S at 0x2A1C (+2 per close), with 0x3298 / 0x329C;
     - the coherency range 0x2D18, and the 0x40 / 0x02 bits of byte 10809.
     No stop condition in the three ports reads these fields, with three exceptions, each handled:
     - (a) the cursor-dependent run fit: `regRuns` ports 82107058 too (chunks with Entry kicks), so it has no stop at all;
     - (b) the allocator: after a kick the pre-pass assumes 820FCDF0 succeeds, using a placeholder range whose writes are
       not mapping-checked (the real allocation is command-buffer memory). A live failure just takes the XDK's failure
       branch, which returns or skips one draw and meets no stop that the success path did not already meet;
     - (c) the resolve's 335C list, reached only when S == 0: fences are odd and each close adds 2, so the pre-pass
       declines if `kicked` is set and S is even (never expected).
     The live pass is unchanged; its kicks are the Entry's real ones. The dry CHECK (SR_NB_REC_CHECK) keeps every kick as
     a stop, since it compares bytes. New counters count entries taken through a transparent kick; the decline reasons
     print as today, and `live-stop` must stay 0.
   - *Not handled* (the build note said "none occurs in gameplay"; REFUTED by frs1, see item 7): every other stop
     reason listed in nb_rec_resolve.inc / nb_rec_clear.inc / nb_rec_gpr.inc - apart from the GPU wait, which
     native/flush-residue-2 models (item 7).
4. **Composition.**
   - *Segmentless:* the kicks for chunks and split draws are ordinary Driver / Entry kicks (footprints by sequence, 2c),
     with records on both sides.
   - *Transport kinds:* inside a Z-pass bracket a kick is a pinned close, bound in order by BRACKET exactly like today's
     guest-leaf kicks. RESIDUE, SWAP, GATE and CALLBACK are untouched.
   - *CPPM4:* chunks render back to exactly the SDK's chunk bytes (no pads to drop). Each split-draw chunk renders back
     to one 0x2102 run plus a DRAW_INDX.
   - *The PM4 output split* writes the XDK's bytes.
   - *The dry CHECK* (SR_NATIVE_FLUSH_CHECK) now compares up to a chunk's kick (before: up to the section start), and
     compares draws with count > 0xFFFF up to their first kick. Its checkAfter still skips any call in which the guest
     body kicked, so chunks are proven offline (below).
   - *STRESS* changes the kick checks only; the chunker uses dev+0x2C, as the SDK does.
   - *Cursor:* the cursor now follows the XDK's full length, where the dedup leaf's deduped PM4 was shorter. So segment
     boundaries shift slightly against gate OFF; they are the SDK's own boundaries.
5. **Proof plan.**
   - *Offline, on the cloud worker:* difftest `--flush-split-test <mode>`.
     - It drives the actual SDK bodies (820FC000 / 820FBBF8 with the dedup suspended; resolve, ClearF and 820F5570
       through `__imp__`) against production dispatch, with the gate's per-present A/B choosing the arm.
     - Fixtures force each path: constant, register and fetch sections crossing the limit (cursor % 16 variants, first
       and last runs chunked, a run needing several kicks, the fetch trailer both ways); count > 0xFFFF draws, indexed
       (16- and 32-bit) and auto-indexed, for prims 4 / 6 / 3 / 13 / 8, including a chunk across a kick, and ipp 0
       declined; row 9 entries whose kick sites kick.
     - Compared: every segment's PM4, byte for byte (the SDK's guest bytes against the records rendered back by
       cpRecordPm4, type-2 pads dropped); the device, cursor and S; the register file and draws an emulated CP applies;
       the counters.
     - Modes records / cppm4 / pm4 / gate-off, plus compiled mutants that must each fail (chunk size, a pad in a chunk, a
       missing kick, the trailer form, the split overlap, the transparent alloc, the parity guard). Every existing
       difftest suite's exit code must equal dev's.
   - *Live:* `native_proof.py --plan flush_split` (dry-run only here): replay, probe, frozen per-present A/B, CPPM4 and
     stress, the producer census pair, the meter and the flicker gate. In the census pair (census0 gate OFF, census1
     gate ON), the flush producers' non-construns keys and row 9's producers must be absent from census1, and
     `sectionFallbacks`, `count>0xFFFF` and row 9's `pre=` must not move.
6. **Status (2026-10-14, native/flush-residue on dev 041d6f9e; code 151c0aa5, Astra round 1 fixes c0b524f7 - its
   matrix, suites, default + release builds and audit in archive/frs/r1 - and Astra round 2 fixes, theirs in
   archive/frs/r2): built, proven OFFLINE; no live roll yet.**
   - *Built* as designed above, behind `SR_NATIVE_FLUSH_SPLIT` (default OFF; one decision per draw / per row-9 entry):
     `Driver::chunkRun` (82107058) under `constRuns` / `regRuns` / `fetchRuns` with the fetch slow trailer;
     `Driver::drawPacket` / `drawPart` (the 820B3668 split); `declined()` admits kinds 0 / 1 over 0xFFFF only with a
     non-zero table entry; row 9's `Prod::transparent` / `kicked` / placeholder allocations, `Prod::flush` ->
     `Entry::flush` (82107058's unconditional 820FD8C0) and the resolve's even-S guard. Counters: the `[NATIVE_FLUSH]
     <why> split` line, `[NB_REC] ... | split kickT= chunks= kickTStop=`.
   - *Offline proof* (worker, `tools/difftest/flush_split_run.py`; round 0 at 151c0aa5 / c3d719e2 in
     archive/frs/split_matrix and archive/frs/final; after Astra round 1 in archive/frs/r1/split_matrix; after round 2
     in archive/frs/r2/split_matrix): `--flush-split-test` drives the actual SDK bodies against production dispatch on
     136 fixtures. The 52 base cases run on both the device-callback allocator and the game's generation ring
     (820FCBE8); the 22 later-kick row-9 sweep cases and the 7 ring-wrap cases on the ring only (on the callback an
     allocation before the kick declines the pre-pass, "alloc via the device callback", a path the game never takes);
     the 3 allocation-failure cases on the callback only - 55 callback + 81 ring. (Round 0 said "76 fixtures, every one
     on both allocators": it was 27 callback + 49 ring, the 22 sweep cases ring-only - Astra round 1, corrected here.)
     The cases:
     - constant / register / fetch sections crossing the limit (cursor % 16 variants, a run through 19 kicks, a
       one-register chunk, the slow and fast fetch trailers);
     - >0xFFFF draws: 16- / 32-bit indexed and auto-indexed; trilist, tristrip, linestrip, line list, triangle fan,
       quad list, rect list, point list; both sides of the 0xFFFF / 0x10000 transition (0xFFFF taken whole by both
       arms, 0x10000 the smallest split); the per-part index offsets (VGT_INDX_OFFSET 0x2102 and the index DMA address:
       a negative baseVertex with startIndex 0x37B7FFF0, whose 16-bit index address wraps 2^32 BETWEEN the parts -
       0xFFFFFFE0, then 0x1_0001FFD8 -> 0x0001FFD8 - asserted per part in both arms as the physical DMA bases 0x20000FE0
       / 0x0001FFD8 with 0x2102 = 0xFFFFF000 in each; an auto-indexed strip whose per-part startVertex 0x2102 wraps,
       0xFFFFFFF0 then 0x0000FFEC, asserted. Astra round 2: round 1's version used startIndex 0x7FFFFFF0, whose address
       wrapped in BOTH parts, 0x908FFFE0 -> 0x9091FFD8, so nothing wrapped between them); a visibility query active
       (every part's viz dword, byte for byte under SR_NATIVE_FLUSH_PM4 - records drop it, the PM4 path never reads it);
       tile auto-predication (declined in both arms: the guest body's per-tile list); a part that kicks; `big-ipp0`
       declines before writing;
     - the game's instanced path (Astra round 2): the GENERATED body of RwDrawInstancedConstBatched 8261B050, called in
       every arm (the SDK arm with its 820FBBF8 / 8210D400 calls declined to the XDK bodies on the thread - a
       difftest-only switch, `t_fsGuestOnly`). It binds stream 0 to the mesh and stream 1 to the instance-index stream
       (SetStreamSource 820F2A50, and unbinds stream 2), writes the sampler words and c4 into the device, and per batch
       of 128 / regs instances uploads the instance constants at c100 (SetVertexShaderConstantF 820F4920, the production
       leaf in every arm) and draws instances x vertices-per-instance through DrawVertices 820FBBF8 over 0xFFFF. Three
       shapes: one tile (300 instances x 600 vertices: 2 split draws + 1); two tiles with SetPredication 3 / 12 / 0 and
       1200 bytes of room, so the first c100 run (2060 bytes) is chunked across the kick (12 draw packets); and two
       tiles each inside a visibility region (Begin / EndRegion 82102448 / 82102608: the draws' viz dword, and the
       regions' own SET_BIN_MASK_LO / HI pairs, guest PM4 counted apart - 8 packets, the flush's own 0). NOT built
       offline (recorded, not claimed): its VS / PS binds 8260B800 / 8260B898 (shader state, called with 0 here) and the
       TilePush / Pop form of the regions (a30 == 1, 82102010 / 82102228). The SDK arm is asserted to have taken none
       of them natively, the ON arm all of them (SetPredication once per tile and once at the end). Under cppm4 the
       regions case's OFF arm (today's path) is held to the per-draw state: with no kick between its tiles the second
       tile's c4 group is still resident, so today's guest constant leaf writes SR_NATIVE_CONST_DEDUP_CHECK's 'DDCK'
       mark and splits its run there (emitType0Dedup, by design) - asserted present, not hidden. The negative-baseVertex
       cases above are index offsets, not instancing (round 1 called them "the Xenos instancing idiom": withdrawn);
     - row 9's three entries (Resolve, ClearF, SetShaderGPRAllocation) with the threshold at every kick site, a run
       crossing the limit inside one, two decline witnesses (a tiled restore past the post-kick allocations, the
       resolve's list path behind an even S) and a tail allocation failing after the transparent kick (the XDK's
       failure branch, the sticky byte, the next close dropping its segment - in both arms);
     - every kick a PINNED close (a Z-pass recording pin: mode-B list records, nothing published, published as the
       pump's replay publishes them) for 7 cases; the generation ring wrapping at the kick (generation 2) for 7.
     Compared with the SDK arm: published segments (phys, dwords) and the pinned list records, device, cursor, ring,
     allocator, the scratch lists, the canonical stream (records rendered back by cpRecordPm4) or the register state at
     every draw, the consumer's register file and draws, the vertex allocations; under SR_NATIVE_FLUSH_PM4 the command
     buffer byte for byte. Round 1: modes records 1797 / records-raw 2477 / cppm4 2477 / pm4 2619 / gate-off 1259
     checks, 0 failed. Round 2 (136 fixtures): records 1907 / records-raw 2617 / cppm4 2611 / pm4 2765 / gate-off 1345,
     0 failed. Eleven compiled faults each fail their named assertions with no SDK-arm
     failure and pass the gate-off control (chunk one dword short, remainder dropped, slow trailer in the fast form,
     split overlap ignored, part not rounded to even primitives - now also on the 8261B050 split - big draws not
     admitted - also the 8261B050 cases - the pre-pass allocation failing past a kick, the even-S guard removed, the
     pre-pass not chunking, the pre-pass opaque at kicks, and (round 2) the index DMA address saturating at 2^32
     instead of wrapping, which only the between-parts wrap case sees); sources restored, the restored build passes
     all five modes. The suite also prints the live proof's snapshot line, which `tools/perf/flush_split_snapshot.py` must parse
     field by field (the format contract between the game and the grader).
   - *Suites and builds* (round 0 at c3d719e2 in archive/frs/final; after Astra round 1 in archive/frs/r1; after round 2
     at c44d899d in archive/frs/r2, the same results - the matrix valid with all eleven faults detected, the suites
     identical, the audit's units unchanged): every
     difftest suite run bare (the runner adapted from dev's dev_4f6c7972, `suites_compare.py`): the 79 dev suites' exit
     codes identical to this lane's 041d6f9e baseline (61 at 0, the same 18 nonzero; the baseline itself identical to
     dev_4f6c7972's), `--flush-split-test` 0. The default build (every target) and the release build
     (SR_RELEASE_COMPOSITION=ON) build; the C1 composition audit's 472 units are entry-for-entry dev_4f6c7972's. Its one
     extra failing / unknown item (kernel/guest_protect.hpp, "no manifest rule") arrived on dev after 4f6c7972
     (bd1a3878 / bc568389, in this base) and is not this lane's.
   - *Merged with dev ce46c966* (Astra round 4: MERGE; merge 0191f035; worker results in archive/frs/m1, the dev
     baseline in archive/frs/mbase): the default and release builds build; the matrix is valid with the same totals
     (records 1907 / records-raw 2617 / cppm4 2611 / pm4 2765 / gate-off 1345, 0 failed), all eleven faults detected
     with their gate-off controls, sources restored and the restored build passing every mode, the snapshot contract
     parsed; dev's composed present matrix (`present_composed_run.py`: 15 modes, 8 faults - this lane touches
     setPredicationLeaf and draw(), which it covers) valid; the 81 dev suites (the 79 + `--present-composed-test` +
     `--shader-boot-load-test`) run bare give exit codes identical to dev ce46c966's own (62 at 0; dev's
     `--swap-sdk-test` fails 22 bare in both, 0 at 041d6f9e: dev's, not this lane's), `--flush-split-test` 0; the C1
     audit's 472 units identical to round 2's, its unknown item gone (ce46c966 moved kernel/guest_protect.hpp to
     content), the verdict FAIL on the standing 25 emulation units as on dev. The ledger is dev's (its one
     p1.flush_residue record); `native_ledger.py --strict` PASSES against dev's evidence (the repo root at the dev
     checkout, where the lane-root archives of the baked records live), and `--write-status` regenerates the README
     / roadmap blocks byte-identical. What the merge changed in the design: the snapshot steps and the new
     `--flush-residue-zero` census step follow dev's run-unique comparison discipline (above).
   - *Still declined under the gate* (counted): a draw over 0xFFFF whose prim has a 0 table entry (prims 0, 7, 9-12, 14,
     15: the XDK's divwu traps; `big decl=`) and BeginVertices 820FB6E8 over 0xFFFF (no split in the XDK);
     the pre-existing S1 declines (tile auto-predication, a live trace, devSpan / arena / ib); every non-kick row-9 stop
     but the GPU wait (frs1 met the GPU wait - item 7 - and no other) and the new even-S guard (never expected). The dry
     CHECKs still stop at kicks and at the GPU wait.
   - *Live:* `native_proof.py --plan flush_split` (dry-run only so far: archive/frs/plan_dry.txt, after round 2
     archive/frs/plan_dry_r2.txt): replay, the S1 dry
     CHECK, row 9's dry CHECK (SR_NB_REC_CHECK=1: record mode off, every Resolve / ClearF / GPR compared - completed
     comparisons > 0 and all ok per entry, no [NB_REC_BAD] anywhere), probe + P2 trace, the frozen per-present A/B,
     CPPM4, stress, the census pair (census0 gate OFF / census1 gate ON, SR_NATIVE_BRACKETS=1: the S1 flushes'
     non-construns keys and row 9's producers absent from census1), the meter (planning estimate -230), the gate.
   - *The strict bar, boot through the last snapshot* (the orchestrator 2026-10-14; Astra rounds 1-3): every pure-ON
     roll - probe, CPPM4, stress, census1, meter1 and both gate rolls - is graded on cumulative counter snapshots, not
     on a sampled line, and only from boot through the last snapshot; freshness relative to the last logged timestamp
     allows 1.0 s (`fresh_bound_s` = the 0.5 s period + 0.5 s, checked by the items and pinned by the grader's
     self-test: a tail at the bound passes, 1 ms past it fails); the remaining execution tail is unmeasured. The
     snapshot is NOT terminal and nothing here claims a whole-run zero: the rolls end by Stop-Process (sr_drive has no
     quit hook; a graceful-quit hook is acceptance.zero's own infra item, as is the census's identical gap), and the
     freshness check measures the gap to the log's last timestamped line, not to termination. (Round 2's wording
     claimed a tighter tail than the grader enforces, which accepts period + 0.5 s - Astra round 3 reproduced a clean
     grade at tail_s 1.0 - so it is withdrawn.) Under the gate the game prints
     `[NATIVE_FLUSH] split snapshot` (kernel/native_flush.cpp `splitSnapshot`: count>0xFFFF with its two
     non-convertible classes - an XDK table entry 0, BeginVertices - counted apart, section fallbacks / aborts, and per
     row-9 entry calls / taken / open / pre / live stops / kickT / kickTStop, all cumulative from boot) on the first
     taken (or dry-checked) draw at least 0.5 s after the previous line: `maybeReport` reads the clock on every such
     draw (round 1 read it every 1,024 draws, which bounded nothing - Astra round 2), so while draws run the lines are
     0.5 s apart plus at most one inter-draw gap and the tick granularity (~16 ms); with no draw there is no line. The
     line is formatted and logged under the producer lock (`g_mu`, draw()'s lock_guard) once a period; that cost to
     the other producer threads is to be measured in a roll (planned with the live plan, not measured yet). The plan's
     `snapshot` steps (`tools/perf/flush_split_snapshot.py`, self-tested) require the last snapshot within 1.0 s of the
     log's last timestamped line (period + 0.5 s), the counters never falling, no snapshot ever showing a residue, and in the last
     one (`last.*`, boot to its time): no convertible count>0xFFFF decline, no section fallback or abort, every row-9
     call taken (calls == taken: open 0, pre 0), no live stop and none after a transparent kick. The sampled `[NB_REC]
     rec` items of those rolls require open=0 and pre=0 with calls == taken too. The census items were checked against rcen1's real logs
     (archive/frs/check_plan_rcen1.txt: census0 shows the residue; census1 of that pre-split exe fails both forbids and
     has no snapshot line at all). The census pair's gap map is transport_census.py's flush_residue group: since the
     merge with dev ce46c966 a plan step of its own (`tcensus` with mode "flush": `--flush-residue-zero`, the group 0 in
     census1 and > 0 in census0 over the census-covered ingame interval, both censuses complete, reconciled and mapped -
     the record's observation; `flush_residue.pk` always written, the tool's tests extended), a run-unique comparison
     output like the snapshot steps, which since the merge also follow dev's discipline (reserve_output, the verdict
     stamped, committed last; `--recompare` re-grades them). This lane adds no guest producer VA to the map (8261B050's
     draws are 820FBBF8's scope; nothing new reads UNMAPPED).
7. **frs1 and native/flush-residue-2 (2026-10-14; dev 5aab5ddd).** frs1 (the live set on exe 2087ca4acdb4, the
   coordinator's bank native/archive/v1_native/frs1) passed replay, the probe and its trace and snapshot, the A/B and
   stress (15/15 each) and census1's snapshot, then stopped on a tooling error (fixed on dev 5aab5ddd). Two findings,
   diagnosed offline:
   - *cppm4 "WORLD NOT DRAWN" (dark 3, 8-15): a scene, not a rendering defect.* The open-loop tape left Superman on
     foot against a black facade (the movement readback stays 10 where stress / abp end in flight at 2; present rates
     63-75 / 40 / 65-80 a second under the same wall-clock inputs), so the world rule's centre near-black share reads
     0.62-0.73 (> 0.6) at luma 11-16; the street, a pedestrian, the HUD and the minimap are drawn (freeze 15), freezes
     1-2 (the same scene in both rolls) measure alike (25.5 / 25.2, 28.3 / 27.3), and every counter is clean (CP bad /
     torn / NEVER-EXECUTED 0, sectionFallbacks 0, the dedup CHECK's BAD 0, row 9 render-len-bad 0, no hang). The closed-
     loop freeze tape is a design note (archive/frs/tape_closed_loop.txt) for the coordinator to schedule. The step's
     "tail -1.000 s" was native_proof's row text turning the grader's legitimate 0.0 tail into -1.0 through `or`
     (`snapshot_detail` now prints the value, n/a when absent; tested).
   - *stress_snap: clearF pre=1, calls != taken - a real row-9 residue the split did not convert.* The record pre-pass
     stopped at "alloc would wait for the GPU" (kernel/native_rec_b.cpp Prod::alloc: 820FCB30 waits on the progress
     word the CP writes); rec_chk's resolve CHECK met the same stop in plain gameplay, and frs1b (the same exe and plan)
     twice more: meter1_snap (resolve pre=1) and gate_span_snap (clearF pre=1), each "alloc would wait for the GPU=1" in
     the `[NB_REC] rec` line - 3 of the 7 snapshot-graded rolls, no other stop reason. Modelled now in RECORD mode under
     SR_NATIVE_FLUSH_SPLIT (no new gate: the pre-pass's `transparent`, the gate and its per-present window; with
     SR_NATIVE_FLUSH_SPLIT=0 row 9 is p1.row9's baked path, unchanged): the pre-pass marks `waited` and goes on (the allocation is v10 either way); the live
     pass's real 820FCDF0 waits inside the Entry as the guest body would - IDA: 820FCB30 never kicks and its spin
     primitive 820F33E8 only reads the progress word; its report 820F3328 adds the wait time to dev+20024 (+20032 for
     kind 3) and calls the timing callback dev+13068 when set; nothing in the title writes dev+13068 (the generated
     code holds one access, the report's load), and the pre-pass stops anyway if it is set. The CP thread never takes
     the producer lock g_mu (only draw() and Entry::open do), so a wait inside an Entry completes as the guest body's.
     The dry CHECK keeps the stop. `waitT=` per entry on the snapshot line and the `[NB_REC] rec` line.
   - *The census (frs1 tcensus FAIL, target_flush_residue_zero only): 243.947 -> 0.017 a present, all of it 820FC000's
     `o1` - 821075E0's alignment pad, p1.row3_construns' output, not flush residue.* `o1` is the census's type-2 NOP key
     (gfx/pm4_coverage.cpp: 512 + kType2). Before each fast-path run GpuCmdEmitShaderConstantRuns 821075E0 stores a NOP
     at cursor+4 and writes the run's header after it when the cursor is 4 mod 8, over it otherwise (IDA 0x82107708 /
     0x82107720; the native port `shaderConstantRuns` pads the same way): at most one pad per run, on its caller's line.
     census1's 820FC000 line held nothing else from boot to exit - only the construns keys (r20 = 3C = r23, r0A twice)
     and `o1` (17 of 131 calls at its first report, 669 of 3,030 by the interval's start, 30 of 5,376 inside it) - and
     its snapshot counters show no other guest writer in the scope (sectionFallbacks / sectionAborts / afterPlanning 0,
     count>0xFFFF 0, row 9 pre 0). Not the wait stop (row 9's producers are their own lines, absent from census1), not
     a draw shape the split declines. The KEY_SPLIT moved 821075E0's four keys but not its pad, which the S1 leaves
     (821078C0 etc.) share in the base. A first attribution by counts (o1 on a construns-only line, at most r23) was
     withdrawn after Astra's round 1: counts prove neither ownership nor one pad per run (two pads on one run, a stray
     NOP), and the cumulative text item never compared them (an opening report r23:10, o1:20 passed). The census now
     WITNESSES the pad by its address: gfx/pm4_coverage keys a type-2 NOP `o6` (kConstRunPadKey) when its next dword in
     the same window is 821075E0's run header - type-0, 4k registers inside [0x2388, 0x23A0), where no S1 leaf's run
     starts (theirs end at 0x2387) - and every other NOP `o1` as before (counted as a type-2 NOP everywhere else; inert
     without the census: the read is behind the engine's `cov`). transport_census.py gives `o6` on an S1 line to
     p1.row3_construns (KEY_SPLIT); every `o1` there is flush residue, and --flush-residue-zero also grades the target
     FROM BOOT (its last report is cumulative and counts never fall: target_flush_residue_zero_from_boot, in the census
     item). native_proof's `s1` rx allows `o6`, never `o1`. Tests: the census suite's construns-pad scenario (the witness's
     header rule, then o6 for the pad before a run, o1 for an ALU run's pad, the first of two NOPs, a one-register
     write's and the window's last dword), transport_census (o6 passes; o1 on a construns-only line, o1 alone, an S1
     leaf's key fail; an opening residue fails from boot while the interval reads 0; the opening's o6 passes) and the rx
     (frs1's o1 line and the opening's r23:10,o1:20 match). An exe without the witness (frs1's 2087ca4acdb4, fsb1's
     a8398dee94d3) prints only `o1`: its census reads the pads as residue - the frs1 items stand as recorded, and a census
     pair proves the zero only on an exe with the witness. On the worker at aaf23b77: the census suite in its four modes
     (native 88, residue 86, segmentless-off 77, gate-off 36 passed, 0 failed; census on and off print the same digest),
     with the witness disabled the construns-pad check fails (87 / 1: the four NOPs and two pads all o1) and the restored
     build passes again; the new Python tests fail against the tools of c48f6bdc (transport_census 5, native_proof 4 and
     the bake shape) and pass on the fixes; the 82 suites as before but --native-census-test 85 -> 88 (the new checks) and
     the worker's timing suites (audio-out, timebase: identical on both trees re-run alone).
   - *Live follow-up (the plan as it stands, on an exe with the witness):* every snapshot step pre=0 and waitT counted
     (stress / meter1 / gate_span met the wait in frs1 / frs1b; any later decline names its reason), the census pair's
     flush_residue group 0 over the interval and from boot, census1's `s1` / `row9` items clean; rec_chk's dry CHECK
     still shows the stop (by design).
   - *Offline proof* (the generation ring with the GPU a generation behind and a helper thread playing the CP: each
     row-9 entry waits in all three arms, the accumulators > 0 in each and masked from the device comparison, every
     other fixture's accumulators 0; a transparent kick then the wait; a twelfth compiled fault, the wait not modelled):
     VALID on the worker at 51d7d929 (the bake included; lane bank archive/frs/split_matrix_frs2b) - the positive build
     in all five modes (records 2,099, records-raw 2,829, cppm4 2,823, pm4 2,981, gate-off 1,521 checks, 0 failed;
     gpr / resolve / clearF -gpu-wait-ring taken through the modelled wait with waitT 1 and every arm's accumulator > 0,
     their OFF arms declined at the wait), the snapshot format contract (waitT parsed), all twelve faults detected
     (prepass_no_wait_model: exactly the three gpu-wait cases' "taken ... modelled GPU wait" and their no-gap checks
     fail, no SDK-arm failure, the gate-off control passes), every source restored and the restored build passing the
     five modes again. The difftest suites with the bake (SR_NATIVE_FLUSH_SPLIT on by default in every suite) against
     a 5aab5ddd baseline built in the lane's own worker tree: 82 of 82 with the same exit code and summary line but
     --flush-split-test (1,907 -> 2,099 passed: the new fixtures) and --audio-out-test (callback-rate timing on the
     loaded worker: 33/1 vs 30/4; re-run alone, both trees 32 passed / 2 failed, the same two); the S1-adjacent suites
     (native-residue, native-bracket, native-census, setconst-render, present-composed, segment ownership, record
     failure) identical.
8. **The bake (native/flush-residue-2, 2026-10-14; the owner's priority: flush-residue-2 the top lane).**
   `SR_NATIVE_FLUSH_SPLIT` is DEFAULT ON (kernel/native_flush.cpp flushSplitGate: unset = ON, `=0` keeps today's path
   for the A/B, in the release configuration too - SR_ORACLE=0 compiles out the in-roll toggle and the dry CHECK, not
   the gate, as for the earlier bakes). The evidence behind it is the frs1 / frs1b set (exe 2087ca4acdb4): replay 9/9,
   probe + trace, the frozen-pass A/B 15/15, stress 15/15, the gate 15/15 (world 15), the meter -240.9 pm4 a frame (t0
   142.3 -> 35.0, t2nop 182.2 -> 52.0); its three failure classes are item 7's: the open-loop tape's dark scene (cppm4),
   row 9's GPU-wait stop (modelled), the census pad (witnessed since Astra's round 1 - a census pair proves the zero
   only on an exe with the witness). Ledger p1.flush_residue gains the static `default_on` item; the claim stays
   `planned` (no live bake roll has run). The live bake proof is `native_proof --plan flush_split_bake` (prefix fsb):
   replay vs the pre-bake exe (--base), the new-game gate with the observe-only strict witness and the snapshot bar on
   both of its rolls, the default path under the CPPM4 split (Astra round 1: pixels exact per freeze, the tripwires and
   tile, the CP's consumer counts - S1 records run with none bad or torn, row 9's applied with no render-length
   mismatch -, the split's line, strict row 9 and the strict snapshot), the default-path meter (=0 vs no env, the
   flush_split bound -230), meter_on's split line, row 9 lines and snapshot, and a default-path census pair (=0 vs no
   env, both SR_NATIVE_BRACKETS=1) graded by --flush-residue-zero over the interval and from boot, with census1's
   snapshot. Every default-path roll now logs the `[NATIVE_FLUSH] split snapshot` line every 0.5 s (the
   gate is on): the proof's instrument, about 0.6 KB a line in a player's log too. The flush_split plan's OFF arms
   (census0, meter0) and the segmentless meters' arms pin `SR_NATIVE_FLUSH_SPLIT=0` (their bounds predate the bake;
   an older exe ignores the variable).

**S1 STATUS (2026-10-08 night): DEFAULT ON again, with the FLICKER ROOT CAUSE FIXED (PROGRESS iter-467 flicker; branch fix/flicker).** With S1 + S2a on, objects vanished or drew with stale constants for one present (owner report). The cause was S2a's mirror update WHILE PLANNING in `Driver::constRuns`: a section aborted to the guest leaf at the segment limit left the mirror claiming groups nobody emitted, and the dedup leaf / the next draws skipped them (stale VS transform 0x4010-0x401B, PS material 0x4404-0x444E). Fixed by storing at COMMIT (ffa023de; `SR_NATIVE_CONST_DEDUP_STORE_EARLY=1` = the mutation, which brings the flicker back: 14/15 freezes). Proof on the fixed path (S1 + S2a ON): dense sim freeze fix1 every gameplay freeze at the noise floor (10 of 12 exactly identical over 39 pairs, two at maxdiff 1 on <= 56 px) while the site fired 188,555 times; d0 span fix2 FLICKER PASS 1/358 (a motion-blur block at the threshold); in-roll A/B ab3 ON windows 0/555 outliers (the PM4 OFF windows 1/643, the PM4 path's own pre-existing rate); replay 9/9 byte-identical (S1 on and off vs base); forcing audit 2; NEVER-EXECUTED records 0 (66-102 before: the same site's spurious spanSkip). The flicker gate (TOOLKIT 1.8f, `flicker_gate.py` / `check_roll.py --flicker`) is now mandatory for every renderer / timing / caching merge. The paragraph below is the state as it was when it first went ON. The flush is
`kernel/native_flush.{hpp,cpp}`, `SR_NATIVE_FLUSH` (default ON, `=0` keeps the A/B).
- Default build (vr1, exe 418fc8be8bcc, ingame 118.9 s, 112.2 fps): **6,835 PM4 packets per ingame
  frame** (max 12,404) against 60,776 at baseline, and 4,502 native records per frame. Whole run:
  5,623 packets per frame.
- Correctness: fc5 (SR_NATIVE_FLUSH_CHECK, every 4th draw) 9.96M draws, BAD bytes 0, len 0,
  device 0; vr1 whole-run sheet correct in every era (intro, title, menu, movie, gameplay); icall 0;
  the CP read no bad records; replay 9/9 byte-identical against the dev-merged base (go7, dev
  420a30b1 with freezefx's quad split) with the flush on and off.
- Budget: met by the whole-frame criterion (ft9/ft10 in-roll A/B, pooled 118 windows): tick
  -0.19 +-0.19 ms, frame p50 -0.26 +-0.13 ms, fps +2.85 +-1.56, CP CPU within the CI. The
  per-draw CP bar (0.20 us) left out the bulk register copy both paths pay (PERFORMANCE 2.19).
- Still PM4 (the remaining ~6.8K packets per frame): the shader bind 82108668, the tiled scissor
  branch 821072F8 and 821075E0 (guest calls inside the flush), draws over 0xFFFF indices,
  resolves, events, UP and Begin/End draws.
- **S2a (2026-10-08, default ON): records carry only changed constants** - the dedup core inside
  the record path (PERFORMANCE 2.20). Record bytes -69%; in-roll CP -0.32 ms/frame, fps +4.4; d1
  (default config) 124.5 fps ingame.
- Next (orchestrator, 2026-10-08): the CP drain. (1) a census (SR_NATIVE_DRAIN_CENSUS), then
  (2) dirty-tracked reuse of prepare / viewport, then (3) pipelined fences (SR_NATIVE_EXEC: the
  drain on an executor thread, fences / memory writes / swaps as stream items, CHECK for fence order
  and data stability). Strand B supplies per-stream shm tickets for pre-resolved draws.
- **Drain census (dc1, 2026-10-08, SR_NATIVE_DRAIN_CENSUS):** 4,027 drained draws per ingame frame; 82%
  carry the previous draw's prepare / viewport inputs (prepare 0.46 + viewport 0.29 ms/frame: the most a
  reuse saves is ~0.59 ms). Effects per frame: EVENT_WRITE_SHD ~20, INTERRUPT 2, XE_SWAP 1, MEM_WRITE /
  REG_TO_MEM 0; WAIT_REG_MEM ~50, all satisfied at parse. Draws between two effects: 57% none, 23% 128+.
- **SR_NATIVE_EXEC (pipelined fences, default OFF, in progress):** the drain on its own thread; stores,
  scratch write-backs and the swap are stream items it performs in order; INTERRUPT and a memory poll
  of a word a queued store targets wait for it. CHECK (fence order + data hashed at push vs execute,
  command-buffer pages included): e1 648K effects / 11.45M draws 0 bad; e2 under forced lag (5 us per
  item, 395 items between push and execute) 257K / 3.76M 0 bad. Sheets correct every era.
- **REJECTED (2026-10-08): "single-tile native tiling".** The 1,523 bin packets per frame suggested
  predicated screen tiling; the tile census (SR_NATIVE_TILE_CENSUS, roll e3) shows none. 11-13 IBs per
  gameplay frame run exactly twice, and between the two executions only bin select differs (window
  offset, window and screen scissors identical in every replay). They are the XDK Z-pass replays
  (BeginZPass 8210DF38 / EndZPass 8210E1A0: depth-only first, then colour). The bin packets are
  GpuSetTilePredicationMask 8210D400 (SET_BIN_MASK_LO per call; RwDrawInstancedConstBatched's per-tile
  loop runs with one tile, mask 3 = both bins). Dropping the Z prepass instead is not exact (the colour
  replay is depth-tested against the prepass depth; ~1,700 of the predicated draws per frame blend), so
  it is not pursued. dev byte 10808 bit 0x02 is tile auto-predication (8210D400; the XDK flush then
  records the draw for a per-tile replay), never set here - not the Z-pass (bit 0x40).
- **Planned with the pre-resolved draws: Z-pass replay reuse.** A replay's bytes are the first
  execution's apart from bin select, so the executor can reuse the first execution's translated draws
  (keyed by IB and offset, bin select applied) instead of parsing and staging them again: 600-860
  replay draws per frame, CP parse 0.5-0.8 ms + drain ~1 ms - sized at ~1.5-2 ms CP / executor per frame.
- The CP is now the pipeline's limiter: 9.26 ms/frame at ~106 fps (fp3), of which the draw drain
  is 5.73 (vertex staging 2.74, of it shared-memory requests 2.02), the record walk 1.67, emitDraw
  0.87 and the present 0.92. Guest-side savings show in fps only once the CP drops.

### Strand A main track: the native DRAW BUILDER (design study 2026-10-07)
Owner 2026-10-07: "a native PC port, not some glorified emulator". The draw builder replaces the
game's per-draw pipeline BELOW the render walk with host code: RenderWare's compiled-object draw,
its state appliers and the XDK draw / setter encode. The render walk and every game decision stay
guest. Sources: IDA (this section's addresses), profile pr_off (exe 06714faf3e76, ingame window,
112 fps, guest render thread `guest:slot17`, `tools/perf/thread_ms.py`), 1a/1b measurements above.

**1. The boundary [static, IDA].** The cut is the three RW compiled-render-object draw calls. They
are reached only by direct calls; the data xrefs at 0x820D7638..48 are .pdata, not a vtable:
- `RwRenderObjectDraw_sub_8261D0A0` (static objects; callers BuildingRenderObj_Render 822FFE80,
  RenderNodeCulledSubmeshes 82307CF8, TagAndTransformModelMeshNodes 823087F0,
  ComputeFacingOrientAndSubmitGeom 8230EC00, sub_822F7168, and one site at 0x822ED3EC outside any
  IDA function, still to be attributed);
- `RwRenderObjectDrawExtXform_sub_8261CD90` (caller-supplied world transform; RenderSkinnedMesh
  82300328, RenderClothMesh 82306BB0, RenderChildPerspectiveTransform 8230E028,
  RenderMeshGeometryPass(+WithShaderConsts) 8230F8E0 / 823103C8, MeshInstanceBatchFlush 82786E30);
- `RwRenderObjectDrawInstanced_sub_8261CED0` (ModelEmitMeshDrawsPerCmdBuffer 822F5290,
  sub_822F5088, RenderMeshGeometryPass(+WithShaderConsts), MeshInstanceBatchFlush).

What a call reads:
- the render object: +0 flags (4 / 8 / 0x10 select the per-state loop), +4 state count, +8 stream
  end; compiled states from (obj+131)&~15. Each state has a 32-byte header (+0 flags: 0x100
  program, 0x200 params, 0x400 world transform, 0x400000 render states, 0x800000 sampler states,
  0x100000 vertex decl, 0x200000 draw record; +4 / +16 restore masks; +12 texture mask; +20 prim
  type) and a draw record (+2 stream count, +4 vertex count, +8 start index, +12 index count,
  +16 IB, +20.. VBs with the stride at vb+32);
- RW globals:
  - the prim type 829989FC;
  - the dirty mask 829989CC and the restore sets 82998914 / 829989D0;
  - the stream cache 8299AC00 / 8299ABF8, the IB cache 8299ABF4 and the decl cache 8299ABF0;
  - the current program 82998884 and its flush callback 8299888C (the effect's constant upload,
    ShaderPs_BindAndUploadConstants 8260B898);
  - the parameter table 82998B00[id] and its dirty bits 82998A00;
- the XDK device `dword_8299ADF4`: state set outside the boundary by the render script, the
  technique passes and the bin walker (targets, viewport, blend / depth, Z-pass mode).

The calls below it are the C2 appliers:
- RwCompiledStateApply 82614758, ApplyNoXform 826148B0 and ApplyWithCallerDecl 82614A08;
- the state section 82613E10, the textures 826146B0, the render / sampler state lists
  82608FC0 / 826090E8, RestoreUntouchedToDefaults 826169E0, and the restore-mask merge 8260A8F0;
- the per-state loops 82617340 / 82617098 / 826173D0 / 8261CC90 / 826174B8 / 82617460.

Then the C3 flush-and-draw functions (MeshBatchFlushStateBindStreamsAndDraw 82612A38,
82612ED8 / 82612C10 / 82612E18 / 82613108, RwDrawInstancedConstBatched 8261B050, which writes
c100+ and the device shadow directly) and the XDK entries (SetStreamSource 820F2A50, SetIndices
820F2C08, SetVertexDeclaration 820F5438, the draw flushes 820FC000 / 820FBBF8).

**S1 scope [static, the recompiled call graph below the three cut calls, stopping at the XDK].**
- 58 RW functions, about 5,400 recompiled statements.
- 12 of them dispatch through tables:
  - the render- / sampler-state handler lists (ApplyRenderStates 82608FC0 / ApplySamplerStates
    826090E8, their untracked and restore-stale variants);
  - the program flush callback 8299888C inside MeshBatchFlushStateBindStreamsAndDraw 82612A38 and
    MeshBatchFlushStateAndRedraw 82613108;
  - the shader constant uploaders ShaderVs/Ps_Upload{Dirty,All}Constants 8260B050 / 8260B2A0 /
    8260B440 / 8260B660.
- 19 XDK entries are reached:
  - SetStreamSource, SetIndices and SetVertexDeclaration;
  - the six Set*ShaderConstant{F,B,I};
  - two GpuBindShaderApplyStateBlock and SetTextureFetchConstant 82100310;
  - the two draw flushes;
  - predication: GpuPredicationTilePush / Pop 82102010 / 82102228, BeginRegion / EndRegion
    82102448 / 82102608 and SetPredication 8210D400.
So the constant upload (the guestcode lane's F-loop) and the predicated-tiling redraw are both
inside the cut, and S1 must route the constant upload through guestcode's native leaf, not
around it.

**The instanced path, where the time is [static, IDA 8261B050].** RwDrawInstancedConstBatched is
constant-batched instancing:
- it binds the VS / PS constants (8260B800 / 8260B898, 0.30 + 0.37 ms);
- it sets stream 0 to the mesh and stream 1 to an instance-index stream;
- it writes fetch-constant sampler words and VS c4 straight into the device shadow
  (dev+1036.. / +1856.., dirty bits);
- then, per tile when tiled (SetPredication 3<<2t, TilePush/Pop or Begin/EndRegion), it loops over
  batches of at most 128/stride instances: SetVertexShaderConstantF(c100, the instance block)
  followed by DrawVertices(prim 4, instances x vertices-per-instance).
A native builder can collapse that into one host draw per (mesh, tile) with the instance data in a
buffer. The c100 window is a Xenos constant-file limit, not game logic. The catch is that the
translated VS reads c100 + i*stride, so the shader translation has to read a buffer instead. That
belongs to S3 or later; S1 and S2 keep the batches.

**2. Game logic vs renderer plumbing.** These stay guest:
- Renderer_DrawFrame 82776038, the render script (RenderScriptRunTechnique 8277B998), the bin
  walkers (RenderBin_RenderBinSimple 82783948 / RenderBinBatched 827828A0), culling / LOD / bins
  (RenderBin_UpdateAll 82783B48: 2.13 ms);
- the object render methods above the cut (they choose LOD, submeshes, transforms and material
  overrides: ModelApplyMaterialParamOverrides 822F2A38 / RwCompiledStateSetParam 826156D8,
  RenderModel_SwapMeshChunkBindings 822EBC98, the skinned palette);
- every producer of parameter values (camera, lights, fog) and the shader generator on a miss
  (ShaderGen_VsCacheFindOrCreate 82619BE0: it creates guest objects and an LRU other code reads).

Plumbing, which the builder replaces, is everything from the three calls down:
- the C2 / C3 functions;
- the RW state caches;
- the XDK device encode and the ring.

Not under the cut (about 10% of the C1 time, so they stay on the XDK / 1a path until later):
- particles (DrawParticleLayer 822FC7C0, 0.50 ms) and the Begin/End immediate draws (UI, Lion);
- cloth (ClothUploadAndDraw 822F3FB0, a D3DX effect), fullscreen quads 82786C68, decals 822F65B0,
  FMV and DebugDraw.

**3. Cost today, pr_off ingame, render thread 7.27 ms busy/frame [measured].** Under the cut:
DrawInstanced 1.91 + DrawExtXform 0.45 + Draw 0.18 + MeshInstanceBatchFlush 0.22 = **~2.75 ms
inclusive (38%)**. Inside that, the XDK draw entries (DrawVertices 0.49 + DrawIndexed 0.31) take
0.79 ms and RW plumbing plus setters take ~1.9 ms. The rest of the thread is game logic: the walk
and techniques ~2.4 ms outside the cut, BeginFrame / RenderBin_UpdateAll 2.13 ms (cloth 1.0,
bounds 0.6, LOD 0.5). The CP spends ~7.3 ms CPU per frame (ab_off2), of which the PM4 parse +
register writes are 0.75 ms and the draw drain 4.6 ms (PERFORMANCE 2.13).

**4. The oracle, and what the per-draw P==N hash does not cover.** The hash (register file +
shader pair + draw arguments + UP bytes, by draw ordinal) covers every draw's GPU-visible input,
and it is the acceptance test for every stage. It does NOT cover:
- (a) the guest-visible state the plumbing leaves behind: the RW caches and the XDK device shadow
  that later guest code reads (the non-cut draws, the next frame, the shader generator);
- (b) segment kicks and fence timing, which change when the ring length changes;
- (c) the replay fixtures. The builder replaces guest code ABOVE the PM4 layer, so `.xtr` replay
  cannot exercise it: the oracle is live and in-process.

So each stage adds a check mode that runs both paths on the same call and compares them. This is
the SR_NATIVE_DRAW_CHECK model from 1b:
- snapshot the RW globals, the device and the pinned pages;
- run the native path and keep its outputs;
- restore the snapshot and run the guest body;
- compare the per-draw hashes, the RW globals, the device dwords and the written pages.

The check also gets a mutation test, the 9/9 replay (the non-cut paths must stay byte-identical)
and whole-run sheets.

**5. Stages, each provable on its own (estimates; measured per stage with the ingame window,
interleaved, tick + thread_cpu sum).**

| Stage | What | Proof | Guest tick | CP |
|---|---|---|---|---|
| S0 | census, observe-only (the kill experiment below) | - | 0 | 0 |
| S1 | native C2 / C3 plumbing below the cut, still calling the guest XDK entries and the effect flush callback | check mode: RW globals + device + PM4 bytes of the call equal; per-draw hash | -0.8 to -1.4 ms (~1.9 ms of recompiled plumbing, native at 2-3x) | 0 |
| S2 | cut draws emit records instead of the XDK encode (the 1a NOP, padded to the XDK length so kicks and fences are unchanged); state set outside the cut is read from the device shadow; the shadow is marked all-dirty when control returns to guest XDK code (the BeginZPass model) | per-draw hash incl. Z-pass replays; the following guest draws' hashes (shadow handoff); ledger pages | -0.6 to -0.9 ms (XDK draws 0.79 + setters) | parse -0.6 ms, plus the record execute at <= 0.20 us/draw (the 1b.2 budget) |
| S3 | a cached native translation per render object (PSO key, bindings, draw arguments), keyed by the object's compiled bytes plus its mutable fields; constants uploaded on change through the dedup core (guestcode's SR_NATIVE_CONST_SKIP folds in here) | check mode on cache hits (rebuild-and-compare every Nth hit); per-draw hash | -0.2 to -0.4 ms | drain -1.5 to -2.5 ms (vertex-fetch staging 1.8, PSO / PrepareDraw 0.45 and the batch build pre-resolved per object) |
| S4 | the mode-B executor (pipelined fences, above) consumes the records; the padding and the ring go | fence values and order vs the PM4 path; per-draw hash | ~0 | parse and execute overlap on two threads |

The builder replaces 1b.2 for the draws under the cut (~85-90% of the C1 time). 1b.2's native
flush is then needed only for the non-cut paths, after S3. Stages S1-S3 together are estimated at
-1.6 to -2.7 ms of the ~8.9 ms ingame tick, which still leaves the 4.17 ms target to the game-logic
side (the walk, techniques, cloth, LOD).

**6. Top 3 risks, and the cheapest experiment that kills the idea early.**
1. **Hidden coupling.** Plumbing state may be read by guest code outside the cut: the device
   shadow, the RW caches, the VS LRU, RwDrawInstancedConstBatched's direct shadow writes, and Lion
   per-tile c100. A missed reader gives wrong state on mixed streams.
2. **Gain smaller than estimated.** The guestcode lane found the leaves below the light mask
   memory-bound (round 5 rejected). Recompiled pointer-chasing may run close to native already, and
   S1 could gain well under 1 ms.
3. **Z-pass and predicated tiling (30% of draws).** The XDK records and replays Z-pass segments.
   If the builder cannot reproduce that structure, those draws stay on the XDK and the gain is
   capped at about 70%.

**Kill experiment S0 (one roll, observe-only, SR_NATIVE_BUILDER_CENSUS).** A thread-local depth
inside the three cut calls, plus the 1b write-ledger machinery restricted to their dynamic extent,
reports four numbers:
- (a) the share of ingame XDK draws inside the cut, by class (plain / Z-pass / UP / immediate-VS);
- (b) the non-device, non-ring pages written inside the extent, and whether anything outside the
  RW cache list above reads them (watchpoints from SR_NATIVE_DRAW_LEDGER_PAGES);
- (c) the share of render objects whose compiled bytes are unchanged frame to frame (S3
  cacheability);
- (d) the rdtsc time inside the extent, split into plumbing and the XDK draw entries.

The idea is dead (fall back to 1b.2 per-draw) if:
- (a) is under 70% of draws; or
- (b) shows writes to game state that a guest reader outside the cut depends on in a way the
  builder cannot reproduce; or
- (d) puts the plumbing under 1.0 ms/frame.

- (e), added by the orchestrator: S1 must gain from ELIMINATED work, not faster code, because
  guestcode's round 5 measured only 6-16% per call for transcribed memory-bound RW plumbing. The
  census measures redundancy ms-weighted per sampled render object (1/4 by address), as
  [NATIVE_BUILDER_REDUNDANCY]:
  - the PM4 bytes the call emitted vs the same object's same occurrence in its previous frame.
    This share is a lower bound: per-frame camera constants and the XDK's dirty-state emission
    change outputs even when the object's own work repeats;
  - the object's compiled bytes unchanged, as the upper side;
  - eliminable_est = cut ms x the output-same share.
  - cacheable_est = cut ms x the share of calls whose output is the same once the ALU float
    constant payloads (type-0 0x4000..0x47FF) are left out. This is what a per-object cache
    removes while still re-uploading the constants that changed.
  The rule has two levels:
  - eliminable_est >= ~0.8 ms/frame: S1 as planned, a transcription that skips exact repeats.
  - eliminable_est under ~0.8 but cacheable_est at or above it: S1 is redesigned around caching.
    S3 moves earlier: the per-object translation is reused and only changed constants are
    re-uploaded.
  - both under ~0.8: the builder has no guest-tick case. What remains is S2's CP-side gain, judged
    on its own.

(c) only decides whether S3 is worth building. Next after S0: S1 on the path the census shows the
time is on. pr_off puts 1.91 of the ~2.75 ms in DrawInstanced -> RwDrawInstancedConstBatched
8261B050, and RwRenderObjectDraw has only 0.18 ms. So the static path (RwRenderObjectDraw +
82614758 + 82612A38) is the smallest closed set for proving correctness, and the instanced path
is where the gain is.

**S0 RESULTS (2026-10-07, rolls bc1b..bc5, exe e4826c46b0f0 / afb4e6b4e1dc / eddeac56a825,
gameplay/port, uncapped, ingame window; tools/perf/builder_census.py) [measured].** Framing (orchestrator,
after the owner's decision): S0 no longer decides WHETHER the native draw path is built (0 PM4
packets requires it). It decides how, in what order, and with what budget.

(a) **Coverage: the cut is FIVE entries.**
- With three entries only 53% of XDK draws were inside (bc1b).
- The two missing entries call the C3 flush-and-draw directly, found by IDA xrefs of 820FC000:
  - RwRenderObjectDrawNullPsThenPs 82617798 (RenderMeshGeometryPass);
  - RenderCtxDrawMeshForPass 822F8710 (ModelDrawInstancesMasked, BuildingRenderObj_Render,
    RenderMeshGeometryPass).
- With five entries (bc1c / bc4), per frame: 2,414-2,424 draws inside, 247 outside, 90.7% covered.
  - Outside: UP ~80/frame, Begin/End ~400 entry calls (~200 draws), and cloth.
  - Z-pass draws seen inside: indexed ~6%, auto ~60%.
- Cut time: 3.8-3.9 ms/frame, census overhead included (tick 8.8 vs ~8.55 off). XDK draw
  entries take 0.98-1.13 of it and plumbing 2.79-2.84.
- Outermost per entry, one window: instanced 2.46, extxform 0.67, ctxpass 0.46, draw 0.41,
  nullps 0 (not called in this gameplay).

(b) **Redundancy.**
- Object compiled bytes: 96-98% the same as the previous frame (ms-weighted).
- Emitted PM4: same 12% (19% without ALU constants). That figure is order-confounded: the XDK
  emits deltas against the previous draw.
- The order-independent device state after the call ([0x400, 0x2A00), bc4): only 0.2% of calls
  match. Per-dword volatility names the reason:
  - outside the ALU constants, exactly ONE dword changes every frame: +0x6E0, 99.2% of calls. It
    is a vertex fetch address in fetch slot 30 (vertex fetch constant 91, dword 0), a dynamic
    stream address.
  - Every other non-constant dword changes in at most 2.1% of calls (+0x6F0); most are at or
    under 0.4%.
  - Of the ALU constants, 1,240 dwords ever change; the guestcode SR_PARAM_OBS measured 99.72% of
    parameter values unchanged at re-application.
- So about 90%+ of cut calls re-produce the same full state except for changed constants and
  one stream address.
- The S1 design this implies is CACHING-FIRST, not transcription:
  - a translated record per (render object, occurrence);
  - per frame, re-apply only the changed constants (the dedup core) and the dynamic fetch words;
  - validate inputs cheaply: generation counters or the object's parameter values, not a re-hash
    of its 6.6 KB of emitted bytes.
- Transcription alone is capped by guestcode's 6-16% per call.

(c) **Coupled pages (bc3 / bc5, byte-exact, cut extent only, ~320K sampled calls).** Value
changes inside the cut are confined to RW plumbing state:
- 82998000: 140 dwords. Among them:
  - the current world transform 82998874 (48.9%);
  - the parameter table 82998B04 / B08 (38.5%);
  - the render-state shadow 82998C18..CF4 (8-40%);
  - the caches 829989CC / F4 / F8 / FC.
- 8299A000: 61 dwords. The stream cache 8299AC00.. (84%), the IB cache 8299ABF4 (52%), the decl
  cache 8299ABF0, and 8299AB00..AB14 / AFD0..AFF8.
- 82999000: 127 dwords, at most 2.8%.
- 8299B000..B00C: at most 5.5%.
- The restore masks 8297D52C.. (at most 6.7%) and 82985CD0..D0C (at most 3.6%).
- 82913000 / 82972000 / 829A0000 / 8293B000 / 82911000 / 829A1000 had write-watch hits but no
  value changes at or above 0.2%: same-value writes or other threads.

Readers outside the cut (IDA):
- the stream / IB caches: Render_CommitStateForImmediateDraw 8261AEF8 (the NON-cut immediate
  path), RwStateCacheResetAndUnbindDevice 82612460, RwDeviceUnbindVertexBufferAllStreams
  82613240, sub_8261AD78;
- the restore masks: RenderState_Restore{Untouched,AllTracked}ToDefaults 826169E0 / 82616B28 and
  sub_82620C80;
- the current transform: 33 sites, including RW code at 8260C920..8261DF88 / 826351B8 / 82635968
  and the game render methods above the cut.

The builder must therefore leave these RW globals as the guest would at every cut exit. The caches
can instead be INVALIDATED, because their consumers rebind on a mismatch (82612A38 compares
{vb, flag} per stream). The transform, the restore masks and the parameter table carry meaning and
must be exact. The S1 check compares them at cut exit.

(d) **The first stage, with its budget: S1 = the lean native draw record at the XDK flush
(1b.2), for every draw.** The cut cache (S2) needs a record format to cache, so the record comes
first.
- Scope: DrawIndexed 820FC000 / DrawVertices 820FBBF8. The flush becomes host code that writes a
  compact record (the dirty state groups + the draw) instead of PM4. Constants go through the
  dedup core.
- What stays PM4 for now:
  - the shader bind (82108668) and the tiled-branch calls, passed through as in 1a;
  - the 'NDRW' marker, one packet per draw, until the executor replaces the ring.
- Budget per draw, measured with SR_NATIVE_DRAW_TIMING (rdtsc) on the same roll as the XDK body:
  guest record build <= 0.25 us (the XDK encode is 0.23-0.28) and CP record execute <= 0.20 us
  (the PM4 parse + register writes). Over budget is not merged.
- Expected: the meter falls from ~60K to the non-draw remainder (shader loads, resolves, events,
  ~15% est.); guest tick ~0 to -0.3 ms; CP -0.6 ms (parse).
- Proof:
  - SR_NATIVE_DRAW_CHECK: record vs PM4 on the same draw, every device dword and written page
    equal;
  - the per-draw P==N hash;
  - replay 9/9 gate-off and whole-run sheets.
- Then S2, the per-object record cache at the five-entry cut. Its guest gain is the one that
  counts: up to the ~2.8 ms plumbing x the hit rate, minus validation. Its check compares the RW
  globals at cut exit.

### Strand B - native render targets, resolves, EDRAM elimination (second agent)
Goal: draws render straight into D3D12 render targets; a resolve is a GPU copy or an alias into the
texture that samples it; no EDRAM tile loads / stores / ownership transfers on the common path.
Measured at 3x (res4k round 2): resolve 1.9 + texture loads 2.1 + transfers 1.16 ms vs 3.0 ms of
real draws.

| Stage | What | Oracle |
|---|---|---|
| B0 | inventory: resolve classes (colour / depth / MSAA / slice / cube / clear-on-resolve), which textures sample resolved memory, whether the CPU ever reads it (load tap / write watch) | counts per class on the 9 fixtures + a live roll |
| B1 | render targets as persistent D3D12 resources keyed by the game's surface (base, pitch, format, MSAA); EDRAM moves only where two surfaces really alias | replay 9/9 byte-identical; per-resolve output hash P vs N (new: hash of each resolve's destination after it) |
| B2 | resolves as GPU copies / format-converting dispatches into the destination texture resource, bound directly when a fetch constant points at it (no guest-memory write, no re-upload) | per-resolve hash (texture readback at 1x); final frame byte-identical |
| B3 | guest-memory write-back only for resolved ranges the CPU reads (B0), lazily | the guest-visible ledger; whole-run frames |
| B4 | present from the resolved texture (N4a native present exists); clears as native clears | frame compare; Xenia X |
| B5 | resolution scaling on native targets (no scaled EDRAM memory) | 1x byte-identical; 3x vs the current 3x path |
| B-CPU | the render thread's CPU side of B's work (added 2026-10-07 at the coordinator's request): texture binding per batch, resolve setup, transfer setup | `SR_PM4_DIAG_TIMING=1` [PM4_FRAME] bind / resolve / xfer ms per frame (replay, interleaved A/B) + strand A's render-thread profile |

**B-CPU (render thread, at 240 fps every thread needs < ~4.17 ms).** Strand A's profile (pr_off,
in-game, ms/frame): the SR_PM4 render thread 8.19, of which `xlatBindBatchTextures` 2.77, resolves
1.40, ntdll waits 2.84. What B removes and what it does not:
- Binding (2.77 ms, ~3,000 batches x up to 16 fetch slots): per slot `getOrCreateTexture2D` hashes
  the 24-byte fetch constant (XXH3) and looks the key up in the clean set `m_texClean` (iter-460: a
  title-city frame binds ~25,000 2D textures and this lookup is their whole cost); only a miss runs
  the page-version scan (`pm4ShmVersion`, a mutex + one stamp per 4 KB page) and, when stale, a load.
  Arrays already short-cut on `m_shmStampClock`. B2 removes the LOADS of resolved textures (39 -> 17
  per lg1 frame with b2f; the f24 shadow layers and 4 materializations go with the secondary
  targets) and each load's CPU (version scans, upload recording, barriers); it does not touch the
  per-slot hash + lookup, which runs for every slot of every batch. Next: a bind memo - a batch
  whose texFetch equals the previous batch's (`texSame`, already flagged by SR_CP_TEX_SKIPSAME for
  the prepass) with no resolve, materialize, clean-set erase or shared-memory upload recorded
  between them reuses the previous batch's 32 slot values (sharedTex + sharedSmp) instead of 16
  hashes + lookups.
- Resolves (1.40 ms CPU for ~26 resolves = ~54 us each): B2 replaces the scaled write + the later
  reload, not the setup (buildResolve, ownership sync of the source, page marking, texture
  invalidation over the destination range). Next: measure the split per resolve (b3 batch:
  `i_cpu_*` jobs) and cache the per-(key, rect, dest) setup across frames - the 26 resolves repeat
  every frame with the same parameters (B0 table).
- Transfers: B1a removes the elided moves' CPU (store / load / predicate recording per move); the
  claim bookkeeping (ownership ranges) stays.
The `ntdll` waits are fences / queue waits on the GPU or the CP, not B's CPU work; B's GPU wins
shorten the fence waits that wait on GPU completion.

**B-CPU built (2026-10-07, gated OFF, from the pr_off profile's breakdown of the render thread):**

| Item (pr_off, ms / frame) | Gate | What it does |
|---|---|---|
| binding: getOrCreateTexture2D 1.03 incl + xlatBindBatchTextures self 0.43 + XXH3 0.23 | `SR_NB_BIND_MEMO` | per slot, the last fetch constant and its answer; reused while the clean set / scaled marks (`m_texBindGen`) are unchanged (plus the stamp clock and epoch for answers that carry an array lookup); only fast-path answers are kept |
| arrays: arrayMipLayout + computeGuestLayout 0.58 (inside pm4ShmLoadArray 0.83) | `SR_NB_BIND_MEMO` | the stacked / cube layout kept per fetch (pure in the fetch and the layer count) |
| resolves: rebaseHashPages 0.81 of pm4ExecResolve 1.40 | `SR_NB_REBASE_PROTECT` | the resolve's rebaseline protects its destination pages (write tracking) before hashing, so the next resolve keeps the hashes instead of re-hashing ~30 MB of guest memory |
| claims: pm4Claim 0.92 (owners / vector growth / operator new / map split-merge) | `SR_NB_CLAIM_FAST` | a claim the claimer already owns whole returns at once; the full path's vectors are reused (no gate) |

Replay frames are identical with all three on (b8 r_on3x / r_on1x); the replay's own profile cannot weigh them
(trace file re-reads and pipeline compiles dominate its main thread), so the CPU number comes from the live
render thread (`perf/threads.csv`, SR_PM4_render ms per guest frame).

Expected (estimates): GPU at 3x -4 to -5 ms (most of resolve + loads + transfers); at 1x -0.5 to
-1 ms; CP -1 to -2 ms (tile moves, shared-memory validation and texture re-validation of resolved
memory); guest tick 0.

Files B owns: `gfx/d3d12_pm4_edram.cpp`, `gfx/d3d12_pm4_shmem.cpp` (resolve / texture paths),
`gfx/native_resolve.inc`, `gfx/edram*`, the RT / resolve / texture members of `gfx/d3d12_backend.*`,
`gfx/d3d12_present_chain.cpp`, `gfx/res_scale.*`, and in `gfx/pm4_engine.cpp` the resolve /
EVENT_WRITE / EDRAM handlers. Strand B's own code: `gfx/native_targets.inc` (included by
`d3d12_pm4_shmem.cpp`), tools `tools/nb_census.py`.

#### Strand B status (2026-10-08 07:45, branch `native/targets`)

| Stage | State | Evidence |
|---|---|---|
| B0 | done | inventory table below (26 resolves / 39 loads / 17 transfer sets per gameplay frame; no CPU reader) |
| B1 | B1a, B1c `SR_NB_CLEAR_PROP` and B1b `SR_NB_COVER_FILL` baked ON (B1c / B1b after check roll cr4b_nb, 2026-10-08): transfers 0.78 -> 0.13 ms per 3x frame | `SR_NB_CLEAR_ELIDE`: replay 9/9 identical 1x / 3x frames 1-3 + rhash 78/78; live 3x -0.3 ms; live 1x transfer pass 1.09 / 0.74 -> 0.67 / 0.65 ms (meter edram_xfer 24.5 -> 16.3 per frame) |
| B2 | baked ON (dev 0ac96585) | `SR_NB_RESOLVE_TEX` + `SR_NB_DEPTH_SRV` + `SR_NB_ARRAY_LAYERS` (+ secondary targets): replay 9/9 at 2x / 3x, rhash 78/78 x 6; whole run every era at 3x; live fight 8.55 -> 6.9 ms GPU at 3x (PERFORMANCE.md 2.15); combined check roll cr1b (3x) PASS |
| B2+ | baked ON (iter-467 merge after check roll cr2: 3x, every lane's entries on, whole run, every era correct) | `SR_NB_OVERHANG` (rows: 320x184 into 320x180, 160x96 into 160x90; columns: the 8x8 luminance steps into 4x4 / 1x1) and `SR_NB_DEPTH_TARGET` (k_24_8_FLOAT into its R32 texture + an R8 stencil side texture) |
| B3 | satisfied by B2 on every fixture and the live rolls | held memory is written back only for another reader (materialize): 0 per steady-state frame |
| B4 | front target baked ON (after cr2, as B2+); native clears: B1c below | `SR_NB_FRONT`: the front-buffer resolve writes a front target (raw dwords, R32, at the scale); the scan-out reads it (`PSPresentTex`) |
| B5 | B2 at 1x: `SR_NB_TARGETS_1X` built, replay-exact, GPU-neutral (texture loads 31 -> 16 per 1x frame, but the dual-write resolves cost what the loads saved: the typed texture stores are slow) - not for baking yet. `SR_NB_SCALED_REMAP` (the 2x / 3x menu-era 5 EDRAM resolves a frame -> 0) baked ON after check roll cr4b_nb (witness `[NB_REMAP] scaled remap`). The 1x native resolve (`SR_NATIVE_RESOLVE`): -13.7 fps uncapped in job15 (rebaseline under m_shmMu; dev 99d90b92 back to OFF), -2.8 +-2.3 after the B-CPU 2 fixes, -0.8 +-2.6 measured alone on the baked exe (job17): baked ON again | b10: frames 3x 9/9 + 2x 6/6, rhash 78/78 x 6, VERIFY 0 mismatch on the four FMV-era traces, meter edram 5 -> 0 (eahd / t16) |
| B-CPU | baked ON (after cr2: entry 119 PASS, 0 stale rebaseline keeps) | `SR_NB_BIND_MEMO` (+ the array layout memo), `SR_NB_REBASE_PROTECT`, `SR_NB_CLAIM_FAST` (B-CPU above): live 2x Metallo fight, render thread 8.30 / 8.66 -> 6.01 / 5.99 ms per frame (l10_on2x* -> l10_bcpu2x*, PERFORMANCE.md 2.15) |
| B-CPU 2 | baked ON on the branch (job16b) | `SR_NB_NATIVE_PREHASH` (rebaseline hashed before m_shmMu, path N and the native targets) + `SR_NB_REBASE_FRESH_FIRST` (kept pages found serially, the pool only for pages that hash): uncapped in-roll A/B +14.1 / +7.4 fps at 3x, +12.6 / +4.4 at 1x; replay 9/9 at 1x and 3x, 0 stale under SR_SHM_WT_CHECK (PERFORMANCE.md 2.15) |
| B-CPU 3 (CP) | `SR_NB_SHM_RANGE_CACHE` baked ON on the branch (check roll cr5_nb) | the CP's validated-range cache: GPU_CP -0.30 to -0.37 ms/frame (in-roll A/B x3), live CHECK 27.3M hits 0 mismatch, mutation caught; `pm4ShmEnsure` + `ShmRangeTicket` = the interface for strand A's pre-resolved draws (PERFORMANCE.md 2.15) |

Replay proof of B2+ / B4 together (b8, exe snapshot db56d181fbc5-fr6, `archive/v1_nativeb/b0`): all three
gates vs dev, frames 1-3 of a 3-loop replay byte-identical at 3x (9/9 traces) and 2x (6/6); per-resolve
hashes 78/78 identical on 6 traces (`SR_NB_RHASH`, vs the gates-off baseline); no failure line; the front
target witness (`[NB_FRONT] scan-out fb=... from its front target`, frames 2 and 3), the 320x184 row overhang and
the depth target witnessed. Census (lg1, frames 3-5): every one of the 26 resolves takes path T (into its
texture), 0 texture loads of resolve output (9 before), 0 materializations. The gated-off build is inert:
default frames byte-identical to dev at 1x and 3x. B-CPU gates on (`SR_NB_BIND_MEMO`, `SR_NB_REBASE_PROTECT`,
`SR_NB_CLAIM_FAST`): frames identical at 3x (9/9) and 1x (4/4), rebaseline keeps witnessed
(`[SHM_WT] ... rebaseline kept N`), and with `SR_SHM_WT_CHECK=1` (every kept hash recomputed) 0 stale.

What still goes through the EDRAM model per steady-state gameplay frame at 3x with all gates (replay lg1,
`[NB_METER]`): resolves into guest-layout memory 0 (11 with the baked four only), resolved-texture loads 0
(9), materializations 0; ownership transfers 16 sets (4,448 tiles moved, 3,808 elided). The transfers are the
game's own surface reinterpretations ("the remaining aliases", below): host RT -> host RT copies decided by
the tile-ownership map. At 1x (live meter) every resolve still goes through the EDRAM buffer (edram_res 26 per
frame): the 1x native resolve (`SR_NATIVE_RESOLVE`, N4a) was never baked - proven again 2026-10-07 (replay 9/9 +
rhash 78/78 x 6; live 1x resolve pass 2.2 -> 1.3 ms, edram_res 26 -> 1.4) and baked ON (check roll cr3 at 1x:
native resolves vs the EDRAM path 27 PASS, B-CPU 81; merged with the B2+ / B4 / B-CPU bake).
Then (B2+ / B4 / B-CPU / 1x native resolve baked after cr2 / cr3): render-to-texture for the 1:1 resolves (the resolve
copy itself), B1b, B5 (B2 at 1x).

#### EDRAM zero: the plan (2026-10-08, the owner's native goal; coordinator order)

Goal: the native meter's EDRAM counters at 0 in the default build at 1x and 3x - no resolve through the EDRAM model,
no move decided by the tile-ownership map in the shipping path - with the EDRAM model kept as the oracle.

Where it stands (default build, dev d6dfa5f5). Live 1x gameplay windows (pf_chk, rc3_1a): edram_res 0.0, edram_xfer
14.0, edram_load 0.0 per frame; whole run edram_res 1.8-2.4 per frame, all of it the first ~25 s. Replay census (fr28,
7 loops, `archive/v1_nativeb/b0/z_cen*`, every one of the 9 traces at 1x and 3x shows the same frame structure):

| Class | Move(s) per frame | Tiles | Runs today as | What the game does | Native replacement |
|---|---|---|---|---|---|
| R1 | resolves before the kernels exist | 17-20K per run, 26 / frame for ~25 s | EDRAM resolve (warm-up: `warm=` in the [NATIVE_RESOLVE] lines of cr4b / cr5 / cr6) | - | shipped kernel bytecode (`SR_NB_AOT`, built; blobs + proof in flight) |
| T1 | C3m->C3, D1m->D1 (gp2: D1m->D1 x2) | 720 each | B1c host clear | clears the 4x-MSAA scene surfaces, then renders the 1x scene surfaces at the same base (sample-count change) | a native clear of C3 / D1 at their bind, from the clear's value, decided per surface |
| T2 | D0m->C0 x2 | 352 | direct copy (depth bits as colour) | reads the 4x depth as colour in the 640x360 MRT pass (blend on) | an explicit depth-to-colour reinterpreting copy between the two host surfaces (the XFER_DEPTH pass), decided per surface |
| T3 | D0m->C4m | 1280 | copy (gameplay) / B1b fill (FMV traces) | the shadow cascade reads depth bits as 16_16 colour (4x) | as T2 (the fill when its draw covers) |
| T4 | C3->C0 x2, C0->C0 x3, C3->C3, C3->C14, C14->C0, C6->C0, D0m->C6, C0->C6 (3x) | 716, 60, 48, 4, 4, 8, 176, 8 | B1b stencil-masked fill after the draw | full-screen post-process quads claiming a reinterpreted surface (format change; pitch change at one base) | the masked fill from the source surface for the pixels the quad leaves (none when it covers), decided per surface |
| T5 | -0->C6, -0->C0 (1x replays only; 2 per live run) | 8 each | EDRAM load | tiles no surface ever wrote | a clear to the EDRAM's initial content |

The meter counts every move the tile map decides, whatever executes it (clear, copy or fill), so the operations above
already run host RT -> host RT; what is left of the EDRAM model is the DECISION (the per-tile owner map, `pm4Claim`).

Steps (each: replay frames 9/9 at 1x and 3x + per-resolve hashes + a check roll; speed neutral or better):
1. R1: `SR_NB_AOT` - kernels from data/shader_cache/nb (generated by SR_NB_AOT_EMIT, keyed by source + defines);
   proof: a live roll whose meter windows show edram_res 0 from the first one.
2. A surface table beside the tile map: per host surface its EDRAM tile range, a write sequence and (T1) its uniform
   clear value; at a bind, the surfaces overlapping the bound one that were written after its own last sync give the
   moves (the overlap's tile range, as rangeRects computes it). Oracle mode `SR_NB_SURF_VERIFY=1`: both decide every
   draw and the move sets are compared (src, dst, tile range) - mismatches by class in the census.
3. The decision switched to the table class by class (T1, T4, T2 / T3, T5), each behind a gate with its replay /
   check-roll proof; the meter's edram_xfer counts only what the tile map still decides.
4. The tile map leaves the shipping path (kept for the VERIFY oracle and the replays' EDRAM-path runs).

Risks: partial overlaps (the strip clears of a 360-row surface, a surface whose range only partly overlaps another) -
the VERIFY mode measures them before any switch; the table has to reproduce the map's "who wrote last" per overlap
region, which the census shows as whole surfaces or row strips at one base in every frame so far.

**Progress (2026-10-08 evening, branch native/targets, fr31-fr36; every step through the flicker gate).**
- **Step 1 done: `SR_NB_AOT` + 21 shipped blobs** (`data/shader_cache/nb`, emitted by `SR_NB_AOT_EMIT=1` replays of lg1
  at 1x and 3x; the scaled set is one define, so it covers every scale > 1; make_player_build stages them). job33
  (fr31): replays 9/9 identical to the same exe compiling, at 1x and 3x, every trace loading from the blobs; replay
  time 25-45 s -> 4-7 s per trace (the DXC compile was most of it); live 1x whole run edram_res 17-20K -> **1**;
  flicker gate fg33 PASS. A kernel the blobs miss logs `[NB_AOT] compiled` (Warn). Regenerate after a kernel source
  change: any replay at 1x and at a scale with `SR_NB_AOT_EMIT=1`.
- **T5 done: `SR_NB_ZERO_INIT`** (baked ON). A tile no surface has claimed since the renderer started holds the
  power-on zero in a live run (the EDRAM buffer and the mirror start zeroed and only owners' tiles are ever stored
  into them); a load of such tiles is a host clear to zero (0 in every colour format, depth 0 / stencil 0). A 1x
  replay restores the trace's EDRAM snapshot, so the gate stands down there (`m_pm4EdramPristine`). job35 (fr33):
  frames 9/9 identical on vs off at 1x (inert, as designed) and 3x (2 loads per trace replaced), 78/78 resolve hashes x
  6 traces at 3x; live edram_load 2 -> **0** at 1x and 3x; flicker gate fg35 PASS.
- **The last EDRAM resolve: `SR_NB_RESOLVE_CLAIM`** (baked ON). `[NB_EDRAM_RESOLVE]` (Warn, first 16) named it: the
  very first resolve of every run (meter frame 0), 00C08000 (k_2_10_10_10_FLOAT 1280x720) over tiles two other colour
  surfaces own (03800800 = k_32_FLOAT, pitch 1 tile, and a second key) - a reshaped, two-colour-owner span the native
  kernel cannot remap. Such a span is now moved into the source surface first (the claim a draw binding it would make,
  the same exact transfers), then resolved natively. job37 (fr35): replays 9/9 identical on vs off (inert: no such
  resolve in the traces); title rolls with `SR_NB_RHASH=1`: the boot resolve's destination hash is identical
  (55BF2B0613E7F341, E path vs claim + N path) and the first 195 resolves of the two runs agree; live **edram_res 0 and
  edram_load 0 for the whole run at 1x and 3x** (zc1 / zc3); flicker gate fg37 PASS. (`SR_NB_ZERO_INIT`'s R0 branch -
  a span of unclaimed tiles plus the source's own, cleared in place - stays for that shape; it did not fire live.)
- **Baked and merged (dev 0f617e1d):** `SR_NB_ZERO_INIT` + `SR_NB_RESOLVE_CLAIM` default ON (=0 off), the blobs
  in the tree. On the bake exe fr36 (e0078c3b03df): check roll cr7_nb at 1x - 13 PASS, 0 FAIL (the three
  scale-only witnesses NO-EVIDENCE, as at every 1x roll), cr7b_nb at 3x - 16/16 PASS, including the new entries
  (nothing compiles at run time; the whole run's NATIVE_METER_RUN edram_res 0 and edram_load 0); flicker gate fg38
  PASS (span 0 of 358, freeze 12 counted, none bad).
- **Steps 2-3 done - the EDRAM model is out of the shipping path (dev 264c7fe8).** `gfx/nb_surfaces.hpp`: a
  surface table keeps, per surface key, the EDRAM tile runs it owns (disjoint, merged); a claim moves the other
  surfaces' runs in the claimed range to the claimer and returns them in the map's order (loads of nobody's runs
  first, then the owners' runs per extent). It is the same knowledge as the per-tile map, organised the way a native
  D3D12 renderer tracks aliased resources: which surface last wrote each range. `SR_NB_SURF_TABLE` (baked ON, =0 = the
  map decides again) makes it decide every claim, resolve span and trace-init owner list; the map is kept up to date
  only under `SR_NB_SURF_VERIFY=1`, where it checks every claim and owner query (the oracle). The moves it decides
  are counted as surface ops (`Pm4EdramMeter.surfaceOps`, [NB_METER]), not as edram_xfer.
  Proof: job39 (fr37, table beside the map) - every claim and owner query identical in 9 traces x 1x / 3x; job40
  (fr38, table deciding) - replays 9/9 identical to the map deciding at 1x and 3x, 0 mismatches with the map as
  oracle, 78/78 resolve hashes x 6 at 3x, live tl1 / tl3 / tlv whole runs **edram_res 0, edram_xfer 0, edram_load 0**
  (ledger correction, 2026-10-09: tlv ran `SR_NB_SURF_VERIFY=1`, but its player-profile log holds no `[NB_SURF]` line
  at all, so "0 mismatch lines" there is vacuous; the live verify witness is the check rolls cr8, `[NB_SURF] verify
  claim: claims=33,309,584 mismatches=0`), flicker gate fg40 PASS by the gate's rule (its span counted 2 outlier
  presents of 358, which `flicker_diff.py` alone prints as `FLICKER FAIL` - its own threshold is 2, the gate's
  span-max 5; freeze 12/12, 10 of them on the pause menu over the drawn world); job41 (bake exe fr39) - replays default vs =0 identical at 1x / 3x,
  speed 2 + 2 interleaved 1x rolls: +2.2 fps, render thread -0.13 ms/frame, GPU_CP +0.19 (all inside roll noise:
  neutral), flicker gate fg41 PASS, check rolls cr8 1x 14 PASS (3 scale-only NO-EVIDENCE) / 3x 17/17 PASS; job42
  (fr40 = the bake + dev 74dc5e16 with strand A's rows 1+2) - replays identical to fr39 at 1x / 3x, flicker gate
  fg42 PASS.
- **The record entry for strand A's row 9:** `pm4ExecResolveRegs(const edram::Regs&, bi)` - a resolve and its clears
  driven by the register snapshot alone (surface / colour / depth info, the copy and clear registers, the window
  offset and scissors, PA_SU_VTX_CNTL, the resolve rectangle); `pm4ExecResolve(batch)` forwards to it. Strand A's
  contract (above, "Record interface for strand B"): kinds 7 / 8 / 9 carry that snapshot. ⚠ SUPERSEDED for the
  payload (2026-10-09, settled with strand A): kinds 7 / 8 / 9 carry an OP LIST, not the snapshot (P1 row 9 below).

#### P1 row 9 - Resolve / ClearF / SetShaderGPRAllocation as native records (iter-467 native-b, 2026-10-09)

> **Where it lives (ledger, 2026-10-09):** the row-9 code is on branch `native/targets` only (bake 4f2b8df5,
> `SR_NB_REC` default ON there), NOT on dev. This section is native/targets' record (f28462d3) with the ledger's
> evidence corrections; at the row-9 merge this text wins over native/targets' copy. Status: `python
> tools/native_ledger.py --id p1.row9`.

- **What converts.** The census producers of row 9 (archive/v1_native/p1m_census_producers.txt: Resolve 782.6, 820F5570
  249.1, ClearF 174.0, Clear 27.2 = ~1,233 PM4 packets per ingame frame).
  - `D3DDeviceResolve 8210C5F8`.
  - `D3DDeviceClearF 82101A58`; Clear 82101998 writes nothing itself and re-enters ClearF.
  - `SetShaderGPRAllocation 820F5570` with its tail call `D3DDeviceClearFullSurface 82100488`, a GPR flush and not a
    clear (ENGINE_MAP 6g.8c).
- **The ports** (`kernel/native_rec_b.cpp`, `kernel/nb_rec_{resolve,clear,gpr}.inc`). Each XDK body is host code that
  computes the same ring bytes, device writes and guest writes from the device and the call's arguments. The ports
  follow the asm block by block and cover these callees:
  - 820FEFF8's mip layout and its five helpers;
  - the depth / colour clear values (8210BF48, 8210C440, and 8210C090 + 8210BEA0 as the spliced body runs them);
  - ClearImpl / ClearSurface / the rect split / the rect emitter;
  - the dirty flush the three entries share (82107520's runs, 821072F8's untiled HiZ write);
  - 8210C518's default block;
  - 820F0208's untiled scissor;
  - the allocator 820FCDF0 on the game's own command buffer (820FCBE8 mode 1 + 820FCB30's no-wait test).
- **What stops a port** (the guest body runs instead, counted per cause): tiling, a colour-as-depth format the
  spliced packer mishandles (6) or reads uninitialised (8/9/11/13), an allocator wait or refusal path, and a kick site
  that would kick (a segment boundary inside the entry; P2-B's business).
- **The payload: an op list per piece**, the run of XDK writes between two kick / allocator sites. It holds type-0
  runs, type-3 packets each predicated as the XDK wrote it, and NOP pads. `render()` writes its exact XDK bytes (spans
  shorter than the marker, the CPPM4 split). `apply()` runs it on the CP through the engine's native calls:
  - `nativeRun`;
  - `nativeDraw`, so the copy draw reaches `pm4ExecResolveRegs` exactly as on the PM4 path;
  - `applyImLoadImmediate`, `applyEventWrite`, `applyBinReg`, `applyWaitRegMem`;
  - INVALIDATE_STATE, consumed as execute() consumes it.
  The snapshot alone would not do: the CP register state these entries leave behind (the copy registers, the flushed
  dirty runs, SQ_GPR_MANAGEMENT, the fetch constant 0 of the copy / clear vertices) is read by later draws.
- **Record mode `SR_NB_REC`** (default OFF until proven). Per call there are two passes:
  - A pre-pass runs the port dry on a copy of the device, exactly as the CHECK runs it. Any stop declines the WHOLE
    entry before anything is written.
  - The live pass then runs on the real device through strand A's Entry v2: one opaque item per piece, `kick()` /
    `alloc()` at the XDK's own sites, and the cursor taken from `Entry::cursor()`. Under `SR_NATIVE_SEG_STRESS` the
    Entry may kick where the XDK would not; the live pass follows it.
  - `[NB_REC] rec` lines: calls, taken, declines by cause, live-stop (expected 0), and the CP's applied / bad.
- **The dry CHECK `SR_NB_REC_CHECK=1`** (diagnostic). Per call the port runs on a device copy, then the guest body
  runs. It compares the ring bytes from the entry's cursor, every device dword except the async fence / time fields
  (unless the port wrote them), and the recorded guest writes. Output: `[NB_REC] check` totals and `[NB_REC_BAD]`
  examples.
- **Progress:**
  - job44 (exe 3312b2e94312): replays 9/9 byte-identical against the CHECK-only build. The dry CHECK compared nothing:
    every Resolve / GPR call stopped at the allocator, because the game's device has `dev+13140 == 0` and so takes
    820FCBE8 mode 1, which was not ported. Half of the ClearF calls stopped at the colour packer. Both are fixed
    (native/targets c72a99e1).
  - job45 (exe 539f779c4282, gameplay c1 + title c2): 1,542,151 calls compared.
    - Ring LEN 0 BAD, ring BYTES 0 BAD, MEM 0 BAD.
    - Stops: kick 0.1-0.3% per entry, and 2 runs crossing a segment limit.
    - ClearF stopped 20% of the time on 8210C090's format 6 (16_16_FLOAT). The spliced body packs that
      `vpkd3d128` type 3 (FLOAT16_2 in Xenia) as D3DCOLOR, so every normal colour packs to 0. That is a
      codegen-defect candidate, assigned to the P2 agent under the difftest-first rule. The port now
      follows the body that runs, and `[NB_REC] check` counts `fmt6 packs / nonzero`.
    - DEVICE ~0.1% BAD, all at async fence / pump fields.
  - job46 (exe b51cbec5bbb6): replays 9/9 identical. The CHECK still showed the DEVICE class.
  - job47 (exe e11807e5093e, title) printed per-dword detail and named it: a CHECK artifact, not a port bug.
    The "port-written" async set came from diffing two reads of the live device, because LLVM forwards
    `memcpy(copy, before)` to the live source (TOOLKIT 1.8h). Fixed in 152830c8: writes are tracked at the
    port.
  - job48 (exe ec9483bbfbf5): replays 9/9 identical; dry CHECK over gameplay f1 + title f2, 1,930,835 calls
    compared: **`NB_CHECK_VERDICT FAIL`** on both rolls (f1 bad 194, f2 bad 325; `archive/v1_nativeb/l48.log`), so
    the job skipped its record-mode rolls.
    - Ring LEN 0 BAD, ring BYTES 0 BAD, MEM 0 BAD.
    - The 519 BAD are all DEVICE dwords, every one inside the ~30 s loading window at dev+0x2D18 / 0x2D1C (the fetch-constant
      range another thread moves; no port touches it); 0 BAD after the loading screen. The field joined the
      async list (f096e7d1).
    - Declines: kick 0.09-0.3% per entry, and 2 runs crossing a segment limit.
    - `fmt6 packs = nonzero` on every call. The counter is an upper bound (any non-zero lane). Per the P2 agent's
      traces, the colour is (0,0,0,1) or (0,0,0,0) in all 44 traced scenes, and FLOAT16_2 packs only x / y, so the
      packed word is 0 with either body: the splice defect is latent in every traced scene.
  - job49 (same exe), record mode `SR_NB_REC=1`. Its log (`l49.log`) kept only `freeze_diff.py`'s raw verdicts,
    not the gate grades quoted here; the ledger re-graded the archived tables with the gate's rule (2026-10-09):
    v_cppm4 PASS 12, v_stress PASS 11, v_abp **FAIL** (bad = freeze 12), the world drawn in every graded freeze.
    - CPPM4 freeze: graded PASS 12/12 ingame freezes exact.
    - STRESS (SEG_STRESS=1024) + CPPM4 freeze: PASS 11, specks 1-37 px at maxdiff 2. Strand A's rows 1+2 / 4
      STRESS rolls show the same class.
    - Frozen-pass A/B under STRESS (`SR_AB_TOGGLE=SR_NB_REC,SR_AB_PRESENT=1`, records and the guest body on
      alternate presents): 10 of 11 graded freezes exact or specks. Freeze 12 changed 98 px at maxdiff 2: the
      STRESS speck class on the cape (diff sheet `v_abp_freeze/freeze_exact_012.png`), over the 64 px tolerance:
      by the gate's rule this A/B is a FAIL. A non-STRESS A/B settles it (job52's x_abp below).
    - Records taken: 99.88% of Resolve calls, 99.77% of ClearF, 99.80% of GPR. The rest declined at a kick
      site. live-stop 0, CP bad 0, render-len-bad 0.
    - **Meter pair (ingame windows): 1,820.4 -> 585.8 PM4 packets per frame (-1,234.6, -67.8%).** By kind:
      type-0 1,171.7 -> 241.7; auto draws 131.4 -> 0.3; shader loads 44.4 -> 0.1; events 29.3 -> 0; bin
      selects 57.7 -> 21.1; sync 73.7 -> 47.3. nrec +50.2. Expected ~1,233.
    - Speed, in-roll A/B (59 windows): tick -0.305 +-0.695 ms (paired -0.243 +-0.688), frame p50 +0.144
      +-0.317, fps -1.3 +-5.1. Neutral.
    - Flicker gate: span PASS (1 of 358). The freeze roll CRASHED: a THREAD_FATAL in strand A's
      `Driver::closeSpan`, which wrote past the record arena. A multi-item opaque span outgrows closeSpan's
      record bound (`len + header + 16`): each opaque item adds a tag word, and a GPR entry carries ~6 items
      before its first alloc. Minidump stack: closeSpan <- Entry::alloc <- recb::Prod::alloc <- portGpr.
      Reported to strand A with the fix (`+ 4 * nItems`); the gate re-runs on it. Strand A fixed it with the exact record
      size (2003a556). Its new boundBad counter then caught a second under-count, SetConst (a07b5535).
  - **job52 (exe 65f616b2e42b, the BAKE: `SR_NB_REC` default ON, dev 5d51f0b7) - HISTORICAL, superseded:** its
    PASS grades below predate the world rule and the black-world fix 41572437; the ledger now grades its rolls as
    expected failures and row 9's evidence is the 2026-10-13 re-proof (r9m, the status above). Every job52 roll ran with
    strand A's closeSpan bound under-count live: `boundBad` 28,022 in h1 and 18,745-33,451 in the others (SetConst
    records 4 bytes over their bound, fixed a07b5535 after this job; `l52.log`).
    - replays 9/9 byte-identical vs strand A's ba5a1b5c3102;
    - dry CHECK h1 **PASS: 818,092 calls compared, 0 BAD** (ring LEN / BYTES, DEVICE, MEM);
    - frozen-pass A/B (records and the guest body on alternate presents): **PASS 11/11 without STRESS** (one 41 px
      speck at maxdiff 2, in one pair of 39; a path difference would hit every alternate pair) and **PASS 11/11 with
      SEG_STRESS=1024**;
    - **flicker gate with no gates: PASS** (span 0 of 358, freeze 11/11);
    - meter, default vs =0: 1,772.6 -> 514.2 PM4 packets per ingame frame (-1,258.4) - not quotable: the default
      roll x_m1 drew a black world (0 of its 42 ingame sparse frames show the world); job54's pair below is.
    - Speed was the one miss: paired tick +0.338 +-0.220 ms, frame p50 +0.289 +-0.149. Two causes, both fixed. First,
      per-call heap traffic in the ports' Prods; they are now thread_local and reset, and the pre-pass renders no
      bytes and keeps no guest writes. Second, strand A's Entry constructor zeroed a 192-item span per call; strand A
      replaced it with an Impl pool (03bd5441).
  - job53 (exe perf53, before the Impl pool): roll y_ab11 measured tick +0.006 +-0.562 ms (paired +0.012 +-0.494),
    p50 +0.032 +-0.315 (paired +0.102 +-0.142), fps -1.1 +-5.2: **neutral**. The other roll, y_ab7, shared the
    machine with a p2 sub-roll for 90 s and is not used. Host cost per call: pre-pass 1.47 us, Entry open 0.55 us,
    live pass 2.48 us.
  - job54 (exe bb6c8476657d = dev 03bd5441 + row 9 baked): replays 9/9 vs the dev exe; flicker gate with no gates
    PASS (span 0 of 358, freeze 9/9) - but its FREEZE roll drew a black world in all 9 graded freezes (ledger,
    2026-10-09), so only the span holds (scene luma 32.3, all 360 presents in play); **meter 1,765.3 -> 525.9 PM4
    packets per ingame frame (-1,239.4)** (the world drawn in 33 of 36 / 44 of 45 ingame sparse frames); the record
    line: Resolve 99.90% taken, live-stop 0, CP bad 0; strand A's closeSpan counters boundBad 0, boundOver 0. Speed
    z_ab: p50 paired +0.296 +-0.160 ms, tick paired +0.823 +-1.212 (noisy). Over three A/Bs the frame p50 cost of
    record mode is +0.10 to +0.30 ms: OPEN, the next optimisation (a single speculative pass instead of pre-pass + live).
  - job55 (exe db4435a0bad3 = dev 0ef8f297 + row 9; dev had baked the FLOAT16_2 splice, and the port now calls
    the splice's own predicate `sr::kernel::spliceVpkF16()`): replays 9/9 vs the dev exe 9dccfa012378; **dry CHECK
    PASS, 916,276 calls compared, 0 BAD** (26,059 format-6 packs, all on the new FLOAT16_2 path); **the default-env
    span showed the 3D world BLACK under a working HUD** (the gate's scene luma 2.0, near-black fraction 0.97, vs
    22.5-32.3 in fg42 / fg49 / fg52 / fg54), and the coordinator stopped the merge. The cause named at the time - "the
    dev splice bake 10fd3d3d, not row 9" - is **WITHDRAWN**: the black world is a startup race in every build (jobs
    56-65, next bullet), and the splice is not its cause. The evidence it rested on: the P2 agent's same-exe
    A/B has no row 9 in it (exe 017f178bf8e1; sr-wt-p2/archive/v1_p2/vpk/w1_gp_on vs w2_gp_off): vpk ON gives a black
    world in 4 of 4 gameplay presents, `SR_SPLICE_VPK_F16=0` draws the world in 4 of 4, and its own flicker-gate span
    is black too (luma 1.2 -> 0.0) - one roll per setting, each of which can lose the race. Dev 0729341b put the default
    back to OFF and 6725dd95 restored it ON; 5f460b43 added a scene floor to the gate. **Lesson:** the dry CHECK compares GENERATION, so it passed with the suspect value on both sides; the port
    mirrors the predicate. The flicker gate passed a black world, because black is stable. Only an EXECUTION check
    with a content floor (the frozen-pass record-vs-guest A/B and span luma) sits outside the code under test, so it
    is part of the bar before any bake. (Why FLOAT16_2 "blacks the world" was the open question here; it does not: in
    P2's traced gameplay frames the format-6 clear words are identical ON vs OFF, and the race explains the rolls.)
  - **The black world, root-caused (jobs 56-65, 2026-10-09): a startup race, in every build. It is not vpk and not
    row 9.**
    - P2's census settled vpk: one distinct fmt-6 input all run, ON == old, DIFFER 0. Job57 on merge57 (vpk OFF):
      replay 9/9, dry CHECK PASS (916,382 / 0 BAD), but the frozen-pass A/B went black (pixel-exact 10/10 with luma
      0.0), so the floor failed it.
    - The traces name the mechanism: the eye-adaptation history (1E246000, read before it is written every frame) is
      NaN and stays NaN (ENGINE_MAP 6g.8d).
    - The boot bracket put the onset before swap 100 (b63_1). Swaps 2-16 then split the rolls: the black roll b64_2
      traced no backend state at all (edram=0, 11 pages), the drawn b64_1 had it from swap 2.
    - main() starts the guest before `d3d12.init()` (6-8 s) and attaches the live backend after it. Until then the CP
      performs nothing and its draw queue (8192) fills.
    - Job65 (6 interleaved rolls, exe f795c4795efe) with the new attach line:
      - `SR_PM4_WAIT_BACKEND=0`: 19 / 55 / 55 swaps before the attach, 3,024 / 8,191 / 8,191 queued. drawn / BLACK /
        BLACK.
      - `=1`: CP waited 7.9 / 6.4 / 7.4 s, 0 / 0 queued, drawn x3.
      - Boot-to-title 20.6-23.8 s both ways (one =0 outlier at 42.6 s). The EA logo movie plays whole with the fix;
        without it, it was cut short and the next movie raced at 5-13x.
    - Baked ON in kernel/gpu_command_processor.cpp. check_roll entry "the live backend is attached before the CP's
      first command".
    - Tools for the hunt (archive/v1_nativeb):
      - `xtr_adapt.py`: the adaptation draw's t0 / t1 per trace;
      - `xtr_chain.py`: the chain stages;
      - `race_eval.py`: attach, wait, boot time, MOVIE_PACE.
    - SR_NB_NANWATCH (guest RAM) is blind to the chain: resolves land in the GPU shared memory.
    - TaskStop does not kill a job script: memory feedback_taskstop_orphans.
    - job67 (dev exe 19a950f35689 = 41572437): flicker gate PASS, but its SPAN landed on the title and main menu
      (the CP waited 23.2 s for the backend; 25 of its 360 span presents were in play; the scene luma 68.0 is the
      title's sky), so the span says nothing about the world (ledger, 2026-10-09). Its freeze roll (the CP waited
      1.0 s) drew the world in all 8 graded freezes; that and job65's three drawn `=1` rolls carry the fix.
  - **Row 9 status (ledger, 2026-10-14): baked and closed - default ON on dev, re-proven on a base with 41572437.**
    The last unknown (record mode's frame cost) is answered by the r9s study (v1_native/r9s, exe 328f9c1b4d64, rule
    pre-registered in its NOTES.txt): four alternating OFF / ON pairs, ingame p50 OFF 12.6 / 12.7 / 12.5 / 12.6 ms,
    ON 12.6 / 12.6 / 12.5 / 12.5 ms - no measurable cost (29c4632c). `native/targets` merged
    dev 5e29947a (ab065060; the dry CHECK keeps 820F0208 on the guest path while armed now that row 5 is default
    ON, c675963a). The whole bar on that merged exe dd9d6dc1c16c (bank `native-b/archive/v1_nativeb/r9m`): replay
    9/9; the dry CHECK 703,537 compared, 0 BAD (resolve, clearF, GPR); the frozen-pass A/B 15/15 (world 14) and
    under SEG_STRESS 15/15 (world 14); CPPM4 15/15 (world 15); the new-game gate span 1 of 358, freeze 15/15
    (world 12); the meter 1,632.8 -> 431.7 PM4 a frame (bound -1000). CPPM4 here covers the surrounding pipeline:
    row 9's own record kinds are opaque to the PM4 render-back and execute natively (native_flush.cpp
    nativeRecord), so the guest-vs-record A/B is row 9's execution evidence. job54's freeze roll and job57's A/B (black,
    before the fix) are superseded witnesses (contradiction `pre_fix_rolls`).
- Left on the EDRAM side: nothing in the default build at 1x / 3x. The EDRAM buffer, the mirror, the tile map and the
  EDRAM resolve path stay as the oracle (SR_NB_SURF_VERIFY, SR_NATIVE_RESOLVE=0, SR_NB_CLEAR_PROP_VERIFY, the 1x
  replays' snapshot loads).

#### Boot attach: the shader load leaves the attach's critical path (branch `render/boot-attach`, 2026-10-14)

**Merged (f9c08c2d). Live bat1 (2026-10-14, exe 7e7323d22abf, every roll staged on J:, rule in
v1_native/bat1/NOTES.txt written before the rolls): replay 9/9, the flicker gate PASS (freeze 15/15, world 15),
check_roll's black-world and boot-attach entries PASS in all six boot rolls; the pair evaluator OPEN** - time to HUD
ON - OFF +1.88 / +0.03 / -0.97 s, pair 1 over the 1.0 s budget (on1 was the session's first roll: CP ready at 1950
ms vs 754-1143 in on2 / on3 - a first-roll effect is plausible, not established). On the SSD both arms attach under
0.85 s (ON <= 782 ms, OFF <= 846 ms) and the installed cache was already loaded at every ON attach: J: cannot show
the slow-disk benefit by construction. Owed: a follow-up set with a declared warm-up roll before the pairs. (bat2 ran
one with the warm-up in a SEPARATE invocation - OPEN again, CP first command pair 1 +1.04 s, the first launch of each
invocation late. Since native/devcreate-close the plan runs `warmup` inside the invocation and the pairs in the
mirrored order, and `boot_attach_eval.py --warmup` requires one run's proof JSON to hold both: boot.device_create's
CLOSE DESIGN item 4.)

Build note, written before the code. It is the follow-up to `defect.black_world` (the CP waits for the live backend,
SR_PM4_WAIT_BACKEND) and to the dcb finding (`_worktrees/devcreate-boot/archive/dcb/FINDINGS.md` and its Astra
round 1).

**The problem (measured by dcb).**
- On the main thread, `D3D12Backend::init` -> `createXlatPipeline` loads two things before `setPm4LiveBackend`:
  - the installed shader cache: 856 files, 6.7 MB;
  - the runtime translator's boot: dxil.dll + dxcompiler.dll are loaded and hashed (~19 MB), then the persisted
    translations are read.
- In all 16 slow rolls, device, queue, swap chain and the geometry PSO were done by 0.47-0.96 s. The two loads then
  took 5.4-90.2 s and 0.9-26.8 s on the busy 7200-rpm D:.
- The CP waits for the attach with a 60 s limit. Three rolls passed that limit ("CP waited 60000 ms (TIMED OUT)",
  attach at 74-101 s; traffic_save queued 150 draw commands first). A slow disk reopens the black-world race path,
  and a player with the game on a hard disk can hit it too.

**Today's order (f431b7f6).**
1. `app/main.cpp:468` opens the window. `:520` starts the guest thread; its first doorbell starts the CP thread
   (kernel/gpu_command_processor.cpp:504).
2. `app/main.cpp:542` calls `d3d12.init` (gfx/d3d12_backend.cpp:2003): factory, device (`:2079`), queue (`:2125`),
   native stores, swap chain (`:2186`), RTVs, the first recording, fence, upload and readback buffers.
   `m_initialized` is set at `:2285`, and `:2288` builds the geometry PSO (D3DCompile).
3. `:2293` calls `createXlatPipeline` (`:2667`), which first runs `getShaderCache().ensureLoaded()`
   (gfx/shader_cache.cpp:205):
   - the installed dir;
   - then the runtime hook `onLoadedImpl` (gfx/shader_runtime_xlat.cpp:599): `xlat::dxcLoad`
     (shader_xlat_core.cpp:749: LoadLibrary of both DLLs and an XXH3 of both files), the translator id, and the
     persisted translations read and admitted.

   `:2669` returns false for an empty installed cache, which switches the xlat path off. Otherwise it creates the root
   signatures, heaps, DSV, null textures (upload + GPU wait), samplers and the glyph band (`glyphs::init`, one small
   file).
4. `app/main.cpp:563` calls `setPm4LiveBackend` (gfx/null_renderer.cpp:172). This is the attach.
5. The CP polls for the attach for up to 60 s (kernel/gpu_command_processor.cpp:303-308), then runs the ring. After a
   timeout it runs the ring with no backend.
6. First use: `Pm4Engine::onShaderLoad` (gfx/pm4_engine.cpp:795-847) calls `ensureLoaded` and looks up the
   IM_LOAD's ucode hash with `findVs` / `findPs`. A miss requests a runtime translation.

**The new order.**
1. Unchanged up to `d3d12.init`.
2. `D3D12Backend::init` first calls `ShaderCache::beginBootLoad()`:
   - A probe of the installed dir decides whether there is an installed cache: one directory listing, with names and
     sizes as the loader filters them, and no file opened.
   - Then the load starts on its own thread, "SR shader load":
     - stage 1 is the installed cache, published when complete;
     - stage 2 is the runtime translator's boot (the same `onLoaded` hook: DXC load + hash, persisted translations),
       published when complete.
   - The device, swap chain, PSOs and xlat objects follow on the main thread without waiting.
   - `createXlatPipeline` gets "is there an installed cache" from `installedPresent()`: the probe's answer until
     stage 1 ends, then the load's own answer.
3. `setPm4LiveBackend` follows once the device, swap chain and PSOs exist. Of the load, init's path keeps:
   - the probe (one directory listing, timed in its log line);
   - the loader thread's start;
   - and, only if no thread can be created, the whole load. That case is logged ("RAN ON THIS THREAD") and the
     evaluator grades it OPEN.

   No claim here says the attach "does not wait for the disk". The live rolls measure each bound instead
   ("Measured bounds", below).
4. The CP waits for the attach, then for stage 1 (WP1, below), then runs the ring. It does not wait for stage 2.
5. Gate `SR_SHADER_LOAD_ASYNC` (default ON). `=0` restores today's order: the whole load runs inside
   `createXlatPipeline` before the attach.

**Every consumer of the cache and the translator, and what it does while the load runs.**

| Consumer (thread) | Before | Now, while the load runs |
|---|---|---|
| `createXlatPipeline` (main) | ensureLoaded, then VS + PS count == 0 -> xlat off | the probe decides; the xlat objects are created; no wait |
| `xlatReady()` readers: tryXlatDraw, the heap binds after resets and resolves, pm4Flush, pm4PresentFrame, renderPm4Batches, pm4GpuInit (CP, render thread) | `m_xlatReady` | `m_xlatReady && installedPresent()`. It never blocks; the CP reads it only after WP1 |
| `Pm4Engine::onShaderLoad`: IM_LOAD, native shader-bind records, GPU trace replay (CP; replay main) | ensureLoaded (a no-op: loaded in init), then find | find only (its ensureLoaded would wait for stage 2); WP2 inside find |
| drain lookups: the drain's ensureLoaded and findVs / findPs, getXlatPso, getPm4Pso, pm4PrepareDraw, pm4ViewportScissor (CP, drain worker, native executor) | find | the drain's ensureLoaded is removed (it would wait for stage 2). Its hashes come from an IM_LOAD that already passed WP2, so these lookups do not block in practice; if one came first, it would block rather than misread |
| miss report: noteVsMissUcode / noteVsMissDraw / flushVsMissReport (CP, drain) | ensureLoaded, to create the report state | the report state is created on first use, without the load (flushVsMissReport runs after every drain and must not wait); the twin lookup waits for stage 1 |
| HLE draw (SR_PM4_LIVE=0 oracle, d3d12_backend.cpp:4964); the S4 probe (kernel/hle_api_observe.cpp:5509) | ensureLoaded | unchanged: ensureLoaded waits for both stages (oracle and diagnostic paths) |
| runtime translator: onLoaded, onMiss, the worker (findInstalled*, admitRuntime), CreateShader containers (loader, CP, worker, guest) | onLoaded inside init | onLoaded runs on the loader thread after stage 1; onMiss is reached only after WP2, so `loaded` / `disabled` are known; containers are captured as before |
| tests: shader_xlat_test, shader_mkey_test (their own caches) | ensureLoaded | unchanged: with no `beginBootLoad`, `ensureLoaded` runs the load on the caller, as call_once did |

**The wait points.**
- **WP1, at the CP's start** (kernel/gpu_command_processor.cpp, after the attach wait).
  - The CP executes no command until stage 1 is complete. The first IM_LOAD, and every draw after it, sees the whole
    installed cache.
  - The CP's first command keeps its old precondition (backend attached, installed cache loaded), without the
    translator part.
  - The guest waits on its own fences meanwhile, as it does behind the attach wait. During either wait the CP is in
    phase 5 (`kCpPhaseHostWait`). The hang watchdog sets aside only the CP's own stall trigger (H3). It fires the
    others annotated with the wait, and escalates a long wait ("Diagnostics in a host wait", below).
  - Logged once: `[SHADER_CACHE] CP start: waited N ms for the installed shader cache (...), ready after WP1 at
    process +T ms`. The CP may consume its first command later: `[GPU_CP] first command consumed at process +T ms`
    is logged when it does (Astra round 2).
- **WP2, inside every reader of the cache** (ShaderCache::findVs / findPs / findInstalled* / findVsTwin / vsCount /
  admitRuntime).
  - The installed maps are read only after stage 1 is published.
  - An installed miss is answered only after stage 2, where the persisted translations are admitted.
  - So a lookup returns what a fully loaded cache returns. No thread can see a partial cache, and a shader that an
    earlier run translated and persisted is never reported missing.
  - After WP1, the CP blocks here only on an installed miss while stage 2 is still running. For that wait it sets CP
    phase 5 (`kCpPhaseHostWait`). The hang watchdog (kernel/gpu_hang_wd.cpp) does not read the CP's own stall as a
    GPU hang. Any fence or present stall still fires, annotated with the wait.
- **Ordering.**
  - Each stage is published with a release store and a notify, and every reader acquires.
  - The installed maps never change after stage 1. Runtime admissions keep their shared lock.
  - The CP is in order, so nothing behind a blocked IM_LOAD executes before the shader's answer is known.
  - A wait never ends early: the only alternative to waiting is a dropped or wrong draw.
  - A wait over 10 s logs a Warn every 10 s with the loader's progress (files read so far), so a stuck read names
    itself.
  - The loader thread never waits on itself. Hooks it calls back into the cache see their own stage as done.
- **Unchanged.** A genuine miss (in neither the installed cache nor the persisted translations) still requests a
  translation, and its draws are dropped until the translation is admitted (iter-463; SR_SHADER_RUNTIME_XLAT_WAIT=1
  blocks instead). This change does not touch that path.
  - *Superseded 2026-10-17 (render/shader-coverage, the miss policy):* WAIT is the default. The IM_LOAD that missed
    waits on the CP thread - bounded, SR_SHADER_RUNTIME_XLAT_WAIT_MS (30 s) - for its request to be answered, as a
    third host wait of the same kind (CP phase 5, `kCpHostWaitTranslation`, reported through this observer). A
    request exists only after WP2 (the translator stage), so the translation wait never overlaps the boot load; the
    worker never waits on the CP, the render thread or a guest thread. A draw still without its shader has a final
    state (no template, refused, failed, the budget passed): dev drops it (`[SHADER_DROP]`), the release stops with
    the state named. Every wait is a `[SHADER_WAIT]` line. Details: gfx/shader_runtime_xlat.hpp.

**Thread safety.**
- Stage 1 writes the installed maps, the mkey index, the installed dir and the "loaded empty" flag on the loader
  thread, then publishes.
- Stage 2 admits through `admitRuntime` (unique lock, release counter), as it did inside init.
- Readers on other threads synchronize through the stage atomic (acquire) or, for the drain, through the draw queue
  behind an IM_LOAD that already did.
- The per-thread find memo caches only found entries, which never move.
- The load touches no D3D12 object and no guest memory; it only reads files, logs, and runs the translator hook it
  always ran.

**Failure handling.**
- **No installed dir.** The probe says no, so the xlat path is off at init, as before. The loader logs its "NO CACHE"
  Warn, and stage 2 still runs.
- **The dir exists but nothing loads** (every entry unreadable or empty). The probe, which sees names and sizes, says
  yes, and stage 1 admits nothing. `installedPresent()` then turns false, and `xlatReady()` with it. That is the old
  decision, reached when stage 1 ends. The xlat objects already exist and stay unused.
- **One file fails to read, or its DXIL is corrupt.** As before: the file is skipped, or PSO creation fails and is
  logged.
- **An exception on the loader thread** (bad_alloc, filesystem). It is logged as an Error, and the stage is published
  with what loaded. Before, it escaped `D3D12Backend::init` and terminated the process.
- **The loader thread cannot start.** `beginBootLoad` runs the whole load on init's thread (today's synchronous
  order). Its line says so ("RAN ON THIS THREAD (no thread could be created): the attach waited for it").
- **DXC is missing or will not load.** As before: `[SHADER_XLAT] DISABLED`, and misses drop their draws. The decision
  now comes at stage 2 instead of at init; a miss waits for stage 2 and gets the same answer.

**The backend wait: no time limit, and never the ring without a backend** (Astra round 1 on this branch).
- SR_PM4_WAIT_BACKEND keeps its default and its `=0` diagnostic. The prohibition stays: the CP never runs the GPU
  command stream without the live backend, because that is the black-world path.
- The wait is `gpuCpWaitForBackend` (kernel/gpu_command_processor.cpp). It has exactly two ends:
  - **An explicit D3D12 init failure.** main() reports it at once (`failPm4LiveBackend`, when `d3d12.init` returns
    false). The CP logs `[DISPLAY] CP waited N ms for the live backend (INIT FAILED)` and a `[FATAL]` line naming the
    reason. It shows a message box to a player (never under SR_INSTANCE_ID) and exits 3.
  - **The attach.** The CP logs `[DISPLAY] CP waited N ms for the live backend (attached), W still-waiting
    warnings`, so the total wait and its warnings are on record, and runs on.
- **No attach yet, and the init has not failed.** The CP never proceeds and never exits, at 60 s or at any other time.
  - It keeps waiting, and logs a Warn every 10 s: `[DISPLAY] CP still waiting for the live backend after N s (no time
    limit ...)`.
  - It sits in CP phase 5 (`kCpPhaseHostWait`). The phase it had before is restored on the attach.
  - A pathological case stays visible in three places: the Warns while it lasts, the watchdog's `[HOST_WAIT_STALL]`
    escalations (below), and the total wait once it ends.
- The 60 s limit and its TIMED OUT outcome are gone. check_roll's fail pattern still names TIMED OUT, for older logs.
- Tests, one per branch (`--shader-boot-load-test` T9):
  - T9a: a child process, fake clock, no attach for 2 simulated hours. It waits, with 11 Warns at a 10-minute
    period, in phase 5, and returns on the attach with the total.
  - T9b: the real clock. It returns on a 150 ms attach with no Warn.
  - T9c: a child process with an init failure. INIT FAILED, `[FATAL]`, exit 3.
- Mutants M10 (gives up at 60 s), M11 (an init failure runs on) and M12 (the wait leaves the phase) must each fail
  the suite.
- The disk-bound wait moved to WP1, which has no limit either, for the reason given under the wait points.

**Diagnostics in a host wait (Astra rounds 2-3).** The never-proceed policy keeps its diagnostics. Phase 5 used to
silence every hang trigger, so a stuck init call, or a real stall around WP2, got only the generic Warns.
- **Attribution is never decided by timing** (round 3). The round-2 rule kept H1 / H2 only if their evidence was
  already old enough when the wait began. That suppressed a fence stall that began 2 s before WP2, and any that began
  during it.
- **What fires during a host wait** (`gpuHangEvaluate`): H0, H1 and H2, exactly as outside one.
  - H0 needs an idle CP, so it cannot hold in phase 5.
  - Their `[GPU_HANG]` report gets one more line, `(a) during a CP host wait: the CP has waited N s for <what>
    ...` (`gpuHangHostWaitNote`). Graders and readers see both facts. The trigger counts as the hang it is
    (gpu_hang_check's phase is then `host-wait`, which is not the swap-phase exception), and the dump is taken as
    usual.
- **What is set aside:** only H3, the CP's own phase stall, while the CP is in phase 5, which the waiting thread sets
  itself. That stall is the wait, and the wait escalates on its own schedule.
- **Escalation** (`gpuHangHostWaitDue`, run by the watchdog's tick, not by the waiting thread). At 60 s of one host
  wait, then every 5 min while it lasts, the watchdog logs the full GPU_HANG-style block under its own tag,
  `[HOST_WAIT_STALL]`.
  - The header names what the CP waits for (the live backend's attach, the installed cache, or the translator
    stage), how long it has waited, and the escalation number.
  - `(h)` gives the backend init's state: its current step, from breadcrumbs in `D3D12Backend::init` (factory,
    device, queue, swap chain, geometry PSO, xlat pipeline, returned), plus its host thread id, its start and its
    CPU time, so a spinning thread can be told from a blocked one. It also gives the live backend's attach or
    failure, and the shader loader's stage, files read, thread id and CPU time.
  - The block then carries the CP state and IB ring, the fence block, the device fields, the guest waits and the
    allocator. The guest threads and the critical-section holders come in the process's first host-wait block only
    (see "Log budget").
  - The block is never a `[GPU_HANG] ==== trigger`, which the graders (gpu_hang_check, native_proof's ABORT,
    replay_batch) read as a guest hang. It is never a minidump (a slow-disk WP1 wait at 60 s must not cost one),
    and never an exit.
- **Log budget** (round 3). Every report line keeps a literal tag in its format string, `[GPU_HANG] ...` or
  `[HOST_WAIT_STALL] ...`, and both tags are milestones (`kMilestoneTags`, app/log.cpp). A player log
  (SR_LOG=player) keeps them whole however often they repeat. Round 2's `[%s]` tag made them ordinary Error lines,
  capped at 64 per format site, so a genuine hang report would have lost sections.
  - The guest-thread and critical-section dumps format their tags at run time. Those lines stay capped and are
    shared with every caller, which is why a repeating host-wait block takes them once per process.
- **Helper threads** (round 3). A report's lock-taking queries (the physical-window registry, the allocator) run on
  a helper thread that the watchdog abandons after 750 ms.
  - At most one helper is outstanding. While the previous one has not returned (a lock is held), the next report
    skips the queries and says `helper SKIPPED`. It does not stack another blocked thread.
- **Tests:**
  - T7a: a fence stall 2 s before the wait, one that begins during it, and a present stall all fire H1 / H2 in
    phase 5.
  - T7b: H3 is set aside in phase 5 and fires in phase 1 (the control).
  - T7c: the schedule. Over 2 h it escalates 24 times, at 60 s and every 300 s.
  - T7d: the annotation names the wait and its length, and is absent outside one.
  - T9d: a child process runs the real watchdog on a 300 ms / 700 ms schedule over a 2.6 s real-clock backend wait.
    It must log `[HOST_WAIT_STALL]` blocks with the `(h)` state and no `[GPU_HANG]`, and the wait returns only on
    the attach.
  - T9e: under SR_LOG=player, 70 host-wait reports then 3 hang reports keep every section of every report, and the
    hang reports keep the CS dump's summary line. The child registers no guest thread and holds no CS, so populated
    thread and CS rows are not exercised (Astra round 4, P3). They share the per-site cap that the once-per-process
    rule protects.
  - T9f: a lock the helper needs is held across the escalations. One helper thread is started, every later report
    skips it and says so, and the process gains at most 4 threads.
- **Mutants:**
  - M13: suppression with no escalation;
  - M14: the blanket phase-5 suppression of round 1;
  - M7: H3 in a host wait;
  - M20: the annotation dropped;
  - M17: a dynamic report tag;
  - M18: host reports dumping the threads every time;
  - M19: a helper per report.

**Measured bounds (log fields, all in ms since the process was created: `msSinceProcessStart`, GetProcessTimes).**

| Line | Field |
|---|---|
| `[SHADER_CACHE] boot load on its own thread ...` | the probe's duration and start; whether the load runs on the loader thread or RAN ON THIS THREAD |
| `[SHADER_CACHE] boot load: started at process +T ms on thread T (main thread M), named '<name>'` | the loader thread's start, its id and name, and the main thread's id |
| `[SHADER_CACHE] boot load: installed stage N ms (... files read), ended at process +T ms, on thread T (main thread M) - <wrapped reads>; uncovered by the wrapper: U ms` | stage 1's end, sampled BEFORE its publication, and dcb's wrapped-read statistics for stage 1 |
| `[SHADER_CACHE] boot load done: N ms on thread T (main thread M): installed cache A ms, runtime translator B ms - <wrapped reads>; uncovered by the wrapper: U ms (...); ended at process +T ms` | the whole load, stage 2 with its wrapped reads, and stage 2's end, sampled before its publication |
| `[DISPLAY] backend init N ms (...; process +A to +B ms)` | the backend init's window |
| `[DISPLAY] PM4 live backend attached: ...; process +T ms; installed shader cache just after the attach: loaded / still loading` | the attach, from the backend's own timestamp, and the cache's own state read just after it |
| `[SHADER_CACHE] CP start: ..., ready after WP1 at process +T ms` | the CP ready after WP1 (not yet a command) |
| `[GPU_CP] first command consumed at process +T ms (a ring packet / a host item ...; the live backend attached)` | the CP's first consumed command, recorded in the item or the packet path once all its words are published (`gpuCpTakeCommand`) |

`boot_attach_eval.py` reads these fields. A short CP wait does not bound the attach, so the attach is read from its
own field.
- **"Still loading at the attach".** This is read from the attach line's own cache state, never from a timestamp
  comparison (Astra round 2). Stages only advance, so "still loading" read just after the attach was true at the
  attach. Each stage's end is sampled before its publication, so a loader descheduled after publishing cannot put
  its end after a reader it released. T10 holds the loader 400 ms after publishing stage 1; mutant M15 samples the
  end after publication.
- **A self-contradicting log is named.** If the attach read the cache loaded before stage 1's end, or the first
  command precedes WP1's readiness, the evaluator says so.

**The dev merge (32df55cc, with native/devcreate-boot's boot-load lines).** dcb measured the boot load inside
`ensureLoaded`'s `call_once` on init's thread. It used a per-thread read scope (`t_readScope`) around a wrapped
`readFile`, one line for the installed cache and one for `boot load done`. This branch's load runs in
`ShaderCache::runLoad`, so the two are now one line set:
- **One thread per load's statistics.** `runLoad`'s thread points `t_readScope` at that load's counters: the
  installed stage's, then the translator stage's. That thread is the loader by default, or init's thread under
  SR_SHADER_LOAD_ASYNC=0 or the inline fallback. Reads on any other thread are not counted.
- **One line per stage, each fact reported once.**
  - `started` gives the process time, the load's thread, the main thread and the thread's name.
  - The installed-stage line carries both this branch's stage fields and dcb's wrapped reads; dcb's separate
    "installed cache read in / ABSENT after" line is gone.
  - The translator's own line is unchanged (DXC load / hash, persisted-dir scan; it now runs on the loader thread).
  - `boot load done` carries the whole load, stage 2's wrapped reads and stage 2's end; this branch's
    "translator stage" line is gone.
- **Consumers.**
  - `boot_pairs.py` and `boot_census.py` read `boot load done: N ms`, which is now the whole load as before.
  - `boot_threads.py` reads the first `on thread T (main thread M)`, which is now the `started` line: the loader
    thread and the main thread.
  - `stage_roll.py` reads `[SHADER_CACHE] loaded ... from <dir> (`, which is unchanged.
  - `test_boot_attach_eval.py` checks the first three against generated lines.
- **What boot_pairs' rule now meets.** Its consistency check `swap < load end <= attach` assumes the load ends inside
  init. On the default (async) build the load ends after the attach, so a devcreate pair of that build reads as
  INCONCLUSIVE, timestamps out of order. Its pairs need SR_SHADER_LOAD_ASYNC=0 in both arms, or an amended rule.
  The rule is that lane's and is not changed here.

**What stays identical on a warm, fast disk.**
- Every lookup answer stays the same: the same maps, the same admissions, the same installed-wins rule. So does every
  PSO key and draw.
- The CP's first command still runs with the backend attached and the installed cache loaded.
- On a warm disk the 856 files load while the device is being created, so the CP does not wait at WP1 and the stage-2
  wait never happens.
- What changes:
  - the attach no longer waits for the loads' file reads (the probe and the thread's start remain), so the CP can
    start earlier;
  - the `[SHADER_CACHE]` / `[SHADER_XLAT]` boot lines come from the loader thread, interleaved with the `[D3D12]`
    init lines.

**The pack file (brief item 4): the decision waits for the live numbers.**
- After steps 1 and 2, the first draw still waits for stage 1 (WP1).
- Stage 1 opens about 1,070 files (the 856 files plus 216 absent .vs.mkey) for 6.7 MB. On a hard disk that is
  seek-bound: 5.4-90.2 s measured, where one sequential read of a single pack would cost about 0.1 s plus one seek.
- The new `[SHADER_CACHE] CP start` line measures that wait in live rolls. If it stays above about 1 s on D:, the
  pack is the next step:
  - one file, with a format version and an XXH3, and the loose files kept as the fallback;
  - made by the player build.

  That is staging work for the installer lane, not for this branch.

**Offline proof (cloud worker tree `bat`, 2026-10-14; outputs in `_worktrees/boot-attach/archive/bat`).**
- **Builds:** default exit 0 (exe 4b70b618abe5, staged in `archive/bat/binary`); release (`build-rel`,
  SR_RELEASE_COMPOSITION=ON) exit 0.
- **difftest suites:** the dev_1c933798 runner, directory changed to C:\sr\wt\bat, against the same runner on the
  worker's dev build (465c06ce, f431b7f6's code): 74 suites, every exit code identical, 56 exit 0 in both
  (`suites_compare.py`: identical). The new `--shader-boot-load-test` is reported apart: 26 passed, 0 failed in both
  the default and the release build.
- **The wait-point suite's measurements** (every stage-1 read slowed):
  - `beginBootLoad` returns in 0.3 ms while the load takes 1,447 ms;
  - a reader blocks 1,470 ms and then gets the file's own entry;
  - an installed hit returns at once while stage 2 runs, and an installed miss blocks 1,521 ms for stage 2, then finds
    the persisted translation;
  - an IM_LOAD through a real `Pm4Engine` blocks 3,856 ms until stage 1 and counts a HIT, returning before stage 2;
  - CP phase 5 fires no watchdog trigger.
- **Mutants: 9 of 9 killed.**
  - On the worker, 8 of the 9 mutants fail the suite, each on its intended check (`shader_boot_load_run.py`).
  - M9 puts `ensureLoaded` back into `createXlatPipeline`, which only T8 sees. T8 needs a swap chain; over ssh
    `CreateSwapChainForHwnd` returns 0x887A0022, so `init` stops before `createXlatPipeline`, and M9 survives there.
  - On the owner's PC (interactive session, the coordinator, `archive/bat/local_t8` and `local_t8_m9`):
    - the staged clean difftest passes 28/0, both T8 checks included;
    - the staged M9 difftest exits 3, 25 passed / 3 failed. Both T8 checks fail by name ("init returned before
      stage 1 was published", init 8,452 ms; "init took less than the slowed stage 1"), and so does T6's side effect
      ("returned while stage 2 still ran"). M9 is killed.
- **shader_xlat_test** (links the real shader_cache.cpp; its runner could not link at the base, fixed):
  - T1, T3 and all twelve T4 game-path checks pass. Those cover WAIT and async admission, persisted admission at
    load, a failed write, damaged entries skipped and installed-wins.
  - The OFF run passes 3/3.
  - T2's 3 FAILs are NO-TEMPLATE: 38 of its 40 corpus dirs are under `archive/` and absent on the worker.
  - The 7 shader_cache / runtime_xlat mutants are killed, each by its own T4 check.
- **shader_mkey_test:** 29/30, the same as the test built from the dev sources. The one FAIL (T4) reads sidecar paths
  the fixture lists as `<repo>\...`. All 11 of its mutants are killed.
- **Pipeline and native-service test exes:** all nine exit 0 in both builds.
- **Not run here:** the game, the harness and any live roll.

**Offline proof, round 2 (Astra round 1: MERGE AFTER FIXES, `archive/bat/astra/round1.txt`; outputs in
`archive/bat/r2`).**
- **What changed:**
  - the backend wait has no time limit (above);
  - the measured-bounds log fields;
  - `startLoadAsync` reports whether a thread ran the load;
  - the evaluator was rewritten on boot_pairs' rule, with 22 unittest cases;
  - the claims were reworded to bounds.
- **Builds:** default and release (`build-rel`) exit 0. Both are staged in `archive/bat/binary2`; the DLLs are
  byte-identical to round 1's.
  - The exe is `54966273970f`, rebuilt from commit 45bbfac6. The tested tree differed from it only in a comment and
    the version header.
  - The difftest, `e1cdef40c1d7`, is byte-identical to the one tested.
- **difftest suites:** the same runner against the same dev baseline. 74 suites, every exit code identical, 56 exit 0
  in both (`r2/suites_compare.json`: identical).
- **`--shader-boot-load-test`:** 32 passed, 0 failed in both builds; T8 is NOT RUN over ssh, as before.
- **The backend-wait checks (T9):**
  - T9a: 7,200 simulated seconds, 11 Warns, phase 5 during the wait, phase 0 restored after;
  - T9b: 167 ms on the real clock, no Warn;
  - T9c: the child exits 3 with INIT FAILED and the `[FATAL]` reason.
- **Mutants: 11 of 12 killed on the worker, each by name.**
  - M1-M8 as in round 1.
  - M10 (the wait gives up at 60 s): the T9a child exits 3, and the three T9a checks fail.
  - M11 (an init failure runs on): the T9c child exits 0, and T9c fails.
  - M12 (the wait leaves the CP phase): T9a's and T9b's phase checks fail.
  - M9 survives here as before. Its code is unchanged since the local kill, and the round-2 T8 re-run is owed to
    the interactive machine (live plan, item 5).
- **SR_ORACLE.** difftest is built with SR_ORACLE=1 in every preset, build-rel included. Only the game target gets
  SR_ORACLE=0, from SR_RELEASE_COMPOSITION. So the suite's pass does not execute the shipping SR_ORACLE=0 build.
  - The cheap check run instead (`archive/bat/oracle_pp.py`, `r2/oracle_pp/oracle_pp.json`): each touched TU was
    preprocessed with build-rel's own SupermanReturns compile command, once as is (-DSR_ORACLE=0) and once with
    -DSR_ORACLE=1. Each boot-attach function was then compared:
    - the whole of shader_cache.cpp;
    - cpMain, gpuCpWaitForBackend, the CP's wait-observer hooks, cpBackendFatal, gpuCpVblank;
    - gpuHangEvaluate;
    - failPm4LiveBackend and setPm4LiveBackend;
    - D3D12Backend::init, xlatReady and createXlatPipeline;
    - Pm4Engine::onShaderLoad.
  - Each preprocesses to identical text under both values.
  - The limit: what those functions call outside the list (oracle headers that become inline no-ops under =0)
    differs, and no wait point executes under SR_ORACLE=0 offline. The live plan's rolls also run the default
    (SR_ORACLE=1) exe; a release-composition roll would be the first SR_ORACLE=0 execution.
- **The evaluator:** `python -m unittest discover -s tools/perf -p test_boot_attach_eval.py` gives 22 tests, OK.
  They include each Astra counterexample:
  - one pair, and three ON-first pairs: INCONCLUSIVE;
  - a different SR_HIGHFPS: INCONCLUSIVE;
  - a missing wait line: INCONCLUSIVE, not a 0 s wait;
  - a crash or timeout without markers, or a missing outcome: OPEN, with the missing evidence named;
  - a nan, infinite or negative budget: rejected.

**Offline proof, round 3 (Astra round 2: MERGE AFTER FIXES, four P2s, `archive/bat/astra/round2.txt`; outputs in
`archive/bat/r3`).**
- **What changed:**
  - the host-wait diagnostics ("Diagnostics in a host wait");
  - the evaluator checks every recorded wait, 0.0 ms included (Astra's repro, an OFF roll's 2.51 s wait rewritten
    as 0.0 ms, is now INCONCLUSIVE);
  - each stage's end is sampled before its publication, and the "still loading at the attach" witness is the
    attach line's own cache state;
  - the CP start line says "ready after WP1", and the real first consumed command has its own line and endpoint.
- **Builds:** default and release exit 0. Both binaries are staged in `archive/bat/binary3`, built from this commit;
  `ids.txt` there gives their hashes.
- **difftest suites:** 74, every exit code identical to the dev baseline, 56 exit 0 in both (`r3/suites_compare.json`).
- **`--shader-boot-load-test`:** 38 passed, 0 failed in both builds; T8 is NOT RUN over ssh.
  - T10: the stage-1 end is at +5,341.6 ms, and the reader its publication released woke at +5,341.7 ms, with the
    loader held 400 ms after publishing.
  - T7a, T7b and T7c pass; the schedule makes 24 escalations in 2 h, the first at 60.00 s.
  - T9d: 4 `[HOST_WAIT_STALL]` blocks through the real watchdog. Each has the CP state and the `(h)` lines and none
    is a `[GPU_HANG]`, and the wait returned on the attach at 2,607 ms (`r3/t9d_child.txt`).
- **Mutants: 14 of 15 killed on the worker, each by name (`r3/mutants/summary.json`).**
  - M7: T7a fails, mask 8 (H3).
  - M13: T9d fails, 0 escalations.
  - M14: T7b fails, mask 0.
  - M15: T10 fails; the end at +5,719.4 ms falls after the reader woke at +5,312.2 ms.
  - M9 still needs the interactive machine (live plan, item 5).
- **SR_ORACLE (`r3/oracle_pp/oracle_pp.json`):** the comparison now also covers gpuHangHostWaitDue, the watchdog's
  tick and report, gpuCpHostWaitName, gpuCpSnapshot, noteBackendInitStep and backendInitState. All the listed
  functions preprocess to identical text under SR_ORACLE=0 and =1, with the same limits as before.
- **The evaluator:** 26 unittest tests, OK. The new ones cover:
  - the recorded 0.0 ms wait;
  - the witness from the attach's own reading, both ways (a true witness with the stage-1 end sampled before the
    attach, and an inconsistent log named);
  - the first-command line (its own endpoint; consumed without the backend: OPEN; before WP1's readiness:
    inconsistent; missing: named);
  - `[HOST_WAIT_STALL]` counted per arm.

**Offline proof, round 4 (Astra round 3: MERGE AFTER FIXES, four P2s, `archive/bat/astra/round3.txt`; outputs in
`archive/bat/r4` and the worker's `r4b` / `r4c`).**
- **What changed:**
  - attribution without timing: H0-H2 fire in a host wait, annotated; only H3 is set aside;
  - the first command is recorded where it is consumed, in both command paths (`gpuCpTakeCommand`);
  - literal, milestone report tags, with `HOST_WAIT_STALL` in `kMilestoneTags`;
  - at most one outstanding diagnostic helper ("Diagnostics in a host wait").
- **Builds:** default and release exit 0. Both binaries are staged in `archive/bat/binary4`, built from this commit;
  `ids.txt` there gives their hashes.
- **difftest suites:** 74, every exit code identical to the dev baseline, 56 exit 0 in both (`r4/suites_compare.json`).
- **`--shader-boot-load-test`:** 44 passed, 0 failed in both builds; T8 is NOT RUN over ssh.
  - T7a: masks 2 and 6. A fence stall that began 2 s before the wait fires H1, and one that began during it fires
    H1 + H2.
  - T11: a first packet whose words complete at 250 ms is recorded at 250, not at 100.
  - T9e: under SR_LOG=player, 70/70 host-wait reports and 3/3 hang reports are whole, the hang reports' CS summary
    line included. No thread or CS rows are populated in that child.
  - T9f: 10 escalations with the lock held: 1 helper started, 9 skipped, threads 7 -> peak 10.
- **Mutants: 19 of 20 killed on the worker, each by name (`r4/mutants/summary.json`).**
  - M14: T7a fails, masks 0 / 0.
  - M7: T7b fails.
  - M20: T7d fails.
  - M16: T11 fails; the packet is recorded at 100 ms and the item at 50 ms.
  - M17: T9e fails; 64 of 70 host-wait reports survive, the per-site cap Astra found.
  - M18: T9e fails; the hang reports lose their CS line.
  - M19: T9f fails; 4 escalations start 4 helpers.
  - M9 still needs the interactive machine.
- **A defect the new test caught on the way:** a note line in the host-wait block quoted the text `[GPU_HANG]`.
  replay_batch.py (any line containing it) and T9d would have read it as a hang. It was reworded before this run.
- **SR_ORACLE (`r4/oracle_pp/oracle_pp.json`):** 25 functions, now including gpuCpTakeCommand,
  gpuHangHostWaitNote, runHelper and helperNote. All preprocess to identical text under SR_ORACLE=0 and =1.
- **The evaluator:** 26 tests, OK. The log lines it reads are unchanged in form.

**Round 5: the dev merge (32df55cc; Astra round 4: MERGE, its P3 narrowed in T9e; outputs in `archive/bat/r5`).**
- **The merge.** dcb's per-load boot-load lines and this branch's stage fields are one line set; see "The dev merge"
  under the measured bounds. The only conflict was gfx/shader_cache.cpp. dcb's `call_once` timer is replaced by
  `runLoad`'s per-stage read scopes on the load's own thread.
- **Builds:** default and release exit 0. The merged binaries are staged in `archive/bat/binary5`, built from the
  merge commit; `ids.txt` there gives their hashes.
- **difftest suites:** the 74, plus dev's five other registered suites (coher-sentinel, native-bracket,
  native-census, native-residue, setconst-render). They were run against the worker's dev build (3ea0c19d), whose
  C++ differs from 32df55cc only in dcb's boot-load lines. All 79 exit codes are identical, 61 exit 0 in both
  (`r5/suites_compare.json`).
- **`--shader-boot-load-test`:** 44 passed, 0 failed in both builds. Its log has 8 loads with one `started` and one
  `boot load done` line each; every load ran on its own thread, never the main thread.
- **Tests:** devcreate-boot's `test_boot_pairs.py` 10 OK, `test_owner_census.py` 35 OK, native_proof's test script
  0 failed. The evaluator has 28 tests, OK; the 2 new ones are an ON load on the main thread (OPEN) and dcb's
  consumers reading the merged lines.
- **SR_ORACLE:** shader_cache.cpp, its new runLoad included, is still identical under SR_ORACLE=0 and =1, and so are
  the 25 functions.
- **Mutants:** 19 of 20 on the merged tree, each by its intended check; M9 still needs the interactive machine
  (`r5/boot_load_mutants.log`).

**The live plan (the orchestrator runs it; dry-run only here).**
1. Stage the branch's exe from the worker (`archive/bat/binary5`, the dev merge). Run
   `python tools/perf/native_proof.py --plan boot_attach --exe <bat exe> --base <dev f431b7f6 exe> --out <dir>`.
   The plan runs:
   - the 9-trace replay against the base exe;
   - three alternating ON / OFF boot pairs (`bat_on1 off1 off2 on2 on3 off3`; OFF = `SR_SHADER_LOAD_ASYNC=0`);
   - the flicker gate on the default (ON) path.
   (bat1 / bat2's form. Since native/devcreate-close the plan runs `bat_warmup` first, inside the invocation, and the
   pairs mirrored - `bat_off1 on1 on2 off2 off3 on3` - graded with `--warmup <dir>/bat_warmup --pair off:<dir>/bat_off1
   on:<dir>/bat_on1 --pair on:<dir>/bat_on2 off:<dir>/bat_off2 --pair off:<dir>/bat_off3 on:<dir>/bat_on3`.)
2. Grade the pairs:
   `python tools/perf/boot_attach_eval.py --pair on:<dir>/bat_on1 off:<dir>/bat_off1 --pair off:<dir>/bat_off2
   on:<dir>/bat_on2 --pair on:<dir>/bat_on3 off:<dir>/bat_off3 --budget=1.0 --out <dir>/boot_pairs.txt
   --json <dir>/boot_pairs.json`.
   - The rule is in the tool's docstring, registered with the plan before any roll.
   - Its design, marker, OPEN and budget checks are ported from devcreate-boot's `boot_pairs.py` (Astra rounds 1-2):
     exactly these three pairs in this order; one exe; the same SR_* configuration but the flag; the same save
     seeding, parsed from every roll's steps.log; each roll's arm engaged (OFF must attach after the load's end); the
     run order checked against the logs.
   - A crash, a timeout or a missing outcome is OPEN, naming any missing evidence. A missing backend-wait line is a
     0 s wait only when the attach came before the CP's start.
   - A nan, infinite or negative budget is rejected.
   - `python -m unittest discover -s tools/perf -p test_boot_attach_eval.py` holds the rule's 26 tests, Astra's
     counterexamples from both rounds among them.
3. What it must show:
   - **Replay:** 9/9 identical to the base exe. Every lookup is a fully loaded cache's, so the pixels cannot move.
   - **Pairs:** `BOOT_ATTACH pairs=3 budget=1.0 ... -> NO REGRESSION OBSERVED IN THESE PAIRS`. That needs:
     - every paired ON - OFF difference <= +1.0 s for the time to the HUD request (primary), the attach and the CP's
       first consumed command (the `[GPU_CP] first command consumed` line, not the WP1 readiness);
     - every roll attaching with 0 guest swaps and 0 queued draws;
     - every first command consumed with the backend attached;
     - `[SHADER_CACHE] CP start ... (loaded;`;
     - no `[FATAL]` and no INIT FAILED;
     - no ON load that ran on init's thread.
   - **What it reports, and what the ledger text may say:** the ON rolls' measured bounds, in ms since process
     start: the attach, the backend init's end, the probe's duration, the loader thread's start, the CP ready after
     WP1 and the CP's first command.
     - It also gives in how many ON rolls the installed cache was still loading at the attach (decoupling witnessed
       in k of 3), read from the attach line's own cache state.
     - It counts any `[HOST_WAIT_STALL]` escalations per arm.
     - It does not say the attach "did not wait for the disk".
   - **The gate:** PASS (span + freezes, world rule) on the default path.
   - **Information (the pack-file decision):** the WP1 wait and the installed / translator stage durations of the ON
     rolls, against the OFF rolls' backend wait. Any still-waiting warnings are counted per roll.
4. Optional:
   - a cold-cache pair (the standby list emptied before each roll, e.g. RAMMap `-Et`, admin) shows the slow-disk case
     on demand;
   - `python tools/check_roll.py --exe <bat exe> --tag bat_cr --only native-b,render` grades one whole-run roll
     against the black-world guard and the new boot-attach entry.
5. Not proven offline, and owed to this plan:
   - T8 on the merged build. Run the staged `archive/bat/binary5/difftest.exe --shader-boot-load-test` on a machine
     with an interactive session, from a checkout's root (it reads `data/shader_cache/dxil`). It opens a hidden
     window that is never shown and launches no game. The round-1 binary's T8 passed there, and M9 failed it there;
   - the CP's own WP1 wait in a roll;
   - the player's message box on an init failure. T9c proves the INIT FAILED / `[FATAL]` / exit 3 path in a child
     process, which sets SR_INSTANCE_ID and so shows no box. A forced D3D12 init failure, e.g. an unsupported feature
     level on a VM, is the only way to see the box;
   - in a real roll, the hang watchdog across a long backend, WP1 or WP2 wait: no `[GPU_HANG]` for the CP's own
     stall, and `[HOST_WAIT_STALL]` blocks past 60 s (a cold-cache pair may show one on D:).

#### Strand B instruments (iter-467 native-b, branch `native/targets`)

- **The census** (`SR_NB_CENSUS=<frames>`, `_EVERY`, `_SKIP`; observe only). One `[NB_CENSUS]` line per
  resolve (R: source key / format / MSAA, rectangle, destination base / pitch / height / format /
  endian, exp bias, raw or convert, clears, which path ran: N native, O via an owner, M mixed owners,
  E EDRAM, C CPU, T native target), per texture load of the GPU loader (T: identity, scaled, direct or
  scratch, the resolve that last wrote its base range and how its identity differs from that
  destination: `match=X` = the same identity, else b/p/f/e/w/h/l/L = base / pitch / format / endian /
  width / height / levels / linear), per transfer set (X: every move's source -> destination key and
  tiles) and per materialize (M, B2), each with its GPU time: the `SR_PM4_GPU_TIME` segments that end
  while the record is current (gtMark stamps the record id beside its tag). CPU readers of GPU-written
  memory (`pm4ShmSyncToGuest`) are listed too. `python tools/nb_census.py <log> [--skip-frames 1]`
  groups them by class and joins every load to its resolve.
- **The per-resolve oracle** (`SR_NB_RHASH=1`, diagnostic: one GPU sync per resolve). After every
  resolve, the SAMPLER-VISIBLE texels of its destination rectangle are read back and hashed: on the
  PM4 path the destination is loaded by the texture loader as its own format into a scratch texture
  (exactly what a fetch of that identity gets, at the scale); on a native-target resolve the target
  itself is read. `python tools/nb_census.py rhash <P log> <N log>` compares two runs of the same
  trace resolve by resolve (exit 1 on any mismatch). Looped replays (`SR_GPU_TRACE_REPLAY_LOOPS=3`)
  exercise the steady state; `tools/replay_batch.py` keeps frames 2 and 3 (`_f2` / `_f3`, compare
  with `--suffix _f2`).

#### B2 design: native targets (`SR_NB_RESOLVE_TEX=1`, default OFF; at a draw resolution scale)

The PM4 path writes each resolve into the scaled resolve memory, and every texture over it is then
LOADED again by the scaled loader. A native target removes both halves for the common case:
- **Match.** A resolve whose destination is exactly the identity of a texture already in the cache
  (the shared resource of `SR_TEX_SHARE`: same base, pitch, bytes per pixel, tiled, scale, one level,
  typeless + UAV, guest size covering the rectangle) and that is current up to this resolve (its
  version = the newest page stamp of its range) is written INTO that texture by the NAT_TEX resolve
  kernels (`native_resolve.inc`): the native resolve's own per-pixel path, then each destination word
  through the texture loader's own transform (the fetch's 32-bit endian swap, the UINT channel split of
  the direct load) - the texel equals what loading the memory gives. The texture is stamped current
  at the new version, so its bind loads nothing.
- **Hold.** The memory is not written. The resolve's extent pages are HELD by the target (a page map
  plus the union rectangle); they are still marked resolution-scaled and their versions still move,
  so every other texture over them reloads.
- **Materialize.** Every reader of the scaled memory - a texture load (2D or an array layer), the
  scan-out, a later resolve into the range, the target's eviction - first materializes held pages:
  the loader run backwards (target texel -> loader inverse -> endian swap -> the scaled tiled
  address), so the bytes are exactly what the PM4 path left there. Bit movers only (32 / 64 bpp
  loaders): their transform is a bijection. CPU readers do not exist at a scale (Xenia: no readback
  of scaled resolves; the guest never sees them).
- First version: colour sources, single owner with the source's sample grid, kinds fast / full 32 and
  64 bpp; anything else keeps the native-resolve / EDRAM path. A target exists only after the
  texture's first load, so a resolve's first frame always takes the old path.
- Second version (b2d+): 16 bpp (kind 3, the shadow cascades) and LAYERS of GPU-loaded single-level
  stacked textures (kind 1 targets: the array's cache key + slice). With `SR_NB_RESOLVE_TEX=1`,
  textures that may become targets are created typeless + UAV, 16 bpp included (the 16 bpp LOAD
  still takes the scratch path unless `SR_TEX_LOAD_DIRECT_16=1`).

#### B0 inventory (2026-10-07, replay census; `archive/v1_nativeb/b0` in the worktree) [measured]

Every gameplay fixture frame (gp1 / gp2 Xenia captures, lg1 / lg2 our live captures; menu and the
eahd FMV trace have the same pipeline behind them) runs the same 26 resolves:

| Resolve (source -> destination) | per frame | sampled as | B2 |
|---|---|---|---|
| depth k_24_8_FLOAT 1280x720 -> k_24_8_FLOAT (raw) | 2 | f23 1280x720, same identity | no (depth: the loader drops stencil, no inverse) - depth-plane SRVs instead |
| colour k_2_10_10_10_FLOAT 1280x720 -> k_16_16_16_16_FLOAT | 2 | f29 (`_EXPAND`), same bits | target |
| colour k_8_8_8_8 1280x720 -> k_8_8_8_8 | 2 | same identity | target |
| colour k_16_16 4x MSAA 640x640 -> k_16 (exp bias 2^-5): the 4 shadow cascades | 6 (3 with clear) | layers of a stacked f24 640x640 x4 | array-layer target (v2) |
| colour k_8_8_8_8 1280x720 -> k_8_8_8_8 + clear (the front buffer) | 1 | the scan-out | - |
| colour k_8_8_8_8 640x360 / k_16_16_FLOAT 640x360 | 2 / 1 | same identity | target |
| colour k_8_8_8_8 320x184, 160x96 / k_2_10_10_10_FLOAT 320x184 | 4 / 1 | 320x180, 160x90 textures | no: the 184/96-row rectangle overhangs the 180/90-row texture |
| colour k_32_FLOAT 64x64, 16x16, 8x8 x3 (the luminance chain) | 5 | 64x64, 16x16 (targets); 4x4, 1x1 textures under 8x8 rectangles | partly |

- **Sampling:** every resolve that is sampled is read through its own identity (`match=X`) or the
  same bits under another format of the same width (df32 read as f29). No reinterpretation at
  another pitch, base or bpp occurs in these frames.
- **CPU readers:** none at a scale; at 1x the only reader is the replay harness's own frame dump.
  The guest never reads resolved memory (our PM4 path never writes it back to guest RAM, and the
  game runs; Xenia's `readback_resolve` defaults to `fast`, ours is effectively `none`).
- **Texture loads:** 39 per frame, 22 of them the shadow array: `pm4ShmLoadArray` reloaded all 4
  layers at every bind between the 6 cascade resolves, the never-resolved 4th layer included.
- **Transfers:** 17 sets per frame (C = colour, D = depth, format, m = 4x MSAA):
  C0>D0m, D1m>D0m, D0m>C4m, C4m>C3m, C4m>D1m, C3m>C3, D1m>D1, C0>C6, C6>D0m, D0m>C6, C6>C0,
  C0>D0m, D0m>C0, C3>C3, C3>C14, C14>C0, C0>C0, C3>C0, D1>D1m. The costly ones (replay clock):
  C6>D0m 0.50 ms, C3m>C3 + D1m>D1 0.36, C4m>C3m + C4m>D1m 0.19, the frame-end C0/C3>C0 0.15.
- **GPU per frame (lg1 / gp1 at 3x, replay clock, 4 frames):** resolves 2.05 / 2.00 ms, texture loads
  2.18 / 2.18, transfer sets 1.25 / 1.23 (`d_pcen3x`). With B2 + depth SRVs + per-layer reload
  (`d_allcen3x`, b2e - before the 16 bpp array targets): resolves 1.42 / 1.43, texture loads 1.00 / 1.00
  (39 -> 23 loads), transfers unchanged. The depth resolve pair 0.85 -> 0.24 ms.

#### B1 finding: transfers between surfaces at different EDRAM bases are unobserved

`SR_NATIVE_RT=2` (drop every ownership transfer between keys at different EDRAM bases; keep the
same-base reinterpretations) gives 9/9 byte-identical frames at 1x and 3x (frames 1-3 of a looped
replay) and 78/78 identical per-resolve hashes on 6 traces. Few of the costly transfers are
different-base though (D1m>D0m, C4m>D1m); most are same-base reinterpretations.

Per class (`SR_NB_XFER_DROP=<class>`, b2e, looped 3x replay of menu / gp1 / lg1; observed = any
per-resolve hash or frame changes when the class is dropped):

| Observed (a real alias the frame depends on) | Unobserved on every fixture |
|---|---|
| C3m>C3 (the scene's 4x MSAA colour read as 1x: 31-45 of 78 resolve hashes, 8/9 frames) | C6>D0m (0.50 ms, the most costly), D0m>C6, C6>C0, C0>C6 |
| D1m>D1 (its 4x depth read as 1x: 30/78) | C4m>C3m, C4m>D1m (0.19 ms) |
| D0m>C0 (45/78, frames) | C0>D0m, D1m>D0m (0.10 ms) |
| D0m>C4m (33/78, frames: the shadow map starts from the depth surface's bits) | D1>D1m (0.07 ms) |
| C0>C0, C3>C0 (6/78 each), C3>C3 (9/78) - pitch changes at one base | C3>C14, C14>C0 |

The unobserved classes are ~0.6 of the ~1.25 ms of transfers in a steady-state frame (lg1, replay
clock; a 0.50 ms C6>D0m seen once was a first-use outlier). Dropping them is not proven safe outside
these frames. The claiming draws' state (census X record, b2f+) gives the rule's first half:
- every unobserved transfer into a DEPTH surface is claimed by a depth-only draw with
  RB_DEPTHCONTROL 0x00008777 - z enable + write, z func ALWAYS, stencil func ALWAYS, z-pass op
  REPLACE: an XDK clear quad that rewrites depth and stencil of every pixel it covers;
- every unobserved transfer into a COLOUR surface is claimed by a draw with blending off
  (RB_BLENDCONTROL0 0x00010001 = ONE / ZERO), write mask 0xF, no depth: it rewrites every covered pixel;
- the OBSERVED colour transfers (C0>C0, C3>C0, C3>C3) are claimed by draws of the same write-only
  form - but the downsample chain draws 320x180 / 160x90 while its resolves read
  320x184 / 160x96 rectangles, so the rows the draw does not cover come from the transfer.
So "write-only claiming draw" is not enough; elision also needs the draw to cover every moved pixel.
Coverage is not known on the CPU (vertex positions come out of the VS). Candidates for the second
half: the XDK clear's own bracket (strand A's `D3DDeviceClearFullSurface_82100488` brackets name the
draws the API issued as full-surface clears), or a geometric bound from the draw's viewport, scissor
and rect-list vertices. Not built yet.

#### B1a: transfers into an XDK clear are not needed (`SR_NB_CLEAR_ELIDE`, built 2026-10-07)

The claiming draws of the unobserved depth transfers, and of C4m>C3m, are the XDK's own EDRAM clear
(IDA, retail): `D3DDeviceClear_sub_821013D0` loads a fixed shader pair with IM_LOAD_IMMEDIATE (VS =
27 dwords at 0x820BA0D0, XXH3 0A6D1DD7767FDF27; PS = 9 dwords at 0x820B59DC, 2E372EA28CC404B7 - no
kill, no depth export) and `D3DDeviceEmitClearRectVertices_sub_82100A28` draws an auto-indexed rect
list whose vertices it writes into the command buffer (vertex fetch 0, 7 floats a vertex, x / y in
surface pixels minus 0.5 when PA_SU_VTX_CNTL.pix_center = 0). `D3DDeviceClearSurface_sub_82100F70`
sets RB_DEPTHCONTROL 0x8777 (z ALWAYS + write, stencil ALWAYS / REPLACE) and RB_STENCILREFMASK write
mask FF; for a colour clear of a 32 bpp surface it takes the "fast" path - the colour is packed into a
depth + stencil value and written as DEPTH through a 4x-MSAA view of the same EDRAM at half the
pitch (`D3DDeviceClearRectTileSplit_sub_82100D20`: the tile-aligned interior), plus 1x strips for the
rows below the last whole tile. That is why so many transfers go C>Dm and back: the clear's own
aliasing. In the lg1 trace the 9 clear draws (#25, #977, #2969-#2976, #3013) cover exactly the tiles
their claims move: e.g. #2970 [0,320) x [0,176) in the 4x view = the 176 tiles of C6>D0m.

So: when a draw is that clear (shader pair hashed from the image bytes at run time, rect list of 3
vertices, no viewport transform, no window offset) and rewrites a binding completely (colour: mask F,
blend ONE / ZERO, no alpha test; depth: z ALWAYS + write and stencil ALWAYS / REPLACE / write mask FF,
front and back), the tiles of a move into that binding that lie wholly inside the rectangle (and the
scissor) are not moved; the rest of the move stays. Ownership is claimed as before. Expected per
steady-state lg1 frame (b2f census, replay clock): C0>D0m + D1m>D0m 0.10 ms, C4m>C3m + C4m>D1m
0.16, C6>D0m 0.04, C0>D0m x2 0.07, D1>D1m 0.07 = ~0.44 of the 1.24 ms of transfers.
Oracle and measurements: below (pending the b3 replay batch).

#### What B2 leaves, and the next lever (analysis, 2026-10-07)

After B2 a resolve still COPIES the surface into its texture: bandwidth-bound at 3x (f3 -> df32,
1280x720, 64 bpp: 0.164 ms each = ~130 MB moved, ~800 GB/s). The copies are not redundant: every
pair of resolves into one destination has a full-screen draw between them (lg1: shadow #40 / #41
filter quad / #42; post chain #3010 / #3011 / #3012 - a ping-pong that samples the texture while it
renders the next step), so "skip an identical second resolve" finds nothing (rejected, see
PERFORMANCE.md section 6). Removing the copy needs render-to-texture: draws of a pass that ends in a
1:1 resolve (same host format, exp bias 1, no MSAA, rectangle at the origin) render straight into the
resolve's texture, decided with lookahead over the frame's batches (all known at render time), and
only when the surface's pixels are dead after the resolve (cleared or fully redrawn before any read,
no transfer out of its tiles). Candidates in lg1: the scene f3 -> df32 pair (0.33 ms), the
1280x720 post / front-buffer f0 -> df6 resolves (0.35), the 640x360 pair (0.06). Not started.

What blocks it (analysis 2026-10-08, code + census `co_cen3x_lg1`, frame 3; nothing built):
- **Sizes.** A host RT is the guest surface's pitch x every tile row to the end of EDRAM
  (`edram::RtKey::height`, Xenia's GetRenderTargetHeight): the 1280-wide scene / post surfaces are
  1280 x 2048 guest pixels, while the texture that samples the resolve is the fetch's 1280 x 720. One
  resource cannot be both (an SRV has no sub-rectangle), so a swap or alias needs RTs sized to their use
  or the texture bound as the pass's RTV, with the pass's draws kept inside it.
- **Exact pairs only where the resolve is a bit copy.** k_8_8_8_8 -> k_8_8_8_8 (`sw1`: red / blue
  swapped - an SRV component mapping would carry it), k_16_16_FLOAT -> df31, k_32_FLOAT -> df36. Not
  the scene's k_2_10_10_10_FLOAT -> 16_16_16_16_FLOAT pair (the resolve rounds to 7e3; the host RT holds
  f16), not the depth pair (20e4 -> R32 + stencil side texture), not the shadow resolves (4x MSAA,
  four-sample average with an exponent bias).
- **The RT's content after the resolve.** The post chain is a ping-pong (draw into C0 sampling T,
  resolve C0 -> T, ...), so a swapped-out RT still has to supply the old pixels its next draw does not
  cover: a lazy copy, or the B1b coverage mark + masked fill.
- **Budget.** The exact pairs are 0.60 ms of the 2.02 ms of resolves per 3x frame (f0 1280x720 x3 0.45,
  one of them the front buffer; f0 640x360 x2 0.08; f6 640x360 0.07). The 3x live frame is CP-bound
  (nb_prof, fr14: GPU_CP running 9.21 ms of a 9.88 ms frame; GPU ~6.9 ms), so a GPU-only saving does not
  move uncapped fps until the CP drops. This is the native device's model (strand A's builder: the
  game's own render-target / resolve calls name the texture), not a PM4-path patch.

#### The aliases removed: B1c, B1b, the resolve fan-out, B2 at 1x (iter-467 native-b, 2026-10-07 night)

Four gates (default OFF on the branch), each a mechanism of the EDRAM model replaced by what a PC renderer does:

| Gate | EDRAM-model mechanism it replaces | What runs instead |
|---|---|---|
| `SR_NB_CLEAR_PROP` (B1c) | the XDK's console clears: a 4x-MSAA twin (C3m / D1m) or a depth surface aliasing a colour target (D0m) is cleared, then the real surface takes the tiles over and the ownership map MOVES the cleared tiles into it (C3m>C3 + D1m>D1, D0m>C4m / C0 / C6) | the clear's uniform value is known (its vertex colour, depth 0, the stencil it replaces), so the move becomes a `ClearRenderTargetView` / `ClearDepthStencilView` of the destination with the value the transfer computes (the store's pack, the load's unpack: `edram::packColor` / `packDepth` / `unpackColor` / `unpackDepth`); a value the destination cannot hold exactly keeps the transfer. An XDK clear also claims only the tile rows its rect touches (the strip clears of a 360-row surface no longer take 23 rows from their owner). `SR_NB_CLEAR_PROP_VERIFY=1` runs the transfers and reads each propagatable destination back (`[NB_CLEAR_PROP] VERIFY ok / MISMATCH`) |
| `SR_NB_COVER_FILL` (B1b) | moves into a surface that the claiming draw overwrites anyway (the opaque full-screen post-process quads: C0>C0, C3>C0, C0>C6, C3>C3, C3>C14, C14>C0) | the coverage proof done by the GPU: the draw runs FIRST with a PSO variant that also marks its coverage in a D24S8 scratch stencil (ALWAYS / REPLACE 1), then the moves run with a stencil EQUAL 0 test - only the pixels the draw left get the old content. Exact for partial and discarding draws by construction; only draws with no depth / stencil binding, no blending and every channel written qualify, and only direct host-RT moves into their colour bindings wait |
| `SR_NB_TEX_FANOUT` | a resolve whose destination has several current targets (the shadow cascades: a 2D fetch + layers of two stacked fetches) wrote one and copied the rectangle into the others | the NAT_TEX kernel stores every texel into up to three targets (u2 / u4 / u5) |
| `SR_NB_TARGETS_1X` (B2 at 1x) | at 1x every resolve went to the guest-layout memory and each texture over it reloaded (30.5 loads a frame in the 1x fight) | the 1x NAT_TEX kernels (NAT_DUAL) store each texel into the target texture AND the memory, which stays exact for every reader (nothing held); the texture is stamped current, so it is not reloaded |

Replay proof (`archive/v1_nativeb/b0`, `eval_b11.py`; each gate on its own exe snapshot, dev baselines t_p3x /
t_p1x / l_p2x / i_prh3x / p_rh1x3): every gate alone and CLEAR_PROP + COVER_FILL + TEX_FANOUT together, frames 1-3
byte-identical at 3x (9/9), 1x (9/9) and 2x (6/6); per-resolve hashes 78/78 on 6 traces; no failure line;
`SR_NB_CLEAR_PROP_VERIFY=1` 0 MISMATCH on 9 traces at 3x and 4 at 1x; witnesses `[NB_CLEAR_PROP] batch=`,
`[NB_FILL] ... stencil-masked`, `[NB_TARGET] native resolve at 1x`. Census (3x, gp1 / lg1): transfers 0.78 ms ->
0.35 (CLEAR_PROP), 0.55 (COVER_FILL), 0.13 (both). TEX_FANOUT: no gain (in steady state a cascade has one current
target; rejected, kept OFF). TARGETS_1X: exact, but the dual-write resolves cost what the texture loads saved
(PERFORMANCE.md 2.15). Next: live in-roll A/B (job14), a check roll, bake CLEAR_PROP + COVER_FILL + SCALED_REMAP.
Coalesced NAT_TEX stores (SR_NB_TEX_COALESCE) were tried for the cascades and TARGETS_1X: exact, no gain (the
typed stores cost per texel, not by pattern) - reverted (PERFORMANCE.md 2.15).

#### The remaining aliases: what is left of the transfers (analysis + census, 2026-10-07)

With B1a, a steady-state gameplay frame (lg1, replay census `l_allelcen3x`, frame 4) still runs 17
transfer sets, 0.78 ms at 3x. Each set is claimed by one draw (census X records):

| Set (moves) | GPU ms | Claiming draw | Fixture-observed (SR_NB_XFER_DROP) |
|---|---|---|---|
| C3m>C3 + D1m>D1 (720 + 720 tiles) | 0.36 | the scene's first 1x draw over the 4x scene surfaces (z on) | yes |
| C0>C0, C0>C0, C3>C0 (12 + 36 + 672) | 0.15 | full-screen quad, blend ONE / ZERO, mask F, no z | yes (C3>C0, C0>C0) |
| D0m>C4m (1280) | 0.06 | the shadow cascade's first draw (blend on) | yes |
| D0m>C0 x2 (176 + 176) | 0.06 | the 640x360 pass, MRT, blend on | yes |
| C0>C6, D0m>C6, C6>C0 (184 / 176 / 184) | 0.11 | 640x360 quads, blend ONE / ZERO | no |
| C3>C3 (48), C3>C14 (4), C14>C0 + C3>C0 (4 + 44), C0>C0 (12) | 0.04 | the downsample / luminance quads | C3>C3 yes, C3>C14 / C14>C0 no |

"Render the reinterpreting surface into the same host target" does not apply to any observed one:
C3m>C3 / D1m>D1 change the sample count (a D3D12 resource has one), D0m>C4m / D0m>C0 reinterpret
depth bits as colour (a depth-stencil resource cannot be a render target), and C0>C0 / C3>C0 /
C3>C3 change the pitch at one EDRAM base, which scrambles rows (pixel (x, y) moves to another tile).
These moves are the game's own reinterpretations; they already run host RT -> host RT (iter-466
SR_PM4_XFER_DIRECT / SR_PM4_XFER_DEPTH: no EDRAM buffer), so what is left of the EDRAM model here
is the tile-ownership map that decides them. The unobserved ones (C0>C6, D0m>C6, C6>C0, C3>C14,
C14>C0; ~0.1 ms) are claimed by opaque full-screen quads whose coverage needs the VS's output
positions (vertex data in NDC through the VS, not screen-space like the XDK clear): a "B1b" would
need the VS run on the CPU for those draws (Xenia's draw_extent_estimator does it for the extent).
Next lever for the EDRAM model is therefore the resolve copy itself (render-to-texture, above),
then B1b; B-CPU first (the render thread is the 240-fps blocker).

### Dependency points
- **D1 (A -> B), not blocking until A stage 5:** today both strands meet at the register file (A's
  records write it, B's draw drain reads the targets from it). When A stops producing a register
  file, records carry a `NativeTargets` struct {colour[4] {base, pitch, format, MSAA}, depth}; B
  defines it in B1 and consumes it from the register file until then. **Defined (2026-10-07):**
  `gfx/native_targets.hpp` - `color[4]` / `depth` as `edram::RtKey` values (base, pitch in 32-px
  tiles, MSAA, depth flag, format), `colorMask[4]`, `extentTiles[5]` (tiles each binding covers),
  `flags` (z enable / write, stencil, z func); `fromDrawTargets` / `toDrawTargets` convert to and
  from the drain's `edram::DrawTargets`. With the census on, every draw batch round-trips it and
  logs `[NB_TARGETS] ... MISMATCH` on any difference.
- **D2 (B uses A), not blocking:** native resolve / clear ENTRIES (D3DDeviceResolve 0x8210C5F8,
  ClearF 0x82101A58, Clear 0x82101998 - 200K / 61K / 8K brackets in u_rec runs) can use A's marker +
  record pool (same 'NDRW' mechanism, a record kind per entry). Until A exposes it (by 1b), B makes
  resolves native behind the PM4 resolve packets, as N4a already does.
- **D3 shared files:** `gfx/pm4_engine.cpp`, `gfx/gpu_trace_replay.cpp`, `gfx/d3d12_backend.hpp` -
  each strand edits only its own blocks/functions listed above, merges dev before touching a
  shared file, and sends the orchestrator a one-line note before editing outside its block.
- **D4 oracles stay independent:** A's per-draw hash covers the register file + shader pair + draw
  arguments (B does not change them); B's per-resolve hash and frame compare cover pixels (A does
  not change them). Both run replay 9/9 + whole-run frames incl. the intro before every merge.

**Mode B, the structural step (native draws from the projection) - STAGE 0 [2026-10-07, iter-467,
rolls z_split / z_cls / z_ledger2, exe 28616345884b / e72e1f865e98, gameplay/port, uncapped].**
Design (agreed with the orchestrator): the draw entry keeps the guest's ordering by writing ONE
private type-3 NOP into the ring, padded to the exact length the XDK encode would have taken (so
the cursor, segment kicks and fences advance exactly as today), carrying a record id; the CP reaches
it in order and runs execRecord() (the projection's register / fetch / ALU / bool file through the
same backend draw, render-target owner logic and shared-memory validation). Stage 1a keeps the XDK
body for every guest-visible side effect but points its encode at a scratch buffer; stage 1b
replaces the encode with native side effects proven against 1a by SR_NATIVE_DRAW_CHECK (cursor,
kicks, every device dword and the written pages equal).
- Size (`[NATIVE_FRAMESPLIT] ENTRIES`, z_split): DrawVertices 7.9 s (11.5M calls) + DrawIndexed
  4.9 s (16.8M) = ~6% of the render thread's wall (the scope timing inflates small entries; same
  share as e1).
- Draw classes (`[NATIVE_REC] classes`, z_cls, 14.9M draw records counted once): plain 3.06M
  (20.5%), Z-pass 4.53M (30.4%), UP / Begin-End 0.89M (6.0%), chunked 736, immediate-VS (the XDK
  loads a patched VS copy via IM_LOAD_IMMEDIATE because the object's fence had not passed) 6.77M
  (45.4%; with Z-pass 0.34M). So the VS fetch patch (45%) is the biggest blocker, then Z-pass.
- Write ledger (SR_NATIVE_DRAW_LEDGER=1, `kernel/native_draw_ledger.cpp`, every 256th entry; the
  device diff covers the XDK body only; write-watch pages compared with phase-matched control
  windows). Device dwords the draw entries change: the cursor +0x28 (100%), the dirty masks
  +0x00..+0x24, the per-stream stride bytes +0x2D20.. (indexed 21%: the draw-time VS fetch patch),
  derived packed registers +0x28A0 / +0x28A4 / +0x28A8 / +0x28C0, and on segment kicks +0x2C /
  +0x30 / fence fields +0x2A1C / +0x2A38..+0x2BB0 / +0x3298 / +0x329C / +0x32D0 / +0x34B8..+0x34C8;
  Begin adds +0x3314 / +0x3318 / +0x3320 / +0x34B8..+0x34C8; End only the cursor. Kicks
  (GpuCommandBufferFlush + new segment, via the bracket scopes): indexed 16 of 10,796 sampled
  draws, auto 117 of 7,145, two scopes each. Z-pass: indexed 7.5% of samples, auto 69%. Pages
  outside the device and the written segment: the same image-global pages as the control windows
  (other threads), a few at 3-12x the control rate (0x8299B000 12.6x, 0x82913000 4.3x, 0x829A1000
  4.1x, 0x82969000 3.9x, 0x82966000 3.3x) - candidate XDK globals the draw path writes, to be pinned
  with a watchpoint before stage 1b (stage 1a keeps them exact by running the XDK body).

**CP frame pipeline [2026-10-04, rolls e1 / f1 / h1-h6].** What Swap and BlockUntilIdle "wait"
on: nothing - [PM4_SWAP_TIME] frameWait 0 / submitWait 0 and CP idle 2 ms per 5 s. In the
synchronous path Swap IS the frame render on the CP (23.5 ms per frame; perf2's render thread
takes it off the CP), and BlockUntilIdle's share is the draw-queue drain (batch build) its fence
packets trigger: the CP must drain before it writes a guest-visible fence (the guest may reuse
the buffers right after), as Xenia's CP orders it - work, not a wait. With the render thread on,
the CP is the limiter (perf2 r_wc_prof); its costs cut here (each exact; replay 9/9
byte-identical): command-buffer-window memo (exact incremental), grant memo validated against the
mutation's range, [IB_RANGE] and the hang watchdog's per-IB deep walk opt-in, SSE2 index remap,
XXH3 at AVX2 width, only the constant blocks the shaders read. Ingame 31.3 / 31.3 -> 35.4 / 33.0
fps (two rolls each), CP IB time -20%; whole-run frames h6 correct, 0 ICALL (PERFORMANCE.md §5).

**Next steps (in order).** (1) More CP: the remaining page validation (XXH3 over ~36K pages per
frame; a write-tracking scheme cheaper than the S1 write watch), Pm4Engine::execute, the drain's
atomics. (2) Mode B when the structural step is due. (3) Begin/End vertex data equivalence, deeper
ALU history, two-colour-owner spans.

**Update 2026-10-01: N0 task 1 (entry sort) is done.** The result is in
`data/native/xdk_entry_classes.tsv` (213 rows), with the table-only setters in
`data/native/xdk_table_setters.tsv` (146 rows). The replacement contracts and the
guest-visible output ledger are in `docs/NATIVE_XDK_SPEC.md`. That pass corrected five
statements in this document: §3.3, A.1, A.4 row 4, N2 and N5. Each is marked inline.

It turns the user decision "100% native before real mods" (CLAUDE.md, THE GOAL) into
phases with exit criteria. It does not replace `docs/RENDERER_PM4_HYBRID.md`, which stays
the history of the PM4 path and the manual for the oracle. That doc's P4 row (mod and
enhancement surfaces) moves after N7 here.

**Sequencing rule (user, 2026-09-30):**

1. correctness: audio, freeze, camera, collision (the C-gate, §5);
2. native conversion (this doc);
3. mods.

Native work may start early only where it is offline or observe-only (N0). Emulation-path
speed gets no investment beyond usability.

**Evidence tags used below.** Every claim carries one of these, so nobody builds on a guess:

| Tag | Meaning |
|---|---|
| **[measured]** | a run artifact, a trace scan or a golden-dump read; the source is cited |
| **[static]** | IDA xrefs/decompiles or a scan of the generated code; high confidence, but a computed address or a table-loaded value can escape it |
| **[estimate]** | a reasoned number or scope; not yet measured |
| **[open]** | unknown; §11 lists what settles it |

---

## 0. What "native" means, checkably

The dividing test follows the NO-FORCING question. Is the code a PC implementation of a
library API, where the game still decides everything? Or does it reproduce what console
**hardware** did with bytes the library prepared? The first ships. The second is oracle.

| Allowed in the shipping path | Oracle only (not allowed in the shipping path) |
|---|---|
| AOT-recompiled game code, including the guest register model and big-endian guest memory | Interpreting PM4 (`gfx/pm4_engine`, the async CP in `kernel/gpu_command_processor.cpp`) |
| Xenos microcode translated to DXIL: the AOT cache plus the runtime translator (`gfx/shader_runtime_xlat`, XenosRecomp) | Emulating EDRAM (`gfx/d3d12_pm4_edram.cpp` tile ownership, EDRAM-layout resolves) |
| Recompiled guest code that is CPU-only: the XAudio mixer, the XGraphics shader compiler and texture tiler, **the XDK's own shader vertex-fetch patcher** (§4.4), D3DX, RenderWare, Lion, EA's DECEAXMA, **the game's WMV3/VC-1 and WMA movie decoders** (B1) | The GPU and XMA register files, MMIO traps or seeds, the register-0x600 physical context array |
| **The XDK per-state setters that only write the device shadow** (62 setters; A1). They stay guest code, and native reads the shadow at draw time as Unleashed does | The ring buffer, the doorbell, CP interrupts, WAIT_REG_MEM, and the synthetic ISR vsync bit (`kernel/stubs.cpp:3865`) |
| Data-format conversion at a resource boundary: untiling, endian swap, 7e3/20e4 decode, Xenos piecewise-linear gamma, when a resource is created or updated | **Emulating the GPU's memory reads**: the `gpuWindow` / shared-memory page model, in which the GPU fetches by physical address at execute time |
| **Guest-physical addresses as a resource-identity key** (B6, A2). Fetch constants carry physical page bases the guest computes itself (`fc[1]>>12`, ENGINE_MAP §6g.6), so the backing-memory → resource registry is keyed by physical page. The physical-memory kernel APIs (`MmGetPhysicalAddress`, `MmAllocatePhysicalMemoryEx`) are platform APIs and stay | Faking interrupt-handler calls, setting the vblank bit at `0x7FC86544`, or answering display-register reads (`D1MODE_V_COUNTER`, `D1MODE_VIEWPORT_SIZE`) (B2) |
| **A host vsync / pacing clock, and guest callbacks delivered when host GPU fences complete** (B2) | Running the XDK's device internals: `D3DDeviceCreate_sub_82106010`'s ring, ISR registration, pump threads, tiling interpreter (`KeLockL2` users) |
| Native kernel APIs: the 16 `XMA*` exports backed by a host decoder (EA's CPU-side XMA code depends on exactly these; F), the XAudio render-driver sink, a native Present/vblank/fence model, `Vd*` as no-ops | The XMA register front end (`SR_XMA_MMIO`, the register-0x600 seed, hwIdx physical arithmetic) |

Notes on the edges:

- **Movies (B1).** The movie decoder is guest CPU code (`WMV3_*`/`Vc1*` at 0x828EF608+ and
  0x824Bxxxx-0x824Cxxxx), not an import. Unleashed, Xenia and Downpour all run the game's
  own decoder. What must be native are its hardware touchpoints: the D3D present
  (including the kick callback, §7 N3/N7), XAudio voices, and file I/O. The render-script
  `fmvmovie` path is a second producer of movie planes and gets the same treatment.
- **Vblank (B2).** The game registers **no** vblank callback of its own **[static +
  measured]**: `dev+15120` is null in all three golden dumps while the counter at
  `dev+15124` runs, and the only two non-XDK stores at +15120/+15124 are on the WMV
  decoder context (Appendix C.2). What native must preserve is **pacing equivalence**:
  the present interval and how many frames the CPU may run ahead. Game time steps (and
  the open camera issue) depend on that cadence.
- **Backing memory (A2, A6).** Superman writes texture backing memory directly
  (`RwRasterUploadLevelPixels_sub_8261C5B0`, the XGraphics tiler, the movie planar memcpy),
  so guest-format backing memory stays for every resource. Native GPU resources are
  mirrors, invalidated at the guest's own boundaries (§4.5).

---

## 1. Acceptance: one list, each item with how it is measured

This is the single N7 exit (A8). "0" means 0 across a whole-run roll of the long tape plus
the full tutorial tape plus imported-save chapter points (C5).

| # | Metric | How it is measured | Target |
|---|---|---|---|
| 1 | **PM4 packets interpreted** | The true per-packet counter in `gfx/pm4_coverage` (type-0 and type-3 per opcode, including inner INDIRECT_BUFFER packets). Not `Pm4Engine::packets`, which counts `execute()` calls. In native mode the engine is not constructed, so the counter is also asserted structurally: `[NATIVE_AUDIT] pm4_engine=absent` | **0** |
| 2 | **PM4 dwords written to command memory** | The native device's command cursor and limit (`dev+0x28`/`+0x2C`) point at a sentinel, so any leftover packet writer reaches `GpuCommandBufferFlush_sub_820FD8C0` or `GpuCmdBufBeginNewSegment_sub_820FCF90`. Both are native tripwires that count and log the caller's LR (B7). Stopping the interpreter alone would not prove the packets are gone: an XDK path that still emits packets nobody reads would lose output silently | **0** tripwire hits |
| 3 | **Hardware-register access** | **Shipping build:** 0x7FC00000-0x7FFFFFFF is mapped no-access in native mode, so any residual access is a loud fatal fault under the existing VEH rule (B7). **Oracle-tap build:** an MMIO load/store counter by function. `sub_82116800` reads 0x7FEA1800 on every boot, so it must be replaced (N6) before the no-access mapping can go on | **0** faults; tap count **0** |
| 4 | **Host emulation events** | Counters for each emulation component: `gpu_ring_doorbell` hits, synthetic ISR vsync bit sets, CP waits/interrupts/swaps, EDRAM `transfers`/`tilesMoved`, `SR_XMA_MMIO` register operations, VdSwap ring scans. Each is emitted in `[NATIVE_AUDIT]` | **0** each |
| 5 | **Residency tripwire** | Generated tripwires on every function in the hardware-facing ranges (§3.3) not classified `native` or `cpu-only-keep`. `scripts/audit_native.py` does the static half: it fails on any unclassified function in those ranges | **0** calls; audit passes |
| 6 | **Graphics oracle match** | Mode B (§6) on paired fixtures (D1): per draw 0 mismatched fields (`tools/native_draw_diff.py`); per frame N−X ≤ P−X + ε (`tools/native_ab.py`); FMV bit-exact | as stated |
| 7 | **Audio oracle match** | `tools/xma_parity.py` on `SR_XMA_CAPTURE` kicks: 0 bytes with the same FFmpeg build, ±1 LSB against live Xenia; `[AUDIO_RMS]` against live Xenia on deterministic segments | as stated |
| 8 | **Guest-visible output ledger** (D3) | A replace-mode run's API call stream diffed against the shadow run's on the same tape, plus the ledger of return values, out-parameters, device fields read, resource fence fields and callback timing (§6.3) | no unexplained divergence |
| 9 | **Pacing equivalence** (B2) | Swaps per second and CPU-ahead depth (frames between a present call and its completion) at each era checkpoint, compared with Xenia running the same scripted scenario (D4) | within tolerance [estimate: ±5% swap rate, same max depth] |
| 10 | **Era match** | The same scripted scenario on live Xenia and on us, compared at era checkpoints (not frame identity, D4); whole-run contact sheets viewed; one blind Astra call | every era present and correct |
| 11 | **C-gate still holds** | Audio, freeze, camera and collision criteria (§5) re-run in native mode | as in §5 |

Every counter is emitted uncapped and cumulative in a `[NATIVE_AUDIT]` line every second,
plus `[NATIVE_AUDIT] SUMMARY` at exit. The tags are registered with
`tools/audit_log_gating.py` so counts from them are provably safe. `tools/native_audit.py
<log>` writes `archive/native_audit/<tag>.json` with pass/fail per item.

---

## 2. What is known and what is estimated

| Item | Status |
|---|---|
| Hardware-register access is 13 functions: 8 GPU in the D3D core, 5 XMA in XAudio (Appendix B.1) | **[static]**, cross-checked against every `eieio`. A register base loaded through a pointer would escape; the oracle-tap MMIO counter is the mechanical proof |
| The hardware-facing kernel imports are complete: 20 `Vd*`, `KeLockL2`/`KeUnlockL2`, 16 `XMA*`, 5 `XAudio*`, the physical-memory APIs. No XMP, XHV or other hardware block (C1) | **[static]** from `data/ida_names.tsv` |
| 216 D3D core entry points are called from outside (1,128 sites); 33 are hooked today | **[static]** |
| About 105 game functions bypass the entry points (B1-B5, §3.2) | **[static]**, two independent scans; exact count **[estimate]** |
| **B3 recorded command buffers are dead code**: no playback needed, not even sane `QueryCommandBufferSize` values (A7) | **[static]**, high confidence (Appendix C.1) |
| The game has no vblank callback; dev+15120 is null in Xenia (B2) | **[static + measured]** (Appendix C.2) |
| The tessellated draw path exists but has no live witness in 289,640 traced draws (B4) | **[static]**; liveness **[measured]** absent in menu, FMV and early/mid gameplay; later eras **[open]** (Appendix C.3) |
| Vector stores, `dcbz`/`dcbzl` and `stwcx.`/`stdcx.` bypass `PPC_STORE_*`, and the XDK's constant uploads use `stvx` (C3) | **[static]**, codegen census (Appendix C.4) |
| Frame split: XDK D3D share of the render thread is 7-46%; everything else ≥ 54%; fence waits < 4% (E5) | **[measured]** but weak: n = 76 samples, heavy logging, LR-based (Appendix C.5) |
| The XAudio XMA HAL is unused | **[open]**. Static evidence says probably (its only voice creators ask for PCM formats; all 320 XMA contexts are zero in the golden menu dump; contexts 256-319 were never initialised in the sound-ISO oracle while EA owned 0-255) but that is not proof |
| Gameplay uses predicated tiling | **[measured]** partly: the menu frame has 1,407 bin-mask packets (`tools/xtr_draws.py` on `menu_pressstart_454107ED_9415.xtr`). Gameplay scope **[open]** |
| EDRAM aliasing is needed natively | **[open]** (whole-run `tilesMoved`) |
| Phase sizes and durations | **[estimate]** throughout; none are given as dates |

---

## 3. Library inventory (summary; full tables in Appendices A and B)

### 3.1 The hardware-facing libraries

| Library | Range | Native disposition |
|---|---|---|
| XDK D3D9 core (Aug 2006 XDK) | `0x820F0048..0x821162E8`, 623 functions | Entry points sorted into three groups (A1, below). Shadow-only setters stay guest code; emitters and resource/sync functions are replaced. Device internals (ring, ISR, pump threads, tiling interpreter) never run in native mode |
| XGraphics texture layout, tiling, byte swap | `0x827B75B8..0x827BAB98` | `cpu-only-keep`: guest code that writes guest memory; native invalidates on its completion |
| XGraphics shader compiler and state compiler | `0x827BAD08..0x827CF4D0`, `0x827D0000..0x828A74D0` | `cpu-only-keep`. The state-blob builders `sub_82845780/958/B38` have no code xrefs (**[static]**); liveness **[open]** (Xenia witness) |
| D3DX9 effects, texture loader, HLSL front end | about `0x82367D98..0x823FE0C0` | `cpu-only-keep`; it calls the core directly |
| RenderWare D3D driver (game) | about `0x82600000..0x82640000` | Guest code. Its inline device writes are read at draw time; its texture-header builders (B5) are hook points |
| Lion render backend (game) | about `0x82733000..0x82756000` | Guest code. Per-tile c100 constant through `D3DDeviceEmitPm4Packet_sub_82113010` (B2) needs a native equivalent |
| XDK XAudio | `0x821162E8..0x82127500` | Mixer, effects, engine: `cpu-only-keep`. Render-driver sink: native kernel API. XMA HAL `0x82119748..0x8211A160` (11 functions): dead or replaced, by witness. `sub_82116800`: always replaced (register-0x600 read) |
| EA DECEAXMA | `0x82658350..0x826597E8` | `cpu-only-keep`. Uses only the 16 `XMA*` exports, which get a native front end |
| XMV movie player | `0x824Bxxxx..0x824Cxxxx` (video), `0x824726B0..` (audio) | `cpu-only-keep` decoders; hardware touchpoints native (B1) |
| Kernel `Vd*` imports | `0x828EE1CC..0x828EE48C` | No-ops in native mode (Unleashed `kernel/imports.cpp:794-894`) |

### 3.2 The five bypass classes (the reason HLE-only failed)

| Class | What | Count | Native handling |
|---|---|---|---|
| **B1** inline device-state writes | Constant-shadow stores (`dev+0x700`, `+0x1700`), fetch-constant bits (`dev+0x400`), dirty-mask ORs, in RenderWare and Lion | 72 functions **[static]** | Read shadows and dirty masks at draw time. This covers every B1 access by construction (C5). Midasm hook only for a residual mismatch the tap names |
| **B2** command-buffer writes from game code | Lion c100 via `82113010`; the movie kick `GpuEmitKickWithCallback_sub_820FDE38`; the render-script register block `GpuRingEmitRegisterBlock_sub_820F5570`; the XGraphics state blobs (liveness open) | 4 writers | Explicit native implementations at those call sites |
| **B3** recorded command buffers | `GpuBuildTiledCmdSegments_sub_82304C38` on a second device | 1 recorder | **Dead code** (A7, Appendix C.1). Classify `dead(static)`; no native work |
| **B4** game writes command-buffer payloads | BeginVertices/EndVertices: 16 payload writers (18 sites), 26 EndVertices callers (33 sites); about 455K draws per 200 s roll | | Record-only: native reads the payload at EndVertices (F). Replace mode: BeginVertices returns a native upload buffer |
| **B5** resource headers the core never sees | `D3DSurfaceInitFetchConstant_sub_82616C40`, `RwRasterUploadLevelPixels_sub_8261C5B0`, `sub_8277B0A8` + `TextureFetchConstantRebaseAddr_sub_827B9E68` (textures that alias resolve targets), `D3DTextureCreateWithBackingMemory_sub_820FFF08` | about 12 functions | Hook the builders; register backing memory by physical page (A2) |

### 3.3 Sorting the 216 entry points (A1)

N0 sorts every external entry into one of three groups, recorded in
`data/native/xdk_entry_classes.tsv` (VA, name, group, evidence):

1. **Shadow-only** (keep as guest code, `cpu-only-keep`). Example: `D3DDeviceSetZEnable_sub_820F0B60`
   writes RB_DEPTHCONTROL at `dev+10420` and dirty bits at `dev+16/+24`, and never touches
   the ring. `D3DSetViewportTransform_sub_820F2810` writes the viewport scale/offset at
   `dev+10376..`. Re-implementing these would re-derive logic for no gain (ZEnable is
   forced to 0 when no depth surface is bound; the viewport is clamped to the target).
   Unleashed does the same: its sampler setters point back at the guest XDK functions
   (table at 0x8330F3DC, `video.cpp` ~2137) and it reads the shadow at draw time
   (`video.cpp` ~4307-4321). The 62 per-state setters are expected to land here **[estimate]**;
   each is confirmed by "no ring or cursor access in the decompile".
2. **Ring / hardware emitters** (replace): draws, clears, resolves, swap, kicks, fences,
   `EmitPm4Packet`, BeginVertices/EndVertices.
3. **Resource, lifetime and sync** (replace): create/lock/unlock/release, AddRef, thread
   ownership, fences, queries, state blocks.

The hook generator covers only groups 2 and 3, and drops the non-D3D helpers that fall in
the range: `XMScalarSin_sub_82108D30`, `j_XMemCpy_sub_82468290`,
`ArrayForEachStride_sub_820FA7E8`.

The second device (`D3DDeviceCreate_sub_820F34C0` with flags=2) is created only by the dead
recorder (A7). Native `CreateDevice` needs to serve only `RenderDeviceInit_sub_82621258`.

**Result (N0.1, 2026-10-01) [static]** (`docs/NATIVE_XDK_SPEC.md`):

| Class | Entries |
|---|---|
| cpu-only-keep | 147 |
| emitter-replace | 23 |
| resource-sync-replace | 37 |
| dead | 6 |
| non-D3D helpers dropped | 3 |

The total is 213 rows. Corrections to the text above:

- **SetViewport `820F2810` is an emitter, not shadow-only.** It always re-applies the
  scissor, and the scissor writes PM4 through `820F0208`. SetRenderTarget, and
  SetDepthStencilSurface when RT0 is null, reach the same chain.
- **Per-state setters:** there are 63, not 62. 60 are shadow-only, 2 are dead
  (tessellation) and SetScissorRect is an emitter.
- **Moved to cpu-only-keep:** AddRef, thread ownership (still observed as bracket
  boundaries) and state blocks. Their decompiles show pure field writes or shadow
  snapshot/replay.
- **Also keep:** SetStreamSource and SetIndices, under the same `dev+0x2A1C`/`dev+0x2A20`
  condition as the shader binds and SetTextureFetchConstant.
- **The Release replacement hooks the destroy dispatch `820F3788`,** not the refcount.
- **146 more setters and getters** are reached only through the device's setter tables
  (`dev+56`/`+444`/`+524`), from D3DX effects, RenderWare state blocks and render
  scripts. All are shadow-only except D3DRS_SCISSORTESTENABLE `820F0F70`, which is a
  second scissor emitter (`data/native/xdk_table_setters.tsv`).
- **Verified (2026-10-01, static; spec §7).**
  - A blind IDA re-derivation of 30 entries across all four classes matched all 30.
  - The transitive call closure of every keep row and every table setter reaches the ring
    or MMIO only through another replaced entry (the state blocks).
  - Added ledger L25: B1 code reads back the fetch-constant shadows.
  - Fixed in the spec: the dirty-map bit order. The register index falls as the bit rises.

**The minimal N1 hook set vs UnleashedRecomp (2026-10-04, iter-465) [static].** Unleashed
replaces 42 XDK functions and stubs 15 (`reference/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp`
7798-7882). Our replace set is the 60 `emitter-replace` + `resource-sync-replace` rows above.

| Unleashed hook | Ours | Our class |
|---|---|---|
| DrawPrimitive / DrawIndexedPrimitive / DrawPrimitiveUP | `820FBBF8` / `820FC000` / `820FBBB0` (+ Begin/EndVertices `820FB6E8`/`820FBBA0`) | emitter |
| Clear | `82101998` Clear, `82101A58` ClearF | emitter |
| StretchRect (resolve) | `8210C5F8` Resolve | resource-sync |
| Present | `82112050` Swap, `82112750` BlitAndSwap | resource-sync |
| SetRenderTarget / SetDepthStencilSurface / SetViewport / SetScissorRect | `820F2CA0` / `820F2FD0` / `820F2810` / `820F2720` | emitter (they write PM4 through `820F0208`) |
| SetTexture | `820F2C98` (j_) | emitter |
| SetStreamSource / SetIndices / SetVertexShader / SetPixelShader / SetVertexDeclaration | `820F2A50` / `820F2C08` / `820F4E88`,`820F5218` / `820F5438` | **keep** (shadow-only; native reads the fetch constants and bound objects at draw) |
| Create{Texture, VertexBuffer, IndexBuffer, Surface}, Lock/Unlock, DestructResource | `820FFF08`, `82100028`, `820F3ED8`, `820F4040`, the 8 Lock/Unlock rows, Release via `820F3788` | resource-sync |
| CreateDevice | `820F34C0` | resource-sync |
| Create{Vertex,Pixel}Shader, CreateVertexDeclaration, Get*Desc | `820F5148`, `820F4D90`, `820F5490`, `82100190`... | keep |
| (no Unleashed equivalent) | predication/tiling/Z-pass (8 rows), EmitPm4Packet `82113010` (Lion c100), movie kick `820FDE38`, render-script block `820F5570`, fences/IsBusy/BlockUntil (7), queries (4), InvalidateGpuCache `820F4280`, surface views (4) | emitter / resource-sync |

So the set is Unleashed's (minus the shader/stream binds, which this XDK keeps shadow-only) plus
the families Sonic does not use (predicated tiling, Z-pass, queries, Lion's per-tile packet, the
movie kick). N1 mode A hooks the four draw entries first (`gfx/native_device`, below); the rest
of the 60 join through the existing N0 brackets.

---

## 4. The inlined-site method

The acceptance is dynamic coverage, not the static list: a static scan is what sank HLE.

### 4.1 Brackets (tier 1, kernel-only, no guest rebuild)

- `scripts/gen_native_brackets.py` reads the entry list and writes
  `kernel/native_brackets.gen.cpp`: an observe shim on every external entry (groups 1-3;
  observing costs nothing and shadow-only setters are useful join points).
- A bracket opens only when the caller's LR is outside `0x820F0048..0x821162E8`. Nested
  internal calls belong to the outermost bracket.
- Brackets are keyed by **(device, thread)** (C4). `AcquireThreadOwnership`/
  `ReleaseThreadOwnership` (`820F3618`/`820F3658`) are bracket boundaries, because
  resource Create and Lock calls come from other threads such as the loader.
- Brackets split at `820FCF90` (segment switch) and `820FD8C0` (flush).
- On entry and exit each records `(thread, device, callId, entryVA, guest LR, dev+0x28
  before/after)`.
- Every shim is registered `observe` in `docs/OVERRIDES.json`, then
  `python scripts/gen_override_manifest.py`. Gate `SR_NATIVE_BRACKETS=1`, default OFF.

### 4.2 Taps (tier 2, the `oracle-tap` build)

The codegen census (C3, Appendix C.4) shows a tap on `PPC_STORE_*` alone misses
essentially every VS/PS float-constant write: `stvx` (14,627 sites) and `stvx128` (3,455)
compile to raw `simde_mm_store_si128`, `dcbz`/`dcbzl` to raw `memset`, `stwcx.`/`stdcx.`
to raw `__sync_bool_compare_and_swap`. The XDK's `SetVertexShaderConstantF`/
`SetPixelShaderConstantF` and `GpuEmitType0RegPacketsVec_sub_821078C0` all copy with
`stvx`. Game-side, 11 `stvx` and 5 `stvx128` indexed constant stores would bypass it.

So:

- **The taps go in the regen pipeline or a tracked header**, not a hand edit of
  `guest/ppc/ppc_context.h` (gitignored, `.gitignore:13`). An oracle that must stay alive
  (N8) cannot depend on an untracked file. Either the generator emits a tap macro around
  every store class, or it emits calls to `PPC_STORE_V128`/`PPC_DCBZ`/`PPC_STWCX` macros
  defined in a tracked `kernel/` header.
- **Store classes covered:** all scalar stores (already through `PPC_STORE_*`), `stvx`,
  `stvx128`, `dcbz`, `dcbzl`, `stwcx.`, `stdcx.`. Loads for the load tap: `lvx`/`lvx128`
  plus scalar loads.
- **Budget a full guest regeneration and rebuild**, not "one guest rebuild" (the regen
  pipeline is in `tools/regen/README.md`; a full regen reverts hand fixes, so the tap
  change must live in the generator, not in `guest/ppc`).
- Three taps, all behind `g_storeWatchOn`-style runtime switches:
  1. **Command-memory store tap:** a page bitmap (16 KB over 512 MB physical) armed on the
     64 KB primary ring and every segment `820FCF90` hands out. For each written dword it
     keeps a stamp `{writer site id (FILE:LINE interned), innermost bracket callId, ctx.lr}`.
  2. **Device-struct store and load tap:** `dev+0x00..0x1F` (dirty masks), `+0x400`
     (fetch), `+0x700`/`+0x1700` (constants), and the XDK-maintained fields (fences
     `+0x2A1C`/`+0x2A10`, cursor `+0x28`, frame counter `+15124`, resource-header fence
     fields; §7 N7). A store from outside the core is an inline site; a load from outside
     the core names a field native must keep current.
  3. **MMIO counter** over 0x7FC00000-0x7FFFFFFF, by function.
- Host copies into command memory would bypass the taps. Today none do: the memcpy
  overrides still delegate to guest code (`sub_82468290` is a null-guard band-aid,
  `sub_8279EC60` is observe-only) (F). Any new host copy gets an explicit tap call; the
  audit rule stands.

### 4.3 Coverage, semantic check and side effects

- **CP side.** `gfx/pm4_coverage.{hpp,cpp}` is called from `Pm4Engine::execute` for each
  packet, including inner INDIRECT_BUFFER packets. It looks up the bracket or stamp for
  the packet's range and classifies it: ACCOUNTED(callId), UNBRACKETED(writer VA, LR),
  UNSTAMPED (source unknown: itself a discovery), or ACCOUNTED-NOT-IMPLEMENTED (the entry
  has no native implementation yet).
- **Report.** `tools/pm4_coverage_report.py` groups unaccounted packets by writer function
  (`source_to_va` + `data/ida_names.tsv`), opcode and LR, into
  `archive/native_cov/<tag>.{json,tsv}`. Expected first hits: the Lion c100 packet, the
  render-script register block, the movie kick.
- **Obligations.** `data/native/pm4_obligations.tsv` maps each opcode and register to
  `must-represent` / `side-effect` / `no-op`. Only the first two count against coverage.
- **Join.** Each PM4 draw sits inside exactly one bracket. Under predicated tiling the CP
  replays a draw once per tile, so **one bracket joins to N draws** (D5). The join key is
  (bracket, tile index).
- **Semantic check.** `gfx/native_projection.hpp` projects native state onto Xenos
  registers (ALU constants, fetch slot s → 0x4800+6s, RB/PA state, RT and depth info,
  viewport/scissor, ucode hash, bool/loop constants, bin mask/select). Only registers the
  draw's shaders reference are compared (`gfx/xenos_ucode.cpp` gains a constant-use
  walk). A mismatching field with no native setter identifies an inline writer, and tap 2
  names it.
- **Side effects.** Everything the CP writes that the CPU can see (fence and scratch
  writebacks, EVENT_WRITE_SHD into 0x7F000000, INTERRUPTs with their CPU mask, swaps) is
  logged with its bracket owner. The native fence/callback/swap model must predict each.

### 4.4 Shader keys (A5, decided)

ENGINE_MAP (~line 930) records that the XDK patches the vertex-fetch instructions of every
vertex shader at bind time (`ShaderPatchVertexFetches_sub_821081C0`), so the DXIL cache is
keyed on the patched bytes (0 of 54 vertex shaders match their containers).

**Decision: option (a).** The XDK patcher stays guest code (`cpu-only-keep`). Native takes
the ucode from the same patched bytes the CP would load, so native and oracle share cache
keys and the native-vs-emulation comparison is shader-identical. The `.vs.vsf`
shared-memory vertex-fetch variant is still an emulation artefact; native binds an
input-layout variant **derived from the patched ucode's fetch instructions**, keyed by the
same ucode hash. Harvesting from PM4 runs into `data/shader_cache/dxil` therefore stays
valid. Option (b) (unpatched ucode + declaration) is rejected: it forks the key space and
buys nothing.

### 4.5 Invalidation of CPU-written resources (A3)

The ENGINE_MAP §6g hard rule applies natively: the planar branch of
`MovieVideoDecodeYuvToRgbLoop_sub_82480D80` memcpys straight into sampled textures without
a Lock. Native uses the guest's own boundaries first:

- Unlock;
- `GpuInvalidateMemoryRange_sub_820F4280` (the RenderWare driver calls it from 82613318,
  8261A028, 8261B668);
- completion of XGraphics tiling (`TextureTileSurfaceLevel_sub_827B8A90` return).

It hashes only where no boundary exists, and N0 measures where that is (the store tap
reports writes into registered backing memory with no following boundary). This is
native cost, so the texture re-hash work is **shared**, not frozen.

### 4.6 What the inline list must prove (C5)

"Every static inline site exercised or proven dead" cannot be met (no tape plays the whole
game, and a zero count proves nothing) and mostly does not need to be. The census shows
every inline access from game code is one of: an OR into a dirty mask, an indexed
constant store, or a sampler-bit write to fetch-constant word 0. Native reads all of these
at draw time by construction. Witnesses are required only for sites that touch anything
outside those shadow ranges; imported Xenia saves at chapter points
(`tools/import_xenia_saves.py`) reach later content.

---

## 5. Entry gate: correctness first (C-gate)

Native conversion (N1 onward) starts when the C-gate holds. Each item is judged against
live Xenia or its dumps.

| Area | Exit condition | State (2026-10-01) |
|---|---|---|
| **Audio** | Movies and EA PCM audible through the render-driver sink; sound flag natural (no `cfgSoundOffThread`); the XMA native front end passes parity on captured kicks. **HAL liveness (C2) is measured in Xenia before the audio design is frozen (E3).** If the HAL is live, correct audio needs the N6 HAL replacement, not the register front end | KPCR and spinlocks landed (bb96dbb). `XAudioSubmitRenderDriverFrame` is still a generic stub (`kernel/stubs.cpp:6490`, whose "not imported" comment is wrong: it is imported at 0x828EEDAC and called by `sub_8211B680`), so every mixed frame is discarded |
| **Freeze** | 3 sequential long-tape rolls (26 min) plus the full tutorial tape with no freeze and no `[GPU_HANG]`, `IN-IMAGE GRANT` or `HEAPCHUNK_GUARD` | KTHREAD (60a90bf), wait fidelity (1bc1962), phys floor/reuse (04bce60), heap-align (89f4afd) landed; clean confirmation roll pending |
| **Camera** | Idle FollowCamera roll is 0 as in Xenia; tilt and zoom explained or fixed | Candidates: mftb ~74x fast (raw `__rdtsc`); host MXCSR not restored in `callGuestAPCSafe` |
| **Collision** | The reported buildings collide in our build and Xenia, same save and route | Investigation open |
| **Usability (E5)** | A tape roll reaches its era within its timeout; a human can play-test the tutorial | Platform costs (VirtualQuery ~9.5 ms/call, critical-section yields, the time base) are correctness-gate usability work, not deferred to native (Appendix C.5) |

Rules for C-gate work:

- **Emulation-path hang fixes (E4).** The "no GPU hang" criterion needs fixes in the
  emulation path (the intermittent CP stall at swap, RENDERER_PM4_HYBRID §9). They are
  justified because the oracle must survive whole-run shadow runs. Classify them `oracle`
  in OVERRIDES.json so they are never counted as native progress.
- The C-gate does not require PM4-path visual parity (Xenia is the visual oracle) or
  emulation-path speed beyond usability.

---

## 6. Verification modes

Mixed ownership inside one frame (the draft's mode C) is **dropped** (D2). It needed native
to draw into the emulation path's EDRAM targets with copies at every handover: interop
thrown away at N7, i.e. exactly the emulation-path investment the user excluded. It also
could not move any side effect the guest sees (fences, callbacks, BeginVertices pointers,
busy flags, query data), so the per-family flips moved only pixels.

| Mode | What runs | What it proves | Cost |
|---|---|---|---|
| **A: record-only** (`SR_NATIVE_RECORD=1`) | Native hooks record the API stream (`.nrt`) and bracket stamps; **no native rendering** during the live run. XDK and CP run and present as today | Structural and semantic coverage over a whole live roll; every era; the `.nrt` half of mode B | Small: no doubled GPU work (removes the draft's R7). The native side reads dirty masks without clearing them |
| **B: offline replay** (deterministic, seconds) | For a swap k captured in mode A: the `.xtr` (`SR_PM4_TRACE_SWAP=k`) and the `.nrt` of the same swap. Offline: **X** = Xenia's `xenia-gpu-d3d12-trace-dump.exe` on our `.xtr`; **P** = our PM4 replay (`tools/replay_ab.ps1`); **N** = native replay of the `.nrt` | Per draw (L1), per resolve (L2), per frame (L3) proof anyone can re-run | Needs paired captures (D1) |
| **A': native presents** (`SR_NATIVE_PRESENT=1`) | Native renders and **presents the whole frame**. XDK and CP still run, unpresented, for side effects and as the oracle | Live play-testing of native pixels; whole-run contact sheets; era checks | About 2x GPU work; acceptable for a verification build |
| **N7: replace** (`SR_NATIVE=1`) | Native owns side effects too; XDK internals never run | The §1 acceptance | Shipping |

### 6.1 Paired fixtures (D1)

Native output cannot be compared with Xenia on Xenia's own captures: the menu, the four
FMV traces and gp1/gp2 are Xenia captures (`tools/pm4_replay_score.py` TRACES/EXTRA), and a
native renderer needs an API-level `.nrt` that does not exist for them. Only the lg1/lg2
kind works: our build records `.nrt` and `.xtr` on the same swap, and Xenia's trace dump
renders our `.xtr` as X.

- Recapture the menu (PRESS START, LOAD dialog, options, pause, HUD), the FMVs and
  gameplay as **paired sets** in mode A, under `data/native_traces/`.
- The N2, N3 and N5 exits are stated on these paired sets. Xenia's own captures remain
  the P-vs-X health check of the emulation path.
- The golden traces were taken on the NO SOUND ISO with audio disabled
  (`docs/oracle/README.md`). Live-Xenia comparisons (era match, pacing, audio) use the
  full ISO and the user's Xenia build with its XMA decoder. Update `docs/oracle/README.md`
  when the recapture lands (F).

### 6.2 The ladder per replacement

- **L0 conformance units** (`tests/xenos_conformance/`, Xenia-ported reference functions,
  BSD-3 with attribution; golden vectors in `data/conformance/`). Cases: formats (7e3,
  20e4, 8_8_8_8_GAMMA PWL, signed 16_16, 16_16_16_16_FLOAT, resolve exponent bias);
  resolve (conversion, 8-px alignment, clear-on-resolve 0x100/0x200, depth flags 0x14,
  MSAA 2x/4x, slice/cube, endian, destination tiling); textures (32x32 untiling, packed
  mips, fetch-constant decode, cube order, CPU-rewrite invalidation); shaders (vfetch +
  patched stride, DX9 0·x = 0, rcp/rsq clamps, predication, kill/alpha test, half-pixel
  and VTE, rect/quad lists); render state (constant blend, per-RT mask, two-sided stencil,
  depth bias in 24-bit units vs float depth, alpha-to-mask); clears (4x depth-only quad at
  z = −0.0); bins and predication; EDRAM aliasing (only measured cases); gamma; swap and
  scaler. API-level cases run the recompiled XDK under `SR_CONFORMANCE=<suite>`
  (registered `diagnostic`, never a game run).
- **L1 per draw, exact.** `tools/native_draw_diff.py` diffs the `.nrt` per-draw JSON
  (schema of `tools/xtr_draws.py --json --list --consts --textures`) against the `.xtr`.
  Values the CP read later that differ from native call-time values are **LATE-MUTATION**
  (the §6g hazard): native must reproduce the execute-time value, usually via §4.5
  invalidation.
- **L2 per resolve.** About 13 resolves per frame (26 in the menu frame) **[measured,
  ENGINE_MAP]**. The first mismatching resolve narrows the fault; then bisect by draw count
  (`SR_NATIVE_MAXDRAW` on both sides).
- **L3 per frame, three-way.** `tools/native_ab.py` wraps `pm4_replay_score.py` (MAD +
  distinct colours + non-black coverage; a black frame scores well against a dark
  target). Acceptance **N−X ≤ P−X + ε**. Where P and X disagree, X wins and the P defect
  is logged, not inherited.
- **L4 whole run.** Mode A' roll through every era (`tools/record_fullrun.ps1` +
  `tools/verify_run.py`), then era comparison against live Xenia (D4, below).

### 6.3 The guest-visible output ledger (D3)

To shrink the N7 big bang, mode A records everything the XDK hands back to guest code:

- return values and out-parameters: `IsBusy`, `QueryGetData`, `IsFencePending`,
  `BlockUntilNotBusy` outcomes, the BeginVertices pointer (as an offset class, not a VA);
- device fields the guest reads (tap 2's load list);
- resource-header fence fields;
- callback timing (movie kick callback, fence completion) relative to present ordinals.

A replace-mode run's API call stream is then diffed against the shadow stream on the same
tape: a changed guest branch shows up as "the guest called something different" before any
pixel is judged. `tools/native_ledger_diff.py`.

### 6.4 Comparing with live Xenia (D4)

Our tapes are indexed per `XamInputGetState` poll (`tools/gen_input_tape.py`); Xenia is
driven by wall-clock pad tokens (`tools/xenia_pad.py`). "Same input tape" cannot run as
written. Two options, in order of preference:

1. **Scripted scenario with era checkpoints** (default): the same `sr_drive` scenario on
   both sides, compared at named checkpoints (era reached, shot, pacing numbers of §1
   item 9), not frame identity. Available now.
2. **Tape-replay input driver in `reference/xenia-canary-oracle`** (poll-indexed, like
   ours): only if checkpoint comparison proves too coarse for a defect.

### 6.5 Audio proof

- In-process dual decode per kick: native core vs the oracle core (Xenia
  `xma_context_new.cc` port behind the register front end) on the same context and input
  snapshot; `tools/xma_parity.py`.
- Render-driver frame stream: native sink dump (`SR_AUDIO_FRAME_DUMP`) vs an instrumented
  `xenia-canary-oracle`, aligned by cross-correlation on deterministic segments,
  `[AUDIO_RMS]`.

---

## 7. Phases

Order (E1, E2): **N0 → N1 → N4a → N3 → N2 → N4b → N5 → N7**, with **N6 in parallel** once
the HAL liveness question (C2) is answered, and **N8** (oracle kept alive) as a standing
rule. Numbering is kept from the draft so references stay stable. Every native change
sits behind an `SR_NATIVE_*` gate; the PM4 path stays the default draw path until N7.

Why this order: presenting is itself a resolve plus VdSwap, so nothing native is visible
until the target/resolve/present core exists (E1). Movies come before UI because they are
the smallest closed family (one draw, three single-channel planes, one shader pair) yet
exercise CPU-written textures, completion callbacks and present, and their bit-exact rung
calibrates the ε the UI exit uses (E2).

### N0 - Instrumentation, decision gates, open questions (observe-only; overlaps the C-gate)

Offline or observe-only, so it uses slack in the game slot during correctness work (add
the gates to rolls that already occupy the slot and cover the era; record the gate on the
roll).

**Deliverables**

0. **Speed decision gate (E5), first.** The weak sample (Appendix C.5) already says
   non-XDK work is at least half the render thread's frame. Firm it up: one roll with
   logging off and an observe-only per-frame timer bracketing the XDK D3D range and the
   fence-wait functions on the render thread (`[NATIVE_FRAMESPLIT]`). Outcome recorded in
   ENGINE_MAP. Consequence either way: platform costs are C-gate usability work, and no
   document claims "native makes it fast" without this number.
1. **Entry sort** (A1): `data/native/xdk_entry_classes.tsv`, three groups, with the
   non-D3D helpers dropped (§3.3). Offline, IDA. **Done 2026-10-01:** 213 rows plus the
   146-row table-only addendum; contracts and ledger in `docs/NATIVE_XDK_SPEC.md`; its §6
   lists the new open questions (Q-N1..Q-N7).
2. **True per-packet counter** and `gfx/pm4_coverage.{hpp,cpp}` (§4.3). **Built 2026-10-01
   (iter-464), gate `SR_PM4_COVERAGE=1`.** Exact: on every replay fixture the count equals the
   trace's packet-record count (menu 63,054). Per frame on the fixtures: menu 63,054, gameplay
   29,672-69,238, FMV 1,955-2,682 (table in ENGINE_MAP §6d "NATIVE N0").
3. **Brackets** keyed by (device, thread) (§4.1), gate `SR_NATIVE_BRACKETS`. **Built
   2026-10-01:** `scripts/gen_native_brackets.py` -> `kernel/native_brackets.gen.cpp` (329
   shims) + `SR_NATIVE_SCOPE` in the 38 existing overrides; runtime `kernel/native_brackets.cpp`.
   **Fixed 2026-10-02 (d9c125d):** the cursor `dev+0x28` points at the LAST dword written (IDA:
   `820FCF90` sets it to segment - 4), so a piece is `(before, after]`, not `[before, after)`;
   the old interval claimed the previous piece's last dword and dropped its own last dword.
4. **Taps in the regen pipeline** covering vector stores, `dcbz`/`dcbzl` and `stwcx.`
   (§4.2, C3); preset `oracle-tap`; full regen budgeted. **Not built (needs a full regen);
   the implementation plan is below this list.**
5. **Coverage report** `tools/pm4_coverage_report.py` and `data/native/pm4_obligations.tsv`.
   **Built 2026-10-01.**
6. **Liveness witnesses, measured in Xenia** (C2), because our build cannot witness audio
   (the mixer is never created while `cfgSoundOffThread` runs) and a zero in our build
   means nothing. Re-apply the `tools/xenia-trace` JIT-prolog entry-count patch to
   `reference/xenia-canary-oracle` (it was built against the older `reference/xenia-canary`)
   and count entries on the full ISO, with imported saves at chapter points:

   | Function | Question | Answer implies |
   |---|---|---|
   | `Xma_CreateContexts_sub_82119980`, `XmaHw_WritebackShadowAndKick_sub_82119E68`, `XmaSourceVoiceCtor_sub_821236E0` | Is the XAudio XMA HAL live? (C-gate input, E3) | 0: HAL is dead; only `sub_82116800` needs replacing. >0: replace the 11 HAL functions (N6) |
   | `D3DDeviceEmitDrawTessellated_sub_820FC478`, `RenderScriptDrawTessellatedFullscreenQuad_sub_82786B60` | Is hardware tessellation live? (B4) | 0: classify dead. >0: port tessellation to the emulation path first, then N4b |
   | `sub_82845780/958/B38` | Are the XGraphics state blobs live? | 0: dead. >0: B2 native work |
   | `D3DDeviceBeginZPass_sub_8210DF38` / `EndZPass 8210E1A0` | Is Z-pass live? (B5) | Scope of N5 predication work |
   | `D3DDeviceSetPredication_sub_8210D400`, BeginTiling/EndTiling | Does gameplay use predicated tiling? | Scope of N5 bin work |

   A cheaper static answer for tessellation: decompress the render scripts from the AST
   archives and grep for `rendertquad` (opcode 0x13). If no script uses it, B4 is dead
   without a run.
   **Answered for boot -> early Metropolis gameplay (2026-10-01, §11.1, two oracle-trace
   runs):** XMA HAL 0 entries (dead), tessellation 0 (dead), XGraphics state blobs 0 (dead),
   Z-pass live (11,189 / 8,943 Begin/End pairs), SetPredication live. Open: later chapters
   (imported saves) and the predicated-TILING half of Q4.
7. **Load-tap field list and aliasing measurement:** device fields the game reads (for N7);
   CPU reads of resolve destinations (which resolves need a guest-memory copy natively);
   whole-run `tilesMoved` and the reinterpretations by EDRAM base (0 / 0x2D0 / 0x5A0 /
   0x658).
8. **Census items (B5):** memexport use in any harvested shader (ucode scan of
   `data/shader_cache`); Xenos PWL gamma vs sRGB (DXGI `_SRGB` is not equivalent; port
   Xenia's conversion into the conformance suite). **Done 2026-10-01 (iter-464):**
   memexport: **0** in 259 valid unique programs from data/ (352 with the archive
   corpora; `tools/native_census_memexport.py`), Xenia's own disassembly agrees; 75 of the
   425 cached DXIL hashes have no kept ucode (open until a `SR_DUMP_SHADER_CONTAINERS=1`
   whole run). PWL vs sRGB (`tools/native_pwl_vs_srgb.py`): decode up to 0.036 linear
   (9.3 UNORM8 steps), encode up to 19 codes, 86.5% of linear values store a different
   code: `_SRGB` is rejected; native ports Xenia's PWL (shader-side, as Xenia does). The
   game selects gamma nowhere we can see (NATIVE_XDK_SPEC §6.1 Q-N1; 0 gamma draws in
   4,000 traced), so the gamma conformance cases have no live witness yet.
9. **Conformance suite skeleton** (§6.2 L0), formats and resolve first. **Done 2026-10-01
   (iter-464):** `tests/xenos_conformance` (target `xenos_conformance`, not in ALL;
   `--verify`), Xenia-ported references (BSD-3), 12 golden files in `data/conformance/`.
   First run: PASS 53, FAIL 2, GAP 2, WAIVED 1. The FAILs are P defects in our PM4/oracle
   path, logged not fixed here: the D24FS8 depth pack rounds where Xenia's default
   truncates (`gfx/edram.hpp:637`, 199,926/456,983 sampled depths one 20e4 step off); the
   k_16_16(_16_16) pack truncates where Xenia's ROV path rounds half away from zero
   (`edram.hpp:551`). GAPs: k_8_8_8_8_GAMMA stored as plain 8888; resolve destination
   formats 8, 9, 14, 50, 54, 55, 56 skipped. Also found: `reference/xenia-canary`
   @0b4e6c44 `Float7e3To32` is wrong on 1,009/1,024 codes (fixed upstream f3e42609, present
   in `reference/xenia-canary-oracle`) - port 7e3 from the oracle tree.
10. **IDA naming** of the 216 entries and the 72 inline writersand the bypass class in a comment.

#### N0.4 implementation plan (written 2026-10-01, iter-464; NOT built - it needs a full regen)

The taps must live in the generator, because `guest/ppc` is generated and gitignored and a
full regen reverts hand edits (`tools/regen/README.md`). The plan, in order:

1. **One tracked header, zero cost by default.** `kernel/ppc_taps.h` defines
   `PPC_TAP_STORE(ea, bytes, site)`, `PPC_TAP_LOAD(ea, bytes, site)` and
   `PPC_TAP_RESERVE(ea, bytes, site)`. Without `SR_ORACLE_TAP` they expand to `((void)0)`,
   so the default build's object code is unchanged (proof: the five replay MD5s and a
   byte compare of one guest TU's `.obj` built with and without the header). With
   `-DSR_ORACLE_TAP=1` they call `sr::tap::store/load(ea, bytes, site)` behind a runtime
   `g_tapOn` switch (the `g_storeWatchOn` pattern).
2. **Scalar stores and loads.** `PPC_STORE_U8/16/32/64` and `PPC_LOAD_*` are already macros
   in the hand file `tools/regen/hand/ppc_context.h` (copied to `guest/ppc` by
   `assemble.py`). Add the tap call inside them under `#ifdef SR_ORACLE_TAP`; `site` =
   `__LINE__` plus a per-TU id (`SR_TAP_TU`, a `-D` per generated file added by
   `guest/CMakeLists.txt`), symbolised later through `tools/va_to_source`.
3. **The raw store classes (C3).** `assemble.py` step 2 (`wrap_line`) already rewrites every
   raw `base + X` access to `base + PPC_GUEST_OFFSET(X)` (~43K sites). Add step 2b next to
   it: when the instruction comment on the previous line is `// stvx`, `// stvx128`,
   `// stvewx`, `// stvlx`, `// stvrx` (+`128`), `// dcbz`, `// dcbzl`, `// stwcx.`,
   `// stdcx.`, emit `PPC_TAP_STORE(<the same X>, <16 | 32 | 128 | 4 | 8>, <site>);`
   on its own line BEFORE the store, leaving the store itself byte-identical. Loads for
   the load tap: `// lvx`, `// lvx128`, `// lvlx`, `// lvrx`, `// lwarx`, `// ldarx`.
   The census of C.4 (14,627 `stvx`, 3,455 `stvx128`, the `dcbz`/`stwcx.` sites) is the
   expected site count; `assemble.py` prints the per-class count and fails if it drops.
4. **Spliced bodies.** The 288 spliced function bodies (`splice_bodies.json`) bypass step 2b
   because they are pasted verbatim. Run the same rewrite over each spliced body at splice
   time (it is a pure text transform on instruction-comment + store-line pairs), and make
   `tools/regen/check_splice_sync.py` count the taps per spliced body so a body with a raw
   store and no tap fails the check.
5. **Host copies.** Every host-side copy into guest memory (none into command memory today,
   R11) calls `PPC_TAP_STORE` explicitly; `scripts/audit_forcing.py` gains a rule that a
   new `memcpy` into a `guestVAToHost` pointer in kernel/ or gfx/ without a tap is flagged.
6. **Preset.** `CMakePresets.json` gets `oracle-tap` (inherits `default`, adds
   `-DSR_ORACLE_TAP=1` to the guest library and kernel, build dir `build-tap`). The taps
   in `kernel/native_taps.cpp` implement roadmap §4.2: (1) the command-memory page bitmap
   armed on the 64 KB ring and every segment `820FCF90` hands out (the bracket runtime
   `kernel/native_brackets.cpp` already knows both), stamping each written dword with
   `{site, innermost bracket callId, ctx.lr}`; (2) the device-struct store/load tap
   (`dev+0x00..0x1F`, `+0x400`, `+0x700`, `+0x1700`, `+0x2A1C`, `+0x2A10`, `+0x28`,
   `+15124`, resource fence fields); (3) the MMIO counter over 0x7FC00000-0x7FFFFFFF.
   `gfx/pm4_coverage` then prefers the store stamp over the bracket interval, which turns
   UNSTAMPED into UNBRACKETED(writer site, LR).
7. **Budget and order.** One full regen (XenonRecomp + assemble + the three post-regen
   patch scripts + `check_splice_sync.py`), one full guest rebuild in `build-tap`
   (135 guest TUs, ~8+ min at -j 8 through `tools/build_slot.py`), then the default build
   to prove zero change (replay MD5s; `audit_forcing.py`; `gen_override_manifest.py`).
   Do it in a lane that owns `tools/regen` for the duration (splice burndown lanes edit
   `splice_bodies.json`).

**Exit**

- The speed gate has a measured number, recorded.
- Every liveness question has a measured answer (or a static proof of deadness), recorded
  in ENGINE_MAP.
- The coverage report runs on a whole-run live PM4 roll, and the static inline list has
  an "exercised %" with every dynamic hit missing from it added.
- The oracle-tap build gives a baseline MMIO count by function.

### N1 - Native device core, record-only (mode A), no visible output

Seed: `gfx/hle_video`, which already follows Unleashed's queue/dispatch/present shape. Do
not write a third renderer.

**Deliverables**

1. `gfx/native_device.{hpp,cpp}`: a draw-time state tracker that reads the guest device
   shadow and dirty masks (dirty `+0x00..+0x18`, fetch `+0x400`, VS `+0x700`, PS
   `+0x1700`) **without clearing** them.
2. Generated hooks on **groups 2 and 3 only** (§3.3), each calling the native recorder then
   `__imp__`. Shadow-only setters are not hooked.
3. **Resource registry** keyed by guest header VA, plus a **backing-memory registry keyed
   by physical page** (A2) that resolves aliasing: textures built over existing memory
   (`CreateWithBackingMemory`, the B5 builders, `827B9E68` rebases onto resolve targets).
   Content-hash identity shared with `gfx/hle_video` (the future mod key).
4. **Shader binding** per §4.4: ucode from the patched bytes, keyed as the oracle keys it.
5. BeginVertices/EndVertices: read the payload at EndVertices (F).
6. `gfx/native_projection.hpp` and the constant-use walk (§4.3).
7. `.nrt` writer (native command log, content-hashed resource bytes, the (bracket, tile)
   join table), paired with `SR_PM4_TRACE_SWAP=k` into `data/native_traces/`.
8. The guest-visible output ledger (§6.3).
9. `tools/native_draw_diff.py`, `tools/native_ab.py`, `tools/native_ledger_diff.py`.

**Exit**

- A record-only roll through every era leaves guest behaviour unchanged: the
  `SR_NATIVE_RECORD=0/1` A/B on a tape shows the same era sequence and the same
  `[APT_REQ]` MD5.
- Every PM4 draw in a whole run joins to exactly one (bracket, tile).
- The per-draw semantic report runs offline in mode B on the first paired captures; its
  mismatch list is the work list for N4a onward.

### N4a - Render targets, resolve and present (the visible core)

**Scope**

- Native render targets in the game's formats: the 7e3 `k_2_10_10_10_FLOAT` HDR main
  target (SetRenderTarget rewrites format 3 to 12), 20e4 `k_24_8_FLOAT` depth, signed
  16_16, `k_32_FLOAT`, 16_16_16_16_FLOAT.
- Resolves as native copies or format conversions: clear-on-resolve 0x100/0x200, depth
  resolves (flags 0x14), slice and cube-face destinations, MSAA 2x/4x averaging (the
  640x640 `k_16_16` 4x target).
- Present: the front-buffer resolve, the display gamma ramp (`GpuSetGammaRamp_sub_82111CB8`,
  `gfx/gamma_ramp.hpp`) with Xenos PWL gamma (not sRGB), the scaler's output size.
- Mode A' infrastructure: `SR_NATIVE_PRESENT=1` presents the native frame while the XDK/CP
  run unpresented.
- A guest-memory copy of a resolve only where N0's load tap showed the CPU reads it.

**Exit**

- Every resolve on the paired menu and gameplay fixtures passes L2.
- The conformance formats, resolve and MSAA cases pass.
- Every aliasing case N0 measured has a test.
- Bake nothing yet (no family draws natively); record the A' infrastructure.

### N3 - Movies native

**Scope**

- The `fmvmovie` pass and the render-script movie producer (B1): WMV3 planes the CPU
  rewrites every frame, invalidated at guest boundaries (§4.5; frame-ring contract
  ENGINE_MAP §6g.1/§6g.2).
- **The movie present contract (A4), specified and ledgered here, owned natively at N7.**
  `MoviePlayerPresentFrame_sub_82473208`:
  - keeps a 3-deep in-flight counter at player+368 and returns 0x8000000A at 3;
  - locks and unlocks the three plane textures;
  - calls `GpuEmitKickWithCallback(dev, 1<<(24+cpu), MoviePresentSwapCallbackThunk_sub_82471890,
    player)`. The callback runs from the ISR's source-1 path in
    `GpuInterruptCallback_sub_820FDBA0`, at raised IRQL, on the CPU that issued the kick.

  The native version must signal only after the host GPU has finished with the planes
  (early risks overwritten planes; late stalls the movie) and run the callback on a KPCR
  thread at raised IRQL. While the XDK still issues this kick (modes A and A'), native
  cannot own it; N3 records its timing in the ledger and checks the open ~25% slow movie
  pace against it.
- Movie audio needs only the render-driver sink (C-gate).

**Exit**

- The paired FMV fixtures are **bit-exact N = X** (as P = X is today on the Xenia
  captures). This calibrates ε.
- The intro era of a mode-A' whole-run roll is viewed (mandatory since the 2026-09-29
  regression).
- Movie pace compared with Xenia's 29.8 fps; recorded either way.
- Bake `SR_NATIVE_MOVIE=1` (A' pixels for the movie family).

### N2 - UI, Apt front end and menus

**Scope**

- Apt/RenderWare 2D draws; the glyph path (`RwTextRenderer_DrawGlyphQuads`, B4 payloads
  read at EndVertices).
- Clear and ClearF, including the 4x depth-only quad at z = −0.0.
- Scissor/viewport with the half-pixel offset and VTE screen-space transform.
- ~~Per-target gamma enable (`820F1718`)~~. Correction (N0.1): `820F1718` is
  D3DRS_HIGHPRECISIONBLENDENABLE. It toggles formats 2↔10 and 3↔12 and never selects
  `k_8_8_8_8_GAMMA`. Where the gamma format is selected is open (NATIVE_XDK_SPEC Q-N1).

**Exit**

- UI rows 100% covered with 0 semantic mismatches in a whole-run record-only roll.
- Every paired UI fixture scores N−X ≤ P−X + ε; the dialog body (P defect at draw #3236)
  renders as in X.
- Mode-A' front-end and HUD frames viewed on contact sheets for every era; one blind
  Astra call (visual milestone).
- Bake `SR_NATIVE_UI=1`.

### N4b - Post-processing, aliasing and tessellation

**Scope**

- Render-script textures aliasing resolve targets (`sub_8277B0A8` +
  `TextureFetchConstantRebaseAddr_sub_827B9E68`) through the physical-page registry.
- `RenderScriptRunTechnique_sub_8277B998`'s raw register block
  (`GpuRingEmitRegisterBlock_sub_820F5570`, B2), expressed natively as the state and clear
  it encodes.
- The fog pass (samples a colour resolve and a raw depth resolve).
- Aliasing: only the cases N0 measured.
- **Tessellation (B4), only if N0 found it live.** Port Xenia's implementation
  (`d3d12/pipeline_cache.cc`, TessellationMode, host hull/domain shaders) to the
  emulation path **first**, so the oracle is right before native is judged, then native.
  The XDK draw is quad list (prim 13), mode via `sub_820F1A48` (dev+10488), max level 12.0
  via `sub_820F1A10` (dev+10492).

**Exit**

- L2 on every resolve of the paired gameplay fixtures; aliasing and (if live)
  tessellation conformance cases pass.
- Bake `SR_NATIVE_POST=1`.

### N5 - World geometry and the remaining inline sites

**Scope** (the largest phase **[estimate]**)

- **B1** covered by draw-time shadow reads (N1). Residual mismatches the device tap
  attributes to a site get a midasm hook there (Unleashed has 191 in `SWA.toml`; normal).
- **B2** Lion per-tile c100 (`LionRenderBackend_SetPerTileConstC100_sub_827333E0` via
  `82113010` under SetPredication): a native per-tile constant table applied to the
  tile's draws, per N0's bin/predication answer (render per tile region, or fold the
  constant).
- **B3: nothing** (A7: dead). Classify the recorder and its callees `dead(static)`.
- **B5** texture headers and tiling hooks for volume, cube and array textures (the 64³
  light volume, the `k_16` 640x640x4 shadow array).
- Instanced building draws (self-indexed `k_32` instance index); two-sided stencil; depth
  bias; alpha-to-mask.
- Predication and queries: `821028B8`/`82102BC8`/`82102DC8`, Z-pass (per N0), conditional
  rendering `82102010..82102608`. **Occlusion queries need a "not ready yet" (S_FALSE)
  latency model** (B3). `QueryGetData` is reached through the wrapper `sub_82779A48`,
  which has two game callers: `OpticalFX_DrawOcclusionFadedFlare_sub_82721118` and
  `HudGlowOcclusionQuery_sub_82304058` (correction, N0.1). Check flare and HUD-glow
  visibility against Xenia.

**Exit**

- 100% `must-represent` coverage, 0 semantic and 0 side-effect mismatches across
  whole-run record-only rolls of the long tape, the full tutorial tape and imported-save
  chapter points (meteor, arena).
- Witnesses only for inline sites outside the shadow ranges (C5).
- N−X ≤ P−X + ε on the paired gameplay fixtures.
- Bake `SR_NATIVE_WORLD=1`. Every family is now native in A'.

### N6 - Audio hardware residue (parallel, after the C2 answer)

- **Always:** replace `sub_82116800`'s register-0x600 `lwbrx` with a native function
  (store the native value into `dword_829AC0E8`, tail-call `0x82117018`). It runs every
  boot, and the no-access MMIO mapping (§1 item 3) depends on it.
- **HAL dead:** classify the 11 HAL functions `dead(witness=0)`.
- **HAL live:** replace them at function level on the shared decoder core, indexed by
  hwIdx, keeping the 96-byte shadow layout; write the shadow contract into ENGINE_MAP §7
  first. The HAL shares the kernel's 320-context pool with EA (it calls the
  `XMACreateContext` export and also writes XMA registers).
- The `XMA*` exports' native front end (built in the C-gate audio arc) becomes the only
  shipping front end; `SR_XMA_MMIO` is oracle-only.

**Exit:** audio MMIO count 0 in a whole run; core-vs-oracle parity 0 bytes per kick;
`[AUDIO_RMS]` within tolerance against live Xenia.

**Status (iter-467 sound lane; `SR_XMA_ARRAY_NATIVE` and `SR_XMA_NATIVE_FRONT` default ON, `=0` keeps the A/B):**
- Counter: `[AUDIO_NATIVE]` (kernel/audio_native_census.cpp, every 30 s, kept in the player log)
  counts entries into the six B.1 functions that touch the XMA register block
  (`guest_mmio_fns`, one field per function), the five register-free HAL functions of B.2
  (`xma_hal_rest`), the HAL voice ctor (`xma_hal_voices`) and the XMA device's register-file
  writes (`xma_regfile_writes`); its last line is the whole-run value.
- `SR_XMA_ARRAY_NATIVE` ("Always" above): `sub_82116800` stores the native device's
  context-array PA (Xenia register 0x600; golden 0x1FCAA000 = Xenia's array) without the lwbrx.
- `SR_XMA_NATIVE_FRONT` (the shipping front end): the XMA* exports kick / lock / clear by
  context; `writeRegister` stays as the oracle path. Parity: difftest `--xma-test` T5n, 5 s of
  game XMA bit-exact, 0 register writes (the register path, 145 writes, is the control). The
  kick / lock / clear bodies are shared, so the two paths cannot drift.
- HAL dead: witness 0 in every roll (`xma_hal_voices=0 xma_hal_rest=0`, and the five MMIO HAL
  counters 0).
- Exit items, measured: the whole run w1b (roll_player: boot, front end, MODS, RESTART NOW, the
  relaunched game, NEW GAME, 4 min of gameplay; both processes) and o11 (intro + title + attract,
  WASAPI, player log) both end on
  `guest_mmio_fns=0 xma_regfile_writes=0 reg600=native:1E301000 xma_hal_voices=0 xma_hal_rest=0`;
  core-vs-oracle parity 0 bytes (T5n); live parity against Xenia (TOOLKIT 1.15): the intro movies,
  title and attract demo bit-exact at the submitted frame (77/82 3 s windows, the rest are
  sim-timed SFX), gameplay music 0.97-0.99 at 0.0 dB, tutorial SFX/VO residual -39.7 dB vs Xenia
  -39.6 dB, the Warworld arena -21.8 dB on both sides (music-dominant chunks -0.4 dB).

### N7 - Cutover: `SR_NATIVE=1` (replace, not delegate)

The iter-442 "delegate + mirror" decision flips to Unleashed's "replace" **for side
effects and ownership**, but not for memory (A6):

- hooks stop calling `__imp__`;
- **guest-format backing memory stays for every resource**; native GPU resources are
  mirrors invalidated on change. Host staging is used only where a whole-run load/store
  tap proves the guest reaches a resource through Lock alone;
- resource headers stay in guest memory in the guest format, because game code reads them.

**Deliverables**

1. Native `CreateDevice` (the `RenderDeviceInit_sub_82621258` path only) allocating the
   guest-visible device struct with the layout the inline code expects; native clears the
   dirty masks. `D3DDeviceCreate_sub_82106010` never runs.
2. `Vd*` no-ops.
3. **Present, vblank and fence model** within the §0 allowed/forbidden line (B2):
   - a host vsync/pacing clock; present interval and CPU-ahead depth matched to Xenia
     (§1 item 9);
   - no vblank callback to deliver (the game registers none; Appendix C.2). `dev+15124`
     is read only by XDK code that no longer runs **[static]**; keep it incrementing
     anyway until the load tap confirms no computed-address reader;
   - **fence invariant (B3):** a native fence never completes before the native path has
     consumed the guest memory its draws reference (either snapshot at call time on the
     game thread, or retire fences from render-thread completion). Otherwise
     `GpuFenceReaper_sub_82616208` or the allocator recycles memory not yet copied. Game
     callers: InsertFence (2), BlockOnFence (1), IsFencePending (1), BlockUntilIdle (3),
     ResourceIsBusy (2), BlockUntilNotBusy (7), QueryGetData (1);
   - resource-header fence fields: `SetPixelShader` (`sub_820F4E88`) copies `dev+0x2A1C`
     into shader+8. Every such field goes on the maintained-fields checklist (R8);
   - the movie kick callback per the N3 contract (A4);
   - InsertCallback CPU callbacks.
4. Long-tail entries: caps, queries, state blocks (`82112BE8`/`CD0`/`D18`), PIX, scaler,
   thread ownership, fences.
5. **No-access MMIO mapping and sentinel cursor** in native mode (§1 items 2-3, B7).
6. `scripts/audit_native.py`: every function in the hardware-facing ranges classified
   `native` / `dead(witness=0)` / `dead(static)` / `cpu-only-keep` / `oracle` in
   `OVERRIDES.json`. Ranges: the 623 D3D core functions; the XAudio XMA HAL
   `0x82119748..0x8211A160`; `sub_82116800`; **and the XDK-derived code outside the core
   that handles hardware-format state (F):** the RenderWare B5 builders
   (`0x82600000..0x82640000`), XGraphics (`0x827B75B8..`), the XGraphics state-blob
   builders.
7. `tools/native_audit.py` for the `[NATIVE_AUDIT]` lines; `tools/native_ledger_diff.py`
   replace-vs-shadow on the same tape.

**Exit:** the §1 list, all items. Then a `NATIVE` entry in `docs/NATURAL_BASELINE.md` with
the repro command after one blind Astra call, and the defaults flip: `SR_NATIVE=1` ON,
`SR_PM4_LIVE`/`SR_PM4_SHMEM` oracle-only, default OFF.

### N8 - The oracle stays alive (standing rule)

- The PM4 path, EDRAM and shared-memory emulation, `gpu_trace_replay` and the XMA register
  front end stay buildable and runnable as `SR_ORACLE=1` and in the `oracle-tap` preset.
  Do not delete them (CLAUDE.md).
- The taps live in the regen pipeline / a tracked header (C3), so the oracle build is
  reproducible from a clean checkout.
- Regression gate: **P-vs-X** (`pm4_replay_score.py --traces menu,eahd,t16,t21,t25,gp1,gp2,lg1,lg2`)
  **plus N-vs-X** (`native_ab.py` on every paired fixture). Any renderer change keeps both
  green.
- **Then mods** (RENDERER_PM4_HYBRID P4, `docs/MODS.md`): hash-keyed texture packs on the
  native resource identity, file shader swaps keyed by ucode hash, resolution scaling and
  ultrawide on native targets.

### Start here (the first three concrete tasks, all offline)

1. **Entry sort (N0.1):** produce `data/native/xdk_entry_classes.tsv` from
   `archive/v1_native/gfx_entry_points_hooks.tsv` + IDA decompiles: shadow-only /
   emitter / resource-sync, dropping the three non-D3D helpers.
2. **Taps in the tracked codegen (N0.4) and the true packet counter (N0.2):** extend the
   regen pipeline so `stvx`, `stvx128`, `dcbz`, `dcbzl`, `stwcx.`, `stdcx.` go through
   tracked tap macros; add `gfx/pm4_coverage` with the per-opcode counter.
3. **Xenia witness build (N0.6):** re-apply `tools/xenia-trace` to
   `reference/xenia-canary-oracle` and build it; in parallel, decompress the render
   scripts and grep for `rendertquad` (may settle B4 without a run). The witness run
   itself needs the Xenia slot.

---

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Inline sites the static scan missed (the class that sank HLE) | Dynamic coverage is the acceptance (§4). UNSTAMPED is a discovery. Draw-time shadow reads cover B1 by construction |
| R2 | Constant timing: 602 LOAD_ALU_CONSTANT per menu frame read memory at execute time; native reads at call time | LATE-MUTATION class in L1; §4.5 invalidation |
| R3 | Predicated tiling and bins (1,407 bin-mask packets on the menu frame; Lion per-tile c100) | Measure in N0; (bracket, tile) join (D5); conformance before build |
| R4 | ~~Recorded command buffers~~ | **Retired** (A7: dead code, Appendix C.1) |
| R5 | EDRAM aliasing between targets sharing bases | Measure `tilesMoved`; model only observed cases |
| R6 | Speed may not follow from native | **Partly measured** (E5): XDK share of the render thread is 7-46%, the rest ≥ 54%. Platform costs are C-gate usability work; firm up in N0.0 before any speed claim |
| R7 | Verification perturbs timing | Mode A is record-only (no doubled GPU work); A' only for pixels and play-tests; mode B for proof |
| R8 | Device and resource fields the XDK maintains stop being written at N7 | The N0 load-tap list, including resource-header fence fields (shader+8), is an N7 checklist. B2 settled that no game code reads dev+15120/+15124 by displacement |
| R9 | Host GPU tolerance on 7e3, float and gamma paths | Calibrate ε on bit-exact FMV (N3); PWL gamma ported, not sRGB |
| R10 | Keeping two renderers correct | One regression gate with both triads (N8); shared components fixed once |
| R11 | Host copies into command memory bypass the taps | None today (memcpy overrides delegate to guest code, F). New host copies call the tap explicitly |
| R12 | XMA: HAL liveness unknown; the XMAFRAMES codec exists only in Xenia's FFmpeg fork (LGPL) | Xenia witness (C2) before the audio design freezes; FFmpeg as LGPL DLLs |
| R13 | The delegate→replace flip exposes guest reads of XDK-internal resource state | Load tap on resource headers; B5 builders stay guest code; backing memory stays guest-format (A6) |
| R14 | **Backing-memory aliasing** (A2): textures over existing memory and rebased onto resolve targets; the same alias problem as the a3647e9 FMV regression | Physical-page registry in N1; intro-era check on every native gate |
| R15 | **Movie kick timing** (A4): early signal overwrites planes, late signal stalls the movie | Contract ledgered in N3, implemented at N7 on a KPCR thread at raised IRQL after host completion |
| R16 | **Early fence completion** (B3) lets the reaper recycle memory the render thread has not copied | Fence invariant in N7; ledger diff |
| R17 | **Tap coverage depends on codegen** (C3) | Taps in the regen pipeline covering every store class; census re-run after each regen |
| R18 | **Tessellation** (B4) unimplemented in both paths | Witness first; if live, oracle port before native |
| R19 | **Oracle comparability** (D1, D4): Xenia captures lack `.nrt`; tapes and pad tokens differ | Paired recaptures; checkpoint-based live comparison |

---

## 9. Audit of in-flight and planned work against the native rule

### STOP (or freeze at current state)

| Work | Why |
|---|---|
| Emulation-path speed work: further GPU-side EDRAM (`SR_PM4_EDRAM_GPU` is ON; stop there), baking `SR_PM4_RENDER_THREAD`, the CP packet-cost floor, shared-memory page-hash batching, vsfIb/decode/drain cost, resolve GPU-sync reduction | User excluded it. Exception: a targeted fix when a correctness roll cannot reach its era within its timeout (usability), recorded as such |
| PM4-only visual defect hunts where Xenia's trace render already answers what the frame should be | Log as P defects ("X wins, not inherited"); fix only what blocks the oracle's use |
| Mod and enhancement features (custom-frontend polish, new overlay features, resolution scaling on the PM4 path, RENDERER_PM4_HYBRID P4) | After N7 |
| The XMA register front end as a shipping path (`SR_XMA_MMIO`, register-0x600 seed, hwIdx physical arithmetic) | Hardware emulation: oracle only, classified `oracle` |
| Tiling-interpreter support stubs (`KeLockL2`/`KeUnlockL2` fidelity), `VdInitializeEngines` running the register-init callback, `GpuHangAnalyzeReport` register reads | Internal to the XDK device N7 removes; only if the oracle needs it |
| Mixed-ownership (mode C) interop between native and EDRAM targets | Dropped (D2) |

### SHARED (keep, because native needs it too)

| Work | Why |
|---|---|
| **Backing-memory / window alias ownership** (A2) | The native path needs a physical-page → resource registry: fetch constants carry guest-computed physical bases, and textures alias resolve targets. Build it once, shared. The GPU-read heuristics specific to the emulated CP (`gpuWindow` rules for CP reads, `SR_GPU_LARGE_PHYS` routing) stay frozen |
| **Texture-invalidation re-hash cost** (A3) | The §6g rule applies natively. Use guest boundaries first (§4.5); measure where hashing is still needed |
| Shader translation defects (XenosRecomp `setp_inv`, VS→PS interpolator linkage, predicated fetch), guest/memory/data defects | Native inherits them; keep fixing |

### CHANGE

| Work | Change |
|---|---|
| **Audio arc** | Order: stage 2 render-driver sink first (fix the `stubs.cpp:6490` comment), then un-force the sound flag, then XMA as **one decoder core with two front ends** (native: the 16 `XMA*` exports storing VAs through the `XmaMemory` VA accessor, host-owned state, host thread; oracle: the register device). **HAL liveness is measured in Xenia before the design freezes** (C2/E3). Classify `SR_XMA_MMIO`, the register-0x600 seed and the 3d hooks `oracle` |
| **HLE layer** (`gfx/hle_video`, `kernel/hle_api_observe`) | Promote from "observe/label" to the seed of the native device (N1). Update CLAUDE.md "CURRENT DIRECTION" and RENDERER_PM4_HYBRID §1 when N1 starts |
| **Runtime shader translation** | Keep. Request translation from the native bind site too, keyed by the patched ucode (§4.4, option a) so the cache is shared |
| **Visual-defect list** | Triage each as shared or PM4-only; fix shared, log PM4-only as P defects unless they block C-gate verification |
| **OVERRIDES.json / audit tooling** | Add classes `native`, `oracle`, `dead(static)`. Classify existing emulation overrides `oracle` (doorbell, synthetic ISR bit, `SR_CP_INT`, XMA MMIO hooks) and C-gate emulation hang fixes `oracle` (E4) |
| **RENDERER_PM4_HYBRID P4 row** | Moves after N7 and points here |
| **Correctness rolls** | Stay on the PM4 default until N7; add observe-only N0 gates when they already cover the era |
| **`docs/oracle/README.md`** | Update "Use the NO SOUND ISO" when the D1 recapture lands (F) |

### KEEP

C-gate platform work (KPCR, KTHREAD, waits, phys allocator, mftb, MXCSR, XInput,
collision/camera investigations); the PM4 engine, xenos_*, EDRAM-on-GPU, shared memory,
`gpu_trace_replay`, the live trace writer, `pm4_replay_score.py`, the golden traces,
`xtr_draws.py` (the oracle); the GPU hang fix and watchdogs; XenosRecomp and the DXIL
cache; the Xenia oracle worktree, sound-ISO dumps, save import, tapes and `sr_drive`;
the NO-FORCING charter and registry; the `mods/` overlay (no new
features until N7); the `archive/v1_native/*` inventory (move the TSVs the generators read
into `data/native/` when N0 lands).

---

## 10. Order at a glance

```
C-gate (audio -> freeze -> camera -> collision; HAL liveness from Xenia; usability)
  N0  speed gate, entry sort, packet counter, brackets, codegen taps, coverage,
      Xenia witnesses, load-tap fields, census, conformance skeleton
  N1  native device core, RECORD-ONLY (mode A) + .nrt (mode B) + output ledger
  N4a render targets, resolve, present; A' infrastructure        -> L2 per resolve
  N3  movies                                                      -> bit-exact N=X, SR_NATIVE_MOVIE
  N2  UI / Apt / menus                                            -> SR_NATIVE_UI
  N4b post, aliasing, (tessellation if live)                      -> SR_NATIVE_POST
  N5  world + B1/B2/B5 (B3 dead)                                  -> 100% coverage, SR_NATIVE_WORLD
  N7  SR_NATIVE=1 replace-mode cutover                            -> §1 list, all items
  N6  audio residue, in parallel after C2                         -> audio MMIO = 0
  N8  oracle kept alive as the regression gate -> then mods
```

---

## 11. Open questions that need the game slot or Xenia

| # | Question | Needs | Settles |
|---|---|---|---|
| Q1 | Is the XAudio XMA HAL live anywhere in the game? | Xenia entry-count build, full ISO, whole run + chapter saves | C-gate audio design; N6 scope |
| Q2 | Is hardware tessellation used by any render script, in any era? | Xenia entry counts on 0x82786B60/0x820FC478 (or offline script decompression) | N4b scope; whether the oracle needs a tessellation port |
| Q3 | Are the XGraphics state blobs (`82845780/958/B38`) and Z-pass live? | Xenia entry counts | B2/N5 scope |
| Q4 | Does gameplay use predicated tiling, and do Lion c100 constants vary per tile? | Our game slot: whole-run record-only roll with brackets + coverage; Xenia gameplay traces | N5 per-tile design |
| Q5 | Is any EDRAM aliasing reinterpretation real? | Game slot: whole-run `tilesMoved` and base reinterpretations | N4a/N4b conformance cases |
| Q6 | What is the real frame split (XDK vs fences vs rest) with logging off? | Game slot: one roll with `[NATIVE_FRAMESPLIT]` | E5 speed gate |
| Q7 | Which XDK-maintained device and resource fields does game code read (computed addresses included)? Which resolve destinations does the CPU read? | Game slot: oracle-tap load tap, whole run | N7 checklist; resolve copies |
| Q8 | Do the paired fixtures (menu, FMV, gameplay) exist? | Game slot (mode A capture) + Xenia trace-dump (offline) | Every N-vs-X exit |
| Q9 | What are Xenia's swap rate and CPU-ahead depth at each era checkpoint? | Xenia (full ISO) running the same `sr_drive` scenario | §1 item 9 target |
| Q10 | Does the movie kick callback timing explain the ~25% slow movie pace? | Game slot (ledger timing) + Xenia pace | A4 contract |
| Q11 | Does the occlusion flare match Xenia under a native query latency model? | Game slot + Xenia at a flare scene | N5 query model |

### 11.1 Answers from the Xenia entry traces (2026-10-01, burndown round 2) [measured]

Two runs of the v2-traced oracle build (`reference/xenia-canary-oracle/build-trace`, canary_experimental@0bd090dbe,
full sound ISO, `tools/xenia-trace/run_oracle_trace.ps1`): boot, intro movies, PRESS START, no profile, the opening
cinematic skipped, then Metropolis gameplay until 480 s. Run B carried a gameplay witness,
`hkCharacterProxy::integrate` 0x825EC5A8: 21,384 entries, and its final screenshot shows Superman on the street. A tag-0
record means Xenia entered a function starting at that VA; 0 means never, in this span. Evidence:
`archive/v1_burndown2/xtrace/burndown_{a,b}/summary.{txt,json}`.

| # | VAs | Run A | Run B | Answer (for this span: boot to early Metropolis gameplay) |
|---|---|---|---|---|
| Q1 | `Xma_CreateContexts_sub_82119980`, `XmaHw_WritebackShadowAndKick_sub_82119E68`, `XmaSourceVoiceCtor_sub_821236E0` | 0 / 0 / 0 | 0 / 0 / 0 | **The XAudio XMA HAL is not live.** Only `sub_82116800` needs a native replacement for this span (the §7 "0" branch). **N6 CLOSED FOR THE WHOLE GAME (2026-10-15):** every chapter seed (01-19, tools/devtools/chapter_seeds.py) and the NEW GAME -> ending -> post-game make roll read guest_mmio_fns 0 and xma_regfile_writes 0. |
| Q2 | `RenderScriptDrawTessellatedFullscreenQuad_sub_82786B60`, `D3DDeviceEmitDrawTessellated_sub_820FC478` | 0 / 0 | 0 / 0 | **No hardware tessellation.** Classify it dead for this span. The oracle does not need a tessellation port for the menu, movies or the first gameplay. |
| Q3 | `sub_82845780/958/B38` (XGraphics state blobs) | 0 / 0 / 0 | 0 / 0 / 0 | **The state blobs are dead.** B2 needs no native work for them. |
| Q3 | `D3DDeviceBeginZPass_sub_8210DF38` / `EndZPass_sub_8210E1A0` | 11,189 / 11,189 | 8,943 / 8,943 | **Z-pass is live.** Every Begin/End pair comes from one caller pair, lr 0x8277C65C / 0x8277C668. N5 must implement Z-pass. |
| (Q4 hint) | `D3DDeviceSetPredication_sub_8210D400` | 92,512 | 79,190 | Live, called in pairs from 0x82733458 and 0x827334BC (equal counts). Predication is in use. Whether that is predicated **tiling** per tile is still Q4 (needs the game slot). |

---

## Appendix A. Graphics inventory

Source: `archive/v1_native/gfx_*` (scans over 34,279 recompiled functions, the golden
dump and IDA). The game links the **August 2006 XDK** (`d:\xenon\aug06\core\private\xtl\graphics\...`).

### A.1 Library facts **[static]**

- Every GPU register access (0x7FC8xxxx) is in 5 core functions: ring write pointer
  `820FD218`, interrupt status `820FDBA0`, engine init `82105FC8`, present pacing `82111E38`,
  swap surface address `82112050` (plus the hang-report helpers `821065F8`/`82106608`).
- The game reaches the device only through its own global `0x8299ADF4`, never through
  `VdGlobalDevice`.
- Nothing outside the core reads the static render-state table `0x8295BF10` or the sampler
  table `0x8295C3A0`; the only reader is `D3DDeviceInitDefaultRenderStates_sub_821058B0`.
  ~~Every setter is reached by a direct call.~~ **Correction (N0.1):** that function copies
  the tables into the device (`dev+56` set, `dev+524` get, `dev+444` sampler set). D3DX
  effects, RenderWare state blocks, render scripts and Lion call setters through that copy,
  which puts 146 setters and getters outside the 216 (`data/native/xdk_table_setters.tsv`).
- Stale names: `D3DResource_*`, `GpuTile*`, `D3DDeviceLockedState*`, `D3DStateBlock*` at
  `0x82118978..0x821221E8` are XAudio/XMA objects (IRQL spinlocks, which D3D never uses).

### A.2 External entry points by family

216 entry points, 1,128 call sites; 33 hooked (445 sites), 183 not (683 sites).

| API family | Entry points | Call sites | Hooked today |
|---|---|---|---|
| Per-state render/sampler setters (`0x820F04E8..0x820F2720`) | 62 | 138 | 0 |
| Shader create/bind, constants, vertex declarations | 20 | 228 | 5 |
| Device and resource lifetime | 8 | 163 | 1 |
| Texture and surface | 26 | 125 | 5 |
| Shader command-buffer / assembler support (XGraphics, D3DX) | 32 | 123 | 0 |
| Render-target, viewport, stream and index binding | 12 | 92 | 6 |
| Vertex/index buffer create and lock | 11 | 81 | 2 |
| Draw and command-buffer recording | 9 | 73 | 5 |
| Swap, state blocks, PIX | 8 | 34 | 3 |
| Resolve, tiling, Z-pass | 6 | 22 | 3 |
| Ring and fence | 7 | 19 | 1 |
| Caps, queries, conditional rendering | 10 | 15 | 0 |
| Clear | 2 | 7 | 2 |

**Hooked today** (`kernel/hle_api_observe.cpp` unless noted): SetScissorRect `820F2720`,
SetViewport `820F2810`, SetStreamSource `820F2A50`, SetIndices `820F2C08`, SetRenderTarget
`820F2CA0`, SetDepthStencilSurface `820F2FD0`; Release `820F3DA0`, IB create `820F4040`,
SetVertexShaderConstantF `820F4920`; emits `820FB6E8`/`820FBBF8`/`820FC000`, EndVertices
`820FBBA0`, DrawVerticesUP `820FBBB0`; LockRect `820FFCB0`, UnlockRect `820FFCC8`,
CreateWithBackingMemory `820FFF08`, SetTextureFetchConstant `82100310`; Clear `82101998`,
ClearF `82101A58`, Resolve `8210C5F8`, BeginTiling `8210D588`, EndTiling `8210DA98`, Swap
`82112050`. Elsewhere: shader binds `820F4E88`/`820F5218`, shader create
`820F4D90`/`820F5148`/`82108300`, ring/fence `820FCB30`/`820FD150`/`820FD218`/`820FD9A8`.

**Unhooked with real traffic:** SetPixelShaderConstantF `820F4A30` (32 sites) and the B/I
setters; SetVertexDeclaration `820F5438` (22); VB Lock/Unlock `820F3FA0`/`820F4000` (38);
AddRef `820F3D28` (35); all 62 per-state setters; SetRenderTargetGammaEnable `820F1718`;
depth bias `820F0F88`/`820F1050`; SetPredication `8210D400` (9); queries
`821028B8`/`82102BC8`/`82102DC8`; Begin/EndZPass `8210DF38`/`8210E1A0`; conditional
rendering `82102010..82102608`; state blocks `82112BE8`/`CD0`/`D18`; InsertFence `820FDD30`,
BlockOnFence `820FDD68`, IsFencePending `820FDD90`, BlockUntilNotBusy `820FDE08`;
Acquire/ReleaseThreadOwnership `820F3618`/`820F3658`.

**Internal only** (no caller outside the core): `GpuBlockOnFence_sub_820FD9A8` (wait reasons
3 swap, 4 idle, 5 resource, 6 explicit), `GpuRingWaitForSpace_sub_820FCB30`,
`D3DDeviceInitRingBuffer_sub_820FDE90`, ring helpers `820FD8C0`/`820FC910`/`820FCBE8`/
`820FCDF0`/`820FCF90`/`820FD218`.

### A.3 Bypass classes in detail

- **B1 (72 functions).** Tracking `0x8299ADF4` finds 57 functions with 151 write sites; a
  signature scan finds 76, 9 outside RenderWare/Lion (likely false positives: Havok,
  cloth). Inline SetVertex/PixelShaderConstantF into `dev+0x700+16r` / `+0x1700` with dirty
  bits at `dev+0`/`+8` (e.g. `0x8260D9B8`, `0x826110A0`, `0x826185E0`, `0x82619AD0`, many
  through RenderWare parameter tables at `0x82988B3C`/`0x8297E07C`); none go through
  `820F4920`. Inline sampler bits into `dev+1024+24s` with dirty bits at `dev+16`
  (`RwDrawInstancedConstBatched_sub_8261B050`, `sub_8274E6D8`, `sub_8274FE18`; an
  unreferenced family at `0x82733040..0x82733290`, probably dead). `0x827332F0..0x82733398`
  and `827333E0` clear the dirty bits and write constants into the command buffer.
- **B2.** Lion `82113010` SET_CONSTANT c100 per tile under SetPredication (never in the
  shadow); `82473208` → `820FDE38`; `8277B998` → `820F5570` (raw registers then a
  full-surface clear); XGraphics `82845780/958/B38` build SET_STATE / IM_LOAD /
  IM_LOAD_IMMEDIATE blobs (only `.pdata` references).
- **B3.** `82304C38`: second device (flags=2), swaps `0x8299ADF4`, records mesh segments.
  Dead (Appendix C.1).
- **B4.** `D3DDeviceEmitDrawIndexedGen_sub_820FB6E8` returns a command-buffer pointer the
  game fills (16 functions, 18 sites, e.g. `D3DEmitIndexedDraw24_sub_8231AC30`,
  `RwTextRenderer_DrawGlyphQuads`); EndVertices 26 functions, 33 sites.
- **B5.** `82616C40` (9 callers) → `D3DTextureInitHeader_sub_8260A208` → XGraphics layout
  and fetch-constant builders; `8277B0A8` + `827B9E68`; `8261C5B0` tiles texels via
  `TextureTileSurfaceLevel_sub_827B8A90`.

### A.4 Xenos behaviour native must reproduce (Xenia paths under `reference/xenia-canary/src/xenia/gpu`)

| # | Behaviour | Xenia reference | This game |
|---|---|---|---|
| 1 | EDRAM: 2048 tiles of 80x16 samples; aliasing via tile ownership | `xenos.h:453-457`, `render_target_cache.h:235-248, 584-610` | bases 0 / 0x2D0 / 0x5A0 / 0x658 |
| 2 | Resolve: conversion, 8-px alignment, clear 0x100/0x200, depth 0x14, slice/cube | `draw_util.h:627, 745`, `xenos.h:973` | ~13/frame; 8,172 depth resolves per roll; 16_16_16_16_FLOAT dest |
| 3 | Formats: 7e3, signed 16_16, 20e4, k_32_FLOAT | `xenos.h:303, 407, 417`, `dxbc_shader_translator.h:539-558` | HDR main target; format 3→12 |
| 4 | Gamma: PWL 8_8_8_8_GAMMA, per-target enable, display ramp | `xenos.h:299`, `xenos.cc:23/66` | ~~`820F1718`~~ (correction N0.1: high-precision blend, not gamma; gamma selection open), `82111CB8`, `82105458` |
| 5 | MSAA 2x/4x layouts, averaging on resolve | `docs/XENIA_EDRAM_CONTRACT.md` | 640x640 k_16_16 4x |
| 6 | Predicated tiling, SET_BIN_MASK/SELECT, VIZ_QUERY | `pm4_command_processor_implement.h:408-418, 511-544` | menu: 1,407 bin-mask packets |
| 7 | Texture layout: 32x32 tiling, packed mips, endian, fetch constants | `texture_address.h:22-155`, `texture_util.h:41-50, 191`, `xenos.h:1158, 1229` | |
| 8 | Shaders: runtime-compiled microcode; stride patched into vfetch | `ucode.h` | 33 effect creations per roll; `821081C0` |
| 9 | Rect/quad lists, half-pixel offset, VTE; clears as draws | `xenos.h:48, 53`, `draw_util.h:434, 516`, `draw_util.cc:171` | 4x depth-only quad at z = −0.0 |
| 10 | Swap and scaler | `pm4_command_processor_implement.h:644-663` | `GpuScaler*` at `0x8210A0F8..` |
| 11 | Tessellation (B4) | `d3d12/pipeline_cache.cc` TessellationMode | `820FC478`; liveness open |

### A.5 Menu-frame PM4 census **[measured]**

`python tools/xtr_draws.py data/gpu_traces/menu_pressstart_454107ED_9415.xtr --frames 1`:
63,054 executed packets. DRAW_INDX 3,370; LOAD_ALU_CONSTANT 602; SET_BIN_MASK_LO/HI
722/685; SET_BIN_SELECT 5/1; IM_LOAD/IM_LOAD_IMMEDIATE 494/281; INVALIDATE_STATE 284;
WAIT_REG_MEM 52; EVENT_WRITE_SHD 24; INTERRUPT 2; XE_SWAP 1.

---

## Appendix B. Audio and hardware-register inventory

Source: `archive/v1_native/mmio_inventory.tsv`, `scan_mmio.py`, `classify_mmio.py`
(all 142 `ppc_recomp.*.cpp`, cross-checked against all 13 `eieio` sites).

### B.1 Every MMIO access **[static]**

The harness has no MMIO trap handler: 0x7FC00000 is plain RAM
(`kernel/guest_context.cpp:3676` zeroes 64 KB there).

**GPU registers (0x7FC80000 + 4·reg), all in the D3D core:**

| Function | Access | Register | When |
|---|---|---|---|
| `GpuRingUpdateWritePointer_sub_820FD218` @0x820FD364 | store | CP_RB_WPTR 0x1C5 | every ring submit; skipped when dev+10809 & 2 |
| `GpuInterruptCallback_sub_820FDBA0` @0x820FDC44 | load | D1MODE_VBLANK_VLINE_STATUS 0x1951 bit 0 | every vsync ISR |
| `GpuPresentPacingUpdate_sub_82111E38` @0x82111E7C/80 | loads | D1MODE_V_COUNTER 0x194C, D1MODE_VIEWPORT_SIZE 0x1961 | CPU callback in the command stream, on CP interrupt |
| `D3DDeviceSwap_sub_82112050` @0x821125D0 | store | D1GRPH_PRIMARY_SURFACE_ADDRESS 0x1844 | only when dev+10809 & 2 |
| `GpuEngineInitRegs_VdInitEnginesCb_sub_82105FC8` | 2 stores | PA_CL_ENHANCE 0xC85 = 7; SQ_INST_STORE_MANAGEMENT 0xD02 = 0x800 | never (our `VdInitializeEngines` is a stub) |
| `GpuRegReadByIndex_sub_821065F8` / `GpuRegWriteByIndex_sub_82106608` | indexed | 0x5D0, 0xF26, 0x57B-0x57D, 0xC81; write 0xC80 = 12 | only from `GpuHangAnalyzeReport_sub_821068C0` |

**XMA registers (0x7FEA0000 + 4·reg), all in XAudio:**

| Function | Access | Register |
|---|---|---|
| `sub_82116800` (engine create; no IDA function) | lwbrx | 0x600 context-array physical base → `dword_829AC0E8`, tail-calls `AudioDeviceCreate_sub_82117018`. **Every boot** |
| `Xma_CreateContexts_sub_82119980` @0x82119A18 | stwbrx | 0x6A0+ Clear |
| `XmaHw_LockContexts_sub_82119BA8` @0x82119BF8 | stwbrx | 0x690+ Lock |
| `XmaHw_WaitIdleOrResetDecoder_sub_82119C28` | 2x stwx | 0x601 (only on a 1/8 s lock timeout) |
| `XmaHw_PollIdleAndSyncShadow_sub_82119CE8` @0x82119D1C | lwbrx | 0x606 |
| `XmaHw_WritebackShadowAndKick_sub_82119E68` @0x82119F74 | stwbrx | 0x650+ Kick |

These match Xenia's only two MMIO ranges (GPU 0x7FC80000; XMA 0x7FEA0000,
`reference/xenia-canary-oracle/src/xenia/apu/xma_decoder.cc:135`).

### B.2 XAudio layers (`0x821162E8..0x82127500`)

- Public wrappers under critical section `0x8295C4B4`; outside callers are only the XMV
  movie audio renderer (`0x824726B0..0x82472AE0`) and EA's DAC (`0x82650700..0x82650E20`).
- Engine: `AudioDeviceCreate/Init_sub_82117018/82117988`, `AudioBuildSpeakerConfig_sub_82117DF8`
  (branches on `KeGetCurrentProcessType`), `AudioWorkerThreadMain_sub_82117810`,
  `XAudioServiceSync_sub_82117F10`, `XAudioUpdateVoiceVolumes_sub_82118068`.
- Mixer/effects: CPU/VMX (`AudioMixProcessBlockVMX_sub_8211FBE8`, reverb, filters).
- Voices: `AudioCreateObjectByType_sub_8211B070` → PCM voice (type 0-3) or XMA voice
  `XmaSourceVoiceCtor_sub_821236E0` (type 4) on the HAL: `Xma_AllocContextBlock_sub_82119748`,
  `Xma_CreateContexts`, `Xma_ReleaseContexts_sub_82119A50`, `Xma_SubmitInputBuffer_sub_82119AC8`
  (uses `MmGetPhysicalAddress`), Lock/WaitIdle/PollIdle/WritebackAndKick,
  `XmaShadow_ConsumeOutputRing_sub_82119FA8`, `XmaShadow_ResetSlot_sub_8211A160`. 96-byte
  shadow per context (first 48 bytes = hardware context copy; hw pointer at +64, index at
  +80), written back with lvx/stvx then kicked.
- Render-driver client: `CreateXAudioRenderDriverClient_sub_8211B520`,
  `XAudioSetRenderDriverClient_sub_8211B6A0`, submit wrapper `sub_8211B680` (vtable
  `0x820BC680`) → `__imp__XAudioSubmitRenderDriverFrame`; callback
  `XAudioRenderDriverCallback_sub_821209F0`.
- 53 functions use KeRaiseIrqlToDpcLevel + spinlocks (served by `kernel/kpcr.cpp`, bb96dbb).

### B.3 EA DECEAXMA (`0x82658350..0x826597E8`): kernel API only

| Function | XMA imports used |
|---|---|
| `EaXma_InitResources256Ctx_sub_826597E8` | XMACreateContext (256), XMADisableContext |
| `EaXma_StreamInitContexts_sub_82658560` | Initialize, SetInputBuffer0/1, SetOutputBufferValid, Enable, Disable |
| `EaXma_FeedInputBuffers_sub_826586A0` | Is/SetInputBuffer0/1Valid, Enable, Disable |
| `EaSndXmaEnableAllContexts_sub_826583C8` | Enable |
| `EaXma_DisableAllExcept_sub_82658350` | Disable |
| `EaXma_DecodeDrainOutputSpin_sub_826592F0` | Get Write/Read offset, IsOutputBufferValid, SetOutputBufferReadOffset, SetOutputBufferValid, Disable, Enable |
| `EaXma_ReleaseResources_sub_82659678` | XMAReleaseContext |

EA reads decoded PCM from its own 6144-byte guest output buffer (`sub_82658958`). Progress
is kick-driven (re-enable at 0x826595B4); Xenia oracle commit 6bef1fb (drain fix) is
required.

### B.4 Other device internals (all removed by N7)

- ISR sources: 0 (vsync; gated on 0x7FC86544; bumps dev+15124; calls dev+15120 if set) and
  1 (CP INTERRUPT 0x54; continuation at pumpctx+16). Ours: `vdVblankThread`
  (`kernel/stubs.cpp:3688`), async-CP source-1 delivery, `SR_CP_INT` fallback.
- Ring: `D3DDeviceInitRingBuffer_sub_820FDE90` → `VdInitializeRingBuffer` (64 KB),
  `VdEnableRingBufferRPtrWriteBack`, `VdSetSystemCommandBufferGpuIdentifierAddress`;
  `GpuRingWaitReadPtrWrap_sub_820FD150`.
- Handshake: SCRATCH writebacks → WAIT_REG_MEM → INTERRUPT → ISR → KeSetEvent →
  `VdInterruptPumpWait_sub_82110A30` → `GpuCommandBufferReplayDriver_sub_82110840` → submit.
  Fence/ring fields +10768/+10772/+10780/+13480 are accessed only inside the core.
- Pump threads: `GpuCreateBlockInterruptThreads_sub_82110EB8` (priority 17), EVENT_WRITE_SHD
  targets 0x7F000000 + 128n.
- Tiling interpreter `GpuTilingCommandInterpreter_sub_82110460`: op 0x89 KeLockL2 + dcbz128,
  0x8A KeUnlockL2, 0x82 submit.
- `Vd*`: implemented VdSwap, VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack,
  VdGetSystemCommandBuffer, VdSetGraphicsInterruptCallback, VdInitializeScalerCommandBuffer,
  VdIsHSIOTrainingSucceeded; generic stubs for the rest (`kernel/stubs.cpp:6456-6475`).
- CPU-level only (nothing to replace): mfmsr/mtmsrd interlocks (428 functions), mftb (497
  sites), cache hints.

### B.5 Audio stages classified

| Stage | Class | Verdict |
|---|---|---|
| 1: gate `cfgSoundOffThread`; XMAReleaseContext glue to kernel | removes forcing | keep |
| 2: render driver (Register/Unregister/Submit, category volume, pacing, SDL/null sink, downmix) | native kernel API | keep; first |
| 3a: real `MmGetPhysicalAddress` | native kernel API | keep as API fidelity |
| 3b: XMA* exports + worker + FFmpeg + Xenia context state machine | split | export front end + core + host thread native; register file, 0x600 seed, hwIdx arithmetic, physical page memo oracle |
| 3c: dedicated thread vs inline decode | implementation | keep; synchronous kick semantics are API behaviour |
| 3d: `SR_XMA_MMIO` hooks on 0x82119980/BA8/E68 | XMA register emulation | oracle only |
| 4: KPCR + spinlocks (bb96dbb) | platform | keep |
| 5: KeGetCurrentProcessType from KTHREAD+0x73 (needs KeWaitForMultipleObjects) | native kernel API | keep |
| Tests T1-T11, `tools/xma_oracle`, `SR_XMA_CAPTURE`, parity | oracle | keep |

---

## Appendix C. Static-check results (critique items settled 2026-10-01)

Read-only: no repo, IDA or run changes. Scripts and outputs in the session scratchpad
(`memops.json`, `offscan.py`, `storecensus.py`, `fnstores.py`, `tess_scan.py`,
`tess_detail.py`, `tess_scan_all.out`, `sampler.py`, `findword.py`).

### C.1 A7: recorded command buffers are dead code (confidence high)

- `GpuBuildTiledCmdSegments_sub_82304C38` has no code callers. Its address appears once
  as a word in `default.xex.img` (0x820CAB18, the `.pdata` entry). No `lis`/`addi` pair
  builds it.
- `GpuCmdContextBeginTiled_sub_820FAEE0` (BeginCommandBuffer),
  `D3DDeviceCloseCommandBufferSegment_sub_820FB3A8` and
  `D3DDeviceQueryCommandBufferSize_sub_820FB5A8` are called only from 82304C38. The flags=2
  `D3DDeviceCreate_sub_820F34C0` exists only there; its other caller is
  `RenderDeviceInit_sub_82621258`. Table 0x82970160 has a single xref, from 82304C38.
- The owner is `ClothRenderObject` (ctor `ClothRenderObjectCtor_sub_82305410`, vtable
  `off_82021768`: +252 = 0, +212 = 1, +232 = 2, never +256). None of its 20 vtable
  methods, `RenderDrawableNode_sub_82316958`, `ModelEmitMeshDrawsPerCmdBuffer_sub_822F5290`
  or `ClothBuildInstanceBuffers_sub_82304FA0` loads +252/+256; no function in
  0x822F0000..0x82320000 does `lbz …,252`. The live path draws immediately.
- No RunCommandBuffer-class code: the INDIRECT_BUFFER header 0xC0013F00 occurs once, in
  `GpuRingUpdateWritePointer_sub_820FD218` (the ring's own kick); no 0xC00137xx constant.
  `GpuCommandBufferReplayDriver_sub_82110840` is the XDK's internal segment list.
- Residual gap: a header built with `rlwimi` or loaded from a table would escape;
  unlikely since the recorder itself is unreachable.

### C.2 B2: no game vblank callback; the two stores are on the WMV decoder context (high)

- `824E0338` (`WmvSetupPictureGeometryAndMbEdgeFlags`) stores at 15120(r3)/15124(r3),
  r3 = the WMV decoder context (callers: `Vc1DecodeFrame_sub_8250AD58` x7,
  `WmvDecodeFrameToOutputSurface_sub_824C0B10` x2, `WmvParsePictureHeader_sub_824D08A0`,
  `VideoDecodeOrSkipFrame_sub_824BD800`); values look like MB-row strides.
- `82507380` (`VidSetupPlaneGeometrySubsampled`): same layout; callers
  `WmvDecoderFlushOutput_sub_824C1950`, `WmvDecoderInitForFourcc_sub_824DF168`.
- The whole binary has four displacement accesses to +15120/+15124: these two stores and
  the reads in `820FDBA0` and `82111E38`. Nothing writes a callback into dev+15120.
- Golden dumps (`tools/golden.py`; device `dword_8299ADF4` = 0x40002880):

  | Dump | dev+15120 | +15124 | +15128 |
  |---|---|---|---|
  | sound_gameplay | 0 | 0x23397 | 0x23395 |
  | sound_menu | 0 | 0x14204 | 0x14203 |
  | nosound | 0 | 0x5B1B | 0x5B1B |

### C.3 B4: tessellated draw exists; no live witness (medium-high for menu/FMV/early-mid gameplay)

- `D3DDeviceEmitDrawTessellated_sub_820FC478` ← `RenderScriptDrawTessellatedFullscreenQuad_sub_82786B60`
  ← case 0x13 of `RenderScriptRunTechnique_sub_8277B998` (call at 0x8277C200). Setters
  `sub_820F1A48` (mode, dev+10488) and `sub_820F1A10` (max level, dev+10492) are called only
  from 82786B60; `sub_820F19D8` (min level) has no callers.
- Parser `sub_8277DB10` maps `"rendertquad"` (0x82098D20) to opcode 19 (`li r4,19` at
  0x8277EA20); `"renderquad"` is 18.
- The keyword appears in no plaintext file on the disc except the xex (scripts are stored
  compressed); in the golden dumps only the parser's string. The resident script text at
  0xBEEE9000 is a stale fragment, so the dumps cannot prove absence.
- No `[API_OBS]` hook covers 820FC478/82786B60.
- **PM4 scan** of all 99 `.xtr` traces (289,640 draws): 0 draws with major_mode ≠ 0,
  0 patch primitives, 0 VGT_OUTPUT_PATH_CNTL ≠ 0. `fmv_stream.xtr` has one
  VGT_HOS_CNTL = 1 write in frame 0 covering 24 point-list copy draws with major_mode 0,
  reset at draw 24: an initial-state artefact. Coverage: Xenia menu, the four FMV frames
  and fmv_stream, Xenia gp1/gp2, our gameplay swaps 1920-5340, tape3 19296-26168.
- The cape material (`SMCape2FX`, `maxTessellation 2.0`, 7 hits in `objtiv.AST`) is
  parsed by `sub_822DF2D0` into material +24/+28; no path found from there to the
  tessellation state (the readers of +24/+28 were not traced).

### C.4 C3: store classes that bypass `PPC_STORE_*` (high)

| Instruction | Sites | Generated code | Through `PPC_STORE_*` |
|---|---|---|---|
| `stvx` | 14,627 | raw `simde_mm_store_si128(base+…&~0xF)` | no |
| `stvx128` | 3,455 | raw `simde_mm_store_si128` | no |
| `dcbz` | 4 | raw `memset(…,0,32)` | no |
| `dcbzl` | 71 | raw `memset(…,0,128)` | no |
| `stwcx.` | 818 | raw `__sync_bool_compare_and_swap` | no |
| `stdcx.` | 6 | raw `__sync_bool_compare_and_swap` | no |
| `stvewx`, `stvewx128`, `stvehx`, `stvlx`, `stvrx` | all | `PPC_STORE_U32/U16/U8` | yes |
| scalar stores | all | `PPC_STORE_*` (MMIO sites `PPC_MM_STORE`) | yes |

- `SetVertexShaderConstantF_sub_820F4920` / `SetPixelShaderConstantF_sub_820F4A30` copy
  into the shadow with `lvlx`/`lvrx` + `stvx` (5 each); only the dirty-mask update is a
  scalar `std`. `GpuEmitType0RegPacketsVec_sub_821078C0` copies the shadow into the command
  buffer with `lvx128`/`stvx`; only headers go through `stwu`.
- Other XDK `stvx` users: Clear (1), `SurfaceClearColorPackByFormat` (9), BeginTiling (1),
  BlitAndSwap (1), `GpuScalerGenPolyphaseTaps` (1 + 1 `stvx128`). The tiling interpreter
  uses `dcbzl` (1). D3D range total: 33 `stvx`, 1 `stvx128`, 1 `dcbzl`, 11 `stwcx.`.
- Game-side constant stores: 11 `stvx` + 5 `stvx128` bypass; 14 `stfsx` + 8 `stfs` go
  through. Dirty-mask `std` and fetch-constant `stw` go through.

### C.5 E5: first cut of the frame split (weak sample)

- Source: `archive/v1_audio/kpcr_long1/attempt1/superman_returns_run0.log`. 76
  `[THREAD_SAMPLER]` instants (every 20 s); `lr` is the return address of the most recent
  call, so time in an XDK leaf is charged to its caller. Heavy logging (~985K `[BOOT]`,
  342K `[FETCH0_WRITE]`, 74K `[API_OBS]` lines) distorts timing. 95% interval ≈ ±11
  points; 0/76 bounds a rate below ~4%.
- Frame time: `[API_OBS] SUMMARY` swap 1500 → 22440 over 22:20:24 → 22:43:09, ~65 ms per
  frame. `[GPU_CP_TIME]` (190 windows): 66.3 ms per swap mean (median 64, p10 36, p90 105).
  The async CP is idle 47%, IB work 26%, swap work 26%; `[PM4_SWAP_TIME]` render median
  17.1 ms. Its ~35 ms busy per swap would cap near 28 fps if the CPU side sped up.
- Render thread (tid 88032), never sampled waiting: XDK D3D share lower bound 5/76 (7%, lr
  inside the range), upper bound 35/76 (46%, most recent call into the range:
  EmitDrawIndexedVertices 13, SetStreamSource 5, SetTextureFetchConstant 4,
  SetVertexShaderConstantF 4, TextureUnlockRect 3, SetPixelShaderConstantF 2, EmitDrawVertices
  2, CmdBufAllocate 1, indirect 1). GPU fence waits 0/76. Everything else ≥ 54%
  (RenderWare B5 driver 38/76 by lr, culling, cloth, particles, Apt).
- Worker threads (87188, 13184): 30-55% of most-recent calls are `NtYieldExecution`,
  `CsEnterWithDeadline`, `CsLeaveCounted`: lock and yield cost, i.e. platform cost.
- Conclusion: native can remove at most the XDK share (~7-46%) plus the CP cost (which
  overlaps CPU work today). "Speed comes from going native" is at best partly true.

---

## Appendix D. Provenance

- Draft: `archive/v1_native/NATIVE_ROADMAP.md` (2026-09-30).
- Inventories and scanners: `archive/v1_native/` (gfx_*, mmio_*, lowhw_*).
- Critique items A1-A8, B1-B7, C1-C5, D1-D5, E1-E5, F: all applied above; each is cited
  inline by its label.
- IDA renames from the inventory pass: `XmaShadow_ConsumeOutputRing_sub_82119FA8`,
  `GpuEngineInitRegs_VdInitEnginesCb_sub_82105FC8`, `GpuRegReadByIndex_sub_821065F8`,
  `GpuRegWriteByIndex_sub_82106608`, `XAudioVoiceWrapper_InitFromDesc_sub_82118CE8`,
  `XAudioVoiceWrapper_Ctor_sub_82118A70`.
