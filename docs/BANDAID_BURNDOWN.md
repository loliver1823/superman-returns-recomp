# ⛔ FORCING INVENTORY — iter-360 audit (2026-07-18, verified)

Full kernel/ sweep for ungated/undisclosed forcing (fabricating guest state the game
should produce itself). **16 live forcing sites NOT behind the master gate
`forcingAllowed()`/SR_ALLOW_FORCING** (~9 fully ungated, ~6 own-gate default-ON). This is
the map to burn down so we stop being ambushed by hidden fakes.

**THE TRUNK — #1 heap stub (attack first; most others are downstream of it):**
`guest_context.cpp:1876-2007` fabricates a dlmalloc mstate @0x82110000 + fallback heap @VA 0 +
crit-sect table, and repoints the 4 real heap-handle globals at the stub, **preempting the
game's own dlmalloc init** (`Function_8246A2A8`→`sub_8246C4D0`→`NtAllocateVirtualMemory`, which
only runs `if DAT_829ac16c==0`). Now gated `SR_HEAP_STUB` (iter-360j, default-ON). A/B
(SR_HEAP_STUB=0) PROVED the real init never runs at all — it's reached only via CRT `_initterm`
(the "missing walker", [[project_missing_walker_found]]), which doesn't fire. **Root-fix = run
the guest's own heap/CRT init; then #2-#6 below may all fall away.**

**Fully ungated (no env var):** #2 `dogma_pool_registry_seed.cpp:55-90` fake pool+vtable →
`*(0x829A93F4)`; #3 `guest_context.cpp:1674-88` DOGMA guard globals 0x829AAE78/74=1; #4
`:1755-80` allocator vtable slots @0x8249AAC8; #5 `:1835-74` stub XEX module/process/header info;
#6 `crt_file_lock_init_fix.cpp:92-96` fabricated CRT file-lock crit-sect; #7
`cs_timeout_underflow_fix.cpp:340-47` loadMovie this=0 → Apt mgr-root substitute; #11
`dogma_hash_hooks.cpp:131-232` fabricated hash buckets + forced {0,0}; #12
`cs_timeout_underflow_fix.cpp:185-235,375-96` corruption-suppression guards (skip guest writes);
#18 `guest_context.cpp:1256-1310` splash filename backfill (check-first, platform-leaning);
#16 `gpu_ring_alloc_fix.cpp` bit31 path = guestHeapAlloc+memset substitute.

**Own-gate default-ON (NOT the master gate — still a charter violation):** #1 `SR_HEAP_STUB`,
#8 `apt_as_watch.cpp` `SR_AS_AUTO_FRAME` (AS auto-frame), #9 `stubs.cpp:2466/2559` `SR_VD_ISR`
(vdVblankThread fires guest gfx-ISR@60Hz + synth GPU-complete r3=1 = kernel driving the whole
per-frame guest chain), #14 `audio_render_driver.cpp:574` `SR_NO_MODGLOB_POLLER` (20kHz thread
pinning GameKernelMgr slots), #15 `fe_flow_watch.cpp` `SR_CFG_SOUND_OFF`, #16 `SR_ALLOC_FIX`
(+0x10 bump), #17 `init_wait_bypass.cpp:48-72` `SR_RUN_REAL_GAMELOOP` (quit-flag write). #13
`audio_render_driver.cpp:245-87` XAudioRegisterRenderDriverClient thread (ungated, platform-leaning).

**Dead/latent (delete so they can't re-arm):** #10 `bootstrapAudioClient` (no live caller since
iter-357d); #19 `fm_world_ptr_patcher.cpp` (observe-only, kObserveOnly=true).

**Confirmed PLATFORM FIXES (NOT forcing, keep):** KeTimeStampBundle, EDRAM/GPU-reg zero-fill,
fake GPU MMIO @0xD0000000 (PM4→D3D12), recomp_switch_fixes (XenonRecomp switch-hole reimpl),
fm_module_walker _initterm skip. **Correctly gated (model):** splash_hooks post-splash driver,
fe_flow_watch auiNatDrive/simMgrGuard, rtam_service, MENULOAD/GAMECONFIG — all via forcingAllowed().

**BURN-DOWN ORDER:** (1) root-fix the heap trunk (run guest CRT/heap init) — may retire #2-#6;
(2) re-gate the fully-ungated fabricators under `forcingAllowed()` (one-line guards) so the
default exe forces nothing; (3) fold own-gate items onto the master gate (`&& forcingAllowed()`),
splitting facility (platform) from driving (forcing) for #9/#13; (4) delete dead #10/#19.

---

# Band-aid Burn-down Playbook — iter-302 (SR_NATURAL_MODULE_INIT era)

