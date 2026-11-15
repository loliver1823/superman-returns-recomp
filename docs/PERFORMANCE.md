# Performance: frame rate, platform costs, memory

Owner: the perf lane (iter-464, branch `perf/platform`). User decision 2026-10-02: prioritise
frame rate and resolution - target 120 fps, 60 fps minimum, target 4K but support every
resolution. The game's own simulation is fixed at 30 Hz (SIM manager 0x8296D8E0 +0x50 =
33,333,334 ns); going above 30 is the highfps lane. **This lane's job: every era at a solid
30+ fps with headroom (frame work well under 33 ms, ideally under 16 ms), by finding and
removing PLATFORM costs** (what our harness does on the game's behalf), Xenia-faithfully where
semantics matter. Guest code speed is the codegen lane (perf/codegen).

## 1. The harness

Everything below is observe-only and default OFF (one bool test per hook when off).

| Gate | What it writes (to `SR_PERF_DIR`, default `archive/v1_perf/run_<pid>`) |
|---|---|
| `SR_PERF_STATS=1` | `frames.csv` (a QPC stamp per guest `VdSwap` = the game's frame, and per host Present), `threads.csv` (per-thread user/kernel ms and context switches, every second), `mem.csv` (private bytes, working set, commit, GPU local/non-local, every second), `meta.txt`; one `[PERF]` line per 10 s |
| `SR_PERF_MEMMAP_SECS=<s>` | with the above: `memmap_<n>.txt` every `s` seconds (default 60, 0 = off): a VirtualQuery census of the address space, committed/resident MB per big allocation (labelled: guest flat region, base-4GB mirror, modules), resident MB per 256 MB guest window, mirror demand commits |
| `SR_HOST_PROFILE_HZ=<hz>` | `hprof_<n>.txt` every `SR_HOST_PROFILE_SECS` (default 10): the HOST sampling profiler. Each thread that used CPU since the last tick is suspended, its RIP read and its host stack unwound (RtlVirtualUnwind over our own .pdata search: no lock, no allocation while suspended, reads only `[Rsp, StackBase)`). Recompiled guest functions are host functions named `sub_XXXXXXXX`, so the symbol is the guest VA; platform functions have their own names |

Tools (`tools/perf/`):

- `perf_run.py --scenario title|tutorial|arena --config default|player --exe <snapshot> --out archive/v1_perf/<tag> [--hz 250] [--seconds N] [--gates A=1,B=0]`
  - `title`: no input; boot, intro movies, title. `run_recomp_x.ps1 -K 1`.
  - `tutorial`: `tools/scenarios/perf_tutorial.json` + the tutorial input tape; boot, intro,
    title, front end, the tutorial flight (5 min after the `main_hud` anchor).
  - `arena`: the dev-tool level skip to the Warworld arena (`skip_run.py --executor kernel`),
    then `--seconds` in the arena. **Dev-tool assisted** (tagged run).
  - `default` = the exe's defaults for an automated roll; `player` = what a player gets
    (`SR_LOG=player`; see section 3).
- `perf_report.py <perf dir> --log <game log> [--json out]`: per era - fps (guest swaps per
  second), frame time p50/p95/p99/max (interval between swaps), frames over 33.4 / 50 ms,
  host presents/s, process CPU in cores, per-thread CPU % and context switches/s, memory
  (private / working set / VRAM / GPU system memory). Eras start at milestone log lines that
  both log profiles keep (`DEFAULT_ERAS`).
- `host_profile.py <perf dir> --eras-json report.json [--stacks N] [--folded f.txt]`: top-N
  self / inclusive functions per thread per era and the top stacks. Symbols: llvm-symbolizer
  over the exe's PDB (System32 DbgHelp cannot read this lld PDB) and PE export tables.
- `tests/perf/host_profile_selftest.cpp`: standalone check of the profiler (deep recursion,
  a leaf without unwind data, a sleeper, a thread exiting mid-run).

Repro (from the main repo root, holding the game slot):

```
python <worktree>/tools/perf/perf_run.py --scenario title --config player \
    --exe <snapshot exe> --out archive/v1_perf/<tag> --hz 250 --seconds 240
```

## 2. Results (iter-465, gameplay = the tutorial after NEW GAME)

Scenario: `perf_run.py --scenario gameplay` (`tools/scenarios/perf_player_gameplay.json`: boot,
intro movies, title, custom MAIN MENU, NEW GAME, the opening FMV, then the tutorial with
`tools/player/gameplay_play.txt` driving walk / look / fly). PORT FEATURE rolls
(`SR_PORT_FEATURES=1`, natural=false): 120 fps target, 2x internal at 1080p, 3x at 4K.
Displayed fps = median of the 2 s `[HIGHFPS_PACE]` windows per era (`tools/player/era_fps.py`;
`ingame` = after the opening FMV). Live rolls are noisy and other lanes load the machine: every
row names the other processes' busy cores (perf_report "contention").

### 2.1 Where the gameplay frame goes (host profile, 100 Hz, every thread)

`archive/v1_perf/g4_ffx_portdev` (before) and `g11_prof_nomirror_noobs` (mirror off), ingame era,
`tools/perf/attribute.py` (hooks = `docs/OVERRIDES.json` overrides; `__imp__sub_X` = guest body):

| Thread | Before (9.3 fps) | After the mirror fix (12.5-13.5 fps) |
|---|---|---|
| Guest render thread (FGGameRender) | CPU-bound, ~98% of a core; ~22% the HLE mirror (publishDrawState, idxGatherPublish, poolWrite, memcpy, a condition-variable wake per command); waits < 1% | CPU-bound, 99%: **94.4% recompiled guest code**, 4.8% platform (SetTexture hook 2.6%), 0.8% system; GPU-fence spin 0.2% |
| Command processor (GPU_CP) | ~60% busy: ib 38 ms + swap 30 ms per frame, idle 40 ms | 99% busy: ib 40 + swap 32 ms (no memo); with SR_PHYSWIN_MEMO ib 28-35 ms, idle 10-13 ms |
| GameKernel runners (core, sim, sound) + main | ~97% each: yield-spin waiting for their next tick (NtYieldExecution, CsEnterWithDeadline) - the console's own idle loops, as in Xenia | same |
| HLE video consumer | 42% of a core, 24K context switches/s (woken per mirrored command) | idle |

Guest render thread, top guest functions (self, g11; for perf/codegen and native): 
GpuEmitType0RegPacketsVec 8.5%, SetVertexShaderConstantF body (GpuCopyVectorConstantsVMX,
`__imp__sub_820F4920`) 7.0%, BoundsVolumeIntersectTest 5.9%, `__imp__sub_820F4A30` 4.3%,
ClothUpdateElementLightMask 3.5%, EnvironmentRenderObj_DrawParticleLayer 3.3%,
MathVmxDotMinReduce 3.1%, AppendVertexData 2.8%, the `__savegprlr_*`/`__restgprlr_*` helpers
~6%, XMemCpy_Vmx128 1.6%. The XDK's own GPU command building (register packets, constant
uploads, draw emission) is ~25% of the thread: the native D3D lane's target.

Logging is NOT a frame cost: ~580 lines/s in gameplay under SR_LOG=dev; SR_LOG=player vs dev
(g5 vs g4) 9.3 vs 9.1 fps. Resolution is not either: 4K/3x = 1080p/2x within noise.

### 2.2 Per-era fps, before / after (1080p 2x and 4K 3x)

| Era | dev 31c522d6, r5 1080p / 4K (player lane) | this branch before (g5, 1080p) | after, 1080p (m1, m5) | after, 4K (m3, m4) |
|---|---|---|---|---|
| Title | 16.4 / 12.3-13.4 | 14.9 | 26.5 / 20.7 | 25.2 / 26.5 |
| Custom MAIN MENU | 13.8 / 12.9-16.1 | 11.4 | 15.6 / 15.7 | 15.8 / 16.3 |
| Gameplay (ingame) | 9.4 / 9.5 | 9.3 | 13.2 / 13.1 | 13.5 / 13.4 |
| Ingame frame p50 | 105 ms | 106 ms | 75 / 74 ms | 73 / 74 ms |
| Other processes' busy cores | - | 2.7 | 6.2 / 6.1 | 5.2 / 5.3 |

"after" = the HLE mirror + rendezvous off (default under PM4) + SR_PHYSWIN_MEMO; these rolls
predate the codegen merge (92042658). Natural rolls are unaffected by the fps target; the
fixes are platform fixes that also apply to them.

### 2.3 On dev 92042658 (codegen register localization merged), identical host, 2026-10-04

Same exe (`archive/exe_snapshots/d2943c151af0-perf-b6` in sr-wt-perf), the dev behaviour
restored by gates (`SR_HLE_MIRROR=1 SR_API_PRESENT_BLOCK=1 SR_S6_TEX=1 SR_PHYSWIN_MEMO=0
SR_PM4_DIAG_TIMING=1 SR_PM4_BULK_TYPE0=0`) vs the defaults; quiet machine (other processes
2.4-2.7 busy cores); 1080p 2x; `archive/v1_perf/q*`:

| Era | dev behaviour (q2 / q6) | this lane's defaults (q1 / q5) |
|---|---|---|
| Title | 16.2 / 17.3 fps | 31.9 / 31.8 |
| Custom MAIN MENU | 13.6 / 13.45 | 18.9 / 19.0 |
| Gameplay (ingame) | 11.0 / 10.6 (p50 90-94 ms) | 17.15 / 16.7 (p50 58-59 ms) |
| CP per frame (idle / ib / swap) | 27 / 36 / 30 ms | 9-10 / 20-23 / 24-26 ms |

Single-gate rolls on the defaults: SR_PM4_RENDER_THREAD=1 16.7 / 17.0 ingame (no gain: the
guest is the limiter), SR_S6_TEX=1 16.4, SR_PM4_DIAG_TIMING=1 16.7 (CP-only relief).
Gameplay host profile (`p1_prof`, 16.9 fps): the guest render thread is 99% busy, 94.7% guest
code, 3.5% platform, 1.9% system, GPU waits < 1%; the CP is ~80% busy. **The limiter is the
guest's own render frame (~58 ms of recompiled code), not the platform.** Its top functions are
in 2.1 (the XDK's GPU command building - GpuEmitType0RegPacketsVec 10.5%, the vertex and pixel
shader constant uploads 9.0% + 5.3% - is ~30%). Replay gate unchanged: menu 2.06, FMV x4 0.00,
gp1 4.59, same colours and register-write counts as dev (`score6_after.txt` vs
`score6_trunk.txt`).

### 2.4 perf2: the guest memory-access macros and the guest ISA (2026-10-04)

Every guest load/store/SIMD access expands from `guest/ppc/ppc_context.h` (~600K sites). Four
changes, each a separate commit on `perf/perf2` (exe 454.5 -> 152.2 MB):

1. PPC_STORE debug watchpoints compiled out (CMake `SR_STORE_WATCHPOINTS`, default OFF). The
   runtime gate was armed in EVERY game (the sub_826F92E0 observer sets g_storeWatchOn at
   boot: 64 `[WATCHPOINT] FMUI_QUEUE` lines per roll), so each 32/64-bit store ran the range
   compares + 3 atomic loads. All callbacks only log.
2. `g_guestBaseTls` is `constinit`: without it clang (MSVC ABI) emitted the on-demand TLS init
   guard (`__dyn_tls_on_demand_init` call + guard byte + TLS slot loads) at EVERY access, which
   also forced ctx/base spills around each one. PPC_FUNC_PROLOGUE now refreshes `base` once per
   entry (`sr_gb`); PPC_GUEST_BASE() is that register.
3. aliasGuestVA's physical-window slow path is one out-of-line `preserve_most` function
   (`aliasGuestVASlow`) instead of a copy (with a thread-safe-static guard) at every site.
   Measured cost of the fold with this build: **aliasGuestVASlow = 3.0% self of the guest
   render thread** in gameplay (r_b4rt_prof), + one inline cmp/jae per access.
4. Guest lib built with `-mssse3` (CMake `SR_GUEST_ISA`, default ssse3): XenonRecomp lowers
   every VMX byte swap / vperm / lvsl / vsldoi to `simde_mm_shuffle_epi8` / `alignr_epi8`
   (42,956 sites), which simde emulated with a 16-byte loop on SSE2. Integer shuffles:
   bit-identical. SSE4.1 (dpps for vmsum, 2,127 sites) and AVX2/FMA stay off.

Gates (cumulative build b4): L6 54/0; golden_call recomp outputs BYTE-IDENTICAL to dev's for
types/gold/aabb/ray_s/ig_s (ig_s = the accepted 4 w-lane cases vs the oracle); 48 difftest
suites equal exit codes (arena-wait-test failed 1/3 on dev's exe under load, 3/3 pass on
b4); replay menu 2.06 / FMV x4 0.00 (same colours as dev); intro movies clean (b4 movie
roll, 40-present stride: `archive/v1_perf2/sheet_movie_b4_first50.png`); 12/12 gameplay rolls
SUCCESS, 0 ICALL_FAIL; forcing count 2 = baseline.

Gameplay rolls (`perf_run.py --scenario gameplay --config port`, 1280x720 internal; displayed
fps = era_fps median; other processes' busy cores in brackets; `archive/v1_perf2/r_*`):

| Build | title | menu | ingame (p50 ms) |
|---|---|---|---|
| base = dev 0764a8b9 | 31.3 / 23.4 | 18.7 / 17.85 | 17.0 (58.1) [2.9] / 16.0 (61.0) [7.2] |
| b1 watches out | 32.7 / 33.25 | 20.6 / 23.2 | 17.9 (54.4) [7.2] / 21.2 (46.7) [4.6] |
| b2 + constinit base | 31.9 | 21.9 | 21.85 (45.1) [2.8] |
| b3 + alias out of line | 28.65 / 37.1 | 21.5 / 23.8 | 20.0 (48.9) [7.1] / 21.5 (45.6) [4.1] |
| b4 + ssse3 | 30.85 / 29.9 | 25.45 / 21.1 | 21.5 (46.0) [4.6] / 19.8 (49.7) [7.4] |
| b4 + SR_PM4_RENDER_THREAD=1 | 47.1 / 37.55 / 42.3 | 31.35 / 30.4 / 29.75 | 28.15 (35.0) [8.5] / 30.0 (32.7) [4.5] / 31.1 (31.8) [3.0] |

Rebased on dev 5bc80bd0 (the native lane's constant / type-0 leaves ON), quiet machine
(1.5 other cores), same scenario: dev exe 20.7 / 21.1 / 21.1 / 21.3 ingame (p50 46.5-47.0
ms); perf2 22.35 / 22.9 (b4 rebased) and 22.9 / 22.8 (b7 = + aliasGuestVASlow next to the
fold, p50 43.3-43.7 ms). The custom MAIN MENU varies 21.95-26.6 fps on dev across six rolls
(7-8 two-second windows per roll); the first unprofiled perf2 menu rolls read lower (21.75-25.6),
so it was re-measured in controlled pairs: profiled (hz 100) dev 23.4 / b7 23.95 and
SR_PM4_DIAG_TIMING dev 21.95 / b7 23.45, with identical CP cost per swap in both
(menu: CP idle 2-5 ms per 120 swaps, ib 16-20 + swap 22-25 ms per frame = CP-bound).
Ingame in the same pairs: 18.5 -> 20.9 (profiled), 19.4 -> 20.65 (timing).

Without the render thread every perf2 build stops at ~21 fps: the command processor (ib +
swap, ~45 ms/frame) is the wall, so the guest-side steps cannot be told apart by fps (b1 alone
reaches it in one roll). With SR_PM4_RENDER_THREAD=1 ingame is 28-31 fps and the profile
(r_b4rt_prof, ingame) changes shape: the guest render thread spends 25.4% in
GpuRingWaitProgress_sub_820F33E8 (waiting for the GPU ring), the host render thread is 100%
busy (xlatBindBatchTextures 25.0%, XXH3 9.7%, driver), the CP ~75% (pm4StageVsf 7.2%).
**SR_PM4_RENDER_THREAD=1 is NOT bakeable yet: every boot-era frame (EA/DC/WB logos, legal
screen, title movie) presents flat brown (80,60,55)** until the first loading screen
(`archive/v1_perf2/sheet_movie_b4rt_first60.png`, r_b4rt_1 frames 001-003); the title, menus,
opening FMV and gameplay are correct.

**Merged** (dev 32e7f82b, main-tree build): confirmation roll r_m_1 ingame 22.0 (p50 44.7 ms),
r_mtr_1/2 22.4 / 22.5, SUCCESS, 0 ICALL_FAIL.

Render-thread follow-up (perf/perf2 b2835225, the path stays default OFF):
- Found and fixed: the shm texture prepass ran on the render thread while the CP drained the
  next set; both stamp one page table and the prepass's collect check used the CP's live
  epoch, so CPU-rewritten pages were skipped (boot dev log: 568 vs 6,692 pages requested per
  frame, movie-plane uploads 0.01 vs 1.42 MiB). The prepass now runs on the CP at submit
  (copies queued, recorded by the render thread first): 6,693 pages / 1.42 MiB per frame,
  as on the synchronous path (r_rtp_movie). Gameplay with it: title 50.2, menu 34.25,
  ingame 32.1 fps (r_rtp_1), frames after the boot intro correct.
- Still broken: the boot intro stays flat brown under the render thread
  (`sheet_movie_rtp_first50.png`). Signals that differ from the synchronous boot:
  `[FRAME_CONT] drain ended MID-FRAME (framesDrained=7 avail=7)` on every drain (6,500x, 0x
  synchronous: the HLE draw queue holds 7 boundary markers from early boot that never advance)
  and `[CRITSEC_STALL]` on CS 0x05AE9A98 (thread 58 waits on 59, LR 0x825FC134). Next: find
  why the boot frames are not drawn while the draw counts match (PM4_FRAME draws=29
  resolves=26 per frame in both modes).
- Rejected: a per-slot bind memo in xlatBindBatchTextures (SR_PM4_BIND_MEMO, fetch bytes +
  texture-cache generation). Replay identical (menu 2.06, FMV 0.00, gp1 4.59 on and off), but
  no fps change: ingame 22.5 / 21.6 vs dev 22.4 / 22.5, RT 31.7 vs 32.1 - the generation
  moves at every texture (re)validation, and the per-frame invalidateAllTextures clears it.
  Not merged.

### 2.5 The render thread, baked ON (perf2, 2026-10-04)

The boot-intro defect was bisected with 50-60 s boot rolls at a 40-present dump stride
(`archive/v1_perf2/r_*_frames`): flat brown with every shm sub-gate (PRESENT, PREPASS, COLLECT,
READBACK, GPU_AUTH, ARRAY, ARRAY_MIPS), with the HLE mirror, without frame continuation, in the
natural config; correct with SR_PM4_SHMEM=0 (slow) - and correct whenever the render thread
starts after the first present (SR_PM4_RT_AFTER=1, 30, 400, 1200: r_t_after*). Only the very
first PM4 frame - the set that carries every command since boot (56 batches, 16 new render
targets, a 424 ms drain) - must not render on the render thread; which first-frame state it
mishandles is still open (the movie decode, resolves, uploads and shader attaches match the
synchronous run line for line). Shipped: the render thread starts at the second present
(SR_PM4_RT_AFTER, default 1) and is default ON. Also in the render-thread path: the texture
prepass runs on the CP at submit, and render-side shm validations are stamped with the frame's
own first epoch (not the CP's live one) so a later frame cannot accept them.

Validation (exe snapshot 34662f4886d4-perf2-b13-rtdefault, quiet machine): whole-run title roll
(240 s, 200 frames: boot logos, legal, title movie, title city, attract mode;
`sheet_rt_whole_0.png`, `sheet_rt_whole_1.png`) all correct; 3 gameplay rolls SUCCESS, 0
ICALL_FAIL, frames correct (menu, opening FMV, cutscene, gameplay: `sheet_rt_gameplay3.png`);
replay menu 2.06 / FMV x4 0.00 / gp1 4.59 (the replay is synchronous). Per-era fps:

| | title | menu | ingame (p50) |
|---|---|---|---|
| render thread (default) | 37.5 / 41.2 / 42.1 | 34.6 / 36.8 / 30.4 | 31.6 / 31.5 / 32.0 (31 ms) |
| SR_PM4_RENDER_THREAD=0, same exe | 39.6 | 26.4 | 22.3 (43.9 ms) |

Next limiter: with the render thread the guest render thread spends ~25% in
GpuRingWaitProgress (waiting for the GPU ring) and the host render thread is busy. CORRECTION
(same day): the host profiler keys every frame by its FUNCTION's begin address (kernel/
host_profile.cpp `rootBegin`), so "all samples on the first instruction" was by design, not an
artefact: `xlatBindBatchTextures` 25% self is real function-level time (inlined code included).
`SR_HOST_PROFILE_RAWPC=1` now keeps the innermost frame's exact rva for line-level attribution
(WPR/ETW needs an elevated shell - a UAC prompt would take the owner's foreground - so the
in-process sampler is the line profiler).

Line-level profile (SR_HOST_PROFILE_RAWPC=1, 250 Hz, r_lp_1 ingame): 27.6% of the host render
thread sat on ONE line, d3d12_backend.cpp `sharedTex[16 + t] = ... | sharedTex[t]` - a read-back
of a value just written into the constant-buffer UPLOAD heap (write-combined memory: an uncached
read that also flushes the WC buffers, per texture slot per batch). Fixed (kept in a register):
xlatBindBatchTextures 29.7% -> 2.5% of the render thread, which drops from 95% to 64% busy
(r_wc_prof). Ingame 32.45 / 32.2 vs dev 31.5 / 31.6 fps (r_wc_*, r_mt_*): the CP is now the
limiter (100% busy): XXH3 page hashing 8.6%, pm4StageVsf 7.4%, physwin window lookups
(inMapLive + commandBufferWindow) 10.4%, Pm4Engine::execute 4.6%, gpuHangWdNoteIb 3.5% (walks
every IB again for the hang report), pm4NoteIbRange 3.1%. The guest render thread waits ~45% in
GpuRingWaitProgress. Replay unchanged (menu 2.06, FMV x4 0.00, gp1 4.59).

Latent defect (open): rendering the FIRST PM4 frame on the render thread (SR_PM4_RT_AFTER=0)
leaves the whole boot intro flat brown; the shipped default starts the thread at present 2.
Repro: `perf_run.py --scenario title --seconds 60 --gates SR_PM4_RT_AFTER=0,SR_PRESENT_DUMP=1,
SR_PRESENT_DUMP_SPARSE_EVERY=40` and look at the frames.

Frame-list ledger (8944afcd): the 4x "[FRAME_LIST] TID MISMATCH" every render-thread roll logged
(the log cap) were the handover - the ledger latched the CP tid at the first frame (rendered
synchronously, above) and flagged every render-thread touch after. It now re-bases when the render
thread starts and names the site: 0 mismatches (archive/v1_perf2/r_tid_boot, r_tid_g1).

### 2.6 The command processor (iter-466 cp, branch perf/cp)

Gameplay scenario, port config, render loop UNLIMITED, 1280x720 internal; medians of the ingame
era (`tools/player/era_fps.py`); CP ms per frame = `[GPU_CP_TIME]` ib + swap per swap. Every
change is its own SR_* gate; the A/B is the same exe with all of them on vs off, interleaved.

| | c1_off / c2_off | c1_on / c2_on |
|---|---|---|
| ingame fps | 38.0 / 38.3 | 52.3 / 51.2 |
| CP ib + swap per frame | 21.5 + 4.5 / 21.4 + 4.3 = 25.9 / 25.7 ms | 8.5 + 1.4 / 8.1 + 1.3 = 9.9 / 9.4 ms |
| CP idle per frame | 0.02 ms (the CP is the frame) | 9.2 / 10.1 ms (it waits) |
| host render thread | 14.2 / 13.9 ms | 7.25 / 7.1 ms |
| GPU busy | 8.5 / 8.2 ms | 6.3 / 6.5 ms |
| guest render tick (FGGameRender) | 26.0 / 26.2 ms | 19.0 / 19.6 ms - now the frame |
| shm pages hashed per frame | 33,050 / 35,082 (repeat across drains 26,790 / 28,859) | 191 / 184 (clean skips ~37-39K) |
| title / menu fps | 48.4 / 43.2, 59.8 / 47.0 | 59.1 / 57.2, 59.5 / 56.5 |

Rolls `archive/v1_cp/c1_*`, `c2_*` (exe snapshot `db6e49e036a7-cp-g6`: perf/cp on native/n0, whose
guest `ppc_context.h` predates perf2's - the A/B is same-exe; the dev-based re-run is below).
Correctness: `k1_check` (all on + SR_SHM_WT_CHECK=1: every page a skip or a rebaseline keep would
trust is hashed anyway) 370,749,828 checks, 0 stale; 0 ICALL / crash markers in all six rolls;
whole-run frames (`archive/v1_cp/c1_on_sheets`: EA HD and WB intro movies, loading, title, MAIN
MENU, the opening FMV, gameplay) match the off roll's eras. Write tracking per run (~10.5K frames):
~5.3K faults releasing ~85K pages, ~10K protect runs, 1 host-write release (NtReadFile), 110M
rebaseline pages kept, 361K texture marks kept across presents vs 47K frame-only.

Where the CP went (line profile b4_g before, p1_prof after): page validation (XXH3 + the pool
wakes + lookups, ~35%) is gone - 99.5% of the requested pages are write-tracked clean; vertex
staging lost its physwin lookups, the strip loop (SSE4.1) and the per-request page walks (range
memo); the type-0 decode is one pshufb copy. The CP now waits half its time; the rest is
emitDraw/drain copies (~7% of samples), first-per-drain physwin range lookups (~7%), lock
traffic. The guest render thread (slot17) is the frame: busy, not spinning; its top platform
cost is `isGuestVAInRange` (10.6%, from the native constant leaf `setShaderConstantF` and its
`hostRange`) - native lane.

Dev re-run (batch 2, `archive/v1_cp/d*`; perf/cp rebased on 2cd51956, exe `83b3ee51d2f8-cp-t2` with
the gates baked ON, OFF = every gate =0; the port config now paces at the 60 Hz monitor target,
`[HIGHFPS] mode=monitor target=60`): ingame d1_off / d2_off 34.2 / 36.6 fps -> d1_on / d2_on 59.9 /
59.8 (at the target; frameWait 8.1 ms), title / menu 59.3-59.6 / 59.8-59.9 on vs 48.6-59.1 / 37.5-40.2
off. SR_CP_VSF_CBDEDUP + SR_CP_IB_CACHE (d1/d2 on) vs without (d3_on1): 59.9 vs 59.9, render thread
8.0 vs 8.0 ms - no measurable gain at the cap (index conversions reused 59%, 5.2M fetch buffers
rebound per run), so both stay OFF. k2_check (all on + check): 346,769,009 checks, 0 stale; 0 ICALL
in all six; whole-run frames d1_on (`archive/v1_cp/d1_on_sheets`: EA HD, WB, loading, title, MAIN
MENU, opening FMV, gameplay + tutorial prompt) correct. Replay: the baked exe's defaults vs every
gate =0, 9/9 byte-identical (`archive/v1_cp/replay_r13_t2.txt`).

### 2.7 The next frame: the guest render tick (for the next lane)

With the CP idling, the frame is FGGameRender on the guest render thread (`guest:slot17`, ~19 ms of
real work per frame uncapped, 100% busy; batch-1 profile `archive/v1_cp/p1_prof`, 250 Hz, ingame;
`tools/perf/hot_lines.py <dir> --thread slot17 [--inlines]`). Inclusive: sub_82776038 77.6% (the
frame's render walk) > sub_82783948 50.8% / sub_827758C8 47.5% > sub_8261CED0 26.4%, sub_82316958
24.4%. Self time is flat across guest functions (top: sub_8231A5F8 1.8%, sub_826092B8 1.5%,
sub_8231AD20 1.5%, sub_823055A8 1.4%, `__savegprlr_22` 1.0%); the platform helpers are the one
concentrated block:

| helper | self | note |
|---|---|---|
| `sr::kernel::isGuestVAInRange` (guest_context.cpp:329-330) | 4.1% (10.6% with its inlined sites, line profile) | called from the native constant leaf below |
| `sr::native::leaf::setShaderConstantF` + its `hostRange` | 3.1% + 1.3% | per-call range checks over guest memory (native lane) |
| `sr::kernel::getEffectiveGuestMemory` | 1.6% | TLS / global base lookup per call |
| `aliasGuestVA`, `hle::blockWriteOnce`, `upDrain` | 0.7%, 0.7%, 0.4% | |

(Done in 2.8.) Next: hoist the range check out of the per-constant path (one check per SetShaderConstantF call,
or a cached arena bound), then codegen (`__savegprlr/__restgprlr` inlining, LTO/PGO) for the flat
remainder. The main thread (slot0) spins in NtYieldExecution with `incrementMainThreadCallCount`'s
`strcmp` at 23% of its samples - not on the frame, but a whole core.

### 2.8 The guest render tick (iter-466 tick, branch perf/tick)

Gameplay scenario, port config, `SR_HIGHFPS=uncapped`, 1280x720 internal, medians of the ingame
era (`tools/player/era_fps.py`); "tick" = `[HIGHFPS_PACE] tick_cost_ms render` (FGGameRender's
own cost, GpuRingWaitProgress spins included). Baseline = dev 0bc5173c (exe
`83b3ee51d2f8-cp-t2`); rolls `archive/v1_tick/*`, interleaved on one quiet machine.

Profile first (`t0_prof`, 250 Hz, RAWPC, ingame, guest render thread inclusive): the native
constant leaves' host side 20.6% (PS 17.1, VS 5.0 - two out-of-line `isGuestVAInRange` per
range, 5 ranges per upload, an 8 KB stack probe, a vcruntime memcpy call), the type-0 leaf
4.6%, the HLE S6 UP / IDX producers 2.6% (their output is dropped by MIRROR1 on the PM4 path),
the guest CRT memcpy 2.5%, the CS stubs 1.8% (two QPC reads per uncontended Enter + one global
map mutex per Enter/Leave), waits 9.3% (ring + the movie AV-sync KeDelayExecutionThread).
And codegen: clang's MSVC-target default `-fms-volatile` emits every guest load / store as an
acquire / release atomic (LLVM IR) - a compiler barrier at every guest access, so every live
`ctx` field was reloaded and re-stored around each one.

| Change | Gate (now) | Evidence |
|---|---|---|
| guest lib `-fno-ms-volatile` + `pure` aliasGuestVASlow | CMake SR_GUEST_ISO_VOLATILE (ON) | b1/b2 (ISO only, gates off) vs a1/a2: ingame 78.9 / 76.5 vs 73.9 / 76.15 fps, tick 12.46 / 12.9 vs 13.34 / 13.12 ms; L6 54/0; golden_call types/gold/aabb/ray_s/ig_s byte-identical to the dev guest lib, same oracle diffs (ig_s 4 w-lane cases) |
| native leaves skip the vacuous range checks (reservation >= 2^32+4: the check can only be true), inline vec4 copy; check buffer thread_local | SR_NATIVE_LEAF_FASTRANGE (ON) | constant leaves 20.6% -> 12.7% of the thread (p1 vs t0_prof) |
| RtlEnterCriticalSection try_lock before the stall timer; the lock-free CS lookup (iter-464, never A/B'd until now) | SR_CS_TRYFIRST, SR_CS_FASTMAP (ON) | CS stubs 1.8% -> 0.1% |
| S6 UP / IDX producers follow the HLE mirror (as S6_TEX since iter-465); `[GATES]` prints the effective TEX / UP / IDX | SR_S6_FOLLOW_MIRROR (ON) | 2.6% -> 0; `[GATES] S6_TEX=off S6_UP=off S6_IDX=off` under PM4_LIVE |
| guest memcpy (0x8279EC60) as one host memcpy when contiguous + disjoint; same stack word, r3 | SR_NATIVE_MEMCPY (ON), _CHECK=1 verifies | 2.5% -> 0.1% |

Rolls (ingame fps / tick ms / CP idle ms per swap):

| | rolls | ingame fps | tick | CP idle / ib / swap | title / menu fps |
|---|---|---|---|---|---|
| dev | t0_base, a1, a2, a3 | 76.55 / 73.9 / 76.15 / 71.75 | 13.11 / 13.34 / 13.12 / 13.92 | 1.9-2.1 / 9.2-10.2 / 1.4-1.5 | 111.9-140.9 / 77.25-97.6 |
| ISO codegen only | b1, b2 | 78.9 / 76.5 | 12.46 / 12.9 | 1.1-1.8 / 9.1-10.3 / 1.5 | 101.2-124.8 / 79.9-82.5 |
| all on (env) | c1, c2 (p1 profiled 84.6) | 81.6 / 83.4 | 12.49 / 11.85 | 0.02-0.03 / 10.0 / 1.5 | 116.3-167.1 / 91.2-95.7 |
| baked (no env) | d1, d2 | 80.3 / 78.2 | 12.47 / 12.52 | 0.02-0.04 / 10.4 / 1.6 | 128.2-143.6 / 83.0-90.1 |

**The command processor is the wall again**: with these changes it never idles (0.02 ms per
swap; ib 10.0-10.4 + swap 1.5-1.6 = ~12 ms), so the guest thread spends the difference in
GpuRingWaitProgress and the type-0 leaf's segment switch (`p1`: waits 10.3% of the thread,
ring 8.2%). Render-thread work without the waits, profiled pair: t0_prof 14.32 ms x 90.7% =
13.0 ms -> p1 11.84 x 89.7% = 10.6 ms (-18%). 120 fps needs both under 8.3 ms: CP -30% and
guest -22%.

Correctness: k2 (baked + SR_NATIVE_CONST_LEAF_CHECK / TYPE0_CHECK / MEMCPY_CHECK, dev log):
constant leaves 685.8M calls re-run by the guest body, checkBad 0, fallback 0; type-0 33.6M, checkBad 0 (136K chunked calls take the guest body); memcpy 21.0M native, 20.97M re-checked (<= 64 KB), checkBad 0; the roll reached gameplay (ingame 62.5 fps with the checks). Replay (`tools/native_replay_check.py`, baked vs dev exe, 9 traces): 9/9 byte-identical (`archive/v1_tick/replay_bake1.txt`: menu, eahd, t16, t21, t25, gp1, gp2, lg1, lg2).
0 ICALL_FAIL in all 14 rolls; forcing audit UNGATED 2 (baseline), DEFAULT-ON 10 -> 11 sites (the
native memcpy's host writes, a platform-fix in OVERRIDES.json like the constant leaves).
Whole-run frames (`archive/v1_tick/c2_sheets`, `a2_sheets`): EA / WB / legal intro movies, title,
MAIN MENU, opening FMV, loading, tutorial gameplay, pause menu match the dev roll.

Open, pre-existing (not this change): the opening FMV (movie #7) presents at ~3% brightness for
most of its 30 s in uncapped rolls - the image is right, scaled to channel values 1-8
(`archive/v1_tick/c2_dark_stretch.png`). Dev exe too: t0_base and a1 dim, a2 and t0_prof bright;
this branch dim in b1, b2, c1, c2, k1, p1. Movie / highfps lane.

Next (guest side): the remaining profile is flat guest code. BoundsVolumeIntersectTest
(0x8231A5F8, 4.0%) is a splice body and ClothUpdateElementLightMask (0x823055A8, 2.6%) is kept
ctx-style by the v64-v127 rule - both could be localized (the splice is a stale PPC_GUEST_OFFSET
hand edit; the v64 rule was meant for the codec asm cluster). Guest PGO plumbing is in
(CMake SR_GUEST_PGO=gen|<profdata>, periodic profile dump for killed rolls); not trained yet.

### 2.9 The command processor, round 2 (iter-466 cp, branch perf/cp2)

Gameplay scenario, port config, `SR_HIGHFPS=uncapped`, 1280x720 internal; ingame-era medians from
`tools/player/era_fps.py` (CP idle / ib / swap per swap = `[GPU_CP_TIME]`). Same exe (snapshot
`af94afcf8604-cp2-r1`, perf/cp2 on dev cbba401b), every new gate on vs every new gate =0,
alternating; rolls `archive/v1_cp2/g*`.

Profile first (dev fed4872e, `archive/v1_cp2/q_prof`, 250 Hz RAWPC, folded stacks): pm4ShmRequest
19% of the CP inclusive (physwin window answers ~12%: gpuWindowRange's uncached std::map walks 7%,
grant-log scans, command-buffer rescans; two steady_clock reads per request 2%; m_shmMu taken twice
per request 2.5% + waits 1.7%); copies (memcpy / memmove) ~19% (drain 6.6%, emitDraw 5.7%, VSF 1.9%);
the swap's texture prepass 9.2%; shader loads 3%; per-draw locked log counters + a census mutex ~4%
(a locked instruction also waits for the draw's write-combined upload stores); the xlat DrawBatch
zeroed on the stack and copied ~3%; regLogWatch's function-static guard 1.5%.
Replay measurement (lg1 / gp1): a gameplay draw reads ~12.4 of 32 float-constant blocks; 62% are
written between two draws, 18% change value (the game re-sends constants unchanged).

| | g1_off / g2_off | g1_on / g2_on | profiled g_poff / g_pon |
|---|---|---|---|
| ingame fps | 92.4 / 95.3 | 92.95 / 101.5 | 86.3 / 92.7 |
| CP ib + swap per swap | 9.20 + 1.38 / 8.95 + 1.36 = 10.58 / 10.31 ms | 7.29 + 0.93 / 6.42 + 0.89 = 8.22 / 7.31 ms | 11.04 / 8.23 ms |
| CP idle per swap | 0.04 / 0.07 ms (the CP is the wall) | 2.22 / 2.38 ms (it waits) | 0.02 / 2.43 ms |
| guest render tick | 10.62 / 10.52 ms | 10.64 / 9.82 ms - now the frame | 11.55 / 10.74 ms |

CP busy per frame -22..29% (10.3-10.6 -> 7.3-8.2 ms). fps moves little because the guest render
tick (~10 ms) is the frame as soon as the CP waits; next lane = the guest tick (2.7, 2.8).
Profiled pair, CP samples per swap (`archive/v1_cp2/cmpprof.py g_poff g_pon`): pm4ShmRequest 1.11 ->
0.72 ms, physwin 0.85 -> 0.35, onShaderLoad 0.21 -> 0.04, clock reads 0.15 -> 0.01, m_shmMu 0.26 ->
0.05, drain lambda 2.31 -> 1.83, copies 2,694 -> 2,436 samples (const uploads down, cvMerge +312).

| Change | Gate (baked ON, =0 A/B) |
|---|---|
| pm4ShmRequest's range / page window answers memoised per thread across drains, versioned by per-64 KB-physical-page mutation counters (registry mutations + command-buffer notes and evictions, wrap-aware) | SR_CP_OWN_MEMO (SR_CP_OWN_MEMO_CHECK=1 verifies) |
| no clock reads per request; a lock-free "all pages done this drain" pre-check | SR_CP_SHM_LEAN |
| per-draw log counters bumped without locked instructions; the PS census mutex skipped once full | SR_CP_DRAIN_LEAN |
| xlat DrawBatch built in its vector slot | SR_CP_BATCH_INPLACE |
| a decoded shader ucode (by XXH3) skips re-swap, control-flow check, census walk, bindings parse | SR_CP_SHLOAD_MEMO |
| draws carry only the constant blocks whose VALUES changed (register-file dirty bits + compare = Xenia up_to_date); the drain merges in sequence and rebinds the set's latest upload while it covers the draw (the bloom-patched tone-map PS upload is never rebound) | SR_CP_CONST_TRACK (_CHECK=1 verifies, _TEST_DROP=1 mutation) |
| a batch whose texture fetch constants repeat the previous pm4 batch's is skipped by the swap's texture collection | SR_CP_TEX_SKIPSAME |
| last found VS / PS per thread | SR_CP_SHADER_MEMO |
| SET_CONSTANT / LOAD_ALU_CONSTANT runs as one pshufb copy | SR_CP_SETCONST_SSE |
| a drain raises only its first batch's pending-copy end (the render's cursor never goes back) | SR_CP_PEND_FIRST |
| regLogWatch read once at static init | (no gate) |
| per-thread physwin memo 2048 -> 16384 entries; constant upload dedupe by compare | SR_CP_PHYSWIN_MEMO_BIG, SR_CP_CONST_DEDUP (OFF: superseded by OWN_MEMO / CONST_TRACK) |

Correctness: `g_check` (all on + SR_CP_OWN_MEMO_CHECK, SR_PHYSWIN_MEMO_CHECK, SR_SHM_WT_CHECK,
SR_CP_CONST_TRACK_CHECK): 377.3M skipped pages hashed, 0 stale; own memo mismatch 0; const track
gaps 0, mismatch 0; mutation SR_CP_CONST_TRACK_TEST_DROP=1 (replay lg1) -> mismatch 64. 0 ICALL / crash
markers in all 7 rolls. Replay (`tools/native_replay_check.py`, 9 traces): every gate on vs off 9/9
byte-identical (also with SR_GFX_BLOOM=0 / 200 on menu gp1 gp2 lg1); the baked exe's defaults vs every
gate =0 9/9 byte-identical. Whole-run frames `archive/v1_cp2/g2_on_sheets` (EA / WB / legal intro
movies, title, MAIN MENU, opening FMV, loading, tutorial gameplay + prompts) match `g2_off_sheets`; the
opening FMV's dim stretch is the pre-existing 7.5 issue (g2_off dim, g2_on bright).

### 2.9 The guest render tick, round 2 (iter-466 tick2, branch perf/tick2)

After cp round 2 (f209a59a) the CP idles ~2.5 ms per frame and the guest render tick (~10.4 ms) is
the frame. Same method as 2.8 (gameplay, port, `SR_HIGHFPS=uncapped`, ingame medians, rolls
`archive/v1_tick/t2*`). Line profile of dev (`p2`, guest render thread self): the native constant
leaf ~17% (copy, the two out-of-line TLS getters behind every call 1.6%, a lock-prefixed counter
1.1%), BoundsVolumeIntersectTest 0x8231A5F8 9.96% and ClothUpdateElementLightMask 0x823055A8 5.7%
(both ctx-style), then flat localized guest code.

| Change | Gate / mechanism | Proof |
|---|---|---|
| constant leaf: range-vacuity answer cached per thread (keyed by the guest base), one shared counter add per 4096 calls | SR_NATIVE_LEAF_TLCACHE (ON, =0 A/B) | leaf check 687.9M calls 0 bad (t2k3) |
| BoundsVolumeIntersectTest leaves the splice list (its only hand edit was PPC_GUEST_OFFSET on lvx/stvx, which assemble.py now applies to every generated body) -> generated + localized | tools/regen splice list | differential check (localized vs the retired ctx-style body, same input context, every call; `archive/v1_tick/localize_check`) 139.5M calls, 0 mismatches |
| ClothUpdateElementLightMask localized despite the v64-v127 rule (v122-v127 are its own callee-saved scratch) | `[localize] localize_despite_regs` | fresh `--solve`: exactly these two bodies change, no bridge (regen_t0..t2); cape renders right (t2d3 sheets) |

Deploying: every checkout's untracked `guest/ppc` takes the two bodies with
`python tools/regen/flag_transform.py --deployed guest/ppc` (tools/regen/README.md). L6 54/0 and
golden_call byte-identical (same oracle diffs) on each build; replay 9/9 byte-identical; 0 ICALL;
forcing ungated 2.

| | rolls | ingame fps | tick ms | CP idle ms/swap |
|---|---|---|---|---|
| dev f209a59a | t2b1-3 | 94.7 / 97.6 / 96.1 | 10.74 / 10.27 / 10.39 | 2.5-2.8 |
| + leaf cache | t2l1-2 | 95.9 / 100.9 | 10.31 / 9.73 | 2.2 |
| + Bounds localized | t2c1-3 | 100.2 / 103.7 / 93.5 | 9.96 / 9.71 / 10.48 | 1.7-2.2 |
| + cloth localized (= merged) | t2d1-3 | 102.2 / 101.25 / 103.2 | 9.69 / 9.81 / 9.73 | 1.9-2.15 |

Rejected: **guest PGO** (CMake SR_GUEST_PGO, plumbing kept). The instrumented exe runs ingame at
2.5 fps (shared counters on four spinning runner threads), the merged profile is 101 MB, and the
optimised build gave no gain over the leaf cache alone (t2q1/q2 95.45 / 96.3 vs t2l1/l2 95.9 /
100.9); golden_call / L6 were identical, so it is a performance verdict.

Where the tick stands: ~9.7 ms (target 8.3). Next: the constant leaf is still the largest single
block (~12% self after the cache: the vec4 copies themselves and the dirty-word RMW, called ~40K
times per frame) - batching consecutive uploads or a native draw-state builder is the native lane's
N1; the rest is flat localized game code (MathVmxDotMinReduce 0x82319108 3.1%,
RenderStateBlock_ApplyShaderParams 0x826092B8 3.1%, 0x822F5990 4.9%, 0x822FC7C0 3.3%).


### 2.10 The constant upload, natively (iter-467 native N1 phase 1, branch native/n0)

Sizing (`archive/v1_native/n1_callers`, SR_NATIVE_LEAF_CALLERS=1, 1.01e9 leaf calls): ~45K
SetShaderConstantF calls per ingame frame, 89% from two engine functions, almost all count 1:
ShaderPs_UploadAllConstants 0x8260B660 (PS c1 72.8%, c2 1.3%) and ShaderVs_UploadAllConstants
0x8260B2A0 (VS c1 16.3%, c129 1.4%, c3 1.0%); the rest are semantic callbacks (0x82618F60,
0x8260CED8, 0x8260D220, 0x8261B050). Both run four 0-terminated lists from the shader binding
(F, I, B, then callbacks); SR_NATIVE_CONST_UPLOAD runs the F list as host code (the leaf's copy +
dirty OR per entry, one shadow range check per call instead of five per entry plus the wrapper and
the guest loop) and keeps the I/B lists and callbacks as the guest's own calls in its order.

Proof: SR_NATIVE_CONST_UPLOAD_CHECK=1 (`u_chk`: native pass on a copy, guest body for real, its F
calls and the shadow right after them compared) 25,427,968 calls, 0 bad, 0 unchecked, every era;
projection checkDraw (`u_rec0` OFF / `u_rec1` ON) registers, ALU constants and bool/loop 0
mismatches in both (fetch slot 31 only, the by-design UP slot; one VS-identity event on vsObj
40099CE0, the object the OFF roll already mismatches 32x as vspost); replay 9/9 byte-identical
(`u_replay`); whole-run sheet (`u_frm_sheet.jpg`) every era incl. the intro; 0 ICALL; forcing 2.

| same exe 07933306d6f4, interleaved | rolls | ingame fps | tick ms |
|---|---|---|---|
| SR_NATIVE_CONST_UPLOAD=0 | u_a1-3 | 95.4 / 97.2 / 97.5 | 10.39 / 10.33 / 10.31 |
| SR_NATIVE_CONST_UPLOAD=1 | u_b1-3 | 107.8 / 109.1 / 109.0 | 9.25 / 9.17 / 9.18 |

**Phase 2 (skip redundant uploads) - SHELVED, do not re-open without new numbers.** Sizing roll
`s1_stats` (SR_NATIVE_CONST_UPLOAD_STATS=1, 1.006e9 F entries): 57.2% write bytes the shadow already
holds with every group of their mask already dirty (exact no-ops), 21.1% write equal bytes but dirty
a clean group, 21.7% write different bytes. Skipping the 21.1% would drop re-uploads, a PM4 change,
for at most a share of the downstream type-0 emission (`p1_prof`, guest render thread self:
emitType0RegPacketsVec 1.6% + sub_821078C0 1.1% = ~2.7%, i.e. <= ~0.15 ms if half the dirtied
groups are redundant). And a clean group is NOT guaranteed GPU-resident: the game's own direct
constant packets (B2, D3DDeviceEmitPm4Packet_sub_82113010, e.g. Lion's c96-c111) write the GPU without
touching the shadow, so the guest's "redundant" re-upload is what restores the shadow value there.
Small gain, real residency risk: not built. The exact no-ops (57.2%) can be skipped without any
PM4 change (same bytes, dirty bits already set), but with the dirty ORs batched an exact no-op
costs only its 16-byte copy, and skipping it needs a compare plus a 57/43 data-dependent branch.

**The native F loop is at its memory floor - lean / prefetch variants REJECTED (no gain).** Line
profile (`r1_rawpc`, SR_HOST_PROFILE_RAWPC=1, guest render thread): the upload bodies' samples sit on
the 16-byte source copy (applyF lines 521-522: 4.4% of the thread) - the ~45K parameter values per
frame are cache misses; groupMask 1.7%, the out-of-line hostRange 1.6%, entry decode ~2%. Two
variants, both byte-exact (check `p_chk` 23.7M calls 0 bad): LEAN (plain host pointers below the
alias windows, no per-entry hostRange, dirty ORs batched into one write) and LEAN+PREFETCH (decode
up to 32 entries, prefetch their sources, then copy). Same exe cb01fa93ad61, interleaved (`p_*`),
ingame fps / tick ms: native 107.2 / 108.1 (9.28 / 9.35; p_a1 94.5 = first-roll outlier), LEAN
106.5 / 108.2 (9.39 / 9.20), LEAN+PREFETCH 107.6 / 107.6 / 106.6 (9.31 / 9.39 / 9.33): inside the
noise. The prefetch has no lead time (lists average ~16 entries) and the misses are the parameter
values themselves; the code was removed. Also seen in that profile: `ntdll!NtWriteFile` 1.8% (ingame era) of the
render thread under SR_LOG=dev (perf rolls log at dev level).

**Where the owner actually stands (player log profile) [measured, `g_*`, exe dad880d45018 = dev
50044c4d, same exe, interleaved].** The perf rolls above pass `SR_LOG=dev` (every line: ~1.48M
lines per roll); the port config a player runs logs at `SR_LOG=player` (milestones only: ~3.2K
lines). Uncapped ingame fps / tick: dev 107.4 / 106.4 (9.25 / 9.55 ms), player 110.5 / 113.4
(8.88 / 8.82 ms). So the owner's number is ~111-113 fps uncapped, tick ~8.85 ms against the 8.3 ms
that 120 needs; dev logging costs ~0.5 ms of tick. **For the logging owner (no lane yet):** under
SR_LOG=dev `ntdll!NtWriteFile` is 1.8% self of the guest render thread in the ingame era
(`r1_rawpc` profile.txt) - log writes are synchronous on the render thread; compare perf numbers
only within one log profile.

**N2 native draw-flush emitters - REJECTED (correct, no gain).** GpuCmdFlushDirtyRegisterRuns
0x82107520 (dword register runs) and GpuEmitType0RegPackets 0x82107760 (fetch constants: NOP
alignment to 4 mod 8, header, 24 bytes per slot, trailer 00025000 0 00025000 0) as host code for
all-fast-path calls (commit bcb6303a, reverted a9a795b4; the semantics are in that commit's
`emitDwordRuns` / `emitFetchConsts`). PM4-byte-exact: SR_NATIVE_FLUSH_CHECK (`f_chk`: cursor rewind,
guest re-emit, byte compare) fetchConsts 16.8M + dwordRuns 19.9M calls, 0 bad (~0.04% / ~0.007%
chunked fallbacks); replay 9/9 byte-identical; 0 ICALL. Same exe fb24c0671f87, player profile,
interleaved (`f_*`): OFF 108.5 (first roll) / 111.7 / 110.9 fps (tick 9.22 / 8.93 / 8.89 ms), ON
112.3 / 110.6 / 109.7 (8.93 / 8.96 / 9.07): inside the noise (both were ~0.5-1% self of the render
thread). And a native PM4 emitter is no step toward 0 PM4 packets, so the code was not kept. The
state setters SetTextureFetchConstant 0x82100310 / SetStreamSource 0x820F2A50 (~1% self each) were
not attempted: they carry HLE observe / MIRROR hooks, and their resource-fence branch ORs in an
uninitialised stack bit.

### 2.11 The guest render tick by subsystem, and codegen round 3 (iter-467 guestcode, branch perf/guestcode3)

Retarget (owner, 2026-10-07): 240 fps uncapped with real frames, i.e. a guest render tick of
<= 4.17 ms (today ~8.85 ms on the player profile). Single-thread codegen tweaks are now measured
against that.

**Where the tick goes** (dev 94193c3b, exe 8bfb22dba4c8, player profile, uncapped, raw-PC
profile `archive/v1_gc3/p0`, 250 Hz, guest render thread = guest:slot17). CORRECTED 2026-10-07
(strand A): the first cut of this table took a window that reached back into the in-engine
movie before play (perf_run 91-143 s), which put the movie's fence waits (MoviePlayerRenderStateTick
/ GpuBlockOnFence 0.58 ms, BlockUntilIdle ~128/s while an XMV is open) into the gameplay tick.
Re-cut on the PURE ingame window, host-profile files 17-24 (160-240 s; the ingame era starts at
143.5 s), 7,640 stack samples; the profiled frame was 9.81 ms at 101.9 fps, the right column
scales to the unprofiled 8.85 ms tick. Each sample goes to the innermost subsystem on its stack
(`python tools/perf/size_tick.py archive/v1_gc3/p0/perf --from 17 --to 24 --frame-ms 9.81 --tick-ms 8.85`):

| Subsystem (innermost frame class) | share | ms @9.81 | ms @8.85 |
|---|---|---|---|
| constant upload (native N1 F lists + leaf: ShaderPs/Vs_UploadAllConstants, sr::native::leaf) | 18.6% | 1.83 | 1.65 |
| culling: BoundsVolumeIntersectTest + BoxOverlapTest + MathVmxDotMinReduce | 13.8% | 1.36 | 1.22 |
| render walk: bins / nodes / scripts (rest of Renderer_DrawFrame) | 10.5% | 1.03 | 0.93 |
| cloth sim (ClothUpdate, Cloth_UpdateFrame/Simulate/Verlet/SatisfyDistanceConstraints) | 9.9% | 0.97 | 0.88 |
| PM4 / D3D device emit (Gpu*, D3DDevice*, host kernel/gfx) | 9.5% | 0.93 | 0.84 |
| render walk: draw emission (RwRenderObject*, RwDraw*, ModelEmit*, Mesh*) | 9.4% | 0.92 | 0.83 |
| cloth light mask (ClothUpdateElementLightMask self) | 6.1% | 0.60 | 0.54 |
| state apply (RwCompiledState*, XformRenderState, StateStream) | 5.9% | 0.58 | 0.52 |
| pose / skinning (PoseSnapshot_GetWorldMatrix, RenderSkinnedMesh) | 5.4% | 0.53 | 0.48 |
| RenderStateBlock_ApplyShaderParams | 4.4% | 0.43 | 0.39 |
| waits (GpuRingWaitProgress, GpuBlockOnFence under D3DDeviceSwap's N-2 throttle, sleeps) | 3.5% | 0.34 | 0.31 |
| particles (EnvironmentRenderObj_DrawParticleLayer) | 3.1% | 0.30 | 0.27 |

Inclusive key nodes (ms @8.85): Renderer_DrawFrame 0x82776038 5.79; RenderBin_RenderBinBatched
3.07; RenderBin_UpdateAll 0x82783B48 3.03; RwRenderObjectDrawInstanced 2.88; RenderBin_RenderBinSimple
2.43; **ClothUpdateElementLightMask 0x823055A8 1.68** (its culling calls included);
ClothUpdateBoundsAndLights 0x82305B88 0.99; BoundsVolumeIntersectTest 0.96; D3DDeviceEmitDrawVertices
0.86; ShaderPs_UploadAllConstants 0.61; ClothUpdate 0.60; Cloth_UpdateFrame 0.60; DrawParticleLayer
0.45; RenderStateBlock_ApplyShaderParams 0.39; ShaderVs_UploadAllConstants 0.37; PoseSnapshot_GetWorldMatrix
0.30; PostFxRunTechniques 0.30; MathVmxDotMinReduce 0.27; D3DDeviceSwap 0.23; GpuBlockOnFence 0.23;
GpuRingWaitProgress 0.17; RenderSkinnedMesh 0.16. MoviePlayerRenderStateTick: 0 samples in this window.

Instruction-level picture of the first (wider) window's samples (`tools/perf/hot_insns.py`): 46% of samples sit right after a
load (37% after guest-memory loads: the game's data misses; ~12.5% after ctx loads, ctx.r1 alone
5%); 5.1% on prologue/epilogue register saves; 2.2% on the aliasGuestVA window checks; 1.0% at
LDMXCSR (the VMX/FPU flush-mode toggles: 2,458 of the unconditional toggles in the tree are
`lfs`, 641 `stfs`, i.e. scalar float moves interleaved with VMX math); 3.2% after a load that
reads a wider span than an earlier store wrote (store-forwarding stalls; 2.0 points of it are
`stwu`'s 32-bit `ctx.r1.u32 = ea` followed by a 64-bit read of r1).

**Codegen round 3 - REJECTED (correct, no gain):** argument-register caching (ARC: r1, r3-r10,
f1-f13, v0-v13 as locals with dirty-tracked write-back at calls/returns) and narrow-write
widening (`ctx.rN.u32 = E` as one 64-bit write) in flag_transform.py, commit 78575435, reverted
4b4cc2ff. Proof of correctness: SR_ARC_CHECK_ON build 7.63e9 checks 0 mismatches over a whole
run; golden_call recomp outputs byte-identical to dev's on all five scripts; L6 54/0; replay
9/9. Interleaved player-profile A/B (`ab3_*`, same session, three exes): dev 90.6 (first
roll) / 108.0 / 113.1 fps, widen 98.6 / 113.3 / 105.2, ARC 110.6 / 107.9 / 109.1 - noise. The
profile shows why: BoundsVolumeIntersectTest self 9.2% -> 5.3% and BoxOverlapTest 1.9% -> 0.5%
under ARC, but small hot leaves and call-heavy bodies lost as much (more callee-saved XMM
spills, entry loads, write-backs). Single-thread codegen is near its floor for this code; the
tick needs subsystems taken native (and threaded where the data is independent).

**Next (in order of size):** ClothUpdateElementLightMask with its culling callees as host code
(1.68 ms inclusive on the pure ingame window; per-element visibility / LOD / light-mask work is
independent, the bucket appends are a sequential pass - threadable with deterministic output),
cloth simulation (0.88 ms), then the render walk. Done in 2.12.

### 2.12 Native cloth light mask, cloth sim leaves and pose leaf (iter-467 guestcode round 4, branch perf/guestcode3)

Host-code transcriptions of render-thread hot paths, each a strong def of the guest function
with a default-ON gate (`=0` runs the guest body), a call-for-call CHECK gate and an in-process
timing gate (`_AB`: blocks of calls alternate implementations, TSC per call):

| guest function | file | gate (default ON) |
|---|---|---|
| ClothUpdateElementLightMask 0x823055A8 + MathVmxDotMinReduce 0x82319108, BoundsVolumeIntersectTest 0x8231A5F8, BoxOverlapTest 0x8231A408, camera getter 0x82775BF0 | kernel/native_cloth.cpp | SR_NATIVE_CLOTHMASK (_CHECK, _AB, _THREADS=N, _MINPAR) |
| Cloth_SatisfyDistanceConstraints 0x822C0360, Cloth_CollideParticlePositionSphereCapsule 0x822C0520 | kernel/native_clothsim.cpp | SR_NATIVE_CLOTHSIM (_CHECK, _AB) |
| PoseSnapshot_GetWorldMatrix 0x822F5990 (virtual slot +48 of ~9 snapshot vtables) | kernel/native_pose.cpp | SR_NATIVE_POSE (_CHECK, _AB) |

How they stay exact (kernel/native_vmx.hpp): every guest memory access the original makes
outside its own frame, at the same program point, through the same alias fold; every VMX op the
same simde expression the recompiler emits (vmaddfp = mul then add, vmsum3/4fp128 = simde's
portable dp_ps summed in host-lane order, vrefp / vrsqrtefp = IEEE divides), compiled at the
guest lib's ISA level (-mssse3, no SSE4.1 / FMA) in the guest's VMX flush mode; lfs/stfs pairs
as bit copies with the IEEE-mode SNaN quieting; fcmpu on lfs values as exact compares (unordered
= not taken); fmuls through the exact double product with an IEEE-MXCSR slow path for denormal
inputs or near-underflow results; the callees' once-init statics written exactly when the
guest writes them. Stack temporaries are register values (the dead red zone is not written);
the one store outside it that the pose leaf makes (r4 into the caller frame at r1+28) is made.
The collide transcription returns before any guest write when the shape has a one-sided plane
(that case calls sub_822E0E78) and the guest body runs.

**Proof (CHECK: guest original kept, native re-run on the restored inputs, every written byte
+ r3 compared, every era).** Exe f92974b340b7 (`cm6chk1`, single-thread): light mask 3,661,824
calls / 96.1M elements 0 bad, unchecked 0; SatisfyDistanceConstraints 20,480 calls / 18.9M
constraints 0 bad; collide 11.10M checked (339,840 plane fallbacks) 0 bad; pose 11.21M 0 bad.
Threaded (`cm7chk3`, SR_NATIVE_CLOTHMASK_THREADS=3, no host profiler): light mask 3,833,856
calls, 2,220,124 of them on the threaded pass (90.3M elements) 0 bad; pose 11.60M 0 bad; whole
run, no crash. (`cm6chk3`, the same with the 250 Hz host profiler, died at 106 s inside the
PROFILER thread - ntdll RtlVirtualUnwind2 read 0xB76D7FF488 while walking another thread's
stack, dump `archive/v1_gc3/cm6chk3_crash.dmp`; 1.18M calls / 685K threaded 0 bad before it.
A profiler race with a thread whose stack is gone, not the native code - noted for the
profiler's owner.) Whole-run sheet with all three ON (`archive/v1_gc3/cm7_frm_sheet`, 190
frames): EA / DC / WB intro movies, legal, loading, the attract flythrough, title, main menu,
the meteor and tie cutscenes, then gameplay 80-180 s with the cape visible and draped correctly
in every frame (close-ups at 130-180 s). Replay dev (8bfb22dba4c8) vs the baked exe c434411e9cc8 (`cm8_replay`):
9/9 byte-identical. 0 ICALL in every roll. Forcing audit: the native sites are own-gated; the
ungated count is dev's 2.

**What it buys.** In-process timing (`cm6abt`, `cm7abt96`, mean us per call over a whole run):

| function | guest | native | threaded |
|---|---|---|---|
| ClothUpdateElementLightMask (~26 elements per call) | 2.82 / 2.78 | 1.44 / 1.41 | 1.76 (MINPAR 16) / 1.60 (MINPAR 96) |
| SatisfyDistanceConstraints (~920 constraints per call) | 34.95 | 21.09 | - |
| CollideParticlePositionSphereCapsule | 0.053 | 0.025 | - |
| PoseSnapshot_GetWorldMatrix | 0.207 | 0.039 | - |

Interleaved uncapped player-profile A/B, same exe f92974b340b7, ingame era (119 s windows):

| rolls | ingame fps | tick ms |
|---|---|---|
| OFF (cm7_off1-3) | 110.3 / 109.3 / 109.5 | 9.07 / 9.26 / 9.07 |
| ON, single-thread: mask + sim + pose (cm7_m11-3) | 116.8 / 115.5 / 116.7 | 8.62 / 8.64 / 8.53 |

-0.53 ms of tick (mean 9.13 -> 8.60; the two sets do not overlap), +6.6 fps; 0 ICALL. The light
mask is the bulk (2x per call); SatisfyDistanceConstraints is a ~920-step dependency chain
(1.66x) and runs ~2 times a frame, so the cloth-sim leaves are worth ~0.06 ms together.

**Threading - measured, OFF by default.** The per-element pass on N+1 threads with in-order
apply is exact (above) but SLOWER per call than single-thread native: the calls are small
(~26 elements; ~300 calls a frame), a job costs a wake / check-in round trip, and the
spinning workers share physical cores with the render thread. Kept behind
SR_NATIVE_CLOTHMASK_THREADS (default 0) with the measurement in the source.

**Not taken in the cloth sim (and why):** Cloth_VerletIntegrateAndCollide's own loop is ~0.1%
of the thread; its cost is per-collider COPIES of the collider shape (Cloth_ColliderShape_CopyCtor
0x822E0F18: three Str copies = three guest heap allocations, freed again by 0x825F94E0 at the end
of each collider, ~0.3 ms) - eliding them changes guest heap state, so it is not a call-for-call
transcription and was left alone.

**CHECK-design lesson (kept in the source):** the first light-mask CHECK snapshotted count*64
bytes per LOD bucket - past the bucket's end into heap memory other threads own - and its
restore wrote stale allocator headers back (roll `cm2chk1`: one "mismatch", then the render
thread spun forever in DlmallocConsolidateBins). CHECK regions must be exactly what the
function writes (a bucket's extent = the records the guest appended), and restores touch only
the bytes that differ.

**The tick after it** (baked exe c434411e9cc8, `cm8_prof`, 250 Hz raw-PC, pure ingame window =
host-profile files 14-22, 8,375 samples, profiled frame 8.96 ms at 111.6 fps, right column scaled
to the A/B's 8.60 ms; `size_tick.py ... --from 14 --to 22`): ClothUpdateElementLightMask
inclusive 1.68 -> 0.79 ms (native: elementLoop self 4.6% of the thread - the element matrices and
bucket appends -, dotMinReduce 1.9%, boundsVolume 2.4%); culling outside it 0.09 ms; pose /
skinning 0.48 -> 0.24 ms. Now on top: constant upload 1.69, PM4 / D3D device emit 1.26 (scene
dependent: D3DDeviceEmitDrawVertices 1.28 inclusive in this roll's traffic), render walk 0.93 +
draw emission 0.89, ClothUpdateBoundsAndLights 0.76 inclusive, cloth sim 0.73, state apply 0.60,
particles 0.48, waits 0.47 (GpuRingWaitProgress 0.39), RenderStateBlock_ApplyShaderParams 0.44.
Next in this lane: RenderStateBlock_ApplyShaderParams (the parameter-table writer the constant
uploads read through) and the constant upload's parameter writers.

### 2.13 Strand A: the waits, and the draw emit (iter-467 native A, branch native/n0)

**Windowing rule.** Every tick / profile number here is the INGAME era only: `tools/perf/ab_tick.py`
prints the era window it took its median from, and `tools/perf/ingame_profile.py <roll>` runs
size_tick.py over exactly the hprof files inside the era markers of report.json (gc3
p0: ingame 143.5-242.3 s = files 16-24). perf_run's "gameplay" era (91-143 s) is the in-engine
movie before play, not gameplay.

**The "movie fence wait" is not an ingame cost.** In gc3 p0 files 17-24 MoviePlayerRenderStateTick
has 0 samples; the waits are 0.31 ms of the 8.85 ms tick, of which GpuBlockOnFence 0.23 ms, every
sample under D3DDeviceSwap (the XDK's N-2 frame throttle: the guest waits for the frame before last
to finish on our CP / render thread). The 0.58 ms in 2.11's table came from a window that reached into
the movie era (files 15-24: MoviePlayerRenderStateTick 0.11 ms, files 13-24: 0.97 ms): state 2 of
MoviePlayerRenderStateTick_8235F488 calls D3DDeviceBlockUntilIdle every tick while an XMV is open.
Measured live (SR_NATIVE_IDLE_DIAG=1, roll dd_chk): ~128 BlockUntilIdle calls/s at mean 1.04 ms
during the movie era, then < 1024 calls in ~2 min of ingame; the CP's drain before a guest-visible
fence averages 114 us (EVENT_WRITE_SHD) / 76 us (INTERRUPT). (The diag's per-caller split is void:
ctx.lr is not maintained across recompiled direct calls.) The ingame throttle wait tracks the CP /
render-thread load (both ~83-87% of a core at ~111 fps), so it shrinks with them, not with guest work.

**Stream-resident constant dedup (SR_NATIVE_CONST_DEDUP + SR_NATIVE_CONST_DEDUP_MODES, 0bfd149f /
15119d97, `kernel/native_const_dedup.*`).** The draw flush's constant upload (the native
GpuEmitType0RegPacketsVec leaf, 7.5% of the ingame tick in gc3 p0 with its memcpy) copied ~6.4 MB of
ALU constants per gameplay frame into the ring, 83% of it byte-identical to what the CP register
file already held (trace live_5400; 81.7% on the menu fixture). The leaf now skips a 64-byte group
when the value the STREAM last wrote to it is the same: a mirror of the ALU file as the stream last
wrote it (core, no PM4 knowledge), invalidated by a PM4 adapter - the segment epoch (segment start
dev+13508 / limit dev+0x2C / wrap generation dev+13500: every IB range self-contained, so replays
re-run the writes a skip relies on), the owner thread, and a header-only scan of the packets other
code wrote between emissions (type-0 / SET_CONSTANT / LOAD_ALU_CONSTANT writes to 0x4000-0x47FF
drop their groups, unknown type-3 or misframe drops all). The device shadow is NOT the reference
(phase 2's rejection stands: B2 direct packets and LOAD_ALU_CONSTANT bypass it).
Proof (exe 06714faf3e76, gameplay/port, every era): SR_NATIVE_CONST_DEDUP_CHECK=1 emits each would-be
skipped run as its own packet behind a {NOP, 'DDCK'} marker and the CP checks it against its
register file at every execution - dd_chk2 (PM4, record modes excluded) 204.8M groups 0 BAD;
dd_chkm2 (+MODES: also inside Z-pass / recording) 1.08B checks of 642M predictions (replays) 0 BAD;
dd_chknd2 (+SR_NATIVE_DRAW) 360M 0 BAD; mutation (SR_NATIVE_CONST_DEDUP_MUTATE: no scan) 304 BAD, so
the check catches a missed writer. (A first check keyed by ring address, dd_chk, reported 0.14% BAD:
stale marks under unscanned guest-body fallbacks; the same mirror logic over three traces in CP
order gives 0 - the marker design replaced it.) ON: 81.0% of groups skipped (dd_onfrm2), chunked
fallbacks 160K -> 36K; replay 9/9 byte-identical; whole-run sheets correct every era with and
without SR_NATIVE_DRAW (`dd_onfrm2_sheet.jpg`, `dd_ndon2_sheet.jpg`); 0 ICALL; forcing 2.
Without MODES only 25% is skipped: the XDK record modes (BeginZPass sets dev byte 10808 |= 0x40 and
dirties all state) cover ~23M of the run's leaf calls.

**Its cost: no gain - merged gated OFF (the core feeds strand A's record path).** Same exe
06714faf3e76, player profile, uncapped, interleaved (`ab_*`, ingame window per roll from
tools/perf/ab_tick.py): OFF 105.6 (first roll) / 111.9 / 113.1 fps, tick 9.72 / 9.02 / 8.82 ms;
DEDUP+MODES 107.2 / 110.6 / 110.3 fps, tick 9.19 / 9.07 / 8.96 ms - noise. Profiles (`pr_off` /
`pr_dd`, 250 Hz, tools/perf/ingame_profile.py, files 12-22): the "emit" category drops 1.02 -> 0.84
ms, but the leaf's own work rises (the constant-upload category, which holds sr::native::leaf,
1.54 -> 1.90 ms): emitType0Dedup 4.15% + the header scan 1.0% of the thread vs the plain copy path's
4.9% (memcpy 2.15 + leaf 1.39 + wrapper 1.35). The 6.4 MB per frame was never the cost - the source
is hot (just written by the N1 upload) and the ring writes stream; a per-group decision costs as
much as the copy. CP side (tools/perf/thread_ms.py, GPU_CP running ms/frame): Pm4Engine::execute
self 0.42 / 0.44, writeReg 0.33 / 0.33 - unchanged (the constant runs are SSE bulk copies).

**What 1a costs (the CP line for the native record).** SR_NATIVE_RECORD=1 and SR_NATIVE_DRAW=1 both
run ingame at 36-38 fps (tick 26.3-28.8 ms vs ~9.0, `ab_rec*`, `ab_nd*`): the per-draw N1
projection record (shader hashes, the ALU carry image, checkDraw) is an oracle, not a perf path. On
the CP (`pr_nd`): 25.8 ms/frame running - ndExecute 13.9 ms incl. (2.2 self), checkDraw 5.7 incl.,
aluDrain 4.8 incl. - against 8.85 ms/frame for the whole CP with PM4 (`pr_off`). The bars a lean
record must beat (ingame, ~3,700 draws/frame): guest encode 0.84-1.02 ms/frame (~0.23-0.28 us/draw),
CP parse + register writes 0.75 ms/frame (execute self + writeReg, ~0.20 us/draw).

**The CP draw drain (strand A from 2026-10-07; machine: Ryzen 9 5950X, 16 cores / 32 logical).**
Pipeline thread CPU, ingame (tools/perf/thread_cpu.py, threads.csv, `ab_off2`): guest render thread
8.68, CP 7.31, SR PM4 render 7.58 ms/frame (sum 23.57) at 111.9 fps - all three near a core; 240 fps
needs each under 4.17 ms. The CP (pr_off, tools/perf/thread_ms.py, running ms/frame): drainDrawQueue
4.6 (2.4 at fences via cpFlushDraws, 2.2 at IB end), Pm4Engine::execute self 0.42 + writeReg 0.33,
emitDraw 0.78. Inside the drain: pm4StageVsf 1.80 (pm4ShmRequest 0.84 of which its recursive_mutex
0.32, remapIndices 0.33, memcpy 0.16, gpuWindow 0.13), the per-draw lambda self 0.53 + memcpy 0.43 +
DrawBatch (~1 KB, zero-initialised per draw) 0.55, pm4PrepareDraw 0.45 (getPm4Pso 0.21),
pm4ViewportScissor 0.20, cvMerge 0.25, ShmCollectFlush 0.15 - the iter-466 cp rounds' long tail.
- **SR_CP_DRAIN_ASYNC (IB-end drain on its own thread) - REJECTED** (`da_frm`, 1316cb757164): frames
  correct, replay 9/9, but no overlap: every segment the XDK kicks ends in a fence write whose
  cpFlushDraws waits for the drain, so the CP's ib time is unchanged plus a new wait at the ring-batch
  flush, and ingame shows periodic 100-240 ms frames every ~1.4 s (87 > 50 ms vs 1 synchronous). A
  real split needs DEFERRED fences (the drain stage writes fences / interrupts after the draws ahead
  of them) - a CP redesign, not done.
- **Drain census by site** (SR_CP_DRAIN_STATS, `ds_stats`, ingame 108 s at 80.5 fps with the dev
  log, ~8,700 frames). Per frame:

  | Site | Calls | Empty | Commands | Drain ms |
  |---|---|---|---|---|
  | fence | 29.4 | 62% | 2,092 | 2.89 |
  | IB end | 50.9 | 49% | 1,682 | 2.18 |
  | swap | 1.0 | 100% | 0 | 0 |

  That is 5.07 ms/frame, about 1.34 us per drained command. The empty drains cost almost nothing
  (fence: 98 us per call on average against 255 us per non-empty call). The drain's cost is the
  per-draw batch build, not drain overhead.
- **The three exact caches + skip-empty: NO GAIN at uncapped, so NOT baked.** Interleaved rolls,
  exe 9a6dd6312b21, ingame:

  | Variant | Gates | Rolls | fps | Tick (ms) | CP (ms/frame) | Pipeline sum (ms/frame) |
  |---|---|---|---|---|---|---|
  | off | none | `cc_off1/2` | 119.2 / 116.8 | 8.54 / 8.58 | 7.23 / 7.34 | 22.85 / 23.32 |
  | on | SR_CP_VSF_CBDEDUP + SR_CP_IB_CACHE + SR_CP_PSO_MEMO | `cc_on1/2` | 116.5 / 116.8 | 8.60 / 8.59 | 7.58 / 7.52 | 23.54 / 23.51 |
  | se | on + SR_CP_DRAIN_SKIP_EMPTY | `cc_se1/2` | 116.0 / 120.5 | 8.56 / 8.31 | 7.76 / 7.31 | 23.86 / 22.79 |

  All within roll noise. The gates stay default OFF. Per the owner direction of 2026-10-07 (a
  native port, not an emulator), CP / PM4-interpreter / drain speed work STOPS here: the CP is the
  oracle, and the draw builder (NATIVE_ROADMAP, strand A main track) is the path to the CP's cost.

**The five "hot" pages are not draw-path writes.** Ledger with the pages pinned (`z_pin`,
SR_NATIVE_DRAW_LEDGER_PAGES on 0x8299B000 / 0x82913000 / 0x829A1000 / 0x82969000 / 0x82966000 /
0x8292D000 / 0x829AB000 / 0x82934000, every 16th draw, 1.38M sampled calls): no dword on them
changes in more than 0.2% of the sampled draw calls - other threads' background writes, not XDK
globals the draw entries own. The draw entries' guest-visible writes are the device dwords and the
ring (the ledger's list).

### 2.14 Round 5: the next native leaves are memory-bound; the constant-upload sizing (iter-467 guestcode)

The baked profile's (2.12) next guest self-time leaves were transcribed, proven exact and
timed in-process - and REJECTED: their time is cache misses on the game's data, not the
recompiled instructions (code in commits 83e0fb64 / 3ffcc222, removed in the next commit):

| function | CHECK (cm10chk, whole run) | us/call guest -> native (cm10abt) |
|---|---|---|
| RenderStateBlock_ApplyShaderParams 0x826092B8 (5.5% self) | 10.75M calls / 209M entries 0 bad | 0.245 -> 0.230 |
| AppendVertexData 0x8231AD20 (2.1%) | 71.3M calls 0 bad | 0.034 -> 0.031 |
| ClothUpdateBoundsAndLights 0x82305B88 (2.7%; guest vtable calls kept) | 8.58M calls 0 bad (after cm9chk caught a wrong planes VA, 0x829A9390) | 0.621 -> 0.524 |

So the per-function native route has run out of cheap wins on the render thread: what is left
on top (constant upload 1.69 ms, render walk + draw emission 1.82, PM4 / device emit 1.26) is
data-miss bound or strand A's.

**The constant upload's inputs (SR_PARAM_OBS, `cm10obs`, whole run).** ApplyShaderParams points
the parameter table (0x82998B00[id]) at every value the uploads then read; the observer walked
every applied list read-only: 266.5M parameter applications (~19 per call), 59.8% inline block
data (avg 65 bytes) and 40.2% external pointers; **99.72% of the applications find the same
16 bytes at that address as its previous application** (changed 0.26%: 687K, new 44.6K;
external 0.49% changed); 337 parameter ids, 44,576 distinct value addresses, all in 1,839 4 KB
pages (1,739 inline + 100 external), of which only 125 saw any change during the run.

**Design (for the upload's owner, strand A's kernel/native_leaves.cpp F loop):** the ~45K source
reads per frame are the cost (2.10: at the memory floor), and almost all of them re-read
unchanged bytes. Track writes to the ~1.8K parameter pages with the existing page-protection
write watch (kernel/gpu_write_watch, the cp lane's texture tracking: protect, fault ->
generation++); per shadow register keep (source VA, page generation, the 16 bytes copied). An F
entry whose source VA and page generation match and whose shadow register still holds those
bytes (an L1-resident compare against the host-side copy) is an exact no-op copy: skip the
source read, still OR its group mask exactly as the guest does - so the dirty state and the PM4
stream are unchanged (no residency question, unlike 2.10's shelved phase 2). Checkable call for
call: a _CHECK gate reads the source anyway and compares. Costs to size first: write faults on
the ~125 pages that change (~3K value changes/s in this run) and the metadata lookups.

### 2.15 Native render targets and resolves (iter-467 native-b, strand B, branch native/targets)

Owner direction (2026-10-07): a native PC port, not an emulator - strand B removes EDRAM-model work
(docs/NATIVE_ROADMAP.md strand B). Gates (default OFF on the branch): `SR_NB_RESOLVE_TEX` (a resolve
writes the texture that samples it; the memory is held and written back only for another reader),
`SR_NB_DEPTH_SRV` (depth resolves read the depth/stencil planes through SRVs, no plane copy),
`SR_NB_ARRAY_LAYERS` (a stacked texture reloads only the layers whose memory changed),
`SR_NB_CLEAR_ELIDE` (no ownership transfer into tiles the XDK's own clear rewrites, ENGINE_MAP 6g.8a).

Oracle (replay, frames 1-3 of a 3-loop replay, all 9 traces, `archive/v1_nativeb/b0` in the worktree):
all four gates vs gates off byte-identical at 1x (elision; B2 needs a resolution scale), 2x and 3x;
per-resolve destination hashes (`SR_NB_RHASH`) 78/78 on 6 traces; the elision witness
(`[NB_ELIDE] ... tiles not moved`) present on gp1 / lg1 / menu. Whole run (perf_player_gameplay:
boot, intro movies, title, custom menu, NEW GAME, opening movie, tutorial) with all four gates at
3x: every step passed, 66 sparse frames viewed (every era), no MISMATCH / failure line; the gameplay
frame matches Xenia's render of the same trace (lg1).

GPU per frame at 3x, replay clock (census, gp1 / lg1, frame 5):

| | resolves | texture loads | transfers | materialize | sum |
|---|---|---|---|---|---|
| gates off (b2g) | 1.98 / 1.98 | 2.17 / 2.16 (39) | 1.21 / 1.23 | 0 | 5.37 |
| B2 only, b2f (before secondary targets) | 1.39 / 1.40 | 0.66 / 0.66 (17) | 1.24 | 0.14 | 3.43 |
| B2 + secondary targets (b2h) | 1.65 / 1.64 | 0.19 / 0.19 (9) | 1.21 / 1.23 | 0 | 3.07 |
| clear elision only | 1.97 / 1.98 | 2.18 / 2.17 | 0.78 / 0.78 | 0 | 4.93 |
| all four (b2l) | 1.64 / 1.65 | 0.19 / 0.18 (9) | 0.78 / 0.78 | 0 | 2.61 |

Live, 3x, Metallo fight (res_boss_fight, exe b8242bdce861-b2l, interleaved A/B, `fight_gpu.py`):

| roll | gates | GPU busy mean / median | draw | transfers | resolve | tex loads (n) | clock |
|---|---|---|---|---|---|---|---|
| l6_da | off | 8.57 / 8.38 | 2.68 | 1.30 | 2.33 | 2.12 (38.1) | 2428 MHz |
| l6_na | all four | 6.82 / 6.29 | 2.96 | 0.99 | 2.29 | 0.37 (11.1) | 2182 MHz |
| l6_db | off | 8.52 / 8.39 | 2.70 | 1.27 | 2.30 | 2.11 (38.1) | 2531 MHz |
| l6_nb | all four | 7.00 / 6.36 | 3.02 | 1.02 | 2.36 | 0.39 (11.3) | 2215 MHz |

-1.6 ms mean, -2.0 ms median, at a ~12% lower graphics clock (the lighter frame boosts less: the
draw pass is the same work and reads 2.96-3.02 vs 2.68-2.70 ms). The live census (l6_census0 /
l6_census) splits it: depth resolves 0.77 -> 0.25 ms (depth SRVs), texture loads 2.11 -> 0.27,
transfers 1.29 -> 0.98, while the shadow-cascade resolves rise 0.27 -> 0.64 ms (the copies into
the cascades' second identities, with their barriers) - still less than the 8 scratch loads +
materializations they replace. Whole run, tutorial window, all four gates: 6.31-6.61 ms busy.

CPU (replay, `SR_PM4_DIAG_TIMING`, render thread, 3x, A/B twice): binding ~10 ms / frame either way
(the replay's figure, far above the 2.77 ms live profile: its trace memory writes invalidate the
clean set every loop); resolve setup 3.0 -> 3.3 ms (the target lookups); transfer setup 0.3 ->
0.1 ms (elision). B does not touch the per-slot bind cost (docs/NATIVE_ROADMAP.md B-CPU).

Rejected / measured along the way:
- Skipping a second identical resolve: none exists - every pair of resolves into one destination
  has a full-screen draw between them (lg1 #40 / #41 / #42, #3010 / #3011 / #3012).
- Dropping whole transfer classes that no fixture observes (`SR_NB_XFER_DROP`): not provably
  safe outside the fixtures; replaced by the coverage proof (B1a).
- The stencil-bit passes of depth transfers cost ~0.1 ms / frame (gates-off census with
  `SR_XFER_DEPTH_NOSTENCIL=1`, diagnostic only: 1.21-1.23 -> 1.10-1.12 ms).

**Round 2 (2026-10-07 evening; the four gates above baked ON on dev 0ac96585 after check roll cr1b).**
Built, gated OFF (branch native/targets, exe snapshot db56d181fbc5-fr6): `SR_NB_OVERHANG` (rows and columns),
`SR_NB_DEPTH_TARGET`, `SR_NB_FRONT` (B4) - the last resolves that still wrote guest-layout memory - and
B-CPU: `SR_NB_BIND_MEMO` (+ the array layout memo), `SR_NB_REBASE_PROTECT`, `SR_NB_CLAIM_FAST`.

Replay (b8, `archive/v1_nativeb/b0`): the three B2+/B4 gates vs dev byte-identical at 3x (9/9 traces, frames
1-3) and 2x (6/6), rhash 78/78 x 6; census lg1: 26/26 resolves into textures, 0 resolved-texture loads (9),
`[NB_METER]` guest-memory resolves 11 -> 0. B-CPU gates: frames identical 3x 9/9, 1x 4/4; with
`SR_SHM_WT_CHECK=1` every kept rebaseline hash recomputed, 0 stale. Gated-off build = dev frames at 1x / 3x.

Live, Metallo fight, 1920x1080 output, `SR_PERF_STATS` (render thread CPU per guest frame, fight half of the
roll, `fight_cpu.py`) and `SR_PM4_GPU_TIME` (`fight_gpu.py`), interleaved, exe db56d181fbc5-fr6:

| roll | scale | gates | SR_PM4_render CPU ms / frame | GPU busy mean / median | transfers | resolve | tex loads (n) | clock |
|---|---|---|---|---|---|---|---|---|
| l10_off1xa / b | 1x | four NB gates =0 | 7.77 / 8.37 | 8.59 / 8.56, 6.03 / 7.80 | 1.09 / 0.74 | 2.19 / 1.54 | 2.16 / 1.49 (38.2) | 487 / 514 MHz |
| l10_on1xa / b | 1x | dev default (four ON) | 7.86 / 9.00 | 8.29 / 8.40, 8.03 / 8.34 | 0.67 / 0.65 | 2.31 / 2.26 | 1.92 / 1.83 (30.6) | 416 / 414 MHz |
| l10_off2xa / b | 2x | four =0 | 8.46 / 9.15 | 5.31 / 4.57, 5.72 / 5.00 | 0.85 / 0.92 | 1.36 / 1.48 | 1.17 / 1.25 (38.3) | 1445 / 1343 MHz |
| l10_on2xa / b | 2x | default | 8.30 / 8.66 | 3.14 / 3.19, 3.90 / 3.31 | 0.40 / 0.51 | 1.01 / 1.23 | 0.14 / 0.16 (11.1) | - / 2625 MHz |
| l10_bcpu2xa / b | 2x | default + B-CPU (memo, rebase protect, claim fast) | **6.01 / 5.99** | 4.52 / 3.52, 7.19 / 6.55 | 0.61 / 1.04 | 1.41 / 2.33 | 0.16 / 0.33 (11) | 1149 / 939 MHz |

- **B-CPU: the render thread 8.30 / 8.66 -> 6.01 / 5.99 ms per frame (-28 to -31%)** at 2x, both rounds; the
  memo reuses 56.2% of slot binds (`[NB_BIND_MEMO]`), the rebaseline keeps ~121M page hashes over the roll
  (`[SHM_WT] ... rebaseline kept`). CP and game threads unchanged (8.5-8.9 / 16.8-17.0 ms).
- **1x / 2x GPU totals are clock-bound, not work-bound:** at 1080p the GPU never boosts in this fight (414-514
  MHz at 1x; 939-2625 at 2x), so busy ms follow the clock. The passes the gates touch: at 1x the clear
  elision cuts the transfer pass 1.09 / 0.74 -> 0.67 / 0.65 ms and the per-layer array reload the loads
  38.2 -> 30.6 (B2 needs a scale > 1), at a 15-20% LOWER clock; at 2x B2 cuts the texture loads 38 -> 11
  (1.17-1.25 -> 0.14-0.16 ms) and the elision the transfers 0.85-0.92 -> 0.40-0.51. A whole-frame 1x number
  needs locked clocks (not possible from a background roll: elevation prompts).
- The NATIVE METER (live, whole run, per frame): at 1x every resolve still goes through the EDRAM buffer
  (edram_res 26.0 with the gates off or on: `SR_NATIVE_RESOLVE`, the 1x native resolve, was never baked);
  edram_xfer 24.5 with the four gates off, 16.3 with them on (1x and 2x: the elision); at 2x edram_res is 0.0 in every 120-frame window of the fight (the
  whole-run average 2.0-2.3 comes from the boot frames before the native pipelines compile, 26 a frame, and
  windows with 5 a frame in another era - not yet attributed).

**1x native resolves (`SR_NATIVE_RESOLVE`, N4a, baked ON 2026-10-07 after check roll cr3
at 1x: native resolves vs the EDRAM path 27 PASS).** At 1x every resolve went through the EDRAM buffer (store of the source RT, then
Xenia's resolve shader from the buffer). Replay (b9): `SR_NATIVE_RESOLVE=1` frames 9/9 identical to dev at 1x
(also with the B-CPU gates), rhash 78/78 x 6 vs the default; census: 26/26 resolves path N. Live 1x Metallo
fight, interleaved (exe db56d181fbc5-fr6):

| roll | gates | GPU busy mean / median | resolve | tex loads (n) | render thread CPU | meter edram_res / frame | clock |
|---|---|---|---|---|---|---|---|
| l11_on1xa / b | default | 7.91 / 8.38, 7.96 / 8.41 | 2.19 / 2.24 | 1.81 / 1.83 (30.5) | 7.62 / 7.58 | 26.0 | 411 / 422 MHz |
| l11_nat1xa / b | + SR_NATIVE_RESOLVE=1 | **6.42 / 6.61, 6.76 / 6.89** | **1.26 / 1.30** | 1.47 / 1.54 (30.6) | 7.41 / 7.53 | **1.4** | 368 / 412 MHz |

-1.2 to -1.5 ms GPU busy at 1x at an equal or lower clock (the resolve pass -0.93 ms: no EDRAM store, no
buffer round trip); the meter's EDRAM resolves 26 -> 1.4 per frame (whole run: the boot frames before the
native pipelines compile).

**Live in-roll A/B (kernel/ab_toggle, tools/perf/ab_windows.py --pool; perf_run gameplay, uncapped, two rolls each,
~118 windows; ON minus OFF, 95% CI, paired).** Job15, exe fr8 (env gates):

| feature | scale | fps | frame p50 ms | guest render tick ms | process CPU ms/s |
|---|---|---|---|---|---|
| `SR_NB_BIND_MEMO` | 2x | **+5.5 +-2.1** | -0.33 +-0.13 | -0.38 +-0.23 | -108 +-61 |
| `SR_NB_CLAIM_FAST` | 2x | +2.0 +-2.3 | -0.06 +-0.15 | -0.20 +-0.26 | -15 +-67 |
| `SR_NB_REBASE_PROTECT` | 2x | -0.2 +-1.7 | -0.02 +-0.13 | +0.02 +-0.18 | -23 +-55 |
| `SR_NB_CLEAR_ELIDE` | 1x | +0.4 +-2.0 | -0.01 +-0.14 | +0.02 +-0.22 | +2 +-62 |
| `SR_NATIVE_RESOLVE` | 1x | **-13.7 +-1.8** | **+1.14 +-0.13** | **+0.98 +-0.20** | -193 +-56 |

The 1x native resolve, baked on GPU busy from vsync-capped rolls (l11 below), LOSES 13.7 fps uncapped: GPU (3.04 ->
2.85 ms) and render thread (5.54 -> 5.34 ms) get lighter, but at 1x uncapped the CP is the wall (ib ~87% of wall
time) and its share rises 0.873 -> 0.888, swaps 118 -> 114.5 per s (split_ab.py by window mask). Cause: path N takes
m_shmMu before its rebaseline, so the ~200 pages x 2 hashes per resolve (and REBASE_PROTECT's protections) run
under the lock the CP's pm4ShmRequest needs - what iter-466 hfr removed from the EDRAM path, which pre-hashes
unlocked. Dev 99d90b92 sets SR_NATIVE_RESOLVE back to default OFF; the fix (SR_NB_NATIVE_PREHASH: the same
pre-hash in pm4NativeResolve and nbResolveToTarget) is built and its A/B queued (job16). Lesson: an A/B on GPU busy
or a capped frame rate does not show a CP-bound regression - judge perf gates on uncapped in-roll fps.

**The rebaseline lock, fixed: `SR_NB_NATIVE_PREHASH` + `SR_NB_REBASE_FRESH_FIRST` (baked ON; job16b, exe fr17
a2773925a53d).** The 3x render-thread profile (nb_prof, fr14, 101 fps) had `rebaseHashPages` at 1.01 ms per frame
inclusive, 0.98 of it in `par::Pool::run`, while the same roll's `[SHM_WT]` totals KEEP ~10,400 pages a frame (every
page of the 26 resolves' extents): with SR_NB_REBASE_PROTECT the hashes are already kept, but the pool was still woken
for every resolve of >= 64 pages to read one protection generation per page. At 3x the native targets
(`nbResolveToTarget`) did that under m_shmMu, as path N did at 1x, and the CP's `pm4ShmRequest` spent 1.18 ms per
frame in the lock (GPU_CP running 9.21 of a 9.88 ms frame: the CP is the wall at 3x too). FRESH_FIRST finds the kept
pages serially and pools only the pages that hash; PREHASH hashes before the lock. In-roll A/B, uncapped, two rolls
per scale, ~117 windows, ON minus OFF (paired, 95%):

| feature | scale | fps | frame p50 ms | process CPU ms/s |
|---|---|---|---|---|
| `SR_NB_REBASE_FRESH_FIRST` | 3x | **+14.1 +-3.0** | -1.11 +-0.14 | **-474 +-70** |
| `SR_NB_NATIVE_PREHASH` | 3x | **+7.4 +-2.7** | -0.66 +-0.12 | +101 +-64 |
| `SR_NB_CLEAR_PROP` | 3x | +2.3 +-2.8 | -0.14 +-0.13 | +29 +-66 |
| `SR_NB_COVER_FILL` | 3x | +0.9 +-2.7 | -0.11 +-0.12 | -12 +-63 |
| `SR_NB_REBASE_FRESH_FIRST` | 1x | **+12.6 +-2.4** | -0.77 +-0.15 | **-530 +-65** |
| `SR_NB_NATIVE_PREHASH` | 1x | **+4.4 +-2.5** | -0.34 +-0.16 | +29 +-68 |
| `SR_NB_CLEAR_PROP` | 1x | +2.3 +-2.3 | -0.20 +-0.14 | +36 +-62 |
| `SR_NB_COVER_FILL` | 1x | -0.1 +-2.4 | +0.00 +-0.15 | +4 +-63 |
| `SR_NATIVE_RESOLVE` | 1x | -2.8 +-2.3 | +0.27 +-0.15 | -82 +-63 |

Mean fps of the rolls: 3x 114.3 (nb_prof's 101 had all four off), 1x 122.8. The 1x native resolve is no longer the
-13.7 fps of job15: averaged over the other gates' states it is -2.8 +-2.3, and inside the windows with FRESH_FIRST on
+0.2 +-5.7 (OLS per-roll intercepts, `archive/v1_nativeb/ab_interact.py`). Job17 (exe fr18 c525c565797e, the two
fixes baked, SR_NATIVE_RESOLVE toggled alone, two 1x rolls nb_n18a / b, 116 windows): **-0.8 +-2.6 fps**, frame p50
-0.01 +-0.13 ms, guest tick -0.07 +-0.22 ms, process CPU -30 +-45 ms/s; 127.4 fps mean - neutral, so the 1x native
resolve is baked ON again (it removes the 1x EDRAM resolve path: meter edram_res 26 -> 1.4, GPU busy -1.2 to -1.5 ms
in l11). Job17 replays: fr18 at its defaults identical to the fr14 baselines at 1x (9/9, SR_NATIVE_RESOLVE=1) and 3x
(9/9), 0 stale under SR_SHM_WT_CHECK; `[NB_REMAP]` fires on eahd / t16 with SR_NB_SCALED_REMAP=1 (frames identical).

**Baked ON (=0 off): `SR_NB_SCALED_REMAP`, `SR_NB_CLEAR_PROP`, `SR_NB_COVER_FILL`** (2026-10-08, after check roll
cr4b_nb, `archive/v1_checkroll/cr4b_nb` in the main worktree: 3x whole run, 12/12 native-b checks PASS - CLEAR_PROP
VERIFY ok 52 / 0 MISMATCH, `[NB_FILL]` stencil-masked 56, `[NB_REMAP]` 2, 0 stale kept hashes - and every era's
frames normal on the contact sheet). Replay: the bake exe fr20 vs new baselines on the dev-merged fr19 (freezefx's
quad split in): 1x 9/9, 3x 9/9, 2x 6/6, per-resolve hashes 78/78 x 6 (job18); again on the S1-on dev exe fr21 vs
the same exe with the three at =0: 1x 9/9, 3x 9/9 (job20). Live: fps-neutral on a CP-bound frame (job16b);
transfers 0.78 -> 0.13 ms per 3x frame (replay census); the 2x / 3x menu-era EDRAM resolves 5 -> 0 a frame.
COVER_FILL is baked for the GPU headroom it leaves and because it removes EDRAM-model moves.

**The CP's shared-memory requests: `SR_NB_SHM_RANGE_CACHE` (baked ON on the branch; check roll cr5_nb).** The CP is
the frame limiter (strand A fp3: 9.26 ms/frame, `pm4ShmRequest` 2.02 of the drain). On the dev-merged exe with the
rebaseline fixes (cp_prof21, fr21, 1x, 125.8 fps) the CP runs 7.89 ms/frame and `pm4ShmRequest` is 1.13 ms: owner
window lookups 0.47, m_shmMu 0.27 (was 1.06 in fp3), the page walk 0.29; 0.78 of it from the vertex staging, 0.36
from the texture prepass. (0.75 ms of the CP's runtime-library time is `cpMain`'s own wait: `archive/v1_nativeb/
callers_of.py`.) The live path drains after every indirect buffer and each drain re-walks every page it touches,
although write tracking already proves the pages unchanged. The range cache remembers, per thread, a range whose
pages were all clean in its window, with the write tracker's release clock (new: `gww::releaseClock` /
`blocksReleasedSince`, a stamp per 64-page block taken before any page is unprotected) and the range's owner version;
until a block of it is released or its owner changes, a request is a table lookup (no lock, no walk).

| roll set | exe | GPU_CP ms/frame (ON - OFF) | fps | notes |
|---|---|---|---|---|
| rc_1a/b (1x) | fr22 | **-0.32 +-0.27** | +1.0 +-2.3 | vertex-staging requests only |
| rc_3a/b (3x) | fr22 | **-0.30 +-0.26** | +2.2 +-3.1 | |
| rc2_1a/b (1x) | fr23 | -0.08 +-0.29 / PREPASS -0.07 +-0.29 | -3.3 +-5.0 | the release race (below) |
| rc3_1a/b (1x) | fr24 | **-0.37 +-0.26** / PREPASS +0.10 +-0.26 | +1.2 +-1.7 | race fixed; 16K-entry table |

Correctness: live CHECK rolls (`SR_NB_SHM_RANGE_CHECK=1` re-walks every hit under the lock; `SR_SHM_WT_CHECK=1`
re-hashes kept pages). rc2_chk (fr23) found 998 mismatches on a few ranges: a page read clean in the walk while a
release was clearing its block, and the clock-unchanged shortcut kept the entry. Releases are serialised under the
tracker's lock, so an entry is now invalid when any of its blocks carries a stamp >= its own (the release in flight at
the walk carries exactly that stamp); rc3_chk (fr24): 27.3M hits, 0 mismatch, 0 stale. The mutation
(`SR_NB_SHM_RANGE_TEST_NOREL=1`, releases unstamped) is caught: rc_mut > 500K mismatches. The profile with it on
(rc3_prof): `pm4ShmRequest` 1.13 -> 0.86 ms per 1x frame, its lock 0.27 -> 0.06; what is left is first-seen ranges
(dynamic vertex data at new addresses every frame: owner lookups 0.30 ms). Replays cannot weigh it (no physwin owner
records in a replay, so no range is cached); frames identical with it on (b17). Rejected: `SR_NB_SHM_RANGE_PREPASS`
(the texture prepass's requests too) - no CP gain, kept OFF.

For strand A's pre-resolved draws: `pm4ShmEnsure(phys, len, ShmRangeTicket&)` - the same proof held per stream by
the caller instead of the per-thread table (d3d12_backend.hpp).

**The texture prepass and first-seen owner answers (fr26; CP thread ms per frame, four in-roll A/B rolls at 1x,
234 windows).** `SR_NB_PREPASS_FLAT` (baked ON): the prepass's per-set dedupe and each fetch's request ranges in one
flat per-thread table keyed by the whole 24-byte fetch, the set's dedupe a generation bump - GPU_CP -0.14 +-0.17, render
thread -0.17 +-0.13 (the CP's prepass holds m_shmMu for its whole run), process -0.9 +-1.6; live CHECK 17.1M reuses,
0 mismatch; mutation caught; replays b18 9/9 at 1x and 3x. The profile (pf_prof vs rc3_prof): shmCollectTextures 0.71
-> 0.70 ms - the unordered_set (0.14) and layout recompute (0.10) go, the table probe costs most of it back; the
remaining cost is the 16-slot walk of every batch. Rejected and removed: `SR_NB_OWN_SUPERSET` (a first-seen range's
window from its 64 KB-aligned superset: exact - CHECK 2.7M uses, 0 mismatch, mutation 14 caught - but GPU_CP +0.02
+-0.17: the superset's memoised gpuWindowRange re-scans the grant log since its generation, and the guest allocates
every frame, so it costs what the range's own lookup cost). The prepass itself cannot move back to the render thread
as is: iter-465 perf2 moved it to the CP because, run there, it shared page stamps with the CP's concurrent drains of
the next set and missed CPU rewrites between frames (flat brown boot movies).
CLEAR_PROP and COVER_FILL are GPU savings (transfers 0.78 -> 0.13 ms per 3x frame) on a CP-bound frame: fps-neutral,
as expected. Replay b13: PREHASH + FRESH_FIRST frames 1-3 identical to the fr14 gates-off baselines at 1x (9/9) and 3x
(9/9) with SR_SHM_WT_CHECK=1 (every kept hash recomputed: 0 stale); the per-bind RT-cache gate hoist (74b703ac,
render thread 0.12 ms / frame) 9/9 x 2.

**The aliases removed: clear propagation, cover fill (iter-467 native-b, 2026-10-08, gated OFF, replay-proven).**
Replay census, 3x, frames 2-6 of a 7-loop replay, transfer sets per frame (GPU ms, gp1 / lg1; the same exe's
gates-off runs before and after each pair bracket the clock):

| exe | gates | transfers ms (gp1 / lg1) | resolves ms |
|---|---|---|---|
| fr9 5c312e516aa1 | off (cp_cen0_3x) | 0.780 / 0.778 | 1.826 / 1.820 |
| fr9 | `SR_NB_CLEAR_PROP=1` (cp_cen3x) | **0.354 / 0.356** | 1.824 / 1.826 |
| fr11 a66363c4918e | off (cf_cen0_3x, cf_cen0b_3x) | 0.780 / 0.780, 0.780 / 0.776 | 1.906 / 1.910, 1.910 / 1.905 |
| fr11 | `SR_NB_COVER_FILL=1` (cf_cen3x) | **0.552 / 0.553** | 1.914 / 2.080 |
| fr11 | `CLEAR_PROP` + `COVER_FILL` + `TEX_FANOUT` (cf_allcen3x) | **0.128 / 0.128** | 1.924 / 1.918 |

Transfers -0.65 ms of 0.78 per 3x frame (-84%) with both; frames 1-3 identical to dev at 3x (9/9), 1x (9/9) and
2x (6/6) for each gate and for the three together; per-resolve hashes 78/78 x 6; `SR_NB_CLEAR_PROP_VERIFY=1`: 0
MISMATCH on 9 traces at 3x and 4 at 1x (every mismatch prints; the ok lines are sampled: first 16, then powers of 2). Live numbers: job14 (in-roll A/B).

**Rejected: `SR_NB_TEX_FANOUT` (no gain).** The shadow cascades' cost on the target path (0.25 ms per 3 resolves at
3x vs 0.14-0.21 on the guest-memory path) is not the copies into a second identity: in steady state each cascade
has ONE current target (the array layer) - only the first resolve of a run finds a second (`seconds=1`). Census
fo_cen0_3x / fo_cen3x / fo_cen0b_3x: resolves 1.98 / 1.90, 1.92 / 1.92, 1.91 / 1.91 ms - unchanged. Kept gated OFF
(exact: frames 9/9, rhash 78/78 x 6).

**`SR_NB_TARGETS_1X` (B2 at 1x): exact, GPU-neutral - the texture stores are the cost.** Replay 1x (frames 9/9,
rhash 78/78 x 6 vs p_rh1x3; with CLEAR_PROP + COVER_FILL 9/9 too). Census lg1 (gp1): texture loads 31 -> 16 (17),
2.88 -> 1.21 (2.70 -> 1.11) ms, but the dual-write resolves cost 3.34 -> 4.76 (3.38 -> 5.59) ms: per class, the
1280x720 k_2_10_10_10_FLOAT -> f16 resolves 0.45 -> 1.05 ms (2), k_8_8_8_8 0.25 -> 0.45 (2). The typed texture
stores (four strided R16 / R32 / RG32 stores per thread, `NatTexStore*`) run at about half the rate of the buffer
stores - the same cost that makes the 3x shadow cascades slower on the target path. Not for baking until the
stores get cheaper.

**Rejected: `SR_NB_TEX_COALESCE` (coalesced texel stores; no gain).** Hypothesis: the strided typed stores (each
thread writes every 4th / 8th texel of a row) waste bandwidth. Built: texels through groupshared memory, then
consecutive lanes store consecutive texels. Replay b12 (fr14, vs the same exe gates off): exact (frames 9/9, rhash
78/78 x 6) but slightly slower - 3x shadow cascades 0.283 / 0.282 vs 0.267 / 0.266 ms (gp1 / lg1), 1280x720 f16
0.384 vs 0.368 / 0.365, k_8_8_8_8 0.289 vs 0.273; 1x TARGETS_1X resolves 4.50 / 4.25 vs 4.01 / 4.68 ms (noise at
the 1x clock). The cost is per texel (one typed store per texel vs one 8-16 byte buffer store per 2-4 texels on
path N), not the pattern. Reverted (no extra kernels compiled). TARGETS_1X stays OFF: its saving (15 texture loads
a frame) is spent again in the texture half of the dual write.

**EDRAM zero, the resolve and load halves (2026-10-08 evening; fr31-fr36).** `SR_NB_AOT` + 21 shipped kernel blobs:
replays 25-45 s -> 4-7 s per trace (DXC compiles gone), live whole-run EDRAM-model resolves 17-20K -> 1 (the first ~25 s
no longer resolve through the EDRAM buffer while the kernels compile). `SR_NB_ZERO_INIT` (loads of never-claimed tiles
-> host zero clears) and `SR_NB_RESOLVE_CLAIM` (a reshaped / two-colour-owner resolve span moved into its source,
then native): live 1x and 3x runs with edram_res 0 and edram_load 0 for the whole run (zc1 28,574 frames, zc3 24,786).
zc1's ingame era: 121.2 fps, p50 8.1 ms (uncapped, 1x, one roll - not an A/B). Exactness: replays 9/9 at 1x and 3x for
each gate, 78/78 resolve hashes x 6 at 3x (zero init), the boot resolve's hash identical E vs claim + N; flicker gate
PASS for each step (fg33 / fg35 / fg37). Left on the EDRAM model: the tile map's decisions, ~12-14 transfers a frame.

**EDRAM zero, the decision half: the surface table (2026-10-09, fr38-fr40, dev 264c7fe8).** `SR_NB_SURF_TABLE`
(baked ON) - the per-surface table decides every move instead of the per-tile map. Speed, separate interleaved 1x
uncapped gameplay rolls (the table cannot toggle in-roll): map pm_a1 / pm_a2 121.0 / 123.6 fps, table pt_b1 / pt_b2
126.8 / 122.2 fps; CPU ms per frame map -> table: SR PM4 render 3.763 -> 3.636, guest:GPU_CP 6.137 -> 6.324, process
52.10 -> 51.37 (archive/v1_nativeb/perf41.txt, perf_rolls.py) - neutral within roll noise. Live whole runs at 1x and
3x: edram_res 0, edram_xfer 0, edram_load 0.

**P1 row 9: Resolve / ClearF / SetShaderGPRAllocation as native records (2026-10-09, `SR_NB_REC`).** The three XDK
entries are host ports that write strand A's opaque records instead of PM4 (NATIVE_ROADMAP "P1 row 9").
- **Meter**, job49 pair on exe ec9483bbfbf5, ingame windows: **1,820.4 -> 585.8 PM4 packets per frame (-1,234.6)**.
- **Speed**, in-roll A/Bs (`SR_AB_TOGGLE=SR_NB_REC`, ~59 windows each, 1x uncapped gameplay):
  - job49, exe rec48: tick -0.305 +-0.695 ms (paired -0.243 +-0.688), frame p50 +0.144 +-0.317 (paired +0.137
    +-0.161), fps -1.3 +-5.1.
  - job52, the bake exe: paired tick +0.338 +-0.220, p50 +0.289 +-0.149 - **a real cost, ~0.3 ms a frame**. It came
    from per-call heap traffic: two Prods with growing vectors, a heap vector per guest write, the pre-pass rendering
    bytes, and the Entry constructor zeroing its 15 KB span (strand A).
  - job53 y_ab11, after the fix (thread_local Prods reset per call, a lean pre-pass; without strand A's Impl pool):
    tick +0.006 +-0.562 (paired +0.012 +-0.494), p50 +0.032 +-0.315 (paired +0.102 +-0.142), fps -1.1 +-5.2 -
    **neutral**.
- **Cost of a record** (the [NB_REC] rec line, job53): pre-pass 1.47 us, Entry open 0.55 us, live pass 2.48 us per
  call. The pre-pass is a 13.8 KB device copy plus a dry port run that decides a whole-entry decline. ~45 calls a
  frame, ~0.2 ms of host work that replaces the guest bodies' own.
- **Rejected / caught on the way:**
  - The first CHECK reported 0.1% DEVICE BADs. They were a CHECK artifact: LLVM's memcpy forwarding made the
    snapshot diff a second racing read (TOOLKIT 1.8h).
  - job49 hit a THREAD_FATAL in strand A's closeSpan: the record bound was too small for multi-item opaque spans.
    Fixed in strand A's 2003a556, which now computes the exact record size.

### 2.16 The constant-upload skip (rejected), round 5 baked, in-roll A/B (iter-467 guestcode)

**The skip (SR_NATIVE_CONST_SKIP, default OFF, kernel/native_leaves.cpp F loop).** 2.14's design,
built on the existing page-protection write watch (kernel/gpu_write_watch `protGen`): per shadow
register the source VA, the 16 bytes copied and the source page's protection generation; a
register is skipped when the VA matches, the page is still protected under that generation and
the shadow still holds the bytes; the dirty OR always runs. Writer coverage is the protection
itself: every user-mode write to a protected page faults before it lands (guest code, our memcpy
fast path, decoder / DMA-style host copies), kernel-mode writers either release the range first
(gww::HostWriteScope: NtReadFile and the async read worker, the only ReadFile sites) or fail on the
read-only page without changing it; the only other VirtualProtect users are two default-OFF
diagnostics; nothing decommits guest pages; SR_PHYS_UNIFY's alias views (0xC/0xE windows that a
0x8/0xA protection does not cover) disable the skip. Not MEM_WRITE_WATCH (it cannot see writes
between a GetWriteWatch and the read, and the uploads read values written in the same frame).

| proof | result |
|---|---|
| CHECK (cm14chk, whole run, every era) | 7.97M skipped registers re-read: 0 bad (with round 5's CHECKs: 9.70M / 66.1M / 8.32M calls, 0 bad) |
| mutation (cm14mut: the watch ignored on the first page seen to change, 0x18B15000 = src 0xB8B15770) | the CHECK fired 59 times, all 59 on the mutated page |
| replay (cm14_replay, gate off and all on) | 9/9 + 9/9 byte-identical to dev 238fddc3 |
| classic 3x3 (cm14_a* vs cm14_b*, uncapped player profile, ingame tick) | 8.79 / 8.89 / 8.81 -> 11.45 / 11.39 / 11.57 ms, 115.4 / 114.1 / 114.9 -> 87.1 / 87.8 / 86.8 fps |
| in-roll A/B (cm14_t1 + t2, paired OLS) | +2.88 +-0.23 ms tick, -24 fps |

**Why it loses.** Only 0.6% of the 1.28G source registers were skipped (cm14chk): 75% sat on pages
that were not protected at copy time and 20% outside the watched windows (VA < 0x80000000). The
watch releases 64-page (256 KB) blocks with a cooldown of up to 64 frames: the parameter pages
share their blocks with game data written every frame, so they are almost never protected. The
cost is paid anyway: ~50 ns per register of metadata (two protection-state loads into the 640 KB
gww arrays per copy, the queue) on ~53K registers per frame, plus 45M protection requests and 62K
VirtualProtect runs per roll on the render thread (TLB shootdowns to every core).

**A 4 KB-granular watch does not rescue it** (experiment cm15: the same exe with gww's fault block
at one page, kBlockLog2 = 0, not committed): 20.5% of the registers skipped (289.5M, every one
re-read by the CHECK: 0 bad), 40% still on unprotected pages, 19% untracked, and 10x the write
faults (186K); in-roll A/B (cm15_t, the skip toggled alone, paired OLS) +0.92 +-0.23 ms tick,
+1.08 +-0.17 ms frame p50, -10.7 +-2.2 fps. Even at zero metadata cost, 20% of ~31 ns per
register is ~0.3 ms, less than the fault and protection traffic it needs. The parameter pages
are written too often for a protection-based skip; the code stays default OFF as the record
(commit 092c360e), its check_roll entry disabled.

**Round 5 baked (per-call rule).** ApplyShaderParams / AppendVertexData / ClothUpdateBoundsAndLights
(2.14's natives, restored unchanged from 3ffcc222): CHECK 0 bad every era, faster per call, no
live regression: classic 3x3 default 8.81 / 9.16 / 8.66 ms vs round 5 8.79 / 8.89 / 8.81 ms
(113.3 -> 114.8 fps mean), in-roll -0.18 / +0.04 / -0.00 ms +-0.21; whole-run sheets (cm14_frm)
normal. Expected from the inclusive times (cm8_prof: 0.44 / 0.24 / 0.76 ms) x the per-call gains:
~0.17 ms together - below what any live A/B here resolves.

**In-roll A/B (TOOLKIT 1.8d), validated against the classic 3x3.** One uncapped roll flips each
listed feature per [HIGHFPS_PACE] window (block-paired schedule, kernel/ab_toggle);
tools/perf/ab_windows.py estimates per-feature deltas. On the same exe the skip measured +2.64 ms
/ -27.6 fps by the classic 3x3 (6 rolls for one comparison) and +2.88 +-0.23 ms / -24.3 +-3.3 fps
in-roll (2 rolls, seven features toggled at once). For the six exact natives every in-roll
estimate is inside +-0.24 ms (paired OLS) and the classic 3x3 differences are inside its own
+-0.5 ms noise. Resolution: +-0.20 ms per feature from two rolls; the per-window metric is
scene-limited (walk / fly / actions), so a smaller effect needs more rolls (CI ~ 1/sqrt(rolls)).
Limits: a feature with after-effects (state it leaves behind, like the skip's protected pages) leaks
into its OFF windows; toggle such a feature alone.

### 2.17 TLB: page walks are not a meaningful share of the guest tick (iter-467 guestcode)

AMD uProf 5.2 (installed 2026-10-07 from AMD's EULA download, `C:\Program Files\AMD\AMDuProf`;
core PMCs work under the Hyper-V root partition, IBS does not), attached for 35 s from HUD + 5 s
(cm14_tlb, default exe, `archive/v1_gc3/tlb_watch.py` + `tlb_report.py`): cycles, retired
instructions, L1 DTLB misses that hit the L2 DTLB (PMCx045 0x0F), L2 DTLB misses = page walks
(PMCx045 0xF0), L1D fills from DRAM, TLB flushes. Zen 3 has no walk-cycle counter, so the cost is
bounded with a measured walk latency (`tlbcost.exe`: a dependent chase over the same lines packed
vs one per 4 KB page: +2.1 ns for an L2 DTLB hit, +14.6 ns per walk with the page tables
cache-resident up to +36 ns at a 512 MB span; serialized, so an upper bound):

| thread | IPC | L2 DTLB misses / k instr | walks / k cycles | walk share of cycles (66..160 c / walk) | L1 miss, L2 hit share |
|---|---|---|---|---|---|
| guest:slot17 (render) | 2.47 | 0.05 | 0.13 | 0.9 .. 2.1% | 1.9% |
| guest:slot15 / 16 / 18 / 0 | 1.2-1.9 | 0.00-0.02 | 0.003-0.03 | 0.0 .. 0.5% | 0.7-1.3% |
| SR PM4 render | 1.56 | 0.29 | 0.46 | 3.0 .. 7.3% | 12.8% |
| guest:GPU_CP | 1.95 | 0.27 | 0.52 | 3.5 .. 8.4% | 4.3% |

At most ~2% of the guest render thread (<= ~0.2 ms of an 8.8 ms tick) is page walks, so 2 MB pages
(MEM_LARGE_PAGES for the guest reservation: SeLockMemoryPrivilege, a sign-out, a fallback for
players without it) are not worth taking to the owner for the tick. The host PM4 render and CP
threads walk more (3-8%) but are not the limiter today. The tick's data misses are cache misses,
not translation.

### 2.18 Codegen: alias-free addressing sized, ThinLTO (iter-467 guestcode)

Codegen changes cannot flip at runtime, so each is a classic interleaved A/B on two exe snapshots
of the same source (cm17, uncapped player profile, ingame tick from ab_tick.py; F = lane exe
1767a36670f5, L = the same source with CMake SR_THINLTO=ON (e8b008d7a625), U = F under
SR_PHYS_UNIFY=1, I = the same source with SR_GUEST_ALIAS_IDENTITY=ON (a3d06b1ab681) under
SR_PHYS_UNIFY=1):

| round | F tick / fps | L tick / fps | U tick / fps | I tick / fps |
|---|---|---|---|---|
| 1 | 8.11 / 122.0 | 8.06 / 123.8 | 8.66 / 118.3 | 8.60 / 121.8 |
| 2 | 8.33 / 118.7 | 8.31 / 120.8 | 8.84 / 114.0 | 8.88 / 114.9 |
| 3 | 8.53 / 117.0 | 8.19 / 123.6 | 9.18 / 111.9 | 8.88 / 116.8 |
| mean | 8.32 / 119.2 | 8.19 / 122.7 | 8.89 / 114.7 | 8.79 / 117.8 |

**Alias windows as views (aliasGuestVA -> identity): not worth it now.** The fold's codegen cost
was sized directly: SR_GUEST_ALIAS_IDENTITY compiles the guest lib with PPC_GUEST_OFFSET /
PPC_RAW_ADDR = base + VA (no compare, no aliasGuestVASlow call), which is exact only when every
window is a real view of the same memory, i.e. under SR_PHYS_UNIFY=1 (the exe exits otherwise).
I vs U (same memory model, only the fold's code differs): -0.10 ms tick, +3.1 fps - the fold's
whole codegen cost. The profile agrees: aliasGuestVASlow is 0.85% self of the render thread
(cm8_prof, ~0.07 ms). Getting there needs the memory model changed, and the view model we have
costs more than the fold: U vs F +0.57 ms tick, -4.5 fps. In today's flat model a 0xC / 0xE
alias folds per 64 KB page to 0x8|p or 0xA|p by owner (SR_ALIAS_OWNER), with a content check
for contested pages - something static views cannot express, so the views would also need
Xenia's layout (0x8 private, 0xA / 0xC / 0xE one physical section) and every GPU grant in the
0xA allocator. The unified run itself reaches ingame and its whole-run sheet is normal
(cm17_v), so the model is viable; it is the cost that rules it out. Revisit only if the native
port retires the 0xC / 0xE consumers (PM4 ring, immediate shader loads) or the view model's
+0.57 ms is understood.

**ThinLTO (CMake SR_THINLTO).** `-flto=thin` for every C/C++ TU, lld-link backends
(/opt:lldltojobs = SR_THINLTO_JOBS, /lldltocache in the build dir). The guest lib's functions are
weak noinline aliases (the override mechanism), so LTO cannot inline guest into guest; the gain
comes from kernel / native code and the helpers guest code calls. Proof: L6 54/0 on its
difftest.exe; golden_call harness built from it byte-identical to a non-LTO harness of the same
source on types / gold / aabb / ray_s / ig_s; replay 9/9 byte-identical (cm17_replay); whole
run cm17_lfrm (intro movies, title, menu, intro FMV, gameplay, tutorial) normal, 0 ICALL. A/B:
8.32 -> 8.19 ms mean tick (-0.13, lower in 3/3 rounds), 119.2 -> 122.7 fps (+3.5, 3/3).
Cost: a one-file kernel change relinks in 105 s (seven exes link the guest lib, each runs the
thin link); each backend job holds one guest module in RAM. **Policy: OFF for dev builds, ON for
player builds** - -0.13 ms (not yet conclusive) does not pay for a full rebuild, ~105 s relinks and
link-time RAM in every lane's loop. The "player" CMake preset (build-player/, SR_THINLTO=ON) is what
tools/player/make_player_build.ps1 now configures, builds and stages (PLAYER_BUILD.md section 4).
A dev build dir that picked up ON from dev f04f8805 (the brief ON default) is reset to OFF once on
its next configure. **Round 2 confirms the sign** (cm19, the same two exes, order reversed L before
F): 8.59 / 8.59 / 8.26 -> 8.35 / 8.21 / 8.08 ms (mean 8.48 -> 8.21, -0.27), 116.6 / 117.6 / 120.9 ->
121.4 / 121.6 / 123.5 fps. Over all six pairs: lower tick in 6/6, mean -0.20 ms, +3.7 fps. The
player preset keeps ThinLTO ON.

**Direct calls on hot guest edges (rejected, +0.04 ms).** The guest lib's functions are weak
noinline aliases, so no guest -> guest call is ever inlined. tools/perf/hot_edges.py ranked the
render thread's guest call edges (cm8_prof, ingame files 14-22) by the time spent in the callee's
body per caller; tools/regen/direct_calls.py rewrote the 33 hottest edges with a small callee
(<= 2048 host bytes, edge self >= 0.02%; 8.77% of the thread in those callees) to call the body
`__imp__sub_X` with `[[clang::always_inline]]`, and tools/regen/direct_calls_guard.py (run by every
guest-lib build) fails the build if any listed callee gets an override. Only 6 of the 33 call
sites are in their callee's TU, so the experiment ran in the player preset: ThinLTO with
`/mllvm:-import-instr-limit=2000` (the default limit imported none of the cross-TU callees; with it
the callers grew, e.g. sub_82614A08 2570 -> 4997 bytes, and their calls to the listed callees are
gone). Gates: L6 54/0 and golden_call byte-identical (5 scripts) on the dev build of the rewritten
source, replay 9/9 byte-identical (cm19_replay), whole run cm19_dfrm normal, 0 ICALL. A/B (cm19, B
102c92c7db68 vs D 22e2757093c2, both that player config from one source): 9.04 / 9.02 / 8.98 ->
9.16 / 8.96 / 9.04 ms (mean 9.01 -> 9.05), 110.2 / 110.7 / 111.3 -> 108.4 / 110.2 / 110.0 fps -
no gain, under the 0.2 ms bar: stopped and reverted (the tools stay; the guard is a no-op without
guest/ppc/direct_calls.list). The small callees' time is their own data work, not call overhead.
Unattributed: B / D tick 9.0 ms against L 8.2 ms in the same batch - newer dev source and the
raised import limit together; the player preset itself has no import-limit flag.

**golden_call residue (not LTO).** ig_s's recomp-mode diff vs the clean-room oracle shows 4
cases since the 2026-10-01 outputs, in the LTO and the non-LTO builds alike (the oracle output is
unchanged). Each is one heap page hash: sub_82363B10 (a 3 x vec4 clone, ld/std pairs) copies a
stack temporary at 0x70EFFB90 whose w lanes hold leftover stack data (the oracle 0x8297945C /
0x70000B00, ours 0; the oracle's watch shows those slots rewritten by unrelated stores, e.g. a
memcpy at 0x8279E3F8, before the clone reads them). The source is in the harness's
excluded scratch window (0x70000000-0x70FFFFFF); every printed result agrees. Dead-stack residue:
the two sides' stacks differ in slots this call never writes. Which earlier writer changed since
10-01 is not identified (the difference stays with every guestcode native off); next step: the
oracle's `run_script.py --watch 70EFFBAC` against a recomp write watch on the same dword.

### 2.19 Strand A S1: the native draw flush (iter-467 native A, branch native/n0)

The flush is `kernel/native_flush.{hpp,cpp}`, `SR_NATIVE_FLUSH`, default ON from 29bced28.
- What it is: the XDK draw flush (820FC000 / 820FBBF8) as host code. It writes native draw
  records; the CP applies them without PM4 decode. NATIVE_ROADMAP has the "S1 STATUS" section.
- Meter: 60,776 -> 6,842 PM4 packets per ingame frame.

**Correctness:**
- SR_NATIVE_FLUSH_CHECK renders the dry native flush as PM4 and compares it with the XDK body's
  output (rolls fc1..fc4):
  - fc4: 9.4M draws, BAD bytes 0, len 0, device 0.
  - The device compare excludes the fence / interrupt block +0x2A18..+0x2BB0 and +0x2C50. Other
    threads advance those fields, and the flush never stores there.
- Whole-run sheet: fr1. Replay gate-off: 9/9.

**Getting to parity.** All numbers are ingame:

| Step | Exe | Guest native (us/draw) | CP record walk (us/draw) | Whole-frame result |
|---|---|---|---|---|
| fr1, first record mode | - | 0.945 (incl. guest calls) | 0.57 (incl. emitDraw) | - |
| per-draw allocations removed, guest-call and emitDraw time split out (ft1/ft2) | - | 0.31-0.34 | 0.39-0.43 | tick +0.26 / +0.39 ms |
| single-writer counters, 16 MB L3-warm arena, single-pass records (ft3/ft4) | - | 0.28-0.30 | 0.36-0.38 | pooled tick +0.12 +-0.19 |
| planners write in place, device cache, spin lock (ft5/ft6) | - | 0.254 | 0.38 | - |
| profile fp1 | - | - | - | the render thread waited 0.89 ms/frame in GpuRingWaitForSpace: the CP was the limiter |
| record walk inlined in Pm4Engine::nativeRecord (ft7/ft8, 95 fps, noisy) | - | - | 0.40 | - |
| lone plain register as a direct store; runs split around side-effect registers (ft9/ft10) | d5d14ba7f9eb | 0.253 | 0.29 | see below |

The last step dropped writeReg under the record walk from 0.38 to 0.08 ms/frame.

**Result (ft9/ft10, pooled 118 windows):** tick **-0.187 +-0.186 ms**, frame p50 -0.264 +-0.126,
fps **+2.85 +-1.56**, CP CPU within the CI.

**Same-exe profiles, fp0b (PM4) vs fp3 (record):**

| Mode | CP running (ms/frame) | fps | Render thread (ms/frame) | Ring-progress waits (ms/frame) |
|---|---|---|---|---|
| PM4 (fp0b) | 9.25 | 105.2 | 7.59 | 1.04 |
| Record (fp3) | 9.26 | 106.2 | 7.37 | 0.76 |

At about 105 fps the CP (9.25 ms/frame) is the pipeline's limiter in both modes.

The per-draw CP bar of 0.20 us came from execute self + writeReg. It left out the bulk
register-copy leaf, which both paths pay, so the whole-frame A/B is the criterion.

**Default ON (batch72, exe 418fc8be8bcc).** vr1 (the default config, whole-run frames): ingame
112.2 fps, 6,835 PM4 packets and 4,502 native records per frame, every era correct, icall 0, 0 bad
records; fc5 CHECK 9.96M draws, BAD 0 / 0 / 0; replay 9/9 byte-identical against the old base
(go6) and against the dev-merged base 420a30b1 with the flush on and off (go7).

### 2.20 Strand A S2a: native records carry only the constants that changed (iter-467 native A, branch native/n0)

`SR_NATIVE_CONST_DEDUP` and `SR_NATIVE_CONST_DEDUP_MODES`, default ON from 5c2f492e (`=0` keeps the
A/B). The S1 flush runs the dedup core from 2.13 inside the record path.
- Per 16-register ALU group, a group the stream already holds is left out of the record. "Holds" is
  the mirror, kept valid by the segment epoch and the adapter's scan of foreign packets.
- The XDK length does not change: the span's 'NFLU' marker covers the skipped bytes, and an
  all-skipped span becomes a header-only record.
- Guest calls inside the flush re-anchor the adapter (it rescans their packets); kicks re-sync
  through the epoch.
- CHECK (`SR_NATIVE_CONST_DEDUP_CHECK=1`, which turns the skips off by itself): each predicted run
  goes into the record with tag bit 30, and the CP proves it against its register file before it
  writes it.

**Correctness:**

| Roll | Gates | Result |
|---|---|---|
| s2c1 | CHECK + MODES | 1.01B groups planned, 79.7% resident. The CP checked 1.36B marked groups, replays included: 0 BAD. icall 0 |
| s2m1 | CHECK + MUTATE (no foreign-writer scan) | 340 BAD: the check catches a missed writer |
| s2on1 | ON, whole-run frames | Every era correct. Record bytes 71.6 -> 22.3 GB per roll (80% of groups skipped). CP bad records 0 |
| d1 (baked, default config) | - | Ingame 124.5 fps. 6,879 PM4 packets and 4,533 records per frame. icall 0. Sheet correct in every era |
| chk3 (baked, every native-a check_roll gate) | meter, census, dedup CHECK, flush CHECK | All PASS. Dedup CHECK 538M groups 0 BAD; flush CHECK 9.18M draws, BAD 0 / 0 / 0 |
| go8 | replay vs dev 8f3e3c39 | 9/9 byte-identical with the dedup on and with =0 |

**Speed (s2ab1 + s2ab2, in-roll A/B, `SR_AB_TOGGLE=SR_NATIVE_CONST_DEDUP`, pooled 118 windows,
paired):**
- CP: -0.316 +-0.157 ms/frame.
- Frame p50: -0.283 +-0.105 ms.
- fps: +4.36 +-2.32, at ~122 fps.
- `tools/perf/ab_windows.py --threads=...` now also prints `cpf:<thread>`, the thread's CPU ms per
  frame, pooled. That is the per-frame cost that sets the frame rate.

## 3. Logging

`SR_LOG=player` (the default for a plain launch) keeps only milestone lines (section 1). It is a
correctness/size win (a player log is kilobytes), not a frame-rate win (2.1).

app/log-async (docs/TOOLKIT.md 3.2): no logging thread writes the log file any more. Producers format and
enqueue under the log lock, which holds no I/O; one writer thread writes. A slow disk now shows up as
`[LOG_STALL]` lines instead of a frozen CP: tcmp1 census0's CP spent ~4.85 s inside a log flush.

## 4. Memory

SR_GUEST_MIRROR=copy|commit|lazy (section 1) - not A/B'd yet.

## 5. Changes and A/B evidence

| Change (commit) | Gate (default) | Evidence |
|---|---|---|
| HLE mirror + present rendezvous follow the HLE draw path (e251a674) | SR_HLE_MIRROR / SR_API_PRESENT_BLOCK (OFF under SR_PM4_LIVE, =1 forces) | g5 9.3 -> g8 13.1 ingame, title 14.9 -> 26.4; frames whole run unchanged (g8 sheet); mirror off with the rendezvous ON = 4.9 fps (g7: each Swap waits the 200 ms fail-open) |
| SR_PHYSWIN_MEMO (b6f93cff) | ON | CP ib 37-42 -> 28-35 ms/frame; SR_PHYSWIN_MEMO_CHECK=1 roll m2: 67M lookups, 60% hits, 0 mismatches; difftest physwin 49/0, win 15/0, physib 55/0 |
| HLE texture capture follows the mirror (c373be8b) | SR_S6_TEX (OFF under SR_PM4_LIVE) | SetTexture hook 2.6% of the render thread; live A/B pending |
| Profiler: `__imp__sub_X` kept (c373be8b) | - | merging the guest body into its hook billed guest work to hooks |
| SR_PM4_DIAG_TIMING (e7d8a7df, a4) | OFF (=1 restores the census ms) | QPC was 7% of CP samples; q8 =1 16.7 vs 16.9 ingame |
| SR_PM4_BULK_TYPE0 (67185d9c) | ON (=0 A/B) | writeReg 12.4% self of the CP (p1_prof); replay register-write counts identical |
| Native XDK leaves: Set{Vertex,Pixel}ShaderConstantF + GpuEmitType0RegPacketsVec as host code (native lane 700acdb5, 20170e90) | SR_NATIVE_CONST_LEAF / SR_NATIVE_TYPE0_LEAF (ON, =0 A/B) | 24.8% of the gameplay render thread (p1_prof). Same exe (snapshot 9f5619cfb2b2), gameplay/port, archive/v1_native f1-f4: ingame ON 20.0 / 21.0 fps (p50 48.7 / 47.5 ms) vs OFF 17.1 / 17.8 (p50 58.6 / 57.3); title 25.8 / 26.6 vs 24.4 / 24.4. Per-call guest differential (c1): 134M constant calls and 6.29M type-0 calls, checkBad 0 (26.5K type-0 calls took the chunked path = guest body) |
| native: CP costs - exact incremental command-buffer-window memo, range-validated grant memo, [IB_RANGE] note + hang-watchdog deep walk opt-in, SSE2 index remap, XXH3 at AVX2 width, used constant blocks only (native lane c136b649, 54790a94, 6e8bc461 + this merge) | SR_PHYSWIN_CB_MEMO / SR_PHYSWIN_GRANT_MEMO / SR_PM4_IB_SIMD / SR_HASH_AVX2 / SR_PM4_CONST_USED (ON, =0 A/B); SR_PM4_IB_NOTES / SR_GPU_HANG_WD_DEEP (OFF, =1) | render thread on, gameplay/port, archive/v1_native: ingame 31.3 / 31.3 (h1/h3, all off) -> 35.4 / 33.0 (h2/h4); CP ib 3440-3621 -> 2779-2891 ms per 120 swaps; memo check h5: 134M lookups, 70% hits, 0 mismatches; replay 9/9 byte-identical; whole-run frames h6 (intro movies, menus, gameplay) correct; 0 ICALL |
| native: the CP drain's page hashes outside m_shmMu + range window before the lock (native lane 09e7950c) | SR_SHM_FLUSH_UNLOCKED (ON, =0 A/B) | on top of hfr eccce508, render thread on, interleaved: ingame 38.8 / 38.4 (m1/m3 locked) -> 39.5 / 39.5 (m2/m4); replay 9/9 byte-identical; whole-run frames m4 correct; 0 ICALL |
| perf2: PPC_STORE watchpoints compiled out (ecf04c1e) | CMake SR_STORE_WATCHPOINTS (OFF) | 2.4; exe 454.5 -> 310.1 MB |
| perf2: constinit guest base + prologue `sr_gb` (edb4ad12) | - | 2.4; per-access TLS init guard gone; exe -> 292.4 MB |
| perf2: aliasGuestVA slow path out of line (45425be7, f40e0a06) | - | 2.4; aliasGuestVASlow = 3.0% of the guest render thread (r_b4rt_prof) |
| perf2: guest lib -mssse3 (e664dcf3) | CMake SR_GUEST_ISA (ssse3) | 2.4; golden_call outputs byte-identical; exe -> 152.2 MB |
| hfr: pm4ShmResolve pre-hashes the resolve extent before m_shmMu; the rebaseline only commits (perf/hfr 6949d5f8) | SR_SHM_REBASE_UNLOCKED (ON, =0 A/B) | the CP was blocked on m_shmMu 16.4-17.8% of its samples while the render thread hashed ~200 pages x 2 windows per resolve under it (b2_unc / b3_on) -> 9.2% (b4_g). Same exe, interleaved, UNLIMITED: ingame 34.7 / 35.65 fps vs 32.7 / 33.0 (=0), CP ib 23.2 / 23.3 vs 25.7 / 25.0 ms per swap (archive/v1_hfr/a1_*, a2_*). Replay 9/9 byte-identical vs dev incl. gp1/gp2/lg1/lg2; whole-run frames correct; 5/5 SUCCESS, 0 ICALL |
| hfr: texture alias probe skipped when src is already the 0xA window; per-batch phase timers under SR_PM4_DIAG_TIMING (bdd33d4a) | - | 2.2% + ~1.1% of the render thread (b2_unc line profile); not on the CP path |
| SR_PM4_RENDER_THREAD baked ON (perf2) | ON (=0 synchronous A/B); SR_PM4_RT_AFTER=1 | 2.5: same exe, ingame 31.6 / 31.5 / 32.0 fps (p50 31 ms) vs 22.3 synchronous; menu 30.4-36.8 vs 26.4; title 37.5-42.1 vs 39.6; whole run frames correct incl. the boot intro; replay unchanged |
| cp: shared-memory write tracking (kernel/gpu_write_watch.*, Xenia EnableAccessCallbacks model): pages protected (0x8 + 0xA copies) before they are hashed; a still-protected page whose last validation took the request's window is skipped; faults release 64-page blocks with a 1-64 frame cooldown; NtReadFile / the replay's raw reads release + pin first | SR_SHM_WRITE_TRACK (ON, =0 A/B); SR_SHM_WT_CHECK=1 (verify), SR_SHM_WT_TEST_DROP=1 (mutation) | 2.6: hashed pages 33-35K -> ~190 per frame; replay 9/9 byte-identical with the check on (gp1: 78,951 -> 5,837 hashed, 0 stale); mutation: stale 2 / 1,710 / 2 (gp1 / lg1 / menu), lg1 frame differs; live k1_check 370.7M checks, 0 stale |
| cp: post-resolve rebaseline keeps hashes still current under the page's protection generation (gww::protGen) | SR_SHM_WT_REBASE (ON, =0 A/B) | 2.6: 110M pages kept per run; the render thread 14.2 -> 7.2 ms with the texture marks below |
| cp: texture clean marks outlive the present when every loader page is protected and unchanged (released blocks invalidate at the present) | SR_SHM_WT_TEXCLEAN (ON, =0 A/B) | 2.6: 361K tracked vs 47K frame-only marks per run |
| cp: vertex staging - per-fetch window choice and the pre-decode dedupe key skipped on the shared-memory path; strip / list / point index remap SSE4.1 (tests/perf/cp_simd_selftest.cpp 200,280 cases, 0 failed; 2 mutations caught); per-drain range memo + lazy range window in pm4ShmRequest | SR_CP_VSF_NOWIN, SR_CP_IB_SSE41, SR_CP_SHM_LAZYWIN (ON, =0 A/B) | 2.6 (bundle) |
| cp: vertex staging - identical fetch-constant buffer rebound; converted index lists reused within the set while the guest buffer's pages hold their protection generation | SR_CP_VSF_CBDEDUP, SR_CP_IB_CACHE (OFF, =1) | replay 9/9 byte-identical (gp1: 801 / 1,425 index conversions reused, 579 fetch buffers rebound); live: no gain at the 60 Hz cap (2.6 dev re-run) - kept OFF |
| cp: type-0 bulk run as one pshufb copy; an empty draw queue rewinds to slot 0 on the producer thread (outside the cp scope split - coordinator's call) | SR_CP_BULK_SSE, SR_CP_QUEUE_REWIND (ON, =0 A/B) | 2.6 (bundle) |
| tick: guest lib ISO volatile (-fno-ms-volatile: guest accesses were acquire/release atomics = a compiler barrier each) + `pure` aliasGuestVASlow | CMake SR_GUEST_ISO_VOLATILE (ON) | 2.8: ingame 78.9 / 76.5 (b1/b2) vs dev 73.9 / 76.15 (a1/a2); L6 54/0; golden_call byte-identical, same oracle diffs |
| tick: native leaves skip vacuous range checks + inline vec4 copy; CS try-first + lock-free CS lookup; S6 UP/IDX follow the HLE mirror; native guest memcpy | SR_NATIVE_LEAF_FASTRANGE, SR_CS_TRYFIRST, SR_CS_FASTMAP, SR_S6_FOLLOW_MIRROR, SR_NATIVE_MEMCPY (ON, =0 A/B) | 2.8: with ISO, ingame 81.6 / 83.4 (c1/c2), baked 80.3 / 78.2 (d1/d2) vs dev 71.75-76.55; the CP is the wall again (idle 0.02 ms/swap); k2 checks 0 bad; replay 9/9 byte-identical |
| tick: guest PGO plumbing (instrumented / profile-optimised guest lib, periodic profile dump) | CMake SR_GUEST_PGO (empty) | not trained yet |
| cp round 2: physwin answers memoised across drains (per-page mutation counters), lean shm requests, lean drain counters, in-place batches, shader-load memo, changed-only constant blocks, texture-collection skip, shader find memo, SSE SET_CONSTANT, first-batch pending end (perf/cp2) | SR_CP_OWN_MEMO, _SHM_LEAN, _DRAIN_LEAN, _BATCH_INPLACE, _SHLOAD_MEMO, _CONST_TRACK, _TEX_SKIPSAME, _SHADER_MEMO, _SETCONST_SSE, _PEND_FIRST (ON, =0 A/B) | 2.9: CP ib+swap 10.6 / 10.3 -> 8.2 / 7.3 ms per swap, CP idle 0.04 -> 2.2-2.4 ms; ingame 92.4 / 95.3 -> 93.0 / 101.5 fps (guest tick is the frame); checks 0 bad; replay 9/9 byte-identical |
| native N1 phase 1: ShaderPs/Vs_UploadAllConstants F lists as host code (native/n0) | SR_NATIVE_CONST_UPLOAD (ON, =0 A/B); SR_NATIVE_CONST_UPLOAD_CHECK (check) | 2.10: tick 10.31-10.39 -> 9.17-9.25 ms, ingame 95.4-97.5 -> 107.8-109.1 fps; check 25.4M calls 0 bad; projection 0 new; replay 9/9 byte-identical |
| native A: stream-resident constant dedup (0bfd149f, 15119d97) | SR_NATIVE_CONST_DEDUP + SR_NATIVE_CONST_DEDUP_MODES (OFF); SR_NATIVE_CONST_DEDUP_CHECK (check) | 2.13: CHECK 0 BAD (204.8M / 1.08B / 360M checks: PM4, +modes, +SR_NATIVE_DRAW), mutation 304 BAD, replay 9/9, sheets every era; 81% of groups skipped; NO gain - tick 9.02 / 8.82 (off) vs 9.07 / 8.96 ms (on), CP execute / writeReg unchanged: kept OFF for the record path |
| native A: CP drain exact caches (SR_CP_VSF_CBDEDUP, SR_CP_IB_CACHE, SR_CP_PSO_MEMO) + SR_CP_DRAIN_SKIP_EMPTY (236f721a); SR_CP_DRAIN_STATS census; SR_CP_DRAIN_ASYNC | all OFF | 2.13: async rejected (no overlap + hitches); caches + skip-empty no gain at uncapped (fps 116.5-120.5 vs 116.8-119.2 off, CP 7.31-7.76 vs 7.23-7.34 ms/frame); drain = 1.34 us per command, empty drains ~free; CP speed work stopped (the CP is the oracle) |
| stutter: SR_HITCH_LOG hitch recorder (kernel/hitch_log.*), [HITCH]/[HITCH_SUM]/[HITCH_IO], profiler hitch windows, hitch_report.py | SR_HITCH_LOG (OFF, observe) | 7.1-7.2 |
| stutter: overlapped guest reads complete on an async worker (STATUS_PENDING, IOSB + event + file object at completion; PENDING -> 997) | SR_ASYNC_READ (ON, =0 A/B) | 7.3: slow-disk A/B 264/259 -> 118/131 hitches, 43/40 -> 19/18 > 2x, excess 5.5/5.0 -> 2.6/2.5 s; render-loop reads 88 -> 0; movies speed 1.00, maxGap 35-40 ms; difftest 7/7, mutation fails 3; whole-run frames f7 correct, 0 ICALL |
| stutter: slow-disk diagnostic | SR_DIAG_READ_LATENCY_MS (off, named diagnostic) | 7.1 |
| guestcode r4: native ClothUpdateElementLightMask (+ its culling callees), Cloth_SatisfyDistanceConstraints, Cloth_CollideParticlePositionSphereCapsule, PoseSnapshot_GetWorldMatrix (kernel/native_cloth.cpp, native_clothsim.cpp, native_pose.cpp, native_vmx.hpp) | SR_NATIVE_CLOTHMASK, SR_NATIVE_CLOTHSIM, SR_NATIVE_POSE (ON, =0 A/B); *_CHECK (check), *_AB (timing); SR_NATIVE_CLOTHMASK_THREADS (0) | 2.12: tick 9.07-9.26 -> 8.53-8.64 ms, ingame 109.3-110.3 -> 115.5-116.8 fps; check 3.66M / 20K / 11.1M / 11.2M calls 0 bad (threaded 2.22M parallel calls 0 bad); whole-run sheet incl. intro and the cape |
| guestcode r5: native RenderStateBlock_ApplyShaderParams, AppendVertexData, ClothUpdateBoundsAndLights (kernel/native_params.cpp, native_vtx.cpp, native_cloth.cpp) | SR_NATIVE_PARAMS, SR_NATIVE_VTXAPPEND, SR_NATIVE_CLOTHBOUNDS (ON, =0 A/B); *_CHECK, *_AB | 2.16: CHECK 0 bad, per call 6-16% faster, live 3x3 + in-roll no regression |
| guestcode: constant-upload skip on write-watched sources | SR_NATIVE_CONST_SKIP (OFF); _CHECK, _MUT (mutation test) | 2.16: rejected, +2.6 ms (64-page watch), +0.9 ms (4 KB watch) |
| guestcode: in-roll A/B toggling (kernel/ab_toggle, tools/perf/ab_windows.py) | SR_AB_TOGGLE, SR_AB_SEED (perf diagnostic) | 2.16 / TOOLKIT 1.8d: skip +2.88 +-0.23 ms in-roll vs +2.64 classic |
| native A S1: the XDK draw flush as host code writing native draw records (kernel/native_flush) | SR_NATIVE_FLUSH (ON, =0 A/B); SR_NATIVE_FLUSH_CHECK, SR_NATIVE_FLUSH_TIMING | 2.19: ft9/ft10 in-roll tick -0.19 +-0.19 ms, fps +2.85 +-1.56; fc5 9.96M draws 0 BAD; meter 60,776 -> 6,835 PM4 packets per ingame frame (vr1); replay 9/9 |
| native A S2a: records carry only the constant groups the stream does not hold (the dedup core in the record path) | SR_NATIVE_CONST_DEDUP + SR_NATIVE_CONST_DEDUP_MODES (ON, =0 A/B); SR_NATIVE_CONST_DEDUP_CHECK (check) | 2.20: in-roll CP -0.32 +-0.16 ms/frame, fps +4.4 +-2.3; CHECK 1.36B groups 0 BAD, mutation caught; record bytes -69% |
| guestcode codegen: ThinLTO for the whole exe | CMake SR_THINLTO (OFF for dev; ON in the "player" preset, build-player/) | 2.18: -0.13 ms tick, +3.5 fps (cm17, 3/3 rounds); L6 54/0, golden_call identical, replay 9/9 |
| guestcode codegen: direct calls on the 33 hottest small-callee guest edges (tools/regen/direct_calls.py, build guard) | not applied (experiment) | 2.18: +0.04 ms with ThinLTO + import limit 2000 - rejected |
| guestcode codegen: alias-free guest addressing (measurement) | CMake SR_GUEST_ALIAS_IDENTITY (OFF; needs SR_PHYS_UNIFY=1) | 2.18: -0.10 ms vs the fold under the unified model, which itself costs +0.57 ms |

## 6. Rejected / open

- The CP render-thread split pays once the guest frame drops under the CP's (~64 ms with the
  memo, ~35 ms without the swap).
- The GameKernel runners' yield-spin (4 cores at ~97%) is the console's design (Xenia spins the
  same); it costs boost clock and SMT siblings, not correctness. Not changed.
- Port-feature cache fragility (seen 2026-10-03 21:02): `mods/.cache/gen/port-custom_frontend-*`
  lost its `.complete` marker; `mod_manager.cpp` then regenerates into `.tmp` and the rename
  onto the existing directory fails ("Access is denied") on EVERY boot, so the custom front end
  silently falls back to the natural one. Owner: the mod lane (remove the stale directory before
  the rename).
- Native ApplyShaderParams / AppendVertexData / ClothUpdateBoundsAndLights (2.14): exact, 6-16%
  per call - memory-bound; removed, then restored and baked under the 2026-10-07 per-call rule (2.16).
- Constant-upload skip on the 64-page write watch (2.16, SR_NATIVE_CONST_SKIP, default OFF): exact
  (CHECK 0 bad, mutation fires) but +2.6 ms on the tick: 0.6% of the sources skippable, the
  metadata and protection churn paid on every register; a 4 KB-granular watch: 20% skippable,
  still +0.9 ms.
- 2 MB pages for the guest reservation (2.17): page walks are <= ~2% of the guest render thread.
- Alias windows as views / aliasGuestVA -> identity (2.18): the fold costs ~0.10 ms of codegen, the
  unified view model +0.57 ms; not worth the memory-model change now.
- Direct calls / forced inlining on hot guest call edges (2.18): +0.04 ms; the small callees' time is
  data work, not call overhead.
- Threaded ClothUpdateElementLightMask element pass (2.12): exact, but slower per call than the
  single-thread native body (1.60-1.76 vs 1.41-1.44 us); OFF (SR_NATIVE_CLOTHMASK_THREADS=0).
- Host profiler (kernel/host_profile.cpp) race, seen once (2.12, `cm6chk3`): the profiler thread
  AV'd in RtlVirtualUnwind2 reading a stack page that was not mapped while walking another
  thread; RtlVirtualUnwind reads beyond the [Rsp, StackBase) the loop checks between frames.
  Owner: the profiler; bound the frame reads (or skip threads whose TEB stack limits moved).

## 7. Stutter: the hitch census and the fixes (iter-466 stutter lane, branch perf/stutter)

Owner, 2026-10-05: "have you noticed the random stutters in the game?" Average fps hides hitches,
so this lane counts them. A hitch = a host present interval > 1.5x the rolling median of the
previous 120 (> 2x and > 4x counted too). Rolls: `perf_run.py --scenario gameplay --config port`
(60 Hz target, render thread on), `archive/v1_stutter/`.

### 7.1 Tools

| Gate / tool | What it does |
|---|---|
| `SR_HITCH_LOG=1` (observe, default OFF; kernel/hitch_log.*) | every subsystem that can stall a frame bumps a slot (count, bytes, ms, longest): PSO creation, CreateCommittedResource, shm texture loads, resolves, runtime shader xlat, write-track protects + faults, guest NtReadFile / NtWriteFile / NtCreateFile / NtAllocateVirtualMemory, the frame-fence wait, Present, render-thread idle, CP submit wait. `[HITCH]` per hitch (the interval, the median, the guest swaps inside it, every slot that moved), `[HITCH_SUM]` every 10 s, `[HITCH_IO]` per guest read slower than `SR_HITCH_IO_MS` (20): thread, render loop or not, host file, offset, lock vs ReadFile ms. With `SR_HOST_PROFILE_HZ` the profiler writes each hitch window's stacks (CP, host render thread, guest render loop) to `hitch_samples.txt` |
| `tools/perf/hitch_report.py <roll> [--era ingame]` | per-era census from frames.csv + attribution of the `[HITCH]` lines |
| `tools/perf/stutter_play.txt` (`perf_run.py --play`) | a streaming-heavy route: superspeed flights across Metropolis, hover, landing, walk, punches |
| `perf_run.py --gates SR_PRESENT_DUMP=0` | now overrides the scenario's own gate. A census roll must not dump: every sparse present dump is a synchronous readback + BMP write on the render thread, 150-570 ms each |
| `SR_DIAG_READ_LATENCY_MS=<ms>` (named DIAGNOSTIC, default off, timing only) | every host read of a guest NtReadFile waits <ms> first, on whichever thread performs it: a slow disk on demand, so an I/O A/B does not depend on what the OS file cache kept from the previous roll |
| `tools/perf/movie_luma.py <roll> --movie N --source <yavg.csv>` | presented FMV frames vs the source movie's per-frame luma (the oracle), with the guest's display model across the source's timestamp gaps |

### 7.2 Census before (roll h1_a: dev b33d266e + the observer, 60 Hz, cold disk cache)

| era | host p50 / p95 / p99 / max ms | hitches >1.5x / >2x / >4x | excess ms |
|---|---|---|---|
| intro movies | 17.5 / 34.8 / 73.9 / 2321 | 250 / 66 / 12 | 14,697 |
| title | 16.6 / 22.1 / 28.9 / 492 | 32 / 14 / 9 | 2,277 |
| ingame (148 s) | 16.9 / 33.5 / 37.1 / 680 | 245 / 53 / 26 | 10,379 |

| cause | count | ms | note |
|---|---|---|---|
| guest file reads holding the frame | 2,185 reads (whole run) | 20,600 total, max 1,405 per read | one 128 KB movie read 270-536 ms on the render loop; the intro's 2.3 s freeze = 63 reads, 3.25 s. D: is an HDD, the box is RAM-starved |
| present-dump captures | every 300 presents | 150-570 each | test artifact (not in a player's run) |
| GPU fence waits | 24 of 41 hitches in a warm roll | up to 172 per wait | GPU-side spikes; open |
| resource creation | 1,567 | 608 total, max 1.3 | a 405-texture loading frame = 140 ms |
| write-track faults | 5,553 | 107 total, max 28.7 | |
| PSO creation | 157 | 72 total, one 92 ms at boot | not a cause with a warm driver cache, so no shader_precompile / shader_async keys were added |
| movie / cutscene at 30 fps | - | - | the media clock, by design (not stutter) |

### 7.3 The fix: SR_ASYNC_READ (default ON)

The console completes a read on a handle opened for overlapped I/O asynchronously, and the title's
stream readers are built for it (CalAsyncFileStream_Read_sub_824776F0 re-issues ReadFile on the
freed blocks of its ring and waits only on the block it consumes). Completed synchronously on the
calling thread (Xenia's NtReadFile does the same), each re-issue blocked for the whole host read.
Now a real-convention NtReadFile with an explicit offset on a non-synchronous handle clears the
event and the file object's event, writes IOSB {PENDING, 0}, queues the read to one FIFO worker
("SR async read") and returns STATUS_PENDING; the worker reads under the same per-handle lock /
position / EOF rules, writes the IOSB, sets the event + the file object and wakes the issuer's
alertable waits. RtlNtStatusToDosError maps exactly STATUS_PENDING -> 997 under this gate.
`difftest --async-read-test` 7/7 (mutation =0 fails A1 A3 A4); `--atomic-read-test` 12/12.
Wiring found by the live roll, not the difftest: NtCreateFile recorded the synchronous flag
only under SR_READ_PENDING, so the first proof batch (h4/h5) never went async.

Same exe (snapshot 3544591737e7-stutter-h7), gameplay + stutter_play route, everything after the
HUD request (the opening FMV, the tutorial, the flights), `SR_HITCH_LOG=1 SR_PRESENT_DUMP=0`:

| | hitches >1.5x | >2x | >4x | excess ms | max ms |
|---|---|---|---|---|---|
| slow disk (`SR_DIAG_READ_LATENCY_MS=40`), async OFF (l7_off1 / l7_off2) | 264 / 259 | 43 / 40 | 3 / 1 | 5,517 / 5,046 | 961 / 823 |
| slow disk, async ON (l7_on1 / l7_on2) | 118 / 131 | 19 / 18 | 3 / 1 | 2,636 / 2,483 | 511 / 913 |

Noise floor of the route: the four warm-cache rolls on exe h4 (c_off1/2, c_on1/2 - async never
engaged there, see the wiring note) range 118-203 hitches, 7-30 > 2x, 2,076-3,045 ms excess.
Under the slow disk, reads on the guest render loop go from 88 (all fmvlegal) to 0; the movie
and stream reads move to the worker. Movies with async on and a present dump every 40 (f7):
every intro movie and the opening FMV at speed 0.995-1.003, maxGap 35-40 ms (h4 without async:
movie#7 maxGap 734 ms, 32 late). p99 after the HUD stays ~34 ms in every roll: the opening FMV's
30 fps frames are in that window. Whole-run frames f7 (`archive/v1_stutter/f7_sheet.jpg`): EA,
DC, WB, legal, title, MAIN MENU, the opening FMV, gameplay - correct; 0 ICALL.

### 7.4 What is left

- The max (511-913 ms with a slow disk) is the gameplay-load transition: synchronous reads on the
  loader thread (sound.AST 88 KB x ~930, ui.AST, voice.AST, lunits.AST, objtiv.AST - handles
  opened FILE_SYNCHRONOUS_IO) that the render loop waits on. Next: a host read-ahead for those
  files, or find what the render loop waits for.
- GPU fence-wait spikes (up to 172-261 ms) in warm rolls: GPU-side, not attributed yet.
- The profiler's hitch windows (sampling only the frame threads) are untested since the first
  version, which sampled every guest thread each tick and wedged a boot (CS_WEDGE, h1_b).

### 7.5 The "dark opening cutscene" (guesttick report) - not a stutter or high-fps bug

The opening FMV (fmv.AST entry 150, WMV3, 818 frames over 30.27 s) is variable-rate: four cuts
have timestamp gaps (12.37 -> 13.40 s, 15.37 -> 16.23, 18.20 -> 18.90, 22.07 -> 22.60) where the
source holds a BLACK frame. The guest copies the planes before its hold decision (ENGINE_MAP
6g.1b), so for each gap the next shot's first fade-in frame (luma ~19-25, "3% bright") is on
screen instead of black, then the shot fades in on time. `movie_luma.py` shows the same at 60 Hz
(c_frames_on, f7), 120 Hz (d_120_off), uncapped (d_unc_off, d_unc_on, guesttick's c2), with and
without async reads: every presented frame matches the source under that model. The remaining
~7% darker midtones are the game's own scan-out gamma ramp (`[GAMMA_OBS]`: green 128 -> 115, set
once at boot). Not checked against Xenia frames yet.

## Lane status (2026-10-03 22:40, handoff block)

- Done (perf/platform): gameplay profiled (section 2.1); HLE mirror/rendezvous/texture capture
  follow the HLE draw path; SR_PHYSWIN_MEMO; tools (gameplay scenario, attribute.py, ingame era).
- Measured on dev 92042658 (2.3): ingame 10.8 -> 16.9 fps, title 16.8 -> 31.9, menu 13.5 -> 19.0.
- Not changed: the guest render frame (~58 ms) is now the limiter - codegen (inlining the
  __savegprlr helpers, ThinLTO/PGO) and native (the XDK command building) lanes.
- Next: CP cost (xlatBindBatchTextures 25%, writeReg 11%, pm4StageVsf 11%, shm validate 10% of
  the CP) and the render-thread split once the guest frame is below the CP's.