**Rewritten 2026-07-10** after iter-301 landed `SR_NATURAL_MODULE_INIT` (default-ON): the game's
OWN FM-module dependency walk now runs — `SystemRunnerStart_sub_8271E928` →
`FmModuleInitGraphWalk_sub_8271E630` (dep-first) → `FmModuleInitOnce_sub_8271F2C8` →
`module->vt[+0x1C](module)` — initializing all 63 FM modules **with their REAL objects, in
dependency order** (splices: `tools/regen/splice_bodies.json` entries `sub_8271E928` +
`sub_8271F2C8`; gate helper `sr::kernel::naturalModuleInitOn()` at `kernel/guest_context.cpp:901`).
Golden proof: 63/63 modules `obj+0x44==1`; iter-301 run: mechanism works 4/4, ResourceSystem up
naturally every run, FMGameRtam (the historical "blocks forever") completes in 64 s.

**This subsumes the entire drive family** that hand-compensated for modules never initializing
(hand ctor walker, singleton fabrications, module-init drives, registration forcing). The old
compensations now actively COLLIDE with the natural walk (iter-301: render runner wedges in E928
pre-walk; `FMGameRenderInit 0x829601C0` shows `chk=1` = pre-marked by the old drive family).
**Retiring them is not cleanup — it is the fix for the active render-walk-wedge blocker.**

A/B escape at every step: `SR_NATURAL_MODULE_INIT=0` restores the old boot exactly.

---

## 0. How to retire a band-aid (the contract)

1. Add `if (sr::kernel::naturalModuleInitOn()) return/skip;` to its gate (or lambda) — the
   `=0` A/B keeps the legacy path alive. Never hard-delete before a K-soak.
2. One K-run (`.\tools\run_recomp_x.ps1 -K 4`) per **tranche**, not per item.
3. Verdict recorded here (flip status column) + PROGRESS.md; after 2 clean soaks, delete the
   dead code and its BANDAIDS.md row.
4. If a natural per-module hang appears: **fix per-module, never re-skip the walk**
   (`F2C8_vt7_pre/post` logs name the culprit).

---

## 1. Gate inventory (complete census, 2026-07-10)

Classes: **A** = subsumed by natural module init (retire in tranches below) · **A-dormant** =
same family but default-OFF (dead experiment code; delete opportunistically, no K-run needed) ·
**B** = environment fix, KEEP (genuine recompiler/host gap the game can't fix itself) ·
**C** = unknown, needs A/B · **DIAG** = diagnostics, not a band-aid.

Default: ON = `!e || e[0] != '0'` (baked); OFF = `e && e[0] == '1'` (opt-in).

### 1a. Trunk / root fixes (KEEP — these are the natural path)

| Gate | Where | Default | Purpose |
|---|---|---|---|
| `SR_NATURAL_MODULE_INIT` | guest_context.cpp:903 + splices `sub_8271E928`/`sub_8271F2C8` | ON | THE trunk: run the natural E630 dep walk; F2C8 runs every vt[7] with the real object (no hard-skips, no timeout fake-success) |
| `SR_RUN_STATIC_CTORS` | splice `sub_821498F8` → `walkStaticCtors_impl` (fm_module_walker.cpp:212) | ON | The real dropped `_initterm`: registers all 63 FM modules into DAT_829a2bb0 in linker order + clears the two Init1-dirtied list-head cycles |
| `SR_RUN_REAL_GAMELOOP` | guest_context.cpp:1079, init_wait_bypass.cpp:45 | ON | iter-294 trunk: run the game's real loop instead of teardown |

### 1b. Class A — subsumed, default-ON (the retirement backlog; tranche column = §3)

| Gate / mechanism | Where | Tranche | Status | Why subsumed (natural replacement) |
|---|---|---|---|---|
| hand FM ctor walker `walkFmModuleCtors_impl` + `kFmCtorTable` + `kSkipList` (r3=0, alphabetical) | fm_module_walker.cpp (body deleted; entry symbols kept) | 1 | **DELETED** (iter-357d — tranche-1 "delete after 2 clean soaks" contract executed; retired iter-302 d1b0792; old body in git history; `SR_NATURAL_MODULE_INIT=0` legacy A/B gone) | E630 walk inits every module with its REAL object in dep order |
| `fireFmuiCtorDrive` (FMUI ctor drive + wedge takeover) | splash_hooks.cpp:140 | 1 | **RETIRED** (@:144) | FMUserInterface init `sub_821516E0(r3=0x8295FC8C)` runs inside the walk |
| `SR_DRIVE_FMRENDER` (drives Function_8214D950) | splash_hooks.cpp:4238 | 1 | **RETIRED** (@:4240) | FMGameRender init dispatched by the walk — this drive PRE-MARKED 0x829601C0 (`chk=1`, iter-301 wedge suspect) |
| `SR_DRIVE_FMENTITY` (drives sub_8214FF48 screen system) | splash_hooks.cpp:4265 | 1 | **RETIRED** (@:4267) | FMGameEntity init dispatched by the walk (custom middle-triplet slot — walk sees it, byte-scan didn't) |
| `SR_DRIVE_RTAMINI` (drives sub_8214BC88 → ResourceSystem singleton) | rtam_service.cpp (`rtamIniMain`; retire check @:625) | 1 | **RETIRED** (natural check in code since d1b0792, rtam_service.cpp:625) | FMGameRtamIni inits naturally — iter-301 logs `[RTAMINI] resource system already up — no drive needed` 4/4; the 5 s-delayed drive can RACE the 64 s natural walk |
| `SR_DRIVE_MENULOAD` (drives sub_8218D128 menu-package load after RtamIni) | splash_hooks.cpp (retire check @:1074); walker-embedded copy DELETED with the walker (iter-357d); THIRD consumer (sub_8261C5B0 fake-success, cs_timeout_underflow_fix.cpp) charter-gated behind forcingAllowed at iter-357c (7101acb) | 1 | **RETIRED** (natural check in code since d1b0792, splash_hooks.cpp:1074) | FMGameRtam's REAL init (0x821507E0) completes under the natural walk (64 s, iter-301) and performs its own first-call package load |
| EB80/EDF8/EDFC dispatcher **fabrication** (ungated: fake mgr+vtable minted when globals NULL) + fake-mgr EB80 re-write heal | splash_hooks.cpp:521–549, 618–631 | 1 | **TODO** | FMUI GlobalInit publishes the REAL flow client to 0x8296EB80; fabricated fake collides with FE-tick "EB80 must be REAL" precondition (g_fabMgrVA) |
| `SR_DRIVE_ACTIVATE_UI` (fabricates [[uiMgr+0x54]] ready-flag, calls ActivateUI) | splash_hooks.cpp:745 | 2 | pending | Interface activation runs off the natural FE flow once FMUI + WorldFlowXML init (WorldFlowXML Function_8214D508 is IN the natural roots arrays — never executed on any old boot) |
| `SR_FORCE_UI_CFG` | splash_hooks.cpp:783 | 2 | pending | UI config built by FMUI's real init |
| `SR_SET_IFACE_STATE` (drives SetInterfaceState_sub_821745D0(mod,4)) | splash_hooks.cpp:4320 | 2 | pending | FlowMgr/ChangeInterfaceState drives states naturally once the AS/flow layer is fed by real module init |
| `SR_FORCE_UI_VISIBLE` (force-clear hide flag +0x1318) | splash_hooks.cpp:4363 | 2 | pending | Natural = FE flow shows a screen (state 6/8) — BANDAIDS.md row 107 |
| `SR_FIX_RENDER_CLIENT` + page-rotation **countdown park** (client+0xC ≈1e9 s @:4432–4437) | splash_hooks.cpp:4393 | 2 | pending | Natural = activation `ClientSelectModePage_sub_8216DA60` from a REAL mode table (client+0x10, built by FE screen-creation data load) — BANDAIDS.md row 107 |
| `SR_FE_CMD_LATE` / `SR_FE_CMD_RETRY` | splash_hooks.cpp:3993 / 4008 | 2 | pending | FE command fires from MAIN/ICONMENU frame-0 script once movies load naturally with payloads |
| `SR_FIX_LEVEL0` (hand-register MAIN clip as _level0) | splash_hooks.cpp:3878 | 2 | pending | _level0 = sub_826B05F0(0) product of the natural movie instantiation |
| `SR_CALL_MAIN` (hand-invoke MAIN frame-0 actions) | splash_hooks.cpp:3673 | 2 | pending | Frame-0 AS runs from natural instantiation |
| `SR_HAND_TRIO` (hand frame-advance + executor + screen pump) | splash_hooks.cpp:3202 | 2 | pending | Executor/screen pump are per-frame FM module updates — dispatched once modules are real |
| `SR_FIX_BUILTIN_TABLES` (restore scribbled builtin tables from golden bytes) | splash_hooks.cpp:3794 | 2 (A/B) | pending | IF the scribble source was the corrupted half-built object graphs (contended FMUI ctor), natural init removes the scribbler; else reclass B |
| `SR_FORCE_APTCTX` (force default Apt ctx, r4=1) | apt_parse_trace.cpp:422 | 3 | pending | Real ctx built at FE-obj+0xE0 by FMUI's real init (SR_USE_REAL_CTX proved the real ctx exists) |
| `SR_DRIVE_APT_TICK` (drives sub_826B05F0(0) service pass) | splash_hooks.cpp:1384 | 3 (A/B) | pending | Natural = FMUI per-frame module update; NOTE per-frame master dispatch sub_8271F848 has NO static caller (posted job, sub_828EEBDC drain) — verify before retiring |
| `SR_DRIVE_LOADMOVIE` (+`SR_LOADMOVIE_NAME`) | splash_hooks.cpp:1507, 1719, 2224 | 3 | pending | Natural: CIS 'FE' → gSM.LoadOverlayScreen → loadMovie → sub_826D2690 package layer |
| `SR_DRIVE_APTPARSE` | splash_hooks.cpp:2424 | 3 | pending | Parse fires from the natural byte-fetch completion |
| `SR_INJECT_MAIN_BLOB` (embedded MAIN .apt blob inject) | splash_hooks.cpp:2693 | 3 | pending | Blob-inject class: natural RTAM package bind delivers bytes WITH geometry/textures (iter-297k: payload pre-baked in native RTAM assets) |
| `SR_LOAD_ICONMENU` (hand blob load + register) | splash_hooks.cpp:3136 | 3 | pending | Same blob-inject class |
| `SR_LOAD_PRESSSTART` (hand ScrPressStart blobs married to overlay pending-load) | splash_hooks.cpp:5144 | 3 | pending | Same blob-inject class |
| `SR_SHARE_CLIP_REGISTRY` | splash_hooks.cpp:4724 | 3 | pending | Executor ctx registry populated naturally when movies instantiate on the real path |
| `SR_DIRTY_MARK` | splash_hooks.cpp:4765 | 3 | pending | Dirty-mark/stamp happen in the natural frame advance |
| `SR_NODE_COMMIT` (re-commit render nodes sub_82698370) + layer-mask force (~:4881) | splash_hooks.cpp:4816 | 3 | pending | Node mint+commit is part of the natural clip ctor/frame path once payloads exist |
| `SR_FORCE_NODE_VISIBLE` (force bit31 over snapshot node graph) | splash_hooks.cpp:5069 | 3 | pending | Visibility set by the natural display-list ops |
| `SR_TICK_CONTINUE` (extra 4 Hz tick frames + frame-ctr bump ~:5067) | splash_hooks.cpp:4556 | 3 (A/B) | pending | Natural per-frame dispatch replaces the synthetic extra ticks |
| `SR_DRIVE_UI_RENDER` (bracketed begin/8218F8/end dispatch) | splash_hooks.cpp:4448 | 3 (A/B) | pending | Natural = client render dispatch from the module update; keep until render walk verified |
| `SR_RTAM_PREP` (translator acquire sub_826D9BB8 + registry verify) | rtam_service.cpp:303 | 4 | pending | Translator distribution = sub_8214CE18 (FMUI module +0x54) — "the never-ran class", runs under natural init |
| `SR_RTAM_SCHED` (scheduler pump thread) | rtam_service.cpp:154 | 4 (A/B) | pending | Golden pump is COOPERATIVE (waiters pump sched->vt[7]); with real waiters running, the thread is redundant |
| `SR_RTAM_FSM` (streaming FSM sub_8271F848 drain thread) | rtam_service.cpp:252 | 4 (A/B) | pending | Golden drains device I/O via the same FSM as posted work; verify the natural post before retiring |
| `SR_RTAM_NATLOAD` (synthetic load-by-path sub_826D2690 drive) | rtam_service.cpp:347 | 4 | pending | Natural loadMovie exercises the identical package layer with the game's own receivers |
| `SR_FIX_FMUI_MODULE` (substitute static obj 0x8295FC8C when dispatcher passes r3=0) | apt_parse_trace.cpp:288 | — | OFF (dormant) | Natural F2C8 passes the real module object — the r3=0 bug was the OLD dispatcher path |

### 1c. Class A-dormant — default-OFF experiment gates (dead code; delete opportunistically)

No K-run needed (they don't execute); remove the code blocks when touching those files.
`SR_DRIVE_MAINMOVIE`(+`SR_MAINMOVIE_NAME`) :939 · `SR_DRIVE_MAINREAD` :1018 · `SR_DRIVE_MAINPKG`
:1092 · `SR_DRIVE_FEMOVIE` :1206 · `SR_DRIVE_SCREEN` :1275 · `SR_FORCE_LEVEL0` :1396 ·
`SR_DRIVE_APTEXEC` :1791 · `SR_DRIVE_ASSETLOAD`(+`SR_ASSETLOAD_MODE`) :1855 · `SR_NATIVE_REGISTER`
:1898 (known-wrong args, BANDAID_BURNDOWN iter-295 note) · `SR_DRIVE_LOADPKG`(+`SR_LOADPKG_NAME`)
:1944 · `SR_FORCE_SCOPE_STORE` :1980 · `SR_DRIVE_STREAMPUMP` :2005 · `SR_DRIVE_POSTER` :2035 ·
`SR_REGISTER_BOOT` :2083 · `SR_BUILD_APTCTX` :2122 · `SR_FORCE_BFAL` :2167 · `SR_DRIVE_FETCHENQ`
:2274 · `SR_DRIVE_APTARM` :2330 · `SR_DRIVE_APTPUMP` :2382 · `SR_DRIVE_RESPUMP` :2470 ·
`SR_NATIVE_ICONMENU` :3280 (all splash_hooks.cpp) · `SR_USE_REAL_CTX` apt_parse_trace.cpp:654 ·
`SR_ENABLE_RTAM` cs_timeout_underflow_fix.cpp:~1140 (walker copy DELETED iter-357d) ·
`SR_ENABLE_SCRIPT` (walker-only — NO code consumers left after iter-357d) · `SR_FORCE_LOOSE` cs_timeout_underflow_fix.cpp:489 ·
`SR_FORCE_GAMECONFIG` cs_timeout_underflow_fix.cpp:1102 · `SR_RTAM_WORKER` rtam_service.cpp:213
(retired iter-298 v5: garbage call) · `SR_RTAM_LOAD` rtam_service.cpp:406 (retired v8 pivot) ·
`SR_DOGMA_HOST_ALLOC` alloc_overlap_watch.cpp:112/238 · `SR_SKIP_FM_WALKER` splice sub_821498F8
(moot: walker body DELETED iter-357d) · `SR_FALLBACK_LOOP_ONLY` splice sub_821498F8 · `SR_DRIVE_FMUI_CTOR`
/ `SR_DRIVE_MODQUAD` splice sub_821863A0 · `SR_LEGACY_INIT_TIMEOUT` splice sub_825FC108 (the
fake-999 primal source, killed iter-297) · `SR_REAL_DOGMA_POOL` splice sub_8278D258 ·
`SR_CRITSEC_FAKE_SUCCESS` stubs.cpp:2927 (legacy A/B for the real OOR-CS locking) ·
`SR_SHOW_PRESS_START` splash_hooks.cpp:435 (host-rendered demo overlay).

### 1d. Class B — environment fixes, KEEP (reclassify out of the debt ledger)

| Fix | Where | Why it stays |
|---|---|---|
| Out-of-range critsec REAL locking (lock-free 4096-slot registry) | stubs.cpp (oorGetOrCreateCS) | Host locking gap; the saturation fallback shrinks after the storm source dies |
| TLS guest-memory fix; per-thread guest ctx/stack rules; VEH "diagnostics must be fault-free" | kernel/guest_context.* | Host architecture invariants |
| Timer `sub_82564E80` steady_clock override; KQPF 50 MHz semantics; SEH div/idiv length-aware skip | apt_parse_trace.cpp / stubs.cpp / guest_harness.cpp | Correctness, not debt (BANDAIDS.md already marks ACCEPTABLE) |
| `SR_FIX_AS_HASH` (AS hash-ladder jump tables reimplemented) | apt_as_watch.cpp:137/168, ON | Recompiler jump-table gap — game can't fix it; without it every builtin resolve dies |
| `SR_VECFILL_GUARD` (EASTL insert-fill insane-input bail) | rtam_service.cpp:600, ON | Leaf-loop spin guard (iter-296d class); active for the FMGameRtamIni stall; retire only when the scribble root is dead |
| Leaf-loop/structure guards in splices (sub_82191D58 tree, sub_8218F890 scan, sub_821DDD18 copy, sub_827692B8 bounds, VM entry/range guards sub_827657C8/sub_82762E78, hash-family fixes) | tools/regen/splice_bodies.json | Crash prevention for corrupt-input classes; load-bearing until R7 payloads all-natural |
| DOGMA guard-check bypass; longjmp RtlUnwind bypass; DOGMA hash degenerate short-circuits; stack-overflow guard; KeWait DISPATCHER_HEADER auto-create; null-vtable early-return sub_82606328 | splices + kernel | Pre-existing roots R1–R4 (see §4); unchanged by module-init work |
| GPU family (fake device, VdSwap fences, wait bypass, test pattern) + XMA/Xam stubs + DATA filename alias | kernel + gfx | Roots R5/R8/R6 — separate track, untouched by this burndown |

### 1e. Class C — unknown, needs its own A/B (not blindly in any tranche)

| Gate | Where | Question |
|---|---|---|
| `SR_DRIVE_FE_TICK` (drives master FE tick sub_821494C0, 1 Hz) | splash_hooks.cpp:2533, ON | Per-frame master dispatch sub_8271F848 has NO static caller (posted job). Does the natural boot post it? A/B **after** tranche 2, alone |
| `SR_AS_AUTO_FRAME` (lr-scoped fresh AS frame on empty-frame entry) | apt_as_watch.cpp:56, ON | May be an artifact of hand-invoked AS (SR_CALL_MAIN/HAND_TRIO); natural invocations frame properly? A/B with tranche 2 |
| `SR_FMUI_SHUTDOWN_GUARD` (refuse boot-time FMUI shutdown while aptCount==0) | apt_parse_trace.cpp:557, ON | The spurious shutdown may have been drive-induced; A/B after tranche 1 soak |
| DOGMA bad-roll guard family `SR_SWEEP_GUARD`/`SR_PICKITER_GUARD`/`SR_STATICFILL_GUARD` (midasm_hooks.cpp:35/103/66) + `SR_HEAPCHUNK_GUARD` (heap_chunk_guard.cpp:35) | ON | Contained symptoms of the contended-FMUI-ctor race; the primal source (fake-999) is dead and the ctor drive is retired — soak, then retire one per K-run |
| FM Manager singleton bootstrap (sub_82720360 between Init1/Init2) + PSM bootstrap (sub_822AE160) + direct splash advance (sub_822A8C68) + per-frame E630 replay (host fallback loop) | splice sub_821498F8 / fm_module_walker.cpp | Both bootstraps are idempotent and E928 NEEDS *(0x829A2BF0); PSM ctor is reached via FMGameRender's natural init (sub_82151560 → sub_822AE160) so its bootstrap should self-no-op — verify, then delete. Fallback-loop replay is dormant under SR_RUN_REAL_GAMELOOP |
| Watches/diag (`SR_AS_WATCH`, `SR_SET_WATCH`, `SR_RENDER_WATCH`, `SR_TRACE_*`, `SR_DIAG_STUCK_MODULES`, `SR_FIND_APTBLOB`, `SR_WATCH_WORLDPTR`, `SR_GUEST_TRACE*`, `SR_INPUT_*`, `SR_THREAD_SAMPLER`) | various | DIAG — keep; not band-aids |

### 1f. Cross-reference gaps (ledger hygiene)

- **BANDAIDS.md rows with NO gate** (can't be A/B'd by env): all `ppc_recomp`/splice-level guards
  (rows §19–§105 of BANDAIDS.md), the FM-mgr/PSM/splash-advance host hooks (rows 92–95), EB80
  fabrication (in row 107's family but the fabricator itself is ungated — tranche 1 adds the
  naturalModuleInitOn() check as its gate).
- **Gates with NO BANDAIDS.md row** (the whole session-drive family — this table is now their
  ledger): every row of §1b/§1c above except `SR_FORCE_UI_VISIBLE`/`SR_FIX_RENDER_CLIENT`/countdown
  park (row 107) and the guard family (iter-296d block). Do not add new BANDAIDS.md rows for
  them; retire them here instead.
- **BANDAIDS.md rows now RETIREABLE by tranche 1** (mark on soak): "FM module CTOR walker
  (kSkipList)" row, "FM Manager singleton bootstrap" + "FMGamePlayStateMgr instance bootstrap"
  rows (pending §1e verify), "sub_8271E928 skip in module init (4 sites)" row (**replaced** by
  the SR_NATURAL_MODULE_INIT splice), "Per-frame sub_8271E630 replay" row (dormant).

---

## 2. Why tranche order matters

Tranche 1 is the **active blocker**: iter-301's natural render walk (call #3, sub_82177F90)
wedges in E928 pre-walk with FMGameRenderInit already marked initialized — the signature of the
old drives dirtying state the natural walk owns. Tranches 2–4 sit BEHIND that wall: they can only
be judged once the render walk completes and the natural boot reaches the FE/apt/RTAM layers on
its own. Each tranche = one hypothesis = one K-run.

## 3. Retirement tranches (each verified by ONE K-run)

### Tranche 1 — module-init family (NOW; unblocks the render-walk wedge)

Exact edits (pattern: natural-mode skip inside the existing gate; `=0` A/B preserved):

| # | Item | File:line | Change | Status |
|---|---|---|---|---|
| 1.1 | hand FM ctor walker | kernel/fm_module_walker.cpp:290 | `if (sr::kernel::naturalModuleInitOn()) { log; return; }` at top of `walkFmModuleCtors_impl` | **DONE** (in tree) |
| 1.2 | FMUI ctor drive | kernel/splash_hooks.cpp:144 | `if (sr::kernel::naturalModuleInitOn()) return false;` at top of `fireFmuiCtorDrive` | **DONE** (in tree) |
| 1.3 | `SR_DRIVE_FMRENDER` | kernel/splash_hooks.cpp:4240 | natural check first line of the gate lambda | **DONE** (in tree) |
| 1.4 | `SR_DRIVE_FMENTITY` | kernel/splash_hooks.cpp:4267 | natural check first line of the gate lambda | **DONE** (in tree) |
| 1.5 | `SR_DRIVE_RTAMINI` | kernel/rtam_service.cpp:556 (`rtamIniMain`) | after the `gateOn` check add `if (sr::kernel::naturalModuleInitOn()) { SR_LOG_BOOT(...,"[RTAMINI] drive retired (natural module init)"); return; }` — the 5 s-delayed drive races the (up-to-64 s) natural FMGameRtamIni init | **DONE** (in tree since d1b0792; check at rtam_service.cpp:625) |
| 1.6 | `SR_DRIVE_MENULOAD` | kernel/splash_hooks.cpp:871 | add `if (sr::kernel::naturalModuleInitOn()) return false;` as first line of the `s_driveMenuLoad` lambda (the fm_module_walker.cpp walker-embedded copy was DELETED with the walker body in iter-357d; the THIRD consumer — the sub_8261C5B0 fake-success in cs_timeout_underflow_fix.cpp — was separately charter-gated behind forcingAllowed at iter-357c, commit 7101acb) | **DONE** (in tree since d1b0792; check at splash_hooks.cpp:1074) |
| 1.7 | EB80/EDF8/EDFC fabrication + fake-mgr heal | kernel/splash_hooks.cpp:547–549 and 618–631 | (a) guard the three publishes: `if (g1 == 0 && !sr::kernel::naturalModuleInitOn()) writeGuestBE32(kEdf8VA, ...)` (same for EDFC:548, EB80:549) and keep `g_fabMgrVA` 0 in natural mode (line 546: store 0 when natural); (b) in the EB80-zeroed heal (~:627) restore ONLY a previously-seen REAL client: `uint32_t pick = s_lastRealEb80 ? s_lastRealEb80 : (sr::kernel::naturalModuleInitOn() ? 0u : managerVA); if (pick) { ... }` | **TODO** |
| 1.8 | E928 walk observability | kernel/guest_context.cpp:891 (`logInit2Progress` once-only) + splice `sub_8271E928` markers | make the E928 progress markers per-call (append call# to the step string at the splice call sites) — iter-301's call-#3 wedge was invisible because markers are once-only | **TODO** (diagnostic, same commit) |

**Verify (K=4):** (1) no `chk=1` pre-mark on FMGameRenderInit 0x829601C0 at render-runner call #3;
(2) render walk passes `sub_82177F90_PRE_8271E928` (or the per-call markers name the true per-module
wedge — then fix that module, never re-skip); (3) `[RTAMINI] resource system already up` 4/4;
(4) no `[FM_CTOR] WALK_BEGIN`, no `[POST_SPLASH_FAB] fabricated dispatcher` lines; (5) aggregate
chain (defExec/parser/aptCtx/feFrames) returns on top of the fully-init engine, or the wedge site
is named. A/B fallback: `SR_NATURAL_MODULE_INIT=0` restores everything.

### Tranche 2 — FE/screen drive family (after tranche 1 soaks green)

Add the same natural-mode skip to: `SR_DRIVE_ACTIVATE_UI` (splash_hooks.cpp:745 lambda),
`SR_FORCE_UI_CFG` (:783), `SR_SET_IFACE_STATE` (:4320), `SR_FORCE_UI_VISIBLE` (:4363),
`SR_FIX_RENDER_CLIENT` + countdown park (:4393, park at :4432–4437), `SR_FE_CMD_LATE` (:3993),
`SR_FE_CMD_RETRY` (:4008), `SR_FIX_LEVEL0` (:3878), `SR_CALL_MAIN` (:3673), `SR_HAND_TRIO`
(:3202), `SR_FIX_BUILTIN_TABLES` (:3794, watch for scribble return → reclass B). Run the
`SR_AS_AUTO_FRAME` and `SR_FMUI_SHUTDOWN_GUARD` A/Bs alongside (class C).
**Verify:** ChangeInterfaceState fires from the game's own FlowMgr (WorldFlowXML now initialized);
screens queue type-1 items; aptCount > 0 without any hand registration; `SR_DRIVE_FE_TICK` A/B
(alone, one extra K-run) answers whether sub_8271F848 gets posted naturally.

### Tranche 3 — render-node / apt drive family

Natural-mode skips on: `SR_FORCE_APTCTX` (apt_parse_trace.cpp:422), `SR_DRIVE_APT_TICK`
(splash_hooks.cpp:1384), `SR_DRIVE_LOADMOVIE` (:1507/:1719/:2224), `SR_DRIVE_APTPARSE` (:2424),
`SR_INJECT_MAIN_BLOB` (:2693), `SR_LOAD_ICONMENU` (:3136), `SR_LOAD_PRESSSTART` (:5144),
`SR_SHARE_CLIP_REGISTRY` (:4724), `SR_DIRTY_MARK` (:4765), `SR_NODE_COMMIT` + layer-mask (:4816,
~:4881), `SR_FORCE_NODE_VISIBLE` (:5069), `SR_TICK_CONTINUE` + frame-ctr bump (:4556, ~:5067),
`SR_DRIVE_UI_RENDER` (:4448).
**Verify:** natural loadMovie completes WITH geometry/textures ([TXT_LIVE] geo≠0, texP≠0);
DEF_EXEC/clipCtor from the natural path only; node commits + visibility from display-list ops;
cmd43 climbs. This K-run is the trunk-hypothesis success criterion (retires the blob-inject class).

### Tranche 4 — RTAM synthetic service drives

Natural-mode skips (or plain `=0` A/B first) on: `SR_RTAM_PREP` (rtam_service.cpp:303),
`SR_RTAM_SCHED` (:154), `SR_RTAM_FSM` (:252), `SR_RTAM_NATLOAD` (:347). Delete the dormant
`SR_RTAM_WORKER`/`SR_RTAM_LOAD` bodies.
**Verify:** translator registry lookups (D2/D3) resolve without prep; package bind + OnAssetLoaded
deliveries with all service threads off; ResStorageManager/OnLoad worker (game's own threads,
DAT_8296D8C8/DAT_829A25FC) carry the load. Keep `SR_VECFILL_GUARD` (class B).

### Post-tranche cleanup

- Delete class A-dormant code blocks (§1c) file-by-file; delete retired class-A blocks after
  their second clean soak.
- Update BANDAIDS.md: strike the rows named in §1f; move class-B "ACCEPTABLE" rows to a
  KEEP section.
- Retire the `SR_NATURAL_MODULE_INIT` legacy else-branch in the E928 splice last of all (it is
  the master A/B for the whole era).

---

## 4. The 8 roots (iter-290 model) — status under the new frame

- **R0 (new) — natural module init**: LANDED iter-301 (`SR_NATURAL_MODULE_INIT`). Subsumes the
  old "FM-loop cluster" remediations wholesale; §3 tranches are its burn-down.
- **R1 — CS primitives**: DONE in practice (real OOR locking iter-190, condvar fixes, timer).
  Residual: `SR_CRITSEC_FAKE_SUCCESS` saturation fallback (shrink after storm source dies).
- **R2 — DOGMA pool/TLS**: DONE (TLS fix; 11 benign bootstrap fallback allocs remain).
- **R3 — Robin-Hood hash grow path**: OPEN (dogma_hash_hooks.cpp short-circuits stay, class B).
- **R4 — condvar lost-wakeup**: largely DONE (iter-291/297); NtWait 8 s escape + sub_82600E38
  cap remain as safety nets — candidates for a class-C A/B after tranche 1.
- **R5/R8 — GPU/PM4/D3D vtable + XMA**: OPEN, separate track (class B family unchanged).
- **R6 — import thunks (signin/timer)**: timer DONE; signin poll cap remains (class B).
- **R7 — real asset payloads**: THE current frontier = tranches 3–4 above; success criterion is
  the natural loadMovie with geometry (trunk hypothesis, memory/project_async_trunk_hypothesis.md).

## 5. Key risks

- A natural per-module hang inside the E630 walk is EXPECTED occasionally (FMGameRtam took 64 s);
  the response is per-module fixes (F2C8 logs name them), never re-skipping the walk and never
  re-enabling a drive.
- `SR_DRIVE_FE_TICK` retirement is the riskiest single item (sub_8271F848 posting unverified) —
  isolate its A/B; everything else in tranche 2 can land together.
- The guard family (SWEEP/PICKITER/STATICFILL/HEAPCHUNK/VECFILL) protects against the corrupt-graph
  classes; retire only AFTER the drives are gone and two soaks show zero guard firings (guards log
  loudly — grep the K-run logs for their tags before deleting).
- BANDAIDS.md splice-level guards (VM entry, >16M size checks, alias fallback) stay load-bearing
  until tranche 3's natural payloads are proven — delete piecemeal = crash (iter-290 warning holds).

---

# iter-436 STABILITY CAMPAIGN — genuine_edits hand-edit audit (2026-07-31)

USER MANDATE: "bandaid burndown, get everything we have working, stable."
`tools/regen/genuine_edits.txt` = 6,798 lines of hand-edits across ~280 recompiled
functions. The iter-433 lesson: ONE such guard (2048-node GC sweep cap) silently
corrupted the Lua heap for months. Pattern-ranked suspect tiers (script:
scan for iterGuard/cap/timeout/skip-write/early-exit):

**TIER 1 — corruption-class (guards that CHANGE guest semantics; review FIRST):**
| hunk | patterns | family | risk |
|---|---|---|---|
| sub_8246A1D8 (27L) | cap-256 + iterGuard | 8246xxxx ALLOCATOR | iter-433-class: a capped walk in the heap = silent corruption |
| sub_8275BDB8 (13L) | cap + iterGuard | 8275xxxx LUA | same class as the killed GC cap |
| sub_82763C50 (19L) | SKIP-WRITE | 82763xxx LUA GC MARK | a skipped write in GC marking = live-object sweep risk |
| sub_828D00F0 (23L) | iterGuard | unknown | unreviewed capped walk |
| sub_8271E6B8 (25L) | early-exit + timeout | 8271xxxx FM/task | task-flow bail = stranded work |
| sub_8271E5B0 (20L) | cap + timeout | 8271xxxx FM/task | " |
| sub_8211BBE0 (7L), sub_82755A38 (5L), sub_820F0048 (3L) | skip-write | D3D/Lua/D3D | suppressed guest stores |

**TIER 2 — timeout-class (wall-clock bails; each can strand a wait chain):**
sub_821498F8 (363L!), sub_8271E630 (109L), sub_82110A30 (97L — the AV-class
worker's own function!), sub_8271F2C8, sub_8229F460, sub_825FC108 (35L — the CS
enter itself!), sub_826FAAC0, sub_8271ED68, sub_82600E38, sub_824680C0,
sub_82720650 (StopLoopThread!), sub_8271EA20, sub_828D0968, sub_826E4D80,
sub_8271FA78.

**TIER 3 — the un-flagged 250+ hunks:** mostly switch-hole fixes and verified
faithful repairs (per hunk comments) — spot-audit opportunistically.

**Also in scope:** 29 OVERRIDES.json band-aid-class kernel overrides;
freeze-family attribution now ALWAYS-ON (CS wait-trace baked, iter-436).

Method per item (charter): understand what the guard compensates for → find the
platform gap → root-fix → delete the guard → K-verify. NEVER delete blind.
