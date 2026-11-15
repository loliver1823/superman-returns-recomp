# ENGINE_MAP — verified ground truth for Superman Returns (Xbox 360)

Single source of truth for **verified** engine contracts: object layouts, function
signatures, global addresses, and the techniques that established them. Every entry
carries a confidence tag:
- **[GOLD]** — verified against the golden Xenia dump (`data/xenia_canary.DMP`, membase `0x100000000`) or proven live on our boot (log evidence).
  Since iter-463 there are also sound-ISO golden dumps from the oracle build 0bd090dbe (`data/xenia_canary_sound_menu.DMP`, `data/xenia_canary_sound_gameplay.DMP`; `tools/golden.py --dump sound_menu|sound_gameplay`). Guest VAs >= `0xE0000000` sit at host `0x100000000 + VA + 0x1000` in every Xenia dump (tools/XENIA_HARNESS.md).
- **[DECOMP]** — read from Ghidra decompilation, behavior not yet observed live.
- **[INFER]** — inferred; treat as hypothesis.

Update this file whenever a contract is verified or corrected. When you verify a
contract, ALSO rename the function in Ghidra + set a plate comment (see §10).

Last updated: 2026-08-13 (iter-455k).

---

## 1. Boot trunk

**Guest main = `Function_821498F8`** [GOLD]:
`Function_825F7130(0)` (Init1) → `sub_8218CFE8` (Init2) → `Function_82176300` (**GAME LOOP**) → `sub_821863A0` (**SHUTDOWN**, not a tick!).

- `Function_82176300` [GOLD]: `while (*(u8*)(gameState+0x18) == 0) { FUN_825f7168(&0); }` — **+0x18 is the QUIT flag**. `FUN_825f7168(&0)` → `Function_828A7978()` → `sub_828EEBDC()` (worker-queue drain; opaque hand-asm in Ghidra). The loop NEVER dispatches FM module update slots — that's why every per-frame subsystem needed driving (see §4 FE tick).
- Historical trap [GOLD]: the harness once mistook the loop for an init-wait, force-set the quit flag, and ran `sub_821863A0` (teardown: `mgr->vt[5](mgr,1); mgr=NULL` over the 4 module mgrs `DAT_8296d8e4/dc/e0/d8`) as the "tick". Source of all "managers=NULL" symptoms pre-iter-294. Fixed: `SR_RUN_REAL_GAMELOOP` (default ON) + suppressed `signalInit3Ready`'s bogus write to `+0x18` (kernel/guest_context.cpp, kernel/init_wait_bypass.cpp).
- **Missing CRT `_initterm`** [GOLD]: XenonRecomp dropped the XEX→CRT edge. `Function_8246A080` walks the 920-slot static-ctor table `[0x82910010, 0x82910E70)`; FM registrators live at `0x828DA960..0x828DC608` within it. Our `walkStaticCtors_impl` (kernel/fm_module_walker.cpp) replays it and must clear BOTH Init1-dirtied FM list heads `DAT_829a2bb0` and `DAT_829a2b98` first (cycle otherwise).
- **CRT `doexit` = `sub_827A1698`** (twin entry `sub_827A1690`) [IDA + golden, iter-463]: `0x829AAE78` = `_C_Termination_Done`, `0x829AAE74` = `_C_Exit_Done`, `0x829AAE70` (byte) = `_exitflag`; onexit table `[*0x829B2FB0, *0x829B2FAC)` walked in reverse, then `_initterm` over `0x82910E84..E90` and `0x82910E94..E98`. Only doexit reads/writes the two flags (IDA xrefs); golden dump reads 0. NOT "DOGMA guard globals" (old name): the host write of 1 to both was deleted in iter-463. The spliced `goto loc_827A1770` in both bodies still skips the flag check and the atexit/terminator walks (open band-aid: exit() never runs atexit handlers).
- Timer [GOLD]: `sub_82564E80` = engine tick source; our override returns real `steady_clock` ms (kernel/apt_parse_trace.cpp). Without it every timed wait spins forever.
  **iter-463: reclassified BAND-AID** - the guest's own `sub_825667C8` = QPC/(QPF/1000) ran 74x fast because mftb was the raw TSC. The gate is baked DEFAULT ON (iter-463 rolls), so the override delegates to the guest by default and its steady-clock body only serves the `=0` legacy A/B; delete it with the `=0` path (see §6b).

## 2. FM module system

- Descriptor table: ~61 records `[0x820028D0..0x820036D8]`, shared fn triplet `{0x8271F428, 0x8231C140, 0x82196960}` [GOLD].
- **FE SYSTEM MODEL [GOLD iter-316]**: static SYSTEM objects = data cells whose +0 is a
  descriptor-vtable: `0x82911370` = **FSGameRenderMain** (vt `0x82001838`), `0x82911388` =
  **FSGameSimulation** (vt `0x82001888`), `0x82911390` (vt `0x820018D8`). System table pairs
  {systemObj, runner} at `0x82911320`. Runner `0x8295C690` runs FSGameRenderMain (started by
  `StartService_FGGameRender_sub_82177F90`), runner `0x8295C65C` runs FSGameSimulation
  (`StartService_FGGameSim_sub_821780C0`). `E928(runner, 0)` binds runner+8 = first system of
  runner+0xC array, calls `sys->vt[+4]()` = **BuildModuleRoots** (RenderMain: `sub_821490B8` →
  14-list at `0x8295C768`, once-flag `0x8295C7A4`, **FMUI at idx 10**; Simulation:
  `sub_82149300` → 29-list at `0x8295C7B8`, flag `0x8295C830`; Sound `sub_821497B8` → 5-list at
  `0x8295C848`, flag `0x8295C860`), then E630 walks the roots.
  E630's loop has **no abort path**; `FmModuleInitOnce_sub_8271F2C8` guard byte = module+0x44.
- **[GOLD iter-320] "Walker-invisible modules" DEBUNKED**: FMfmvManager `0x8295F1C4` = sim root #8,
  FMGameEntity `0x8295F7A8` (vt `0x82002D4C`, vt[+0x1C] = init `sub_8214FF48`) = sim root #10 —
  ordinary walked roots; cells `0x829113xx/0x829116xx` are STATICALLY initialized in the XEX.
  They starved because the **E630 splice's 2s timeout** (missed by iter-301's F2C8 gating) skipped
  every `sub_8271F2C8` init after trip while iteration continued (FMGameRtam root #3 takes 30-64s →
  every sim walk tripped). Retired under natural mode in iter-320. Related: full-list walk
  `sub_8271F548` is DEAD CODE (never called); `Function_8271F648` + counter-visitor `sub_8271F690`
  = "count modules with +0x44==0" readiness poll; module registrar `Function_8271F130` pushes onto
  list head `DAT_829A2BB0` (node: +0x38 visit flag, +0x3C next, +0x44 init-once guard).
- FMUI module object = **`0x8295FC8C`** [GOLD]; its static ctor `sub_828DBC88` installs vtable `0x820033E4`. Walk-init slot vt[+0x1C] = FmuiGlobalInit.
- **FMUI GlobalInit = `sub_821516E0`** (tail `0x82151DA0` is the same lifted fn; the eb80 write at `0x82151E34` is in the tail past Ghidra's bad-data halt and is UNCONDITIONAL) [GOLD]. **Runs NATURALLY from the render-system walk on good rolls (proven iter-316: `[FMUI_GI]` ENTER lr=8271F330)**. Builds: `DAT_8296EC4C` (= aptMgr `DAT_829A23E0` via get-or-create `sub_826D4930`), real flow client g3 → **`DAT_8296EB80`** (= `ec4c->vt[+0x10](0)`), then a PSM/FE registration block gated on `[[0x8296D874]+0x90]==1` (gameconfig UserInterface flag; naturally 1) incl. PSM lazy-ctor `Function_822AE160` and 2× `eb80->vt[+0x48]` registrations.
- FSGameSimulation module ctor `sub_821492E8` [GOLD]: object = `{+0 = descriptor vtable 0x82001888, +4 = first-frame byte = 1}`. Update slot (idx 5 of that vtable) = the FE tick (§4).
- Module mgrs (built by Init2): `DAT_8296d8e4/dc/e0/d8` [GOLD]. `DAT_8296d8e0+0x84` = frame-dt float read by the FE tick prologue.
- **Walk-wedge lottery [iter-317]**: bad rolls wedge INSIDE one module init, same module across a
  batch, rotating between batches: FMGameScript `0x8295F520`/init `0x82150878` (free()-caller loop
  at `Function_828CF538` = dlmalloc-style free, circular block list), FMDecals `0x829600E8`/init
  `0x8214AA10` (509KB DOGMA alloc `0x7C470` → timed-wait livelock `Function_828A7860`/`0x828D0300`,
  pool exhaustion — nothing frees during boot), FMSoundSystem `0x8295F2F4`/init `0x82150A70`
  (whole body gated on gameconfig Sound flag `[[0x8296D874]+0x88]==1` — host-zero it =
  `SR_CFG_SOUND_OFF`, init returns 1 instantly), matlib `0x82960130`/init `0x8214D5F0` +
  FMGameRender `0x8295F760`/init `0x82151560` (slow but complete on good rolls).

## 3. Splash / PSM / FE state machine

- PSM instance global `0x8296EDAC` [GOLD] (heap obj, e.g. `0x8A24FF28`). `+0x230` = splash counter, `+0x234` = splash-done flag.
- `sub_822A8C68(psm)` [GOLD]: climbs the counter; at >5 sets `+0x234` AND fires `sub_82470700` → `sub_824704B0(2,4,0)`. **CORRECTED iter-449q (5-agent workflow, IDA-verified):** `sub_824704B0` is NOT a direct FE state-2 transition — it is `XMsgStartIORequestEx(0xFA, 0x7001A, {2,4,0})`, an async XMP/music-class XAM message (old label came from the iter-330 receiver mapping to `SetInterfaceState_sub_821745D0(mod,4,0)`). The doneFlag branch (0x8235C8B0) ALSO calls `sub_8231D7F0`, which (when `[g_FeFlowConfig+136]==1`) builds an event-14 object and pumps listener queue `0x829A936C` via `sub_82149A60`→`sub_82773A38` — the live candidate trigger for the single AreFMVsDone evaluation (+45 ms after the 6th movie's SM epilogue; untested).
- **TITLE-SCREEN DISPATCH [GOLD, iter-449q — solved end-to-end]:** the dispatcher is the 44-state **WorldFlow FSM instance `0x83F287C0`** (BaseWorldFlow graph, ticked by `Function_82293108` from flow-mgr lr=0x82225F44). Its state scripts are **Lua** (dostrings at lr=0x82191C7C, `[FSMA]` lines); natives registered by `sub_822B62E0` (lua_pushcclosure table — NOT the AS-native table): `ActivateUI`=sub_822A2590, `PlayFMVs`=sub_822A2640, `AreFMVsDone`=sub_822A26E8, `ShowLoadingScreen`=sub_822A2738 (worker `sub_8214F1B0`, "SHIELD"→0/"BIZARRO"→1/"ENGAGING"→2), `HideLoadingScreen`=sub_822A2820, `Set/GetToggleAttractMode`=sub_822A2870/28B8, `QuitGameUI`=sub_822A25E0. Transition predicates = Lua pcalls via cond-native `sub_8224CDC0` → `FmScriptPcallRegistryRef_sub_821648B8([0x8296D8CC], ref, 0, 1)`. **AreFMVsDone** (`sub_822A26E8`) returns byte PSM+0x234 when `[g_FeFlowConfig+144]!=0`, else constant TRUE; evaluated exactly ONCE per roll (edge-triggered, not polled). The ENTIRE dispatch — ShowLoadingScreen("SHIELD"), ActivateUI (→ `sub_8214FB78` → `sub_826BB6F0` FE root load, log `[APT_LOADMOVIE] "main"/_level0 ret=1`), IconMenu BigFileAsset load — runs inside the SINGLE 0→11 FSM transition; the walk is 0→11→10→1→13→**17** and parks at state 17 (menu idle / press-start wait) with 4 Lua exit predicates `[p=8224CDC0→2][→31][→43][→20]` evaluating false thereafter. **This chain fires IDENTICALLY on every roll examined (6/6: stuck and progressed)** — the attract-era starfield stall is DOWNSTREAM of guest dispatch (renderer widget-fill/draw delivery), not a flow stall. Note: the AS builtin `loadMovie` (`sub_826CB500`, tag `[LOADMOVIE]`) has NEVER fired naturally in any archived roll — the FE root load uses the `sub_826BB6F0` path.
- **BLACKOUT = LOADING SCREEN [GOLD iter-455l loadscr4 runtime, scoped to that acquisition]:** the terminal exact-black era is the ACTIVE mode-0 SHIELD loading screen: SHOW 9.643 seconds after the gameplay-load `main_hud` request, visibility/alpha -> 255, sequencer HOLDS state 5 (the mode-0 steady hold awaiting external dismissal; tip rotation in 5 is mode-2 behavior), and no HIDE/SET/state change ever follows - exact black (fnv `3F1965ED15C74383`) from the first sampled present after SHOW to process end. Evidence separation: RUNTIME = the observed FRONTEND hide entered through `luaD_precall`; STATIC = the `aaaP` FSM state contains a `HideLoadingScreen()` call; UNOBSERVED = execution of `aaaP` or its hide call in the gameplay transition (the open packet's target chain). State 8 = idle-after-release (live-confirmed). NOT established: why load completion never fires - the aaaP-entry predicate packet is the open trunk. Full evidence: CLAIMS_LEDGER "iter-455l loadscr-roll-2".
- Splash dispatcher `sub_822AB748(psm, mode)`; per-name blit hook `sub_822B7F38`.
- FE state machine: `sub_8235C8D0` → `sub_8235C780` (reads PSM+0x234) [DECOMP].
- ActivateUI = `sub_822A2590` → `sub_8214FB78` [GOLD]: gate #1 `[[0x8296D874]+144] == 1`; then `client->vt[+20](client, cfg+152)` = frontend movie load. `cfg+152` = inline root-movie name — **must be "MAIN" uppercase** (TOC hash is case-sensitive). uiMgr global `0x82911460`, gate flag at `uiMgr+0x54`.
- **ActivateUI is an ACTIONSCRIPT NATIVE [GOLD iter-316]**: zero code xrefs; registered in the AS-native table at `0x820C8328` (`{fn, 0x4000XXYY}` pairs, `0x820C82F8` region). It is `sub_8214FB78`'s ONLY caller. On hardware the splash/legal movie's script calls it — a chicken-and-egg on our boot (no movie loaded yet). Bridge: `[AUI_NAT]` wall-clock one-shot driver in kernel/fe_flow_watch.cpp (`SR_AUI_NATDRIVE`, default ON) — waits uiMgr + real eb80 (exclude `g_fabMgrVA`) stable 3×500ms, applies cfg-gate + "MAIN" recipe, calls via dispatch table with GuestCaller. **Proven iter-317: ActivateUI returned rc=0 and triggered the golden loader — 80 ui.AST packages (BigFileAsset::Load) incl ScrPressStart, real payload reads.**
- Old POST_SPLASH_FAB AUI driver self-deadlock [iter-316 post-mortem]: `managerVA` aliases the REAL client when eb80 was already published (`eb80 != managerVA` false forever) + its 1s tick loop blocks inside the `sub_822AB748` drive body. Do not clock drives off that loop.

## 3a. RTAM RESOURCE MODEL & FE FLOW FSM [GOLD, iter-350e→l]

**RTAM record** (asset handle target): `{+0 namePtr, +8 typeHash, +0xC payloadPtr, +0x10 refcnt, +0x14 rawBuf, +0x18 rawSize, +0x1C stateWord(halfword=state bits), +0x20 next}`. Retail menu-idle loaded records = `+0x1C` word `0x00C00001`; a name found in NO catalog resolves to `0x0008xxxx`. **Record state check `RecordStateCheck_sub_826E4D80(handle)`**: reads `*(rec+0x1C)&0x18` → `0x10`=2 (FAILED), `0x08`=1 (no-source), else 0 (loadable). Consulted by every acquire; `rc!=0` = abandon the request. [WAS the year-long menu wall: a hand-edit forced rc=1 after a 10 s init-walk timeout — retired iter-350j to `SR_LEGACY_INIT_TIMEOUT`, default OFF. See BANDAIDS.md.]
- Manager globals: `resMgr = *0x829A2434` (vt `0x8208AFF8`: `[7]` acquire `ResMgrAcquire_sub_826FAAC0`, `[8]` request `sub_826FAA70`, `[10]` translate `sub_826FB078`). Scheduler `= *0x829A2424` (vt `0x8208A5C8`: `[3]` add, `[7]` pump7 `sub_826F44B8` [never called naturally], `[8]` pump8). **No per-frame pump exists** — pumping is cooperative (blocking waiters) + an OnLoad job-worker thread (`"OnLoad"`, loop `sub_826F92E0`, 16 ms condvar poll, singleton `*0x829A25FC`). `rtam_service.cpp`'s synthetic pump/worker/FSM threads are FORCING → charter-gated OFF.
- Catalog class vt `0x82089C70` (`[3]` BigFileAsset::Load `sub_826E83D0`; `[25]`@+0x64 open `sub_826E70F8`). Sync I/O WORKS: gameconfig 1372 B, GameCredits 63 KB, uitextlist 66 KB all `NtReadFile` rc=1 (loose-file + `.AST` catalogs). Compiled-XML blobs are **binary string-pool DOMs** (retail format; golden-verified) — the TEXT translator returns rc=0 but consumers parse via the cb path.
- ⚠ **The catalog class is NOT the APT package class** [DECOMP, iter-455h]. `0x82089C70`
  is the `.AST` catalog/BigFileAsset class: its `[3]`/+12 is `BigFileAsset::Load sub_826E83D0`
  and its `[5]`/+20 is `sub_826E6998` = *a name getter* (`sub_8279F740(buf, this+8, size);
  return 1`). The APT package is a DIFFERENT class, **`AptAux::RtamPackage`, vtable
  `0x82088B80`** (88 bytes, allocated by `RtamPkgMgrCreatePackage_sub_826D91F0`, tag
  "AptAux::RtamPackage" @`0x82088A20`): `[4]`/+16 = `RtamPackageGetAssetSync_sub_826D8DA0`,
  **`[5]`/+20 = `RtamPackageRequestAssetAsync_sub_826D97A0`** (this, assetId, callbackObj).
  Do not conflate the two — they differ at the same slot.
- **APT asset request → resource cache, proven route** [DECOMP, iter-455h]:
  `AptAuxLoadAptFileByPath_sub_826D2690` (the `[APT_REQ]` gauge site) →
  `AptAuxOpenPkgWithCallback_sub_826D2188` → `PkgLoadedCbImplCtor_sub_826D56B8` (vt
  `0x8208868C`; stores the request path BASENAME at +12, 31 chars max — the clean identity
  binding) → pkgMgr `vt[12]` → `PkgLoadedCb_OnPackageBound_sub_826D5FE8` →
  `AssetLoadedCbImplCtor_sub_826D5898` (vt `0x820886C4`, pending=2) → `package->vt[20]` ×2 →
  `resMgr->vt[36]` = `ResMgrEnqueueAccessProc_sub_826FAA20` → `resMgr->vt[28]` =
  **`ResMgrAcquire_sub_826FAAC0`**. (`resMgr` = `sub_826E5838()` = `*0x829A2434`; its vt
  `0x8208AFF8` slots 7/8/10 verified against this section.)
- **`RtamAsset::vt[16]` returns the SHARED record payload** [DECOMP, iter-455h]: **on the
  allocation-success path** `RtamAssetWrapCreateDeliver_sub_826D8E60` builds a fresh 60-byte
  "RtamAsset" (vt `0x82088B44`) per delivery — ⚠ if the 60-byte allocation FAILS it still
  invokes the user callback, with a **NULL wrapper** and status 0, so "fresh wrapper per
  delivery" is conditional on allocation succeeding. Its accessor `sub_826DA0F8` is literally
  `lwz r11,8(r3); lwz r3,12(r11); blr` = **`return *(*(this+8) + 0xC)`** — the RTAM record's
  `+0xC payloadPtr`, i.e. the SAME field `ResMgrAcquire` @`0x826fab0c` tests for zero to
  decide a cache hit. Fresh wrapper, shared mutable payload. (Hex-Rays refuses this
  function; decoded from bytes and cross-checked against
  `guest/ppc/ppc_recomp.91.cpp:20581`.)
- **RTAM type→handler registry** [GOLD, iter-455h]: `sub_826E5868()` = `*0x829A2420` (golden
  `0x05B34050`, vt `0x8208A6BC`); `vt[28]` = `sub_826F5480` → `sub_826F5CC0(out,
  registry+8, key)` → `sub_826F58A0`: open-hash on `map{+4 bucketBase, +8 bucketEnd}`,
  `count=(end-base)>>2`, `idx = key % count`, chain nodes `{+0 next, +4 key, +8 handler}`.
  Key = `sub_826E4D00(handle)` = `*(*(handle)+8)` = record `+8` typeHash. Golden: 193
  buckets `[0x05B34638,0x05B3493C)`; **APT types `0x264923D5` (.apt) → handler `0x050702A8`
  and `0x264923D6` (.const) → handler `0x050702C0`, both vt `0x82088C90`.**
- **A CACHE HIT CAN RE-DRIVE DELIVERY** [DECOMP, iter-455h; cross-checked against generated
  PPC]. ⚠ **This is a REACHABLE MECHANISM (a capability).** The occurrence side is now
  OBSERVED — see the `iter-455k runtime` block below — but no causal link to the
  terminal-black roll is claimed. It also
  holds only on the wrapper **allocation-success** path (see above). `H7n` remains OPEN: a
  second registrar entry being reachable does NOT mean `record[0]` is double-rebased, because
  the earlier top-level bases are themselves rebased and dereferenced before the record loop,
  so a second pass may fault or divert first.
  Mechanism: in `ResMgrAcquire`'s `payloadPtr != 0` branch the loader is skipped, and the handler's
  `vt[32]` is `sub_8231C140` = a bare `blr` **no-op** for both APT types — so `+32` is NOT
  the delivery route. Delivery happens through the QI'd POST hook instead: the branch calls
  `chain->vt[0](chain, 553113585, &out)`, and that IID is answered by
  `AptRttiCastQuery_sub_826DAB88` returning the **RtamPackageLoadCallback** itself (the
  AccessProcess `sub_826FBFB8` and Selector `sub_826FC288` QIs delegate with r4/r5
  preserved — only `r3` is replaced, verified in disassembly). The branch then calls
  `out->vt[24]` = `RtamPkgLoadCbDispatch_sub_826DAB70` = `this->vt[16](...)` =
  **`RtamPkgLoadCbOnComplete_sub_826DAC20`** → `RtamAssetWrapCreateDeliver_sub_826D8E60`
  → `userCb->vt[12](userCb, wrapper, 0)` = `OnAssetLoaded`, status 0. The DELIVERY CALL is
  unconditional (no success test, no already-delivered guard), but `wrapper` is **NULL if the
  60-byte allocation failed** — status is still 0 in that case.
  ⇒ IF a repeat request for an already-loaded APT asset occurs AND the wrapper allocation
  succeeds, it delivers a fresh wrapper over the SAME already-relocated payload. `vt[20]`
  (PRE) and `vt[28]` are also `sub_8231C140` no-ops.
- **RESOLVER-ENTRY RUNTIME FINDINGS** [GOLD — iter-455k runtime; named-run METRIC findings,
  `archive/iter455k_h7target/attempt1/run.log`, probe `SR_H7_ENTRYWATCH` @ `12ca880`,
  director-verified]. Acquired via `sr_drive.ps1` + `frontend_navigate.json`; the
  `screens/hud/main_hud` request (`[APT_REQ] #17`, `lr=826AFB40` inside
  `AptResolvePendingClips_sub_826AFA58`) landed at 05:41:20.718.
  - **H7u narrow occurrence (CLOSED):** four tracked entries reached **destructor entry
    while a complete walk found them linked**; all four were subsequently confirmed absent
    through the destructor's removal. Identities: `screens/misc/ScrGenPop` (state 6/prior
    4), `screens/bootflow/ScrPressStart` (state 6/prior 4), two
    `../common/ButtonHints/XBox360/H` (state 4/prior 3). The destructor invokes
    `AptListRemoveByValue_sub_8269B540` before its caller performs the 0x1C pool free
    (`guest/ppc/ppc_recomp.87.cpp:27920`), so the **stronger dangling-free reading is
    killed for these four events** (globally unexamined elsewhere). Say "destructor
    entered while linked" — entry, return, unlink and free are distinct events.
  - **H7c occurrence (CLOSED):** two record-array addresses (`88BD592C`, `88BD55BC`) were
    each **delivered twice with stable blob/header/storage identity and a different
    delivery node** — repeated registrar delivery over shared storage, witnessed by 32
    `[H7_PARSEFIRST]` first-sightings and 0 refusals.
  - **State-6 teardown follows another route:** `AptInstantiateChildClip_sub_826B0960`
    (`[H7_EXEC6]`) never ran in either observed sequence, so it is retired as the
    explanation for THIS transition (not globally). Task uniqueness, `task+4` vs
    `*(clip+32)+40` identity, and the erase-guard remain unexercised at runtime.
  - **Timing (correction over the first report):** last nonuniform frame 05:41:18.637;
    linked destructor entries 05:41:20.770–.813; parser repeats .986; first authoritative
    uniform-black sample 05:41:22.219 — at most ~1.5 s after `main_hud`, and the sampling
    interval does not establish the exact onset. **No blackout causality is claimed**; the
    directed killer is the H7c repeat-suppression control/treatment experiment.
- **H7c REPEAT-DELIVERY TABLE OPERAND** [GOLD — iter-455l runtime; D1
  `archive/iter455l_h7c_d1/attempt1/run.log`, observer gate @ `ddc55cc`,
  director-adjudicated 27dc22b]. For BOTH ButtonHints repeat identities
  (`recordArray 88BD592C/88BD55BC`), `recordArray[4]` was **raw `0x10` at first registrar
  entry, `blob+0x10` (absolute) after first registration, and raw `0x10` AGAIN at repeat
  entry** (`currentMatchesPre=yes`, both). ⇒ `recordArray[4]` was NOT replayed in its
  post-first absolute state; for THIS table operand the second registration's
  raw→absolute rebase is NECESSARY work (other relocation operands unsampled).
  **The table-pointer double-add chain (`blob+0x10` → `2·blob+0x10`) is REFUTED for
  these D1 deliveries, and the zero-base treatment is RETIRED** (gate deleted per its
  contract; named-isolation infrastructure retained with its own `SR_ISOLATION_TEST`).
  ⚠ SCOPE (director): this does NOT establish "all repeated relocation is safe" — other
  non-idempotent registrar operands were unsampled; complete refill and the restoring
  writer's identity remain unproved; three black reproductions after repeat delivery
  (C1/S1/D1) establish replicated TEMPORAL ORDERING, not causality. Next directed step:
  the read-only blackout-boundary audit (guest-render-goes-black vs
  presentation-selects-black fork).
- **BLACKOUT BOUNDARY** [GOLD — iter-455l read-only audit over C1/S1/D1,
  `archive/iter455l_boundary_audit/`, director-ruled f7256ae]. Canonical finding
  (director's wording): *"Across C1, S1, and D1, the final D3D12 backbuffer collapses
  from nonuniform content to byte-identical uniform RGB black while the same host
  present branch, presentation cadence, frame-list execution, HLE command processing,
  guest draw submission, surface-pool drawing, and resolve activity continue. The
  failure is therefore in render/composition content upstream of `Present`, not a
  presentation stall. This does not identify whether the black output is guest-intended
  transition state or incorrect host HLE source/composition."* `[PRESENT_SIG]` reads the
  swap-chain RT after command-list execution, immediately before Present
  (gfx/d3d12_backend.cpp:8077) — authoritative for the final backbuffer; terminal pixel
  exactly BGRA (0,0,0,255); terminal fnv `3F1965ED15C74383` identical in all three runs.
  ⚠ EVIDENCE RULES (director): `[DRAW_FLOW] batches=0` is the legacy PM4 path — NOT
  proof of continuing rendering; use HLE_VIDEO/HLE_MIRROR, API draw totals, SURF_POOL,
  resolves, frame-list executes, presents. `[PRESENT_PATH]` proves the host branch/input
  shape unchanged — NOT the internal HLE surface/resolve source identity. ⚠ REJECTED
  inference: "the next screen never arrives" — in all three runs `ShowMiniMap` becomes
  non-sentinel immediately before the repeat and `OVERLAY_10.ShowMiniMap` resolves
  shortly after: the HUD-side object MATERIALIZES; the open question is why the
  composition stays black. C1's fade was underway 89 ms before the first repeat — do NOT
  pursue H7c as the fade initiator without a new positive static prediction. Sub-fork to
  resolve (static packet): guest transition deliberately emits/holds black VS host HLE
  selects/composes a black source despite continuing guest activity.

**FE flow FSM** [iter-350k]: config asset = **`WorldFlow`** (gameconfig key `WorldFlowXML` via `ConfigGetString_Function_82187548(store *0x8296D87C)`; content in `baseworl.AST` = catalog 3). Module init `WorldFlowXML_Function_8214D508` requests it via resMgr vt[8]. Parse: `FsmParseEntry_Function_82293330` (push actor→`Function_8278C1C0`→pop on stack `0x8296E114`/`8296E118`) + element cb `FsmParseElementCb_sub_82293398` (elements: `FSM/states/state/connectivity/transition/action/function/script/start_state`). The graph is 44 states (2 graphs; DOM `"BaseWorldFlow"`, golden plaintext at `0x0642db44`). Action factory `FeFlowActionFactory_Function_82291AD8` builds command objects by type 0x00–0x42 (type 0xF = show-screen); state class vt `0x8201731C` (special case 0x34 = vt `0x82017778`). **KEY CORRECTION: show-screen is NOT a parsed-XML action** — the parse binds only STATE objects (`sub_8278C3D8`, vt in `[0x82017300,0x82017C00)`); the live show-screen command objects (vt `0x82017af4`) at menu belong to **`SUPERMANFSM_G_*` gameplay-HUD graphs**, not the FE menu. **FSM tick**: flow-mgr update (lr `0x82225F44`) calls `Function_82293108(ctx, fsm)` at 30 Hz; gates on `*(fsm+8)!=-1` (cur idx); `fsm{+8 cur, +0x18 cnt, +0x1C stateArr}`; `DAT_8296E0E8` = TRANSIENT current-state (set during `Function_8278C500`, cleared after — polls between ticks read 0). State lifecycle trio wraps `sub_82259630(state, mode)`: `Function_82291798`=SHOW(0), `sub_822918B0`=ENTER(1), `sub_822919D0`=EXIT(2). Our FSM walks 0→40 naturally then parks (plausibly correct — the big graph is gameplay orchestration, e.g. `ObjectiveTeleportEntity`, `MainActor SUPERMAN`).

**Flow-FSM instance, stat cap and trigger input event** [VERIFIED iter-463 phase 1b: IDA + all three Xenia goldens through `tools/objstate.py`, TOOLKIT §1.11]: every loaded flow's FSM is `*(flow+0x6C)`, ticked through vt[1] = `FlowFsm_Tick_sub_82293108` by `WorldFlow_Update_sub_82225E60` (class vt `0x8200E5E4`). fsm `+4` start index, **`+8` current index** (-1 = stopped), `+0xC` previous index (both stored by `FsmSetState_sub_8278C1C0`), `+0x14`/`+0x15` start-pending / skip-transitions bytes (`sub_8278C500`), `+0x18` count, `+0x1C` state*[]; state `+8` transition count, `+0xC` transition*[]; a transition's `+4` is the destination INDEX (`FsmWalkTransitions_sub_8278C448`). The index is the document order of the graph's `<state>` elements (`FsmParseElementCb_sub_82293398` pushes each id and uses count-1); the parse-time id vector is freed, so map through `tools/worldflow_fsm.py`: on all three goldens all 44 live BaseWorldFlow transition lists equal the decoded graph. Values: gameplay golden index 3 = state **107** (InObjective), previous 2023575322; press-start goldens index 17 = 1793378647. **Stats:** CurrentXP (class vt `0x8200E900`) is the only capped class: cap = f64 `+0x28`, set by SetStatCap (vt+20 = `0x821F1288`, which also lowers a larger value to the cap); Inc/Dec/Add/Sub (vt+4..+16) clamp at it, gated by flags `+0x22` bit 0; `SetStatValueByName` = vt+32 = `0x8223FD58`, a plain `stfd f1,8(r3)` for every class (no clamp, no notify). BaseWorldFlow state 105's Lua sets the cap to heroPointChapter<cur+1>Requirements (7500 in the gameplay golden, 0 before the first 105). **Trigger state `+0x18`** = `sub_8229A568(triggerEvent)` (FNV-1 over the `{id, name*, FNV-1}` `INPUT_EVENT_*` table at `0x82912B68`, id 0 ends it), 0 when the string is empty.

**Game run state + the in-game save prompt** [VERIFIED iter-463: IDA + the three Xenia goldens via `tools/devtools/skipexec.py`]: `SetGameRunState_sub_8214F348(uiMgr, n)` stores `n` at **`uiMgr+0x48`** (uiMgr = `*0x82911460`, the static object `0x8295FC8C` in all goldens) and switches input / audio / world-update groups per state. Goldens: gameplay **3**, `sound_menu` 1, `nosound` 2. The Lua native `PromptUserSaveGame` = `ScriptCmd_PromptUserSaveGame_sub_822B5D78` -> `PromptUserSaveGame_ShowPopupIfSignedIn_sub_822B5070(*0x8296EE8C)`: only when `XamUserGetSigninState(*(*0x8296E8C8+0xC))` is nonzero and `XamUserGetName` succeeds does it set run state **4** and call `FlowMgr_CppShowPopup_sub_822B15F8` (title `PromptInGameSave_Title`, body `PromptInGameSave_Body`, handle -> `+0x44`); with no signed-in profile it does nothing. BaseWorldFlow's Lua calls it only when `MSTobj03` is won, in the objective-unload state before 105. The level-skip executor's `pad_prompt` is run state 4 (DEVTOOLS_DESIGN 5.5). **Answer + blocking [VERIFIED iter-463 item 7: decoded from the image by `tools/devtools/save_prompt.py`, identical on all three goldens; Apt side rank 4].**
  - The popup descriptor: buttons `[PromptInGameSave_Save_Btn, PromptInGameSave_Continue_Btn]` (`0x82913810`), default 0, **canCancel 0**, closeOnPress 1.
  - `sub_822B32B8` (the popup-complete handler; r30 = owner, r31 = id, r28 = button) at `0x822B3488`: button 0 -> `sub_822A4F78` = AS `ShowSaveSlots` (Main_Hud `mSlots.Start(0)`). Any other button -> `SetGameRunState(uiMgr, 3)`.
  - Decline = **`DOWN A`**. B does nothing, because ScrGenPop cancels only with `canCancel`. A B in the HUD save slots -> `HudAdapter.CanceledSaveGame` = `sub_822B3F40` -> the `CancelInGameSave` popup (id `+0x4C`: button 0 = `SetGameRunState(3)`, else `ShowSaveSlots`).
  - **Run state -> slot-switch mode** (`*0x8296DF4C`: `+4` slot table, `+8` mode, `+0xC` previous, `+0x10` dirty; `sub_821E8588` sets it, `SlotSwitchTickDispatch_sub_821F93F8` runs `slot[mode]` vt+8 from `FeMasterTick_821494C0`): run states 1/2/3 -> mode 1; run state 4 -> mode 2; Lua `SetGameActiveState(false)` -> mode 3.
  - The slot ticks: mode 1 = `0x8223F228` (thunk to `GameTick_ActiveWorldUpdate_sub_8223EB08`); mode 2 = `IgcActivateContextAndBroadcast_sub_821E8460` (a bit-5 broadcast at dt 0.0 + VFX; no direct world-flow update); mode 3 = `GameTick_InactiveFlowOnlyUpdate_sub_8223F230`.
  - So the save popup **freezes the BaseWorldFlow FSM** until it is answered. The prompt is raised from state `1233294090`'s enter action, only for a won MSTobj03.

**FE boot screen** [iter-350l, golden]: the menu ladder's one missing object. Golden boot client = `mod+0x220` (mod `=*0x8296D86C`; golden `0x0069ADC0`), vt `0x820059C0`, holds `scrId(+0x580)=1`, screen ptr `(+0x584)=0x00719ED0` (class vt `0x82009798`, id `+0x58`=1). Boot client tick vt[4]=`sub_82188860` reads `+0x580`, calls `ScreenRegistryLookup_Function_821C23E0(scrId)` (registry `*0x8296DD0C`; rejects id 0) → bails every frame while `scrId=0`. Boot-screen ctor `Function_821D0D68` (class vt `0x82009798`) has NO static callers → built via the global class-factory table `0x820BE080..0x820CEC28` (8565× 8-byte `{ctor, packed}`; boot-screen = entry 2593 @`0x820C3188`, packed `0x40001904`). Registry remove `Function_821C7758`. **OPEN (workflow in flight): who creates+registers the boot screen + sets `client+0x580=1` at boot on retail, and the platform gap suppressing it in our boot.**
  - **CORRECTED iter-464 (DT-0 `entityhud.py`, DT-2 free camera) [GOLD]:** `*0x8296DD0C` is the **entity registry** (12-byte `{ID, entity*, flags}` elements, `EntityRegistryInsert` 0x821CCB30, walked by `DispatcherForEachInvokeCallback` 0x821C2600), not a screen registry, and `0x00719ED0` (vt `0x82009798`, id 1) is **entity 1 = the Superman actor** (tag ACTR, FSM `SUPERMANFSM_G_21A4B50D`; `archive/v1_dt2/entityhud/xenia_nosound_dump.json`). `*0x8296D86C` is the **camera / view manager** (`CameraManagerCtor_sub_8218C4C0`): `SetInterfaceState_sub_821745D0` selects its active view (the Apt UI view, state 4, is one of them), `+0x220` is a camera on entity `+0x580` (1 = Superman), state 7 = `+0x1150` is the retail debug camera (vt `0x820059A4`; docs/DEVTOOLS.md 6.4). The "screen" names in this section and in the SCREEN SYSTEM below are the iter-297 reading, kept for history.

## 4. THE FE TICK — `sub_821494C0` [GOLD, runs on our boot]

Master per-frame front-end update (FSGameSimulation update slot; **no static callers** — vtable dispatch). Runs the whole natural cascade. Driver: kernel/splash_hooks.cpp `[FE_TICK]` (gate `SR_DRIVE_FE_TICK`, default ON).

### ⭐ THE NATURAL FE-TICK DRIVE CHAIN + the VdGlobalDevice fence contract [VERIFIED iter-390, 2026-07-27]

Who actually calls the FE tick on a natural boot (every prior framing of this was wrong):
- **`SystemRunnersMasterFrameDispatch_sub_8271F848` is NEVER called in any roll** (0 hits, healthy AND frozen — it's a vtable-only orphan on our boot path). Not the driver.
- The real driver: **GameKernelLoop 30Hz runner `894BF440`** (host = guest thread created with startVA `0x827A7420`, a dedicated runnable named `"GameKernel::Loop"` made by `GameKernelMgr_StartLoopThread_sub_82720590` — NOT a pool job). Its `TickOnce sub_82720C60` = `TimeGate sub_827206B8` (the ONLY real gate — the readiness vt[+32] is `sub_82196960` = literal `return 1` for all four runners) → rebase `sub_82720740` → payload vt[+4] = thunk `sub_82166328` → `SystemRunnerPumpOrTick_sub_8271ED68(node 0x8295C65C)` → FE module `0x82911388` vt[+20] = `sub_821494C0`. The four service nodes (statics, list head `dword_829A2B98`, next +0x2C): `8295C62C/65C/690/6C0`; dispatch-gate byte at node+5 (=1 at the golden menu for all four); the only +5 writers are `ServiceStart_RunInitGraph_sub_8271E928` (=1) and `SystemRunnerShutdownCurrent_sub_8271EA20` (=0, also nulls module +8).
- The four runner mgr objects `894BF390/440/4F0/5A0` (60/30/30/60 Hz; thresholds `0xFE502C`/`0x1FCA056` ns): the KLOOP census's `ticks=0` is a **probe miscount** (it counts TickOnce's RETURN VALUE, which is the payload chain's rc — 0 even when ticking). Healthy pass rates measured by [TGCENSUS]: exactly 60/s (390, 5A0) and 30/s (440, 4F0) at ~300-450k TimeGate calls/s per loop.

**The mode-B post-flow FE freeze (root-fixed iter-390d/e, `3391e37`):** `D3DDeviceIsFencePending_sub_820FDD90` (**iter-440 rename** — was "FencePassed"; polarity corrected: returns TRUE = fence still PENDING, `(S-fence)<(S-completed)` ⟺ completed<fence; the iter-390 fix below is polarity-independent) reads the D3D device as `dev = *( *(cell 0x82000948) )` — cell = the **`VdGlobalDevice` DATA IMPORT** — then compares fence vs `dev+0x2A1C` (submit counter) and `*(dev+0x2A10)` (completed-counter ptr); fence==submit triggers `GpuCommandBufferFlush_sub_820FD8C0`. We left the raw token in the cell → it read as `0xBE010100` = **deep main-thread stack**; the title stores its device ptr THROUGH the resolved cell on console, so here it stored into stack and read it back — until a deep post-flow call chain clobbered the slot, after which every fence wait failed forever and the FE tick wedged inside the **GPU fence reaper `sub_82616208`** (walks pending list head `dword_82998814`, next at +8; per-entry fence wait via `sub_825FE6E8(0)` vt[+8] under `CsEnterWithDeadline_sub_825FC108`; enter-counter `dword_82998880`, wrapper `sub_8261AA68`) — loop thread alive, never iterating again. Fix: `initKernelDataStructures` resolves the import (cell → zero-init slot `0xD0013000`; `SR_VD_GLOBAL_DEVICE` default-ON). Same class as the KeTimeStampBundle clock import (§6b) — **any data-import cell left holding its token is a live landmine: the guest reads/writes THROUGH it into pseudo-random VA space.**
- HARNESS FACT (iter-390e): the image-write watchdog's periodic 8MB restore snapshots the image AFTER `initKernelDataStructures`, so it re-asserts patched import cells every 1000ms (the guest CRT transiently zeroes them during its own re-init in every roll — self-heals ≤1s). Never WRITE guest memory from an auxiliary host thread without checking the watchdog's PAGE_READONLY window — the AV is VEH-fatal (4/4 boot crashes in iter-390d's first A/B).

Prologue/hot-path globals it dereferences **unguarded** (preconditions for driving):
| global | meaning | note |
|---|---|---|
| `DAT_8296D8E0` | module mgr D | float read at +0x84 first line |
| `DAT_8296D874` | AUI cfg | +0x88/+0x90 reads |
| `DAT_8296EC4C` | resMgr global | `(*it + 0x1c)()` per-frame pump call |
| `DAT_8296EB80` | flow client g3 | `(*it + 0xc)(it, -1)` dispatch — MUST be the real g3, not the fabricated manager (check vs `g_fabMgrVA`) |
| `DAT_829A93C8` | render-ctx pair | see below |
| `DAT_8296E8C8` | event obj | +0xc read |
| r3 | FSGameSimulation obj | `{vt 0x82001888, +4=1}` fabricable |

**Render pair `DAT_829A93C8`** [GOLD]: natural builder `sub_827878C8(allocator)` just allocs 8 bytes and zeroes both slots (`DAT_829a93cc` = allocator). Slot 0/1 = render contexts. Frame bracketing: begin `sub_82787650(ctx, quad)` (critsec at ctx+0, writes ctx+0x40, calls `Function_82787468`) / end `sub_82787730(ctx)`.
**`Function_82787468` scans a BUFFER RING**: `ctx+0x54` = count, `ctx+0x60` = ptr array; per buffer `{+0 i64 stamp, +8 cmd-array ptr, +0xc state (1=busy, 2/3=free), +0x18 cmd capacity}`; picks oldest free, `FUN_82787c20` resets (`fills *(buf+8) with 0xFFFFFFFF for *(buf+0x18) entries`), `ctx+0x50` = current idx. **An empty/zeroed ring picks index −1 → near-endless wild 0xFFFFFFFF fill** (looks like a hang; sprays freed-pattern poison). Fabricate a real 2-buffer ring and force-free both buffers between frames (nothing consumes them on our boot).

## 5. Apt (EA Flash) runtime object model

### Resource manager (AptAux)
- Global **`0x829A23E0`** = resMgr [GOLD]. **Type marker: `*(resMgr+4) == 0x82088654`** — REQUIRED structural check (early boot briefly publishes garbage there).
- Fields [GOLD]: `+0x7c` completed-fetch list (drain input), `+0x80`, `+0x84` pending, **`+0xe0` AND `+0xe4` = TWO apt contexts** (the drain passes `+0xe4`'s; the exec graph often lives on the sibling).
- resIO/ICoreAllocator global `0x829A23E4` [GOLD].
- Import-callback table `0x829a1a60..0x829a1b08`, initializer `sub_826D4520` (roll-dependent). **`0x829A1AC0` = char-type-1 (Shape) geometry resolver = `GfxRendererCreateBoundResource_sub_826D1590`** (static ptr; replay the write when 0). **CORRECTED 2026-09-30: not an import.** It fetches the character's OWN asset by char index from the movie package (`AptFileRecordGetAssetSync_sub_826D0A50`, nested BGFA tag 0x264923D2, id = char index: 925/925 Shape chars) and binds a renderer resource. `0x829A1AF8` (missCb) = NULL on retail too — red herring.

### Apt context (the "ctx")
Published transiently at **`0x829A17BC`** (per-call publish/restore — 1 Hz polls NEVER catch it; latch at publication sites) [GOLD]. Sibling `0x829A17B8`. Layout:
- `+0x18` exec-ctx (screen-pump input), `+0x1c` import dict (symbol map; **CORRECTED 2026-09-30, was "export dict"**: the registrar inserts each 16-byte IMPORT record's movie url here, and executor `sub_826BF630` first calls `AptResolvePendingClips_sub_826AFA58` on it), `+0x20` clip manager (executor input), `+0x2c` clip registry list.
- A usable register-ctx has `+0x1c` == 0 or valid ptr; **reject image-range ctx ptrs** (`0x82000000..0x82920000` = binary image — one bad roll published `0x8209A458` with dict = ASCII "disp").
- **THE ctx is the executor's own** [GOLD, iter-297c]: golden `0x829A17BC` = `0x05083C3C` = `*(executor+0xE0)` — NOT `*(mgr+0xe0)` (`0x050837A8`) or `*(mgr+0xe4)` (`0x05180840`). Registering MAIN into a mgr-side ctx puts it in a graph the runtime never ticks (the iter-217→297 screens wall).

### Per-thread AS executor ★ (TLS slot `*(resMgr+0xd4)`) [GOLD, iter-297c]
The object every AS dispatch resolves through. TWO per-thread TLS slots exist: Apt ctx in slot `*(0x829A1A5C)` (AptRegisterMovie's prologue TlsSets it), and THE EXECUTOR in slot `*(resMgr+0xd4)` (Xenia slot 11, ours 20).
- **Class vtable `0x820886F8`**, size 0xE8. Golden singleton `0x050ABA30` with `+4 = mgr+4` (default-bound).
- **Factory = resMgr→vt[0x10] = `AptExecutorFactory_sub_826D4AB0(mgr, ctxOr0)`**; r4=0 → binds `mgr+4` (golden-exact). Allocates via `DAT_829a23e4->vt[8]`, runs ctor.
- **Ctor `AptExecutorCtor_sub_826D3FD0(mem, iface)`**: builds its OWN Apt ctx from iface rect (`+0x34..0x48`) via `sub_826AF590` → **`exec+0xE0`** (full graph: +0x18/+0x1c/+0x20 populated); tail = `TlsSet(*(resMgr+0xd4), this)` — installs ITSELF.
- **Pump = exec→vt[0x0C] = `AptExecutorPump_sub_826D2718(exec, r4)`** — THE natural whole-runtime tick: re-TlsSets itself → `sub_82686770(*(exec+0xE0))` → drain `AptDrainCompleted_826D23A0` (parse+register completed loads) → cmd-queue drain `sub_826D2558` (exec+0x84, types 0/1/2 → `sub_8268D160/8268D118/82691CC8` on the ctx) → frame clock `sub_826D0F50` (exec+0x10 ts) → `sub_826C0C38(*(exec+0xE0), nFrames, r4, *(*(exec+4)+0x30))` movie-tree update.
- **Frame item type 5 = BackgroundColor** `{+4 0xAARRGGBB}`. **CORRECTED 2026-09-30, was "Frame ACTION items (type 5) … POSTS actionData".** There is exactly one per movie (820/820; 747 are 0xFFFFFFFF). `AptExecuteFrameTags_sub_826BE100` applies it once per process (guard `byte_829A0AA7`) through cb slot `0x829A1A70` = `sub_826D1530` → `TLS(*(resMgr+0xd4))->vt[0x34]` = `sub_826D0DB0`. IDA shows that function is a single store of the value at `exec+0x18`: no queue, no AS. The IDA name `AptActionPost` is a misnomer. AS frame scripts are types 1 and 8 (see Frame-item types below).
- `Function_825F70C0` = get-or-create per-thread HEAP (TLS slot from `DAT_82929374`) — different slot; arming it jumped clipCtor 11→83.
- `0x828EE16C` = KeTlsGetValue import thunk (the "opaque 0x828EE region" is the import thunk table).

### Movie file format & register contract ★ the big one
- ui.AST = **BGFA1.05** container with 828 TOC records (id = FNV-1a-64 of the UPPER name, 828/828). 821 of them are nested per-movie BGFA containers, and 820 of those hold an `Apt Data` .apt plus an `Apt constant file` .const. Reader, writer and byte-exact regression tests: `tools/apt/` (README there).
- **CORRECTED 2026-09-30: every carve name in `data/ui_ast_extracted/` is wrong.** `tools/extract_ast.py` carve *n* holds TOC record *n+2* (1640/1640 files; `archive/v1_frontend/tooling/carve_evidence.py`).
  - **MAIN = carve `0038_ICONMENU`**: TOC #40 at ui.AST 835968, nested .apt at +0x68 = 836072. The loose root `main.AST` (168,731 B) is byte-identical to MAIN's whole nested container.
  - `0040_MAIN` = PARTHIGHLIGHTBAR, `0045_SAVELOADSLOTS` = SCRPRESSSTART, `0047_SCRPRESSSTART` = STAMINAMETER.
  - **`kernel/main_apt_blob.h` (labelled "MAIN") is PARTHIGHLIGHTBAR**: TRUE .apt 0x1b51 = 6993 B and .const 942 B (carved as 7000/4048 B). The "TRUE length 0x1b51" measured earlier is PARTHIGHLIGHTBAR's.
  - Select movies by TOC name (`tools/apt/bgfa.py` `movie()`), never by carve number.
- **An edited ui.AST runs in the game [VERIFIED E1, iter-463, docs/MODS.md].** A rebuilt ui.AST (tools/apt writer; SCRPRESSSTART's `.const` `SUP_PRESS_START` -> `Generic_QUITGAME`, the other 827 entries byte-identical, 89595436 B) served through the `mods/` overlay makes the title read QUIT GAME (2/2 rolls) with an unchanged APT_REQ list; no checksum or size check rejects it. The loose disc-root `ScrPressStart.AST` is byte-identical to the SCRPRESSSTART TOC entry's data.
- **[2026-10-02 VOID: E1 never put the file in front of the game - the host listing packed it into the same NtQueryDirectoryFile answer as GAMECONFIG and the game reads only the first record per call (FindFirstFileA sub_828A7D50 / FindNextFileA sub_828A7C10, 328-byte buffer, sub_828A8640 ignores NextEntryOffset); fixed by SR_NTQDIR_XENIA, docs/MODS.md section 12; the _workinprogress storage: priority 0x7D0, recursive, 29 types keyed by stem, the owner's private mod project research/modsystem/DESIGN.md] ~~REJECTED (E1)~~ for `<TOC NAME>.ast`: per-movie delivery through `d:\DATA\_workinprogress\SCRPRESSSTART.ast`.** `MountLocalizedBigFiles_sub_8218D128` mounts a LooseFileStorage there and the boot enumeration (`*.*`, `__ASSET__*.*`, `*`) lists a `SCRPRESSSTART.ast` (the nested container), but nothing opens it and ScrPressStart still loads from ui.AST. The disc's own `_workinprogress/GAMECONFIG` and `__ASSET__GAMECONFIG` are never opened either. What (if anything) that storage serves is not checked. **Untested forms:** `AptResNameParseToDescriptor_sub_826EA798` keys a loose file by extension storage type + a 64-bit id that is either `ResNameHashFnv1a64NoCase_sub_82702FC0(name up to '.')` (which name the Apt request hashes is not checked) or an explicit id parsed from `__ASSET__<16 hex digits>` (never tried). The game also probes absent loose files (`BaseWorldFlow_PHYSICS.AST`/`_TEMP.AST` in the disc root; `DATA/parammap|arenas|xml|pus.AST`).
- **Apt FSCommand -> host [VERIFIED iter-463, custom_frontend step 5].** `fscommand(cmd,args)` = ActionGetURL (0x83, static `{url*, target*}` operand) or ActionGetURL2 (0x9A, **no operand byte in Apt**: `AsInterpretBytecode_sub_82688B90` dispatches handler table `0x82931420[op](vm, ctx)` and `AptAS2ActionGetURL2LoadMovie_sub_826BB0F0` never advances pc; it pops url (second from top) and target (top), both stringified). A url starting `"FSCommand:"` (`off_829313C4`) goes to `sub_82682DF8` -> `*0x829A1A90` = `AptFsCommandDispatch_sub_826D39A0(cmd, args)` -> `AptNamedRegistryLookup_sub_826D3780(*0x829A23E0, cmd)->vt[3](args)`, no null check. kernel/mod_bridge.cpp strong-defines it (registry: platform-fix): `SR_*` handled on the host whichever mod issued it (not tied to a mod name since the step-4 review; retail issues none), everything else the game's own body. Live: `[SR_FSCMD]` lines from both ops (archive/v1_frontend/menu/*).
- **`_global.FlowMgr.CppShowPopup` is looked up by name on every C++ popup [VERIFIED iter-463].** `sub_822B15F8` builds the 10 AS args (title/body/buttons localized through `sub_822AC408` unless the per-field literal byte +33/+34/+35 is set) and calls AptExecutor vt+32 with path `FlowMgr`/`CppShowPopup`; a member reassigned from AS (ScrPressStart patch) receives the call. The id it passes (`cppId`, arg 9) is the value the C++ stores at `saveFlow(*0x8296EE8C)+16` - but only AFTER the AS call returns (log: `saveFlow+16=0` at the claim, `=1012091577` = cppId at the answer). Answering `GlobalAdapter.CppPopupComplete(cppId, 0|1)` from a mod popup runs the natural LoadCreate buttons: 0 -> intro + gameplay HUD (`APT_REQ main_hud`), 1 -> `ShowSlotSelector(1)` (LOAD A GAME, 3 EMPTY SLOT on an empty root); B there -> `BootFlowLoadCanceled` re-raises the popup with the SAME cppId.
- **A popup shown from inside a ScrGenPop selectedCallback is lost [VERIFIED iter-463, roll menu/loadexit1].** `ScrGenPop.HandleInput` calls `selectedCallback.call(context, this, idx)` and THEN `gPopupMgr.ClosePopup(this)`; a `ShowGenPop` issued inside the callback produced no APT_REQ and no panel. Defer it: the game's own pattern is `setInterval(FlowMgr, "ShowPopupDelayed", 20, ...)` (MainLib 0x2270); the mod's `SRFrontEnd.Defer` does the same and the chained popup appears (loadexit2..5).
- **Apt key codes on this build (pad):** CROSS=302 (A), START=301, SELECT=300 (BACK; keyboard Esc/Backspace), DOWN=40 (`[SR_FSCMD] menu key=` lines). The keyboard merge maps Enter to START, not A, so a ScrGenPop ignores Enter unless a mod maps it.
- **Game options contract [VERIFIED iter-463 IDA + rolls settings/set3-4, custom_frontend step 7].** `SettingsAdapter` natives are registered by `sub_822AF3E8` (Get/SetMusicVol `sub_822AD2C8`/`sub_822AD5A0`, SFX `sub_822AD408`/`sub_822AD6F0`, Get/SetControlScheme `sub_822A7060`/`sub_822A7178`, X/Y inversion `sub_822A70D8`/`sub_822A7100` get, `sub_822A7248`/`sub_822A7298` set, AutoCenter `sub_822A7128`/`sub_822A72E8`, Rumble `sub_822A7150`/`sub_822A7338`, StopRumble `sub_822A0E18`). State: per-input-slot 16-byte records at `*dword_8296EDB0` (+0 scheme, +4 displayed scheme, +8 X inv, +9 Y inv, +0xA vibration, +0xB auto-center) and the audio mixer (`dword_82976054`+`dword_82976070`, volume = mixer entry id 12 / 20 `+76 * +40`; without the mixer the getters return a cached float that starts at 0 and the setters reach nothing). **`ProfileSettings_applyOptions_sub_822A45D0` is profile -> game** (sign-in and NEW GAME): XamUserReadProfileSettings ids 0x10040023/24/02/03, results `+72` movement (scheme 2 vs 0), `+112` Y inversion, `+152` vibration (==3), auto-center forced on. **Saves carry options**: save record writer `sub_82165088` (music, SFX, per slot +0/+8/+9/+0xA) and reader `ScriptCmd_FillRecordArray_sub_821652E0` (called by `AptDisplayListBuildTable_sub_8218E4A0` on the LOAD answer, then `LoadComplete(true)`). **Our front end has NO audio mixer** (`dword_82976054`=0 at every FE capture) while the golden Xenia dump at the main menu has 0x03672F40 / 0x0385E718: FE volume sliders read 0 and do nothing (open platform gap).
- **A ScrPressStart reloaded in the same session never reaches InitGui [OBSERVED iter-463, rolls settings/set1, set2, back1; cause OPEN].** After `SwitchScreen(SCREEN.AUDIO_OPT)` then `SwitchScreen(SCREEN.PRESS_START)`: APT_REQ ScrPressStart, the class InitAction runs again (mod log `initaction`), its `onLoad` runs (log), but `InitGui` never does (no log; SaveLoadSlots left visible; the screen manager's transition input lock never released). Keeping `_global.ScrPressStart` + its registerClass alive across the unload changed nothing. The custom front end avoids it for SETTINGS by loading options screens as overlays (`FlowMgr.SwitchLevel("NONE", id, 60)`).
  **QUIT GAME A/B (iter-463 settings review 4, `archive/v1_frontend/settings_fix/`, scenarios `femenu_quit.json` / `natural_quit.json`):** mod OFF (`quit_off1`): pause -> QUIT GAME -> YES -> the title reloads and works, START -> `[XAM_PROFILE] #3` -> the natural LOAD A GAME? popup. Mod ON (`quit_on1`, `quit_on2`): the same reload shows ScrPressStart uninitialised (SaveLoadSlots boxes, the background well over the title, no input) and the attract loop starts later: **a soft-lock caused by the mod**. Bisect with SR_Log diagnostics in the InitActions / onLoad / InitGui (`quit_diag1`): on the reload InitActions 60, 61 and 59 (registerClass) all complete, then **onLoad never runs**. Diagnostics alone (`quit_diagonly1`): reload OK. Menu patch without the LoadComplete op (`quit_v1_nolc`): fails. InitAction append only (`quit_v3_iaonly`): fails. **The InitAction append present but jumped over, never executed (`quit_v6_skipped`): fails** -> the cause is STRUCTURAL (the edited movie: ~1030 appended instructions, pool 88 -> 178 entries, still < 256), not the mod's AS logic. REJECTED (each tried, reload still failed): deleting `_global.SRFrontEnd` in ExitScreen so no function object of the unloaded movie survives (`quit_on2`); dropping the top-level register-1 write in the InitAction append (`quit_v5_noreg1`); keeping the class alive (set2). Next: find what the Apt loader does differently on a second load of an edited movie (the movie cache / relocation path) with IDA, and bisect the structure with never-executed code (block size, function count) instead of AS changes.
- **Host -> AS call [VERIFIED iter-463, kernel/mod_bridge.cpp].** From inside a guest call (the FSCommand handler) the host can call an AS method exactly as natives do: `AptAS2AllocIntObj_sub_82693028(int)` -> AS value, then `(*g_AptExecutor 0x8296EB80)->vt[+0x20](exec, "Method", 0, "TargetPath", nargs, value...)` (= `sub_826D0D30` -> `AptAS2InvokeByPathCtxRaw_sub_826BB990`; varargs read from 8-byte slots, low word). Target path `"FlowMgr"` resolves from `_level0` (as `sub_822B15F8` calls FlowMgr.CppShowPopup); `0x8296EB00` is the SaveLoadAdapter listener path. Delivered in rolls (log `reply FlowMgr.SR_OnHostSettings(n) delivered`, followed by the AS method's own log line).
- **AS objects read from the host [VERIFIED iter-464: IDA + all three Xenia goldens (Python twin) + a synthetic heap (difftest `--mouse-ui-test` M4); kernel/mouse_ui.cpp, docs/MOUSE_CURSOR.md 3].** Value object `+0` vtable, `+4` flags (low 7 bits type, bit 27 live): 1 string (`+8` -> string obj, chars `+8`), 5 bool, 6 number (f32 `+8`), 7 int (i32 `+8`), 12 MovieClip wrapper (vt `0x82086FF0`), 14 Array (`+0x20` elements, `+0x24` cap, `+0x28` count), 17 `_global`, 19 Object, 34/35 functions. Property table: `obj+8` for objects/arrays/functions (vt[2] `sub_826819C0`), `*(*(wrapper+0x20)+12)` for a MovieClip (vt[2] `sub_82680448`); `{cap pow2, entries[{name*, value*}], +8 __proto__, +12 prototype}` (`AsPropertyGet_sub_8268C228` special-cases the two names; `prototype` of a class = `_global.<Class>` table +12). Name key `+6` = `AptStringHashFNV1a16CI_sub_82681C40` (lazily cached, 0 = not yet), empty slot 0, deleted `0x829311B4`. **AS `_global` = `*0x829A1588`** (`*0x829A1584` is the native adapters object). MovieClip wrapper `+0x18` next sibling, `+0x1C` parent, `+0x20` instance D; D `+4` state S, `+8` char type <<26, `+12` props, `+28` display list; S `+8` matrix* (a,b,c,d,tx,ty), `+24` bit31 `_visible`, `+22` u16 >= 0x8000 placed, `+80` text bounds; shape bounds chardef `+16`. `AptMatrixConcat_sub_82684F38(A,B)` = A·B (B first); world(child) = world(parent)·M(child).
- **The input action layer [VERIFIED iter-466 controls2, IDA + difftest --pc-actions-test; docs/PC_CONTROLS.md 2.1].** InputMgr `*0x829A2C04` (+4 slot count, +12 MapInfo[] of 32 B: +12 state 0 = live, +16 enabled, +20 context); player 1's slot = `*(*0x8296E8C8 + 12)`. A context = `{vt, +4 count, +8 InputCommand*[event id]}` (ids = the INPUT_EVENT_* table 0x82912B68). Per tick `InputMgr_DispatchQueuedInput_sub_82721E68` -> `InputContext_UpdateCommands_sub_82722DB8(ctx, InputState)` calls each entry's Update (vt slot 6; Reset slot 7 when no pad), so an object under two ids updates twice. Classes (vtable / Update): EventButton 0x8208CF34 / 82723A60, Release 0x8208D44C / 82725C90, Mapped 0x8208D29C / 827250B8, Axis 0x8208CE5C / 82723518, AxisMapped 0x8208D374 / 82725740, Alternative 0x8208D1C4 / 82724A18, Combo 0x8208D0EC / 82724660, DualButton 0x8208D00C / 82723D70 (fields in PC_CONTROLS 2.1). Consumers all go through the query `sub_82181860(igcMgr, slot, id, consume, &value)` (DisableInputEvent list `*0x8296D888`, buffered latch for 65/66/68/77/79/58, else `InputMgr_IsCommandActive_sub_82722390` + GetValue vt slot 5). InputState: buttons = bitset `+0x20` (count `+0x14`) -> 0/255, axes = int[] `+0x28` in -1000..1000. Menu router `MenuButtonRepeatTick_sub_8214FC78` / `ProcessAnalogMenuNav_sub_8214E9D8` posts `AptAuxInputPostButton_sub_826D2C10` (AptExecutor vt+96) from the table 0x82911470; the pause tick `PauseFlowStateTick_sub_82151298` calls `FlowMgr.ChangeInterfaceState` via AptExecutor vt+28. The Apt invoke primitive for host calls: `AsInvokeByPath_sub_826B8DC0` (args on the AS stack 0x829A19B0, method via `AptAS2GetVariableByPath_sub_826B3998`, `AptAS2CallFunction_sub_8268FB58`, register frame 0x829A1624/28) inside `AptAS2InvokeByPathCtxRaw_sub_826BB990` (publishes exec+0xE0 to 0x829A17BC and its TLS slot). The menus' input router instance is `_level0.gEH` (`BistroFW_EventHandler.HandleEvent(keyCode, controllerId)`). D-pad down drives no in-game command: x-ray vision has a power definition (BLINDVISIONINCLUDE.xml, `xray_stand_in` VFX) but no input.
- **Apt mouse input is COMPILED OUT; menus take key codes [VERIFIED iter-464, docs/MOUSE_CURSOR.md 2].** Readers survive (`_xmouse/_ymouse` from `0x829A1788/0x829A178C`, `hitTest`, `getBounds`, `startDrag`, the onPress..onRollOver ids 211-215 and their clip-event masks `0x82085988`), but executor cmd type 1 (vt slot 21 `AptExecutorPostCmdType1Mouse_sub_826D29E0`) is handled by `AptExecCmdType1MouseCompiledOut_sub_8268D118` (empty body), the position is only ever zeroed, and 0 of 820 movies has a Button char or a mouse handler. Input = `Key` listener `BistroFW_EventHandler.HandleEvent` (MAIN 0xe94e): drop while `gSM.mInputLockMgr.mLocks` is non-empty; walk `gSM.mScreens` from the END to index 1, stop at the first `HandleInput` returning false; pass-through: `ScrBackground_IG`, `ScrForeground_IG`, `ScrVideoClips`, `Main_Hud` (unless its save slots show). Key state word `dword_829A19F8` = `keyCode<<17 | controller<<2 | 1` (`AptInputRecordKeyOrStick_sub_8268A538`, set per queued event by `AptScreenPump_826BF268`). Pad: CROSS 302 = A, TRIANGLE 304 = B, CIRCLE 303 = Y, SQUARE 305 = X.
- **Register entry `sub_826AECC8(r3=ctx, r4=&holderSlot, r5=RAW .apt bytes, r6=RAW .const bytes, r7=movieObj)`** [GOLD]. **The "defHdr" (r6) IS the .const file**: `const+0x14` = movie-def offset into the .apt (0x98 for the embedded blob = PARTHIGHLIGHTBAR; 0x6A0 for the real MAIN), `const+0x1c` = const-table offset. Publishes ctx to `0x829A17BC` for the call, restores after.
- Parser/registrar `sub_826AE6D0(dict, &holder, blob, defHdr, movieObj)` [GOLD]: relocates `*(defHdr+0x14) += blobBase` → defObj, `*(defHdr+0x1c) += defHdr`, calls registrar `sub_826ADD40(defObj+0x10, blob, defHdr, movieObj)`, **RESTORES both fields before returning** (transient — that's why golden diffs show no self-relocated dword). Writes holder `{+8=3, +0x10=blob, +0x14=defObj(absolute), +0x18=movieObj}`.
- Registrar `sub_826ADD40(P=defObj+0x10, blob, defHdr, movieObj)` [GOLD]: chars: `*(P+0xc)` count / `*(P+0x10)` offset-array (each +*blob*, each char+4 ← table base); per-type reloc switch (1=**Shape** → `(*0x829A1AC0)(movieObj, idx)` fetches its geometry asset → result to char+0x20; 5/9=Movie/Sprite → `sub_826A39A0` frame-tree walk; 2,3,10 field relocs). A NULL char-table slot is an imported character. **CORRECTED 2026-09-30:** type 1 is not an import, and the two record tables below had their names swapped:
  - **Imports**: `*(P+0x20)` count / `*(P+0x24)` array (= movie+0x30/+0x34). Stride 0x10 `{movieUrl*, symbol*, localCharId, rtSlot}`; the registrar relocates +0/+4, inserts the url into `*(DAT_829a17bc+0x1c)` via `sub_8269B658`, and stores the node at +0xC. Every localCharId names a NULL char slot (112/112; e.g. SCRMAINMENU imports `components/IconMenu` symbol `IconMenu` as char 1).
  - **Exports**: `*(P+0x28)` count / `*(P+0x2C)` array (= movie+0x38/+0x3C). Stride 8 `{name*, charId}`, relocate +0 only; linkage names such as `IconMenu` and `__Packages.IconMenu` (496 in all).
  - **MAIN registers its 40 classes FIRST** (lane E step 5 E3-E6 r3, the Ally roll of 2026-10-23): each class InitAction is guarded by `if (!_global.X)` and MAIN is _level0, so a patch to ScrGenPop / ButtonIcon / ButtonHints / DisplayWell / PartText / ... inside any other movie never runs - `tools/apt/patch_movie.py` refuses one (THE FIRST-REGISTRAR RULE). Mount order by construction: `MountLocalizedBigFiles_sub_8218D128` (the `_workinprogress` loose storage) is called only from the file module's init `BinaryXmlAssetAllocatorInit_sub_821507E0` (0x821507F8; referenced only from its module vtable 0x820BF8B8), which `FmModuleInitOnce_sub_8271F2C8` runs once (done flag +68) inside the module init graph walk `FmModuleInitGraphWalk_sub_8271E630` (from `ServiceStart_RunInitGraph_sub_8271E928` at 0x8271E9D4 and `SystemRunnerApplyTransition_sub_8271EB00` at 0x8271EBF4, before the state's update); MAIN is loaded from a state script (`APT_LOADMOVIE("main")`, lr 0x82762CDC in `luaD_precall_sub_82762B10`). The `[ASSET] MAIN` receipt (`kernel/asset_receipt.hpp`) shows per run which MAIN registered.
  - **MAIN has ZERO imports and 40 exports** (all `__Packages.*`), and no movie imports MAIN (all 112 records are `components/*`, `parts/*` or a bare name). So the ctx+0x1c dict does not grow for MAIN, and "dict grew" is still the WRONG success marker for it.
  - The old "MAIN: 11 chars at `0x6c` array, movie/defObj at blob+0x98" describes the embedded blob, which is PARTHIGHLIGHTBAR (2 Shapes, 3 exports). The real MAIN has 41 chars (40 Sprites), its char table at .apt+0x5FC and its movie at .apt+0x6A0.
- Holder: `+0` refcount (init high, e.g. 0x10, so pool-free never triggers on our heap objects).

### Drain / async fetch (the natural delivery we bypass)
- Chain [GOLD]: fetch → `sub_826D5FE8` (onDataArrived) → `sub_826D7850` (poster; routes TWO sub-assets = .apt + .const into record+4/+8 by ID) → node on `mgr+0x7c` → pump `sub_826D2718(mgr)` → drain `sub_826D23A0(mgr)`: per node `blob = node[+4]->vt[0x10]()`, `defHdr = node[+8]->vt[0x10]()`, `sub_826AECC8(*(mgr+0xe4-ish ctx), &node[+0xc], blob, defHdr, node)`. Record accessor vtable `0x820886c4`. On our boot the fetch never posts (mgr+0x7c always empty) — hence blob injection.
- Parse builder `sub_82700F80` [DECOMP/GOLD-partial]: param_1=OUT def (0x150), param_3=hdrBuf is **strncpy'd as the NAME**, param_4 → `def+0x100` = base (ALL offsets = fileOff + base), `def+0x108` = body ptr, `def+0x120` = constDef (set by caller `sub_826E83D0`), `def+0x149` = OK flag. Raw-memory mode `def+0x130` exists but is zeroed at entry and body I/O ignores it — NOT armable from host. Device I/O via `def+0x114` (`Function_826075A8`/`Function_82604350`).
- `sub_826E83D0` = BigFileAsset::Load(asset, loadMode, looseFlag) [GOLD]: asset `{+8 name, +0x208 aptDef OUT, +0x20c constDef OUT, +0x210 state, +0x218/+0x21c/+0x224/+0x228 hdr fields}`. Vtable method (slot at `0x82089c7c`, vtable `0x82089c70`) — no static callers.

### Instantiation & execution ★
- **Movie build `sub_826AF688(newObj, slotObj, &holderPtr)`** [GOLD]: base ctor `sub_826AEFD8` (arms `+0x10 = -1` frame counter, `+0x14 |= 0xc0`, allocs `+0xc` frame-state `{*=8}` and `+0x1c`), installs runtime-movie vtable **`0x82087738`**, stores holder at `+0x28`. Our fabrication also needs **`wrapper+4 = defObj`** and **`wrapper+8 = type<<26` (9 = Movie)** — base ctor doesn't set them on a zeroed object.
- **Install `sub_826B0358(clip, movieObj, 1)`** [GOLD]: writes **`clip+0x20`** (the movieDef slot logged by `[CLIP_CTOR]`), registers clip into `*(DAT_829a17bc+0x2c)` (`sub_82698370`) — publish a real ctx to `0x829A17BC` around the call.
- **Activation** [GOLD]: `sub_826811C8(clip, 1)` (active bit 0x08000000 in clip+4, spinlock `0x829a19a8`) + `sub_826807F8(clip, 1, 1)` (needs-exec `clip+0xc |= 0x2000000`, propagates up `clip+0x1c` parent chain; type-checks `wrapper+8>>26 ∈ {1,2,10}` and `clip+4 &0x7f != 0x25`).
- **Frame advance `sub_826BE8C0(clip)`** [GOLD]: gated on needs-exec; wrapper type must be 5/9; executes frame items via `sub_826BE100(*(*(wrapper+4)+4)+0x10, wrapper+0x1c, clip, curFrame)`; frameCount = `*(*(def+4)+0x10)`, loop flag `def+0x18`; curFrame `wrapper+0x10`.
- **Executor `sub_826BF630(clipMgr = *(ctx+0x20))`** [GOLD]: matches pending movie-loads **via the import dict** at ctx+0x1c (MAIN can never match: no movie's import records name it, 0 of 112; **CORRECTED 2026-09-30**: it was called the export dict, with the reason "0 exports", but MAIN has 40 exports and 0 imports), builds+installs (`sub_826AF688` + `sub_826B0358`), first-frame kicks. Natural caller `sub_826C04F8` (bytestream pump) passes `*(DAT_829a17bc+0x20)`.
- **Screen pump `sub_826BF268(execCtx = *(ctx+0x18))`** [GOLD]: drains a work ring; first-frame kick `sub_826BF198` (array `DAT_829a1798/179c`).
- **AS INTERPRETER `sub_82688B90(vm, bytecode, scopeObj, len, ...)`** [GOLD, iter-297c — CORRECTS the old "screen stack" model]: THE ActionScript bytecode interpreter. Opcode dispatch table at **`0x82931420`** (one handler per opcode byte); `len=-1` = call-function-object mode (pushes scopeObj on the VM call stack `vm[9]/vm[0xb]` and invokes `scopeObj->vt[0]`). **`0x829A19B0` = the AS VM object, NOT a screen count**: `+0` value-stack count (transiently nonzero only DURING execution — golden shows 0 at menu idle), `+8` value-stack array, `+0x24/+0x2c` call depth/frames, capacity `0x80`. The old "aptCount>0" north star was a mis-model; the real marker is AS-interpreter entries (`[AS_EXEC]` watch, kernel/apt_as_watch.cpp) and FlowMgr object creation.
- **Frame-item types** (executor `AptExecuteFrameTags_sub_826BE100` dispatch) [GOLD]. **CORRECTED 2026-09-30** against IDA sub_826BE100 and `AptSpriteFramesRelocate_826A39A0`, plus a census of all 820 movies (3615×3, 820×5, 204×8, 159×1, 60×2, 56×4). It was "4 = label, 5 = one-shot action-cb, 8 = FRAME ACTION".
  - **1 = Action** `{+4 bytecode}`, the master timeline script. `sub_826BE100` NEVER touches this class. Natural path: `ClipFrameAdvance → sub_8268C198` enqueues it into the exec-ctx ring `*(17BC+0x18)+0xc`, and the screen-pump drain runs the interpreter. MAIN's (carve `0038_ICONMENU`) carries the whole framework bootstrap.
  - **2 = FrameLabel** `{+4 name, +0xC frame}`. It is folded into the sprite's label map at load by sub_826A39A0 and is not dispatched per frame.
  - **3 = PlaceObject**: clip ctors, via `AptPlaceObjectTag_sub_826BD238`.
  - **4 = RemoveObject** `{+4 depth}`: display-list lookup by depth (`sub_82683918` with a null name), then remove.
  - **5 = BackgroundColor** `{+4 0xAARRGGBB}`: applied once per process through `(*0x829A1A70)(item[1])`. See the Per-thread AS executor section.
  - **8 = InitAction** `{+4 spriteId, +8 bytecode}`: `sub_82688B90(0x829A19B0, item[2], clip, -1, ...)` runs AS bytecode, but only while `item[1] >= 0`. Afterwards `item[1]` is negated, so each one runs once per load. All 204 retail InitActions have spriteId > 0.
  - **Order within a frame (verified 2026-09-30, IDA):** `sub_826BE100` makes two passes over the frame's item array. The first pass runs every type-8 item, in array order. The second pass handles 3/4/5. Type 1 is queued separately (`AptEnqueueFrameActionsType1_sub_8268C198`, callers `ClipFrameAdvance_826BE8C0` and `AptMovieClipGotoFrame_sub_826BE728`). So an InitAction's position relative to other item types does not matter. Only its order among other InitActions does.
  - **Who reads InitAction `+4`:** the sign test and the negate in `sub_826BE100`. `AptSpriteFramesRelocate_sub_826A39A0` relocates only `+8`. `sub_8268C198` and `AptExecuteFramePlaceObjects_sub_8269B7B0` ignore type 8. This was not proven exhaustive: there was no whole-binary search for other readers. So a mod InitAction needs `+4 > 0` and nothing else found so far (`tools/apt/patch_movie.py` uses the movie's char count by default).
- **Apt writer invariants: retail corpus census, 2026-09-30, `tools/apt/`.** Keep these when adding code:
  - Every `.const` item is referenced by exactly one Push/ConstantPool array element: 19,812/19,812, with 0 shared and 0 unreferenced. ConstantPool entries are all strings (7,039). Push entries are all registers (12,773).
  - Each bytecode block has at most one ConstantPool, always its first instruction (219 blocks have one, 379 have none). Pool-index ops resolve against it. Function bodies index the defining block's pool even when they are called later, from another context. That is inferred from retail code; the interpreter's pool capture was not traced.
  - The word form `EA_PushConstWord` (0xA3) appears only for pool indices of 256 and above (142 uses, largest pool 328). All other pool ops take a 1-byte index.
  - A DefineFunction(2) with no params still has a non-null params pointer (926/926), aimed at the next block. The trailer words are always `0x98765432, 0x12345678` (1,551/1,551).
  - **Items must be stored in the runtime's UNLOAD-walk order, one item per slot [VERIFIED iter-463 IDA + offline simulation; replaces "retail allocates items in instruction order"].** Load: `AptActionBytecodeRelocate_sub_8269F3A0` (a3 != 0) reads each 0x88/0x96 array slot as a `.const` item index (0x8269F5BC, `8*idx + *(const+28)`) and overwrites it in place with the live AS value. Unload: `AptMovieUnrelocate_sub_826A3180` (only code caller `AptMovieHolderDestroy_sub_826A3880`) resets a counter (0x826A31A4) and walks chars in index order (types 5/9 only; NULL slots and any index an import record names are skipped - the skip is by import-table membership, `+8` localCharId, because at runtime an imported slot is live during this loop: the first loop releases it and a later loop, 0x826A36F0, nulls it) -> `AptSpriteFramesUnrelocate_sub_826A0758`: frames -> items (type 1 Action bytecode +4, type 3 PlaceObject clip-action records +8, type 8 InitAction bytecode +8) -> `sub_8269F3A0` with a3 == 0, a linear scan to the first End that releases each value and writes **`slot = counter++`** (0x8269F6BC): the original index is NOT restored. A later load of the same movie reuses the resident payload (only the ui.AST header and TOC are read again; `AptRegistrar_826ADD40` relocates with no already-relocated guard) and relocates those counter values. So slot k must hold k and #slots == #items; all 820 retail movies do (`tools/apt/apt_unload.py`: 820/820). The instruction-length table of the scan is `apt_model.insn_end`, which matches the runtime switch.
    - **What broke it:** the toolchain appended new items at the end (`aptasm`, `patch_movie._grow_pool`, `Graph.add_const_string`) and `Graph.gc()` kept that order, so the 5 patched custom_frontend movies stored 439 / 225 / 183 / 130 / 199 slots (SCRPRESSSTART / SCRSAVELOAD / SCRGAMEPLAYOPT / SCRAUDIOOPT / SCRCONTROLLEROPT) out of walk order. The first load was right; the reload of SCRPRESSSTART after QUIT GAME resolved InitAction 59's `Object.registerClass('ScrPressStart', ...)` pool to `[reg2, reg2, reg2]`: the class was never bound, the clip was built as a plain MovieClip, so onLoad and InitGui never ran (the QUIT GAME soft-lock). InitActions still "completed" and their SR_Log lines printed because GetURL (0x83) strings are inline pointers, not pool items. The same cause fits the set1 / set2 / back1 "reloaded ScrPressStart never reaches InitGui" rolls (the same cached blob was reused there). Offline proof: `python tools/apt/rt_patch.py` section Q reproduces the `[reg2]x3` reload on the un-renumbered build. **Live-confirmed 2026-09-30** (`archive/v1_frontend/quit_verify/femenu_quit2`, `femenu_quit3`, mod ON): after YES the reload requests ScrPressStart (`PKG n=41`, `PARSE b14=0000036C`), its InitGui runs (the mod's `SR_Log 'install'`), the custom menu works again and EXIT ends with code 0; CUSTOM_FRONTEND.md 5.4 item 4.
    - **The writer rule:** `apt_graph.Graph.renumber_walk_order()` runs after `gc()` and before `emit()` for every patched movie (`patch_movie.Patcher.finish`): slot k gets index k and item k is the constant it named; an index array shared by two instructions is copied, a constant named by two slots becomes two items. It is a no-op on every retail movie (`rt_graph.py` 820/820). `patch_movie.verify()` and `build_custom_frontend.py` (every output movie, including `--check`) fail unless the byte-level simulation (`apt_unload.check`) shows index == counter, slots == items, every slot walked, the same constant per slot as before the renumbering, and a reload after a simulated unload resolving the same constants. Mutations caught: no renumbering (5/5 movies), two items swapped, two slots swapped, one item dropped, and a walker with the wrong item order or without clip actions (retail then fails).
  - `tools/apt/aptasm.py` / `patch_movie.py` follow all of these. `tools/apt/rt_asm.py` shows that re-assembling every block with fresh items reproduces the retail `.const` byte for byte (820/820): in retail, instruction order within a block and the unload walk agree.
- **AS REGISTER FILE ★ (iter-297e — the FlowMgr root cause)** [GOLD]: const-file entries are typed — type 1 = string, **type 4 = REGISTER reference**, resolved by the push handler (`sub_82689FC0` → `sub_82688AC8`) as `*(idx*4 + DAT_829A1624)`; type 8 = vm-relative (`vm[0x11]`). **`DAT_829A1624` = current register-frame base, `DAT_829A1628` = frame size.** Invocation protocol: save base, `base += size*4`, `size = 0`, call interpreter, restore (`sub_826A5A90`). **Every stock invocation site opens the frame size-0 with NO preloads** — a timeline script's register-1 ("this") targeted assignments (e.g. MAIN's (carve `0038_ICONMENU`) `onEnterFrame=…`, `gFWS/gFM/gIL/gSM/gCM`, `iDelayFrames`) silently no-op unless the caller preloads r1 = clip and reserves slots (write base'+4 = clip, size = 8). The register-framed re-run (kernel/splash_hooks.cpp) does exactly this; with it the BistroFW framework bootstraps and `_global.FlowMgr.ChangeInterfaceState` executes.
- **MovieClip host setter `sub_826BBDA0(env, clipObj, &nameObj, valueObj)`** [GOLD]: interpreter SetMember/SetVariable on clip-bound scopes routes here; event-name property IDs 0xC8-0xD6 (descriptor lookup `Function_8267F550`, mask table `0x82085988`) OR the handler mask + add clip to the exec-ctx enter-frame list `*(17BC+0x18)+0x18/0x1c`. Watch: kernel/apt_set_watch.cpp `[SET_WATCH]`. Event-mask refresh-by-scan `sub_826B2490` (sole natural caller = class-bind `sub_826B8040`: export name → class registry `DAT_829a14dc` → ctor via `sub_8268FB58(vm, this, fn, argc, argv)` → refresh → list).
- **AS name resolver `sub_826B1040(clipOrScope, &nameHolder, altScope)`** [GOLD]: builtins → scope chain (`clip->vt[2]()` binding + parents) → `_global` (`DAT_829a1584`). Name holder: `sub_8268F3E8(&holder, cstr)` / release `sub_8229FD48`.
- Clip ctor (type-12) `sub_826B0458(node, ?, movieDef)` [GOLD] — hook exports `g_lastClipNode`. Type-37 `sub_826CB2E0`. `[DEF_EXEC]` = `sub_826B2298` begin-def. _level0 create = `sub_826B05F0(0)`.
- AS-VM roots [GOLD from golden]: pool `DAT_829a0aa8` (string alloc `sub_8268D750`, free `Function_8268DA40`), `DAT_829a0ad8`, resolver roots `DAT_829a09e0` (vt `0x82087740`) / `DAT_829a0ab8` (vt `0x82086f78`). Golden menu objects: type-12 clips vt `0x82086FF0`, AS symbols vt `0x82086E10`. `loadMovie` resolve = `sub_826B3998→33C8→1830` (pure lookup).

### Apt RENDER path ★ (iter-297e agent map — menu-on-screen frontier) [DECOMP/GOLD-partial]
Full chain from frame to GPU (each hop verified in decompile; golden objects noted):
`FeMasterTick_821494C0` → `RenderFrameBegin_82787650(DAT_829A93C8 pair[0])` → **`sub_821808F8(dt, DAT_8296D86C)`** (AUI render dispatch, static call, RUNS every frame — but GATES on `*(mod+0x1310)!=0 && (*(mod+0x1318)==0 || state@+0x1320 ∈{6,8})`) → `Function_8217CB78` (render client, vt 0x82005A00, 30Hz throttle @+0xc; golden wrapper 0x69BD10) → `sub_821859A8` (screen-client composite, vt 0x82005A20 slot4; golden client 0x4838C0) → `sub_82179230` (kicker: exec = TLS `*(resMgr+0xd4)`, stamps exec+0x20, dedupes +0x28) → **`sub_826D1D60` = AptExecutor::Render (vt 0x820886F8 slot4)** → `sub_826A0BE8(*(exec+0xE0), …)` (publishes ctx → **DAT_829A17C0** render mirror + TLS) → `sub_8269F118` frame render pass (gate `*(ctx+0x18)!=0`; frame ctrs **DAT_829A1770/1774**) → `Function_82691A68(*(ctx+0x2c), frame)` root display SNAPSHOT (node {+0x18 flags, 0x2000000=skip; +0x20 stamp ≤ frame; +0x24 refcnt}) → **`sub_82690360` display-tree walk** (sibling +0x2c, child +0x30, visible bit31 of +0x18; render ctx **DAT_829A14B4**; matrix stack DAT_829A158C/1598) → per-char vtable **slot3 (+0xC) Render**: text vt 0x82086DC0→`sub_826AD410`→glyph `sub_826ACD18`→cb **0x829A1AD0=`sub_826D1648`** (writes draw params resMgr+0x54..0x60 then `importObj->vt[+0x20](obj, resMgr+0x54)`); shape vt 0x820870D4→`sub_8268CA10`→cb **0x829A1AB0=`sub_826D17A0`**→`geomObj->vt[+0xc](resMgr+0x54)`. Those game-side vt calls append to the ring the FE tick bracketed = the "queueSize" feed. Consumer (separate dispatch): `sub_821491A0` → `sub_82787520` ready-poll → **`DAT_829A9384` renderer/device obj (golden 0x02370058; vt +4 begin/+8 end/+0xc draw)** replays the buffer → `Function_827876A8` release.
- **Walls on our boot (in order)**: (1) `*(DAT_8296D86C+0x1310)` render-client wrapper NULL → sub_821808F8 no-ops — **SOLVED iter-297g** (SR_SET_IFACE_STATE, see FE INTERFACE STATE MACHINE below); (2) renderer `DAT_829A9384` never exists — **SOLVED iter-297f** (SR_DRIVE_FMRENDER); (3) import provider `*(*(exec+4)+0xc)` NULL → sub_826D1590 returns 0 → text/import chars silently skip; (4) root-snapshot stamp/flag state unverified. **Current wall (census): comp→kick — iter-297h CORRECTION: NOT a TLS wall.** `sub_821859A8` (composite, vt 0x82005A20 slot4) never calls the kicker: it reads the ACTIVE SCREEN ID at `client+0x130` (golden = 1), looks it up in the SCREEN REGISTRY `DAT_8296DD0C` via `Function_821C23E0(id)` (result cached `+0x134`; golden screen obj 0x719ED0, vt 0x82009798), and renders that screen; the kicker `sub_82179230` is reached from the per-screen render `sub_821855B8` (vt ~0x82005DC8 slot4). NULL lookup ⇒ bail: **no screen object = no kick** (see SCREEN SYSTEM below).

### FE INTERFACE STATE MACHINE ★ (iter-297g — the render-client selection root cause) [GOLD]
The AUI module (`DAT_8296D86C`) is a composite; screens render through whichever embedded
client the state machine selects. **`SetInterfaceState_sub_821745D0(mod, stateId, mode)`** [GOLD]:
state→client table **0=+0x220 base, 1=+0x7F0, 2=+0xD60, 3=+0xEA0, 4=+0xF50 (Apt render
client vt 0x82005A00 — THE GOLDEN MENU-IDLE STATE), 5=+0xF70, 6=+0x1060, 7=+0x1150 (the retail debug camera, vt 0x820059A4: iter-464 DT-2),
8=+0x1220 transition, 9=NULL (movie)**; writes `+0x1310` selector AND `+0x1320` state
together, then `client->vt[0x14]` activation (**skipped when mode==3**). Guard: `*(mod+0x10)`
(owner mode) must be 0 or ==mode, else request pends at `+0x1328` (`+0x132c` time; 10 = none).
Golden menu: state=4, pending=10, mode=0, hide `+0x1318`=0, page-flip ctr `+0x1314`=0x45.
- `RequestInterfaceState_sub_8218CD68(f1=dur, mod, target, …, mode)`: states {0,1,2,6,7} go
  via the TRANSITION client (`+0x1310=mod+0x1220`, state 8; target applied on completion by
  `TransitionComplete_Function_821800E0` → SetInterfaceState(target, 3) + fires screen
  hidden/shown AS events via `EB80->vt[0x1c]`); {3,4,5,9} set directly. Pending applied by
  `ApplyPendingInterfaceState_FUN_8218cf80(mod, mode)`.
- **No native caller passes state 4 as a literal** — it arrives via AS FlowMgr flow values
  (saved/restored: `_DAT_8291349c`, movie-restore slots). Harness drives
  SetInterfaceState(mod, 4, 0|3) once when the machine idles at state 0
  (`SR_SET_IFACE_STATE`, kernel/splash_hooks.cpp): the state HOLDS across the run — the
  per-frame "reverts" were never a re-writer, they were the never-written state.
- Render client TICK `Function_8217CB78(f1=dt, r4=client)`: `client+0xC` countdown −= dt;
  alive → composite `*(client+0x14)->vt[0x10]` (= `sub_821859A8`); expired →
  `ClientSelectModePage_sub_8216DA60(client, (page+1)%pageCount(+4))` RESELECTS
  `client+0x14 = *(*(client+0x10) + page*0x14 + 0x10)` (entry+0xC = page duration →
  countdown). NULL mode-table ⇒ the reselect destroys the wiring — harness parks the
  countdown at ~1e9 s (band-aid; natural retirement = whatever builds the page table).
  Activation derefs `*(0x8296DD08)` (Function_821CC2E0) — unset ⇒ crash ⇒ use mode 3.
  Golden client 0x69BD10: {+4=4 pages, +8=1, +0xC≈7.9s, +0x10=0x05B36A00 table,
  +0x14=0x4838C0 screen client, +0x18=mod+0x90}.

### SCREEN SYSTEM (FMGameEntity) ★ (iter-297h) [GOLD/DECOMP]
- **`sub_8214FF48(moduleObj)` = FMGameEntity FM init — builds the whole screen system**:
  registry map `DAT_8296DD0C` (container via `sub_821C2578(0x800)`), screen-list managers
  `DAT_8296DD30/DD2C` (`sub_821D0A00`), list `DAT_8296DD08` (golden 0x6A0CB8 — also the
  activation dep of ClientSelectModePage), pool wrapper `DAT_8296DD10`, ~12 pool globals
  `DAT_8296DCxx/DFxx`, + sub-inits (sub_821E99E0/821F9C80/821A5170/821C8BB8/822269A0).
  Descriptor `0x82002D40`: `{8271F428, 8214B298 CUSTOM middle, 82196960, 8214FF48 init,
  8214B2A0 shutdown, "FMGameEntity"}` — the custom middle slot makes it **INVISIBLE to the
  fm_module_walker triplet scan** (`{8271F428, 8231C140, 82196960}`), so it never ran on any
  boot. Driven once with a zeroed scratch moduleObj (init only touches `moduleObj+0x48`):
  `SR_DRIVE_FMENTITY`, kernel/splash_hooks.cpp `[SCREEN_SYS]` — all globals build, rc=0,
  proven K-batch 2026-07-04. **Check other walker-missing modules for custom middle slots.**
- Screen class vt **0x82009798**: ctor `Function_821D0D68` (no static callers — factory via
  dispatch), dtor `sub_821CA598` (removes from DD08 list + DD0C map). Registry insert =
  `sub_821CCB30(screen)` keyed `screen+0x58` (`+0x54` also captured) — no static callers;
  sits in a native-method descriptor table entry at `0x820C3088` `{fnPtr, 0x40003403}`
  (low metadata byte likely argc; the table's dispatcher not yet located — indirect).
- **Remaining trigger: screen CREATION comes from the AS FlowMgr flow** (gSM.ChangeScreen*),
  which needs `_global.FlowMgr` to resolve. iter-297h log proof (run2): the BistroFW
  bootstrap burst lands (gFM/gIL/gSM/gCM `[SET_WATCH]`), the FE command retries fire AFTER
  it (`SR_FE_CMD_RETRY`, every 4th frame to fr=28) — and `_global.FlowMgr` STILL never
  defines. **gFM (instance global) ≠ `_global.FlowMgr`** (what the native
  ChangeInterfaceState wrapper resolves, name @0x8201AECC). See AS GLOBALS MODEL below.

### AS GLOBALS MODEL ★ (iter-297i — golden-solved; the FlowMgr resolution target) [GOLD]
- **TWO "_global" roots.** `DAT_829A1584` = the _global OBJECT (vt 0x82087260; its cap-0x10
  map at golden 0x05085880 holds ONLY the native Adapter classes: MapAdapter, HudAdapter,
  SaveLoadAdapter, …). **The resolver's builtin "_global" (sub_826B1040 case 0x13) returns
  `DAT_829A1588`** = the SCRIPT-GLOBALS store (vt 0x82087224, golden 0x050939C4: cap 0x80,
  map 0x051546A0). **Golden `_global.FlowMgr` lives there**: slot `hash16(0x153D)&0x7F=61` →
  {key 0x05087A34 "FlowMgr", value **0x0509D298 = the FlowMgr CLASS-CTOR function object**
  (vt 0x82087164, type (flags&0x7F)=0x13 fn; its own cap-0x20 member map holds
  ChangeInterfaceState etc.)}. NOT a __Packages import (no "__Packages.FlowMgr" anywhere in
  golden); defined by plain AS (MAIN's (carve `0038_ICONMENU`) class-init/master-script world). A COPY also
  sits in a runtime movie's own variable store (movie vt 0x82087738 obj at golden
  0x0508EFF8, store+0x1310-style at +0x64: cap 0x20 map 0x05091E60 slot 29).
- **Property-store layout** (lookup `sub_8268C228(obj+8, &nameHolder)` → probe
  `sub_82684768`): store = {+0 capacity, +4 table, +8/+0xC specials (name-hash 0x6BBD/0x699)};
  table = open-addressing {keyStrNode, value} 8-byte pairs, slot = `hash16 & (cap-1)`,
  ±8/16-slot probe window. Key string node = {u16 refcnt, u16 len, u32(hash16@+6), chars@+8}
  (hash cached lazily by FUN_82681c40). Name-path invoke `sub_826B8DC0(method, out, path,
  argc, argv)`: resolves the DOTTED path via `sub_826B3998(vm, _level0=sub_826B05F0(0), 0,
  &holder, 1,1,0)` then the method on the result, then `sub_8268FB58` with an EMPTY register
  frame.
- **iter-297i attempts (3, stopped per discipline)**: (1) `SR_CALL_MAIN` — "main" is NEVER
  a property ([SET_WATCH] census; only initCafeFWSettings/initGlobalVariables/delayMainCall/
  INPUTCODE/sys/extern*/g* land); (2) direct framed `initGlobalVariables()` calls — resolve
  + run but dAS=1, no define (body bails; closure/arg poverty); (3) `SR_AS_AUTO_FRAME` —
  auto-open an 8-slot register frame (r0/r1=scope) at the interpreter choke point. Blanket
  version FROZE the AS loop (nested call-mode entries must not be framed); **kept narrowed
  to lr∈[0x826BE100,0x826BE8C0) (raw frame-item dispatch) — safe, fires on MAIN's 2 frame
  actions; most type-8 entries arrive with size≠0 (uninstrumented why)**.
- **LIVE PROBE (in [FE_CMD_LATE] line)**: our boot HAS the script-globals store with golden
  shape (`_globalB@1588` set, cap 0x80) and **slot 61 is OCCUPIED** (key ptr present) — yet
  the wrapper still reports FlowMgr unresolved. NEXT LEADS (in order): (a) dump slot-61's
  key STRING + walk the ±8 probe window (is it FlowMgr itself → resolution fails UPSTREAM;
  or a collision neighbor → FlowMgr truly absent); (b) verify the wrapper's root:
  `sub_826B05F0(0)` must return the REAL _level0 on our boot (path resolve dies at step 1
  if not); (c) instrument why type-8 entries at lr=0x826BE1B8 arrive with size≠0; (d)
  `sub_826B2558` = the 1588 store builder (WRITE xref) — check when/what populates it.
- Texture cbs: 0x829A1AB4/1ABC=sub_826D2028/sub_826D3DF8 (create/bind), 0x829A1AB8=Function_826D20A0 (release). Pick mode: DAT_829A177C bit 0x4 (point DAT_829A1788/178C). Ghidra desyncs at 0x8216Dxxx/0x8217Fxxx/0x826CB5xx (data-in-code) — recomp source authoritative.

### RENDER-NODE SNAPSHOT GRAPH & COMMIT MODEL ★ (iter-297k — the childless-snapshot root cause) [DECOMP]
The Apt renderer is two-sided: the LOGIC side owns live display-list objects; the RENDER
side walks an immutable, **versioned node graph** rebuilt incrementally by explicit commits.
- **Live object** (0x28 pool alloc, e.g. ClipCtorType12_826B0458): `+0x14` prev, `+0x18`
  next, `+0x1c` parent, `+0x20` **node-handle object**; handle `+4` = current node version,
  `handle+8 >> 26` = char type (5/9 = level container, child derived via `handle+0x1c`).
- **Render node**: `+0x14` depth (u16), `+0x18` flags (**bit31 visible**, 0x2000000 skip,
  0x40000000 hidden-in-list, `&0xfc0000==0x400000` no-recurse), `+0x20` frame stamp,
  `+0x24` refcnt (lwarx/stwcx), `+0x28` **version chain** (newer versions appended),
  `+0x2c` sibling, `+0x30` child. `FUN_826857c0(node, frame)` = version SELECT (newest with
  stamp ≤ frame, not skipped). `Function_8268AF90(handle)` = get WRITABLE version for the
  current producer frame `DAT_829A1770` (copy-on-write). NOT a builder — nothing here
  clones children automatically.
- **Link primitives**: `Function_82685B20(parentNode, childNode)` sets `+0x30`;
  `Function_82685950(prevNode, nextNode)` sets `+0x2c`; `sub_826919F0(rootSlot, obj)` sets
  the registry root (`Function_826C3AB0` create / `Function_826C39F8` repoint).
- **THE COMMIT**: `sub_82698370(*(ctx+0x2c), liveObj, DAT_829A1770)` recomputes ONE object's
  node links from its live prev/next/parent state (idempotent). Called automatically by the
  live-list INSERT `sub_8269AFD0` / `sub_8269B0B0(headSlot, depth, obj)` and UNLINK
  `sub_82696EF8` — **but always through the transient logic-ctx global `DAT_829A17BC`**.
  Parent child-ptr resync = `sub_8268CDE8(root, parentObj, frame)`.
- **Context publication**: `sub_826866F0(ctx)` = SetCurrentAptContext (spinlock 0x829A1A1C;
  sets `DAT_829A17BC` logic + `DAT_829A17C0` render). `sub_82686860()` resets both to the
  DEFAULT ctx `DAT_829A17B8`. Scoped wrappers (`sub_826BB6F0/750/7A8`) save/set/restore
  around every natural logic operation. `AptExecutor::Render sub_826D1D60` publishes
  `*(exec+0xE0)` via `sub_826A0BE8` → that ctx's `+0x2c` registry is what the frame pass
  walks.
- **HARD RULE corollary**: any host-driven insert/exec that can commit node links MUST
  publish 17BC first (naturally via `__imp__sub_826866F0`) — with 17BC=0 the commit writes
  through garbage and the snapshot stays childless while everything else looks green
  (walk runs, txt/shp never fire). Fix: `SR_NODE_COMMIT` (splash_hooks.cpp) publishes
  `exec+0xE0` + re-commits every insert ring-latched by the `sub_8269AFD0` watch
  (kernel/apt_render_watch.cpp `[LIVE_INSERT]`).
- **LAYER MASK (the "walk runs but nothing draws" wall)**: `AptExecutor::Render
  sub_826D1D60(exec, r4)` — **r4 is the LAYER MASK, not a frame delta** (the delta comes
  from the executor clock `sub_826D0E80`). Mask flows `sub_826A0BE8(ctx, delta, mask)` →
  frame pass → walk `param_6` (param_7=1 enables filtering, top level only — recursion
  passes 0). Top-level node renders ONLY if `(1 << (depth@+0x14 & 0x3f)) & mask` — depth
  0xFFFF (factory default) shifts by 63 → 0 → always skipped. Drive with r4=0xFFFFFFFF
  and give minted container nodes depth 0 (keep +0x16=0xFFFF: >0x7fff selects the
  direct-render branch; ≤0x7fff = clip-layer → `sub_8268B830` open/delegate).
  Node registry entries: `*(ctx+0x2c)` = holder → head entry {node,next}; root claims
  APPEND at tail (`Function_826C39F8`); walker `sub_826C3920` advances+frees the head
  past entries with stamp ≤ walk frame. Leaf draw: type-1 node vt 0x82086DF4 slot4 =
  `sub_826ADA70` → `sub_826ACD18(*(node+4))` (gated char type *def==1 = TEXT) →
  cb `*(0x829A1AD0)` = `sub_826D1648(importObj, …)` → NULL importObj bails, else
  `importObj->vt[+0x20](obj, DAT_829A23E0+0x54)` = the ring write. CB table
  0x829A1AB0..0x829A1ADC verified installed on our boot (== golden).
- **~~Recomp TU-binding gotcha~~ — RETRACTED, THE OPPOSITE IS TRUE [iter-371, verified]**:
  this previously read "same-TU direct calls bind LOCALLY, so a watch that stays 0 may be
  invisible rather than unreached". That is WRONG for this toolchain and it was being used
  to explain away real zeros. clang/lld-link COFF: `PPC_WEAK_FUNC` emits
  `IMAGE_SYM_UNDEFINED` + `StorageClass WeakExternal` + `AuxWeakExternal{Search: Alias}`,
  so EVERY call — same-TU or cross-TU — is an `IMAGE_REL_AMD64_REL32` relocation and the
  `PPC_FUNC` strong def always wins. Runtime control: `sub_8277B0A8` is a pure same-TU-only
  callee and its hook logs `[APTTEX]` 21× per run. **There are no watch blind spots: a
  counter that stays 0 means the function was NOT ENTERED.** Read zeros as evidence.

### FE-tick stall class (leaf-loop spins; sampler names them) [DECOMP, iter-296d]
The FE tick (`sub_821494C0`) stalls at DIFFERENT leaf loops per roll — each names
itself via `[THREAD_SAMPLER]` lr. Named so far (both guarded by mid-asm hooks in
`tools/regen/superman_regen.toml` → `kernel/midasm_hooks.cpp`):
- **`AllocDeferredFreeSweep_sub_82763628`** (lr frozen `0x82763630`): walks the
  deferred-free list at `heap(+0x10)+0x10`; skip path (flags `+5`) has no calls —
  a CYCLIC list spins forever. Hooks: entry reset @`0x82763644`, Brent cycle
  detector @`0x8276365C`, bail → natural exit `0x827636C0` (`SR_SWEEP_GUARD`).
- **`ScreenObjPickIterate_sub_8219F7A8`** (lr `0x8219F7B0`, ctr `0x826CB730` =
  tiny virtual getter `lfs f1,4(r3)`): iterates a live (id,obj) vector at
  `container+8/{begin,end}`, re-reading end each pass; null entries = call-free
  leaf → corrupt begin/end spins forever. 9 callers. Hook @`0x8219F7F0` (after
  count in r27), logs >10K, bails → exit `0x8219F968` at >2^20
  (`SR_PICKITER_GUARD`). NOTE: Ghidra desyncs mid-function (data-in-code) —
  trust the recomp output, not the listing.

### ⭐ ResourceScheduler drain contract + the world-preload SELF-DEADLOCK [VERIFIED iter-365f, 2026-07-24]
Scheduler obj `*0x829A2424`; queue A `+60/+64`, queue B `+72/+76`, CS `+8`
(deadline const `0x8208A4C0` = INFINITE), **single in-flight slot `+56`**,
in-flight tid `+96`. Contracts (IDA decompile, all three verified):
- **[iter-365m] The drain LOOP `ResSchedulerDrainLoop_sub_826F4700(sched, keepDraining)`**
  (IDA renamed): `loop { vt[+0x78] of FUN_826e5858-obj; serviceA sub_826F4230;
  serviceB sub_826F4378 } while (work && keepDraining)`. **lr=0x826F4740 = the
  return from serviceB** — the iter-365l "third stall variant" park site; the
  livelock is serviceB degenerating to the passive poll (below) when the slot
  is occupied / items can never complete. **`ctr=FFFFFFFF` in a sampler line at
  this park = the INFINITE deadline const riding in ctr** (not a loop counter).
- **[iter-365m] The FE tick is a JOB on one of the 4 `GameKernelLoop_Run_sub_82720B50`
  runners** (objects `0x896028C0/89602970/89602A20/89602AD0`; 60 Hz period field
  16,666,668 ns at +72; per-runner CS at +8 and own job list; idle-wait site
  lr=0x82720C30 via `IdleYieldOrTimedWait_sub_825F7168`). "The FE thread parked"
  always means "a runner executing the FE-tick job parked inside it".
- **[iter-365m] The intro-FMV bail-out collision (both iter-365l stall shapes):**
  the EA-logo XMV create fails instantly (hr=8000FFFF, demuxer wall) → FMV SM
  bails ~8 s in → lands mid-preload (console plays the movie ~60 s+ = the
  preload's natural window; race-inversion law clause 2: preload-duration vs
  intro-movie-duration). Shape B = the drain-park above. Shape C = the bail
  teardown FREES the conveyor iterator mid-walk (cursor +0x38 poisoned
  FFFFFFFF) and ERASES the scheduler (slot=0, both queues begin==end; NOT the
  sub_826F4090 reap — zero [RESSCHED_REAP]) → `loadMovie` never issued.
  `SR_FMV_HOLD=<sec>` (opt-in, splash_hooks.cpp) defers the outer SM tick
  (sub_8235C8D0) during phase 0 and eliminates the collision (K=3: 0 parks).
- **[iter-365m→SUPERSEDED by 365n] The +12 s "JOREL wall":** the JOREL -40
  storm / conveyor stop are DOWNSTREAM of the I/O-death law below (a different
  roll hung on `X2_CAR_SEDAN_CROSSBOW_PHYS` after rc=1 with no storm at all).
  Rejected: `BaseWorldFlow_PHYSICS/TEMP.AST` FILE_NOT_FOUND is a synthesized
  package-name file fallback absent from every AST — fails on console too,
  red herring.
- **[iter-365s] ⭐⭐⭐ THE PROVIDER-LESS DEADLOCK (the boot's root stall — proven
  live on the NATURAL baseline, object level):**
  Queue-A in-flight slot is **+52** (queue B's is +56 — the earlier "+56 only"
  note hid this). `ResSchedulerServiceNext_sub_826F4230` degenerates to
  `ResSchedulerPassivePoll_sub_826F37C0` (**executes nothing**) whenever +52 is
  occupied, so an occupied slot freezes ALL dispatch. Item class vtable
  **0x8208B908**; fields re-derived from the vt getter stubs @0x82704D28:
  `+0x14` phase, **`+0x18` NAME ptr**, `+0x38` cursor, **`+0x40` provider**,
  `+0x44` owner, `+0x4C` started, `+0x4D` resolved-ok, **`+0x4F` complete**
  (`vt[120]` is literally `lbz r3,0x4F(r3)`; **no vtable setter exists** — the
  load path writes it). Exec = scheduler `vt[40]` =
  `ResSchedulerExecItem_sub_826F3BA8` → phase/mode →
  `AccessSchedulerProcessLoadName_sub_826F2EA0`, which gates on
  `provider->vt[24]`.
  **The stall:** a sync acquire for a name in NO registered catalog (the rc=-40
  `NAME_LOOKUP` storm) builds a **provider-less item** (`+0x40 = 0`,
  cursor `= FFFFFFFF`), installs it in the single slot, then blocks its own
  thread in `ResSchedulerDrainLoop_sub_826F4700`. The request can only be
  satisfied by a catalog registration; the only agent that drives stream
  loads/registration is that blocked thread; and its own request pins the slot
  so nothing else can dispatch. **Self-deadlock.** Live proof
  (`SR_RESTASK_ALL=1`, natural no-env run): exec #968 `'FLAGCLOTHTEST'`
  `prov=00000000`, while all 967 prior execs carry real providers.
  **No rescue pump exists:** the drain's per-iteration
  `sub_826E5858()->vt[0x78]` = `sub_826F7840` = a single `blr`, and the golden
  dump shows console uses the **same** class (`dword_829A2430` → vtable
  0x8208AE30) ⇒ the root is TIMING (race-inversion), not a missing platform
  pump. **Counterfactual:** `SR_CONVEYOR_PACE=50000` ⇒ **0/983** provider-less
  execs, slot never pinned, boot reaches deep Apt (defExec=666, `loadMovie`,
  `RTAM_COMPLETE`×9). N=1/side: mechanism proven, rate needs K-batches.
- **[iter-365q] ⭐ THE NT HEAP (verified via IDA + golden dump; settles a false alarm):**
  `sub_8246CA80` = **RtlAllocateHeap** — `a1` = heap object (from `sub_8246DF08`
  = `dword_829AC16C`), `FreeLists[]` at **heap+384** (LIST_ENTRY pairs, index =
  size>>4), small-bin bitmap at `heap+4*((idx>>5)+88)`, lock at `heap+1408`,
  `sub_8279F290` = memset (HEAP_ZERO_MEMORY + new-segment header), big-block
  fallback via `NtAllocateVirtualMemory`. Entry chain: `sub_82467F40`
  (central alloc entry, bit31-clear path) → `sub_82469B48` → here; **`lr =
  0x82469B74` is merely the return address INSIDE RtlAllocateHeap.**
  **The heap control block is STATICS-RESIDENT BY DESIGN** (~`0x8296A2D0`,
  free lists at `0x8296A44C/0x8296A450`…) — golden dump at the menu holds
  `0x064625E8` in `0x8296A450`, and that block links back to `0x8296A444`.
  ⇒ **Low-region (`0x05/0x06xxxxxx`) pointers in those slots are NORMAL free
  list entries, NOT corruption**; self-pointing slots simply mean an empty
  list. ❌ REJECTED (iter-365p, refuted next day): "XMV block-cache memcpy
  scribbles a poisoned dlmalloc bin at 0x8296A450". METHOD RULE: a watchpoint
  proves WHO wrote, never that the write was WRONG — golden-read the same
  address before calling anything corruption.
- **[iter-365n] ⭐ THE I/O-DEATH LAW (5/5 runs):** ALL file I/O stops at the
  REGIME-TRANSITION instant — the 4 `GameKernelLoop` runners spawn ([KLOOP]
  ENTER ×4) and the intro-XMV create attempt fires; the XMV's 3×128KB ring
  prefetch reads are the LAST NtReadFile of the entire run. Station states at
  freeze (live Frida walk): **FM file worker** `sub_826045C0` loop — pops
  intrusive job list obj+8/+12 (count +16) under CS +20, exec vt[+68], done-CS
  +156, done-condvar +200, idle-wait condvar +64 with deadline
  `dword_82074190` = **INFINITE**; elevator/seek-reorder `sub_82600EC0`
  (budget +300, window +296); live object queue EMPTY (producers dead, no lost
  wakeup). **Delivery worker** `sub_826F92E0` — pops vector +180/+184 under CS
  +92, item vt[+4]/vt[+12]/vt[+8], **16 ms CondvarTimedWait poll**
  (unwedgeable); completion handlers `sub_82700910`/`sub_827009E8`
  (vt-dispatched) → deliver step `sub_82700820` (vt[+20] deliver). Scheduler
  queues empty; the conveyor item orphaned ALIVE mid-exec (cursor +0x38 not
  incremented). OPEN: which producer class dies at the transition — the
  boot-phase completion pump's duty never handed to the kernel loops, or the
  XMV create's abandoned 4th async read (event ring) poisoning the chain.
- **`sub_826F4378` (queue-B drain step):** lock CS → B empty? return 0. **Slot
  `+56` occupied → passive-poll ONLY** (nothing else in this scheduler can
  dispatch until the slot clears — the guest-level head-of-line block). Else
  `item = sub_826F4090(this,&queueB)`; dispatch iff `item->vt[+120]()` (ready)
  **OR `item->vt[+124]()` (owner tid) == current tid** — a thread may run its
  OWN not-ready items (why the FE thread adopts the whole world preload inside
  its drain-wait). Dispatch = install in `+56` (`sub_826F4D98`) → `vt[+28](1)`
  addref → unlock → `this->vt[+40](this,item)` execute.
- **`sub_826F4090` (queue scan):** NOT head-only — walks entries: per item QI
  `0xDA36A20D` → state `vt[+12]()`; state==1 → return item (dispatch it);
  state==0 → **SKIP, keep scanning**; other → reap (swap-with-last removal +
  destroy). So queue ORDER doesn't block; the block is the in-flight slot +
  item states.
- **`sub_826F37C0` (passive poll):** pure predicate — walks A then B calling
  `vt[+120]` isComplete; any incomplete → 1 (busy), all complete → 0. Executes
  nothing.
- **Sync-wait (`ResMgrAcquire_sub_826FAAC0`, iter-365e):** cb=0 requests wait
  FOREVER by design (bails only on terminal record state or cb->keepWaiting).
- **THE DEADLOCK — FINAL SHAPE (iter-365f/g, deterministic repro
  `SR_SCHED_FAST_POLL=1 SR_TIMER_RES_1MS=1`, dies ~+50s every roll):** the
  metropol preloadDefs walk (ON the FE thread inside the drain pump) hits a
  world-effect entry (FLAGCLOTHTEST-class) → the type-0 action handler
  `AptCreateObjectByResourceFourCC_sub_821F57C8` issues a cb=0 SYNC acquire →
  resolver -40 from EVERY catalog → the item is left in queue A **started=1 /
  complete=0, executed exactly ONCE, never re-executed** (live walk: in-flight
  slot empty, queue B empty, ONE orphaned item; ~16 lookups = one sweep) → FE
  waits forever by design. 19-min run proves NO self-heal; late registration
  alone could not unstick it (nothing re-runs the item).
- **Manifest ACTION dispatcher `sub_8222F670`** (per 'file' element): cases
  0 (named acquire, sync when entry async=0) / 1 ANIM / 3 GEOFX
  (`dword_829AAE48+52` vt[4]) / 4 SND / 5 WORLD_STREAM / 6 TILE / 8 SHADOW —
  **types 2 (VFX_XML) and 7 have NO case (dropped in the shipped binary)**;
  zero `*_vxd` requests in every log. Type ids
  (`ResourceTypeFromString_sub_821F5378`): 1 ANIM, 2 VFX_XML, 3 GEOFX_XML,
  4 SND_XML, 5 WORLD_STREAM, 6 TILE_XML, 7 ENVIRONMENT_XML, 8 SHADOWTYPE,
  9 BSI_DATA, 10 RAGDOLL_XML, 11 TEXARENA, 12 TEXARENA_XML.
- **Resolver `sub_826F8838`:** sweeps the catalog-mgr list (a1+8; the "mgrs"
  are per-AST CATALOG objects — one live-reads path "d:\DATA\objtiv1.AST");
  not-found → modes 0/2 hard -40, else adopt a mgr whose state vt[+32]()==2
  (catalog STILL LOADING) and wait. Name-hash `sub_82702FC0`: case-fold,
  `h = 435*(c^h)`, lookup seed 0x84240F25 (`NamedTable_GetValueByName_826E6B80`:
  mgr+528 ready else -13, table mgr+520, probe `sub_826FEEF8`).
- **Console oracle:** `golden.py find FLAGCLOTHTEST` = ONE hit (the parsed
  manifest table 0x064fb409) — no string copy elsewhere; catalogs key by hash
  so content must be compared via entry counts, not strings. `errorMissingFX`
  (same request class) completes gracefully every run → class doesn't decide;
  catalog states do.
- **⭐ CATALOG / STREAM MODEL [iter-365h, live-verified, SUPERSEDES the
  "truncated catalog" theory]:** world AST catalogs are TINY and CORRECT —
  they list subsystem STREAM packages (metropol.AST = 7:
  `METROPOLISWORLDFLOW_{RENDER,PHYSICS,ANIM,SOUND,SCRIPT,TEMP,OTHER}`,
  baseworl = 5; leaf-style ASTs list resources directly: ui.AST = 828,
  voice = 6358). Loading a stream creates a `***FROM BUFFER***` catalog with
  the stream's INNER entries — world-effect names (WATER_STREAMING_FIRE_HOSE,
  SP_FREEZE_BURST…) are stream-catalog entries. Which victim name kills a
  roll = which streams happened to load before the manifest's effect entry
  fired.
- **Catalog table format (reusable):** descriptor `tbl = mgr+520` (AST path
  string at tbl+0, or `***FROM BUFFER***`); mgr+528 state: 0 uninit (-13),
  2 loading (inserts allowed / resolver will WAIT on it), 1 ready. Packed
  entry table ptr at tbl+292: count@+0, entriesBase@+8, keyWidth@+25 (8),
  keyOff@+33, mode@+40 (0 linear / 1 bsearch via `sub_8279FAA8`, comparator
  `sub_82705D98`), stride@+44. Entry: flags byte (bit 0x10 = valid), then
  **LITTLE-ENDIAN key** (PC-authored TOC data; key lo32 = name-hash), fields,
  inline NAME string. **Name-hash: case-fold, `h = 435*(c^h)`, seed
  `0x84222325`** (FLAGCLOTHTEST = 0xE83D8FE1).
- **⇒ The standing wall (final form):** the manifest walk fires ASYNC stream
  loads (57C2D2E5, cb!=0) then sync-requests an effect whose stream is
  neither loaded nor state-2 loading → hard -40 → FE parks → **the park
  stops the async stream loads too** (zero I/O for 19 min — their
  record/translator pipeline is driven by the parked FeMasterTick, not by
  the wait's scheduler-only pump). Console completes stream loads on I/O
  workers independent of the FE wait; the arriving state-2 catalog satisfies
  the resolver's wait path. FIX TARGET: make async stream-load records
  progress during the FE sync-wait — instrument what advances a 57C2D2E5
  record from bytes-read to catalog-registered (state 2→1) and why that step
  cannot run while FeMasterTick is parked.

## 6. Global address quick table

| VA | what | conf |
|---|---|---|
| 0x8296EDAC | PSM instance ptr | GOLD |
| 0x8296EB80 | flow client g3 | GOLD |
| 0x8296EC4C | resMgr global (FE-tick pump) | GOLD |
| 0x8296D874 | AUI cfg (+144 gate, +152 movie name) | GOLD |
| 0x8296D8C8 | FMGameRtam ini (menu package loader arg) | GOLD |
| 0x8296D8E0/D8E4/D8DC/D8D8 | module mgrs | GOLD |
| 0x829A23E0 / 0x829A23E4 | Apt resMgr / resIO-allocator | GOLD |
| 0x829A17BC / 0x829A17B8 | published apt ctx (TRANSIENT) | GOLD |
| 0x829A19B0 / 0x829A19B8 | Apt screen stack count/base | GOLD |
| 0x829A1AC0 | import-resolver cb (= sub_826D1590) | GOLD |
| 0x829A0AA8 | Apt string pool | GOLD |
| 0x829A93C8 / 0x829A93CC | render-ctx pair / its allocator | GOLD |
| 0x829A2430 | ResStorageManager (packages, NOT movies) | GOLD |
| 0x82911460 | uiMgr | GOLD |
| 0x8295FC8C | FMUI module object | GOLD |
| 0x829a2bb0 / 0x829a2b98 | FM list heads (clear before ctor walk) | GOLD |
| 0x8296DD0C | entity registry (iter-464; NOT a screen registry: the correction under "FE boot screen", section 3a) | GOLD |
| 0x8296DC18 | streaming core (`RenderMgrSubmitGeometry_sub_8219DB18`'s object): +0x08 streaming focus = the active camera's position, +0x19C load-unit grid bounds 7009/-9104/4796/-7887 (600-unit cells, 27 x 22), SlidingPVS vt 0x82099E54 pads it by 6 cells (`tools/devtools/poshud.py`) | GOLD |
| 0x8296D86C | camera / view manager (iter-464; the "AUI module" of section 5, FE INTERFACE STATE MACHINE): +0x10 owner mode, +0x1150 debug camera, +0x1310 selected camera, +0x1320 state, +0x1328 / +0x132C pending state / transition time | GOLD |
| 0x8296D884 | input context manager: +0x1C slot count, +0x20 slot array; slot 1 +0x1C = controller 2's context (7 = DebugContext) | DECOMP |
| 0x8296DF2C | script timer manager: EASTL map at +0x04 (anchor; begin +0x08, count +0x14); node +0x10 owner, +0x14 hash, +0x18 flow-owned, +0x25 running (`sub_82209270` pauses) | GOLD |
| 0x8296DF5C | objective manager: +4 mini events enabled (`AreMiniEventsEnabled`) | GOLD |
| 0x8296DF50 | Metropolis health: +0x04 health, +0x08 max, +0x10 fraction, +0x2C damage enabled, +0x2D lock | GOLD |

## 6a. REMOTE-DEBUG CAPTURE SERVICE (port 9201) — THE .DATA-WIPE ROOT [GOLD, iter-348]

The game ships a developer remote-capture/input service. On retail hardware it
idles forever (no client → `accept()` = -1). **Any host NetDll stub that fakes
success turns it into a .data shredder** — this was THE year-long wipe root.

- `sub_8263CF70` — service ctor: socket (port `0x23F1` = 9201, magic
  `0xBC614E` = 12345678), allocs the shared tri-buffer (`obj+0x44`,
  ctor `sub_825F7C10`, vt `0x82074B8C`), spawns thread (entry `0x8263CEF8`).
- `sub_8263CCD0` — packet loop: accept via `Function_825677B0`
  (**checks `NetDll_accept != -1`** — a 0 return = phantom client!), recv via
  `sub_8263CBE8`/`sub_82567928` (recv<0 → close; ==0 → retry; >0 → nul-term
  text), strncmp-dispatched text commands; reset branch calls
  `resize(obj, 0, getters…)`.
- `sub_8263C750` — config parser: **atoi's THREE NUMERIC FIELDS out of the
  packet text and calls `sub_825FF9F8` (tri-buffer resize) with them —
  UNVALIDATED when mode==1.** Garbage text → pointer-sized "size" →
  `memset(buf, 0, ~2.2GB)` (buf2-memset call site `0x825FFB74`, lr
  `0x825FFB78`) → flattens `.data` (splash names 0x8291363C + FE service
  descriptors 0x8295C62C in one sweep).
- Tri-buffer copy-assign `sub_82604190(dst, src)` (consumer `sub_8263CC68`
  under deadline-CS `CsEnterWithDeadline_sub_825FC108`); resize
  `sub_825FF9F8(obj, mode, sz1, sz2, sz3)` — sizes at obj+0x14/18/1C, bufs at
  +0x20/24/28; allocator = DAT_8297AF08, REAL vtable 0x8209A448, vt[1] =
  `sub_821938E0` (5-pair create; writes ALL 5 sret slots, 0 on fail).
- **HARD RULE (platform):** NetDll accept/connect/recv/recvfrom/send/sendto
  must return SOCKET_ERROR (-1) with no client; accept paces like a blocking
  call. Generic-success (0) stubs = phantom debugger = wipe.
- Post-fix natural state (4/4): all four FE services served (SVCTICK reaches
  Sim 0x8295C690), FeMasterTick runs, FMV_SM ticks ×4 at state 0 (the next
  frontier: first movie name never queued).
- Descriptor "shift-by-8" events near the window are (likely) legit array
  compaction + the cells self-heal via periodic service-system rewrites —
  only the memset was durable.

## 6c. LUA SCRIPT VM [GOLD, iter-348; VERSION CORRECTED iter-466: **Lua 5.0.2**, see below]

> **iter-466 correction (map lane, IDA + all three golden dumps):** the VM is **Lua 5.0.2**, not 5.1:
> the string "Lua 5.0.2" at 0x8294343C, `lua_rawget(L, -10001)` = 5.0's LUA_GLOBALSINDEX, and the
> lua_State / global_State offsets match 5.0.2 exactly. Numbers are 32-bit floats. ONE shared state:
> script manager `*0x8296D8CC` (`LuaScriptContextCreate_sub_82186ED0`), `L = *(mgr+0x0C)`, `_G` TObject
> at `L+0x40` (tag 5) / `L+0x44`; Table `+0x07` lsizenode, `+0x10` node; Node 20 B (key +0, value +8,
> next +0x10); TString `+0x08` hash, `+0x0C` len, `+0x10` chars; stock luaS_hash. FSM state scripts run
> on that state with no own environment, so a script global lands in `_G`. One EA change: the number-key
> hash in `luaH_mainposition_sub_82767688`. Read-only reader: kernel/map_markers.cpp (docs/MAP.md 4.3).

> **GC threshold = game behaviour, confirmed on the oracle (step4/audit M2, 2026-10-18).** `AptVmAdvanceMainScript_sub_82176DF8` (0x82176E3C) calls `lua_setgcthreshold_sub_8275D600(L, 3 * (mgr+8) / 4)` with `mgr+8` = 563,200 BYTES (GameplayFsmCtor 0x82186D50: GameScriptStateSize KB << 10) while the API takes KB (`a2 << 10` unless > 0x3FFFFF), so `G+0x20` GCthreshold = 432,537,600; `G = *(L+0x10)`, `G+0x24` nblocks. All three Xenia golden dumps read the same value: mgr 0x00472860, L 0x00AB35C4, G 0x00AB361C, GCthreshold 432,537,600, nblocks 436,069 (nosound menu) / 436,108 (sound_menu) / 465,925 (sound_gameplay, 0bd090dbe) (`python tools/logic_audit/lua_gc_golden.py`). So Lua's automatic GC never fires on the console either. **The visible consequence** (lane K, M2 milestone; `_worktrees/postgame/archive/m2_claude_20261017/K/NOTES.md` "THE LUA ERRMEM IS GAME CODE" and "GAME BEHAVIOUR"; the witness log K/ally_logs/C1b_run.log `[DAS_WITNESS] POOL_ALLOC_NULL` / `LUA_REALLOC_NULL`; IDA comments 0x82176E3C, 0x8216486C): the script heap grows until its allocator refuses - LUA_ERRMEM on FRESH 24-byte allocations (block 0, lr 0x827609A4 in luaM_realloc) from the "lua-script" pool 0x00611090 (overflow path, ovf 32768, maxPooled 4096; backing 0x006109E4 vt 0x8209A448 = the "Gameplay::FSM" DogmaPoolAllocator) about 3.3-3.6 min into Metropolis sim time, on every path and both machines; at that point G+0x24 nblocks = 1,686,007 (57,039 blocks; C1b). In a normal session the scripts recover after a burst of 64-127 errors (R3, R4); on the C1b cold load the errors continue at ~30/s. No failed GROW (nothing is copied to guest 0). [the threshold: observed on the oracle; the refusal on Xenia: derived, to be watched once on live Xenia.] **Ruling (Fable 7, dev archive/step4_claude_20261018/FABLE_RULINGS.md):** original-game behaviour, kept; lane B confirms ONCE that the refusal also happens on Xenia at a comparable point (if ours reaches it where retail does not, that is a port divergence to fix); the 5b scripting docs must say that the automatic GC never fires.

> **The refusal is ours, not the console's (step4/audit F4, 2026-10-20; docs/LOGIC_AUDIT.md F4).** The game's only collect trigger is `AptVmInvokeAction_sub_82176EB8` (FmScript_RunString, called by the script resource loader `AptScriptLoadAndExecute_sub_82191AE8`): it calls `lua_setgcthreshold(L, 0)` before `luaL_loadbuffer` (a full collect) and is the only writer of the run counter `dword_8296DBF0`. Retail on Xenia stays flat at ~442 KB for 10 min of ch19 idle (x19_1..3), and its golden heap is code only (2722 strings, 1438 closures, 865 protos, 10 tables). Ours grew 5.9 KB/s because OUR composed costume-switcher state (kernel/mod_costume_data.cpp kCostumeSwitchLua, installed in place of SUPERMENACTORWORLDFLOWFSM_S_60265281 whenever port features are on) declared six helper closures inside main(mode), so it built them again on every 30 Hz 'process' tick. Fixed (helpers at chunk level; SR_COSTUME_SWITCH_LEGACY=1 = the old text). Rule for any Lua we or mods inject: a per-tick path must not build closures, tables or strings from changing values. Census tool: `tools/logic_audit/lua_heap.py` (dump / live / diff; metro_watch records nblocks and the run counter).

DOGMA's script VM is stock-shaped **Lua 5.1** (strings "C stack overflow"
@0x82943c00, "cannot resume dead coroutine"; LUAI_MAXCCALLS=200 guards):

| fn | identity |
|---|---|
| sub_82762E78 | `luaD_call` (depth guard ==200 → runerror; >224 → throw) |
| sub_82762B10 | `luaD_precall` |
| Function_82762DA8 | `luaD_poscall` (result move; **intra-TU — unhookable**) |
| sub_827657C8 | `luaV_execute` |
| Function_8276A178 | `luaG_runerror` chain |
| Function_827A4270 | throw = noreturn **fiber switch** (coroutines are fibers) |
| Function_828CF538 | `DogmaHeapFree` (bin/unlink free) |

`lua_State` (32-bit BE): +0x08 top · +0x0C base · +0x14 ci (CallInfo 0x18 B:
+0x18 func, +0x1C ci->top, +0x20 nresults/status, +0x24 savedpc) · +0x18
stack_last · +0x1C stack · +0x2E nCcalls (u16) · +0x50 errorJmp. TValue = 8 B.
No errorJmp → panic vt[+0x28] → exit(1). Lua internals call each other
intra-TU: strong-def hooks NEVER fire (use VEH-side capture or TOML mid-asm).

## 6b. GUEST CLOCK & TIMED-WAIT STACK [GOLD, iter-303]

**The game's entire sense of time flows through ONE read**: `FUN_824680c0` =
`*(u32*)(*(u32*)0x820007D0 + 0x10)` — the **KeTimeStampBundle DATA import**
(cell 0x820007D0, ordinal 0xAD; layout +0x00 u64 interruptTime 100ns, +0x08 u64
systemTime FILETIME, +0x10 u32 milliseconds-since-boot). The harness historically
left the raw import token in the cell → clock frozen → nothing time-gated ever
fired. Fixed iter-303: real bundle at guest 0xD0012000, 1ms host updater,
`SR_KE_TIMESTAMP` (default ON, `=0` restores the frozen clock for A/B).

Layered on that clock (all verified in Ghidra):
- `Function_825F7180` = ms + 10000 (biased "now"; the +10000 cancels in all
  deadline math — don't be fooled by it).
- **CS-enter helper `Function_825FC108(cs, pDeadline)`**: `*pDeadline==0xFFFFFFFF`
  → blocking `RtlEnterCriticalSection`; else TryEnter-poll with `sleep(1)` until
  `now+10000 >= *pDeadline` → returns **-2 WITHOUT incrementing the ownership
  counter at cs+0x20** (the underflow source — see kernel/cs_timeout_underflow_fix.cpp;
  our regen SPLICE also modifies this fn — check splice_bodies.json before reasoning
  from the static decompile). Deadline globals: 0x8208A864, 0x82074190, 0x8208CA48
  (all static 0xFFFFFFFF = blocking).
- **Condvar timed wait `sub_825FE8F8(cv, cs, pDeadline)`**: decrements cs+0x20;
  if it was <1 the WAIT IS SKIPPED entirely (returns -1) — a poisoned counter
  turns the caller into a hot loop. Real block = `sub_825FC1A0(cv+0xc, deadline)`
  → `sub_824692A8(handle, ms)` → `sub_8246A3D8` → **NtWaitForSingleObjectEx**
  (timeout LARGE_INTEGER = ms × −10000 built by `sub_8246DF18`). Signal side:
  `sub_825F6D38` → `Function_828A78F8` → **NtReleaseSemaphore**.
- **GameKernelLoop_Run_sub_82720B50** (thread body of all four FGGame service
  loops, started via `GameKernelMgr_StartLoopThread_sub_82720590`): per lap —
  vt[9] TickOnce `sub_82720C60`; under CS mgr+8, accumulate
  `Function_827203A0()` = `(FUN_824680c0()+1)×1e6` into mgr+0x68 and compare to
  period mgr+0x48; if due and vt[8] agrees → tick, else idle via
  `FUN_825f7168(*0x8208CA44)` — **config 0 = YIELD (hot), nonzero = timed wait**.
  Frozen clock ⇒ never due ⇒ four permanent yield-spin storms (the iter-303
  signature: TryEnter/Leave import storm, CS VAs alternating in pairs).
- **Vd frame pump `sub_82110A30`** (two threads park at lr=0x82110ACC):
  KeWaitForSingleObject(obj+8, 30ms loop); on timeout checks
  `*VdGlobalDevice+0x2A3A & 2` → ring-buffer work `sub_82111EC8`/`sub_82112050`.
  This is the render-side frame kick — feed it when chasing pixels.
- **Pending-factory retry `sub_8256B3C0`** (list 0x82979558, node {factoryFn,
  next, outSlot}): factories returning 0 are UNLINKED into a stack-local array
  (ptr r1+0x50, count r1+0x54) and retried in a HARD spin (no wait) — wedge
  signature lr=0x8256B4A4; `[FACTORY_WEDGE]` sampler dump names the stuck fns.

**iter-463 amendment - ONE 50 MHz guest clock (`SR_TIMEBASE_50MHZ`, DEFAULT ON since the
iter-463 A/B rolls; `=0` = legacy A/B; kernel/sr_timebase.cpp) [code-verified + unit-tested + rolled]:**
- `FUN_824680c0` is a registered SPLICE (not the static read above): since the
  iter-2xx "import cell unresolved" era it returned host `steady_clock` ms. Its
  premise has been stale since iter-303. Gate on: the ORIGINAL body runs
  (`lis/lwz 2000/lwz 16`) and reads bundle+0x10. So on a legacy run the
  GameKernelLoop pacing and the condvar deadlines (`sub_825FC1A0` calls it) are NOT
  on the bundle clock - they are on the splice's own steady epoch.
- mftb: XenonRecomp lowers every `mftb` to `__rdtsc()` = ~3.7 GHz raw TSC (74x the
  50 MHz timebase; low 32 bits wrap every 1.16 s vs 85.9 s). The XDK inlines
  QueryPerformanceCounter as mftb (`sub_824680D0`), so QPC/QPF math in the game
  (EAStdC Stopwatch, `sub_825667C8` ms clock, shader time param 0x29, LoadScreen
  sequencer, WMV decode budgets, XMA lock timeout, Apt variable dt) ran 74x fast.
  Gate on: guest/sr_mftb.h (force-included only into the 16 TUs containing
  `__rdtsc`) routes it to `sr_guest_mftb()` = host QPC scaled to 50 MHz
  (split form (d/den)*num + ((d%den)*num)/den; __int128 division does not link).
- Same domain in the kernel: bundle ms = ticks/50000 (saturating, Xenia),
  interrupt = ticks/5, system = FILETIME-at-origin + ticks/5, refreshed on a 1 ms
  high-resolution waitable timer; `KeQuerySystemTime` = the same and stored into
  bundle+8 (Xenia); `KeQueryPerformanceCounter` = mftb. `KeQueryPerformanceFrequency`
  = 50e6 in the FULL 64-bit r3 (both callers `std`/`rldicl` it) and no longer writes
  8 bytes at a guest r3 in [0x82000000,0x83000000) (ungated: that was a stray guest
  write; in `XmaHw_WaitIdleOrResetDecoder_82119C28` r3 is the decoder object).
- Review (iter-463, against reference/xenia-canary-oracle): mftb/TB = guest ticks (Xenia
  `InstrEmit_mftb` -> LoadClock; this XEX has 497 `mftb` TBR 268 and NO mftbu/mfspr-TB,
  golden-dump opcode scan, so the missing `>>32` for TBU is moot here). Bundle ms and
  system time match `Clock::QueryGuestUptimeMillis` / `QueryGuestSystemTime`. ONE deliberate
  deviation: Xenia's `QueryGuestInterruptTime` is the HOST interrupt time (KUSER_SHARED, 100 ns
  since host boot, clock.cc); ours is ticks/5 since the guest clock origin. Same rate and
  unit, different origin; moot for this XEX: the only guest read of the bundle cell
  0x820007D0 is `sub_824680C0` (+0x10 ms), so bundle+0 is never read.
- Ticker granularity MEASURED: in a bare process sleep_for(1ms) = 15.6 ms mean (difftest
  B-info); IN-GAME the legacy ticker's `[KE_TS_PERIOD]` = 1.97 ms mean / 4.0 ms max (something
  in the process raises the timer resolution), the gated high-resolution timer 1.007 / 3.8 ms.
- The `[KE_TS_FOREIGN] cell 0x820007D0 REPOINTED to 00000000` / `[VD_CELL_WATCH] read
  00000000` lines seen 8x per run were FALSE: the ticker thread never set its thread-local
  guest base, so its detector reads returned 0. Fixed iter-463 (setGuestMemory on entry).
- Tests: `build/difftest.exe --timebase-test`.
- CONTRACT (rolled, archive/v1_fix2/ab2 + ab3, exe fix2_4ca5bc4093fc, 9 rolls): with the gate
  on, the LoadScreen sequencer (`TimedSequencerAdvanceRandomState_8230F580`, `[LOADSEQ_OBS]`)
  and the shader time param 0x29 (`[SHTIME_OBS]`) advance at 1.000x wall; legacy = 74.0x. The
  sequencer's 12000 ms item hold lasts 12.0 s wall (legacy ~170 ms, so the load-screen items
  flicked by 74x too fast). Sim is NOT on this clock: the integrator dt (`[COLL_PATH]`
  |out-in|/|v|) is 1/30 and the sim runs ~28.5 ticks/s in BOTH arms, flight speeds identical.
  Intro FMV frames match the Xenia FMV oracles (MAD 0, at the oracle present index or +2) in
  both arms. Movie open->EOM time follows PRESENT THROUGHPUT (6-16 presents/s under
  SR_PRESENT_DUMP_EVERY=1 and machine load), not the clock: the earlier "gate-on movies
  slower" signal was dump/load noise (without dense dumps the gate-on intro movies run
  5.5/6.2/5.9/9.2/4.9 s against ASF content 4.0-7.3 s; fmvlegal.AST ASF headers).
  0 crashes / 0 new freezes with the gate on; one gate-OFF roll died at the FE in the known
  heap-corruption class (`[CS_GARBAGE]` cs=00000004/00010418 caller 825FC788, then
  `[ICALL_FAIL]` null target at 82654A78).
  Still open: the `sub_82564E80` override and the legacy branch of the `sub_824680C0` splice
  serve only `=0` now; delete them with the `=0` path after a soak. Phase 2 (recompiler emits
  PPC_MFTB, drop the force-include) is a follow-up with no behavioural gain.

**Rule: before diagnosing any "never fires / spins forever" loop, check whether
it is clocked off `FUN_824680c0` and whether the relevant deadline/config global
is what golden says it is. Audit other kVariable data imports for leftover
sentinel tokens (only VdGlobalDevice and KeTimeStampBundle are fixed).**

## 6d. GPU / PM4 COMMAND STREAM [GOLD, iter-358h]

**Framing (three independent oracles agree):** the command stream is CONTIGUOUS
4-byte big-endian PM4 — type `hdr>>30`, type-3 opcode `hdr>>8 & 0xFF`, count
`(hdr>>16 & 0x3FFF)+1`, INDIRECT_BUFFER recursion on **0x3F and 0x37** with size
`dword2 & 0xFFFFF`. Oracles: Xenia `pm4_command_processor_implement.h`; the XEX's
own PM4 walker `sub_82111798` (Microsoft's PIX-monitor tracer — real framing, IB
recursion via vtable+0x18); live RING_DUMP (iter-358h). There is NO 8-byte slot
format anywhere — that was the deleted fake device's write format.

**`VdSwap` contract (Xenia `xboxkrnl_video.cc` VdSwap_entry, CONFIRMED live):**
r3 = pointer INTO the command stream at a **64-dword block the caller reserves
for VdSwap itself** to fill (console kernel writes a type-0
SHADER_CONSTANT_FETCH_00_0 packet with the front-buffer fetch constant from r4,
then swap/NOP packets). Frame N's commands live in `[r3_{N-1}+0x100, r3_N)`.
Our `stub_VdSwap` writes the block (fetch packet + type-2 NOP fill), so scans
anchored at a previous r3 parse cleanly across swap boundaries.
r4 = D3D9 texture-header fetch constant (6 dwords, front buffer); r5 = writeback;
r8 = ptr to front-buffer VA. **The front buffer at swap is named by the r4 fetch
constant** — live streams show NO EVENT_WRITE_EXT resolve (resolves=0), so the
swap fetch constant is the present-path source of truth.

**`VdInitializeRingBuffer` contract:** `(ptr=MmGetPhysicalAddress(ring), r4 =
log2_size)`; size = `1 << (log2_size + 3)` (Xenia `command_processor.cc:436`).
This game passes r4=0xD → **64 KB primary ring** (holds IB jumps, not bulk
commands). ⚠️ r6 is NOT a parameter — our stub's r6 fallback stored garbage
(`0x8AB3F0F0`, which happens to sit near the game's GPU fence/writeback page
`0x0AB3F07x` phys). The live scan no longer uses `g_ringBufBase` (cleanup filed).

**Live boot stream census (natural, zero-env):** VdSwap r3 walks a ~6.9 MB
region `[~0x8A43F000, 0x8AAD0100)` (from the `sub_82467F40` alloc family),
~224 KB/frame, wraps every ~31 frames. Per frame: 24 draws (DRAW_INDX_2 0x36),
~2.7-2.9K reg writes (type-0 + SET_CONSTANT), 1 VS + 1 PS via IM_LOAD_IMMEDIATE
(0x2B), 12 EVENT_WRITE_SHD fences, 28 WAIT_REG_MEM (0x3C). Boot VS = 27 dwords,
1 binding, stride 7 words: pos k_32_32_32_FLOAT + color k_32_32_32_32_FLOAT
(solid-color-quad pair). Unhandled live opcodes (triage vs Xenia): 0x21, 0x3B,
0x3C, 0x54, 0x60-0x63.

**THE engine:** `gfx/pm4_engine.{hpp,cpp}` — the golden-trace-proven parser
(replay parity re-verified byte-exact after the move: 63,054 packets / 3,501
draws / CLEAN-EOF). Both the replay and the live VdSwap scan run it. Never add
a second PM4 parser; extend this one and re-verify against the golden trace.

**Draw contract [iter-358i]:** `VGT_DRAW_INITIATOR` = prim_type bits [5:0],
**num_indices bits [31:16]** (Xenia registers.h:327 — a `& 0xFFFF` read here
cost every draw count for ~200 iterations). The engine persists the parsed
vfetch bindings at IM_LOAD (`lastVsBindings`) and emits draws with the real
fetch slot, real stride, and the position attr's offset/format
(`GuestDrawCommand.posOffsetBytes/.posFormat`).

**Bin predication [iter-358i]:** type-3 packets with header bit 0 set execute
only while `(binSelect & binMask) != 0` (both init ~0; set by 0x60-0x63
SET_BIN_MASK/SELECT LO/HI). The XEX's own walker checks the same bit. Live
boot stream: predSkip=0; golden menu trace: exactly 1 packet culled.

**THE FRONT BUFFER exists (live, every roll):** created by the FMV player at
~t+104s — `fbVA=0x8AB7F000`, 1280x720, pitch 5120 B, format k_8_8_8_8, **TILED**.
`stub_VdSwap` decodes the r4 fetch and registers it (preference: fetch > r8 fb
> test fallback). **UNTILE IMPLEMENTED [iter-358j]:** `gfx/xenos_tiling.hpp`
(Xenia `texture_address.h` port, numerically proven vs the crunch reference);
`presentFramebuffer(...,tiled)` untiles validated spans. The game's
front-buffer D3D9 texture header lives at GLOBAL **`0x8299873C`** (golden: D0=
8A000002 D1=BF130006 D2=0059E4FF; fb phys 0x1F130000 in both golden sessions).

**Draw execution [iter-358k]:** `DRAW_INDX` payload = `[viz_token, initiator,
VGT_DMA_BASE, VGT_DMA_SIZE]` (kDMA); index buffer = phys base aligned to index
size, u16/u32 via initiator bit 11, endian via `VGT_DMA_SIZE[31:30]` (k8in16
for u16). Prim types live: 8=kRectangleList (UI, `vte=0x300` screen-space),
6=strip / 4=list (`vte=0x43F` = full viewport transform, needs 1/W). Rect
expansion: v3 = −v0+v1+v2, ALL interpolators mirrored (Xenia GS model). The
renderer skeleton executes draws → offscreen RT → resolve → guest fb; the menu
IMAGE requires textures (UI quads sample textures; UVs not colors in the vfetch
census) — shader endgame = XenosRecomp (see memory project_render_pm4_engine).

**Verification limits of the offline sources [iter-358j]:** the golden TRACE
records only the FIRST GPU read of each fb page (a pre-resolve depth-class
pass; the presenter's read of the final image is deduped — Xenia's player
regenerates it by EXECUTING draws), and the golden DUMP's fb guest pages are
ZEROS (Xenia renders host-side; guest memory holds pixels only with readback
enabled). **No offline source contains the final color image — proving the
rendered menu requires executing draws.** The present chain itself (fetch
decode → untile → upload) is complete; FMV frames (CPU-written) will display
through it as soon as XMV decode works.

**FMV/XMV render + WMV bitreader contract [iter-363, live + Ghidra + L6]:**
- Natural per-frame chain: render runner `sub_8230C038` → `sub_82776038` →
  readiness/execute `sub_82776EB0` → sweep `sub_82776850` → pass executor
  `sub_8277B998` → pass 0x1C `fmvmovie` → `sub_822E5100` →
  `sub_8235F488` → decode/draw `sub_8235CA48`.
- `sub_824BF348` allocates the WMV bitreader as exactly 0x30 bytes; the allocation
  is reached through tail trampoline LR `0x824BF3DC`. Core field `core+0x4C`
  points to it. `sub_824C3E48(bitreader, reader)` zeroes the 0x30 bytes then
  stores the refill-reader callback object at the FINAL word, `bitreader+0x2C`.
- `sub_824C4250(bitreader, bytes, len, mode, ...)` initializes accumulator,
  bit count, cursor/end and mode, but deliberately does not rewrite `+0x2C`.
- On exhaustion, `sub_824C4428` consumes remaining bytes. If mode==1 it calls
  `sub_824C4178`, which invokes the reader via tail thunk `sub_827BAB80` and
  replaces cursor/end from the reader's `{buffer,length}` outputs.
- **Captured failure signature:** callback `+0x2C=0` → null indirect call leaves
  the stack outputs stale (`buffer=0x18,length=-24`) → `cur=0x18`,
  `end=0xFFFFFFFF` → the next unsigned `sub_824C4428` loop scans almost 4 GiB.
  A worker sampled at `lr=0x828F8D40` is at the return edge from this refill,
  not recursively calling itself and not waiting on a host lock/fence.
- **Allocator invariant:** every exact-tail payload field needs 0x10 bytes of
  back-boundary room under the current platform compensation. The 0x30
  bitreader was outside the WMA caller-range rule; `SR_FMV_BITREADER_ALLOC_FIX`
  reserves 0x40 only for `{lr=0x824BF3DC,size=0x30}`. `=0` is the A/B.
- **Codegen verdict:** `sub_824C4428` L6 = 2/2 pass (finite refill + empty
  non-callback exit). Do not re-investigate its unsigned loop without first
  validating the bitreader cursor/end and reader tail.

**Shaders [iter-359 spike, recon wf_801228d7]:** every shader funnels through
**⚠ CORRECTED iter-405 — the BIND functions were named backwards.** iter-397q
recorded `sub_820F5218` = SetPixelShader / `sub_820F4E88` = SetVertexShader.
Measured, 8/8 on each side, two independent signals agreeing: the container
magic's stage bit AND the container offset the object carries.
| entry point | container magic | bit0 | offset | => stage |
|---|---|---|---|---|
| `sub_820F5218` | `102A1101` | 1 | `+0x368` (VS slot) | **SetVertexShader** (device slot `dev+12420`) |
| `sub_820F4E88` | `102A1100` | 0 | `+0x28` (PS slot) | **SetPixelShader** (device slot `dev+12416`) |
Anything reading "SetPS" off a `sub_820F5218` hook is measuring VERTEX binds —
which invalidated iter-404's "~1800 pixel binds during the movie" (they were
vertex binds). PM4-level shader measurements are unaffected: the IM_LOAD packet
carries its own stage.

**`sub_820F5148` (D3DDevice_CreateVertexShader)** / **`sub_820F4D90`
(D3DDevice_CreatePixelShader)**; r3 = the D3D9 compiled-shader CONTAINER
(36B BE header: +0 magic `0x102A11xx`, bit0 1=VS 0=PS; +4 virtualSize;
+8 physicalSize; +0x10 ctabOffset; +0x18 shaderOffset → {physOfs,size};
raw ucode at base+virt+physOfs). VS object: +0x20 ucode VA, +0x368 container
copy; PS: +0x18 / +0x28; GetFunction pair `sub_820F4CE8`/`sub_820F4C90`.
Containers in the golden dump: ~400 unique-by-ucode (bundle phys `0x40000000`
w/ full D3DX ctabs vs_3_0/ps_3_0; heap objects at `0xB7/D7/F7`-alias VAs are
COMPLETE containers; 27 XEX built-ins have ctO=0). The golden trace holds RAW
UCODE only (110 uniques: 54 VS + 56 PS from 775 IM_LOADs). **KEY: the D3D
runtime vfetch-PATCHES every VS at bind time** (fetch slot/offset/stride
operands per vertex decl, 3-dword groups; PS never patched) — 56/56 PS match
containers byte-exact, 0/54 VS do; a runtime shader cache MUST key on the
PATCHED IM_LOAD bytes. Pipeline (offline, replay-testable):
`tools/extract_shader_containers.py` carves + synthesizes (template container
+ patched trace ucode) → `data/shaders_carved` + manifest (sha1(ucode)→file)
→ patched XenosRecomp (recipe+patch: `tools/xenosrecomp/`) → HLSL/DXIL cache.
Status: 109/110 containers, 95/109 DXIL-viable; residue = Normal0
interpolator semantics, empty-swizzle vfetch emission, r247/r250 reg decode,
one 60B XEX built-in VS (no container). Runtime contract to implement
(UnleashedRecomp model): root CBVs b0/b1/b2 space4 (256/224-float4 + shared),
our extra b3/b4 = SAME VA as b0/b1 (dynamic-index alias), bindless heaps
t0 spaces 0-2 + s0 space3.

**The IM_LOAD address and the immediate copy read PHYSICAL aliases [iter-460, IDA-verified].**
- `GpuEmitDrawState_sub_82108668` writes IM_LOAD's address as
  `((va >> 20) + 0x200) & 0x1000) + (va & 0x1FFFFFFF)` with `va = ucodeVA (obj+0x20) + physOfs`
  (the +0x1000 only for 0xE-window VAs).
- `GpuLoadShaderImmediate_sub_82108300` (IM_LOAD_IMMEDIATE, taken while the GPU still uses the
  ucode) copies the ucode with `memcpy_sub_8279EC60(ring, phys - 0x40000000, size)`: the source is
  the **0xC0000000 physical alias**. On the console every physical window holds the same bytes; the
  harness folds 0xC onto 0x80000000|p (aliasGuestVA), so for ucode the physical allocator placed at
  0xA/0xB VAs the copy read other memory: ~66K all-zero immediate loads per boot (`[PM4_REJECT]
  addr=0x00000000`), each leaving the previous vertex shader bound. Fix (b0a88dd, `SR_ALIAS_OWNER`
  default ON): kernel/phys_window.cpp foldPhysAlias - pages only the physical allocator owns fold to
  0xA0000000|p; pages both sides claim take the GPU reads' rule (0x8 unless its word is zero).
- Every created VS's ucode copy matches its container at creation (`[SHCREATE_UCODE]` logs only
  mismatches; 0 under LIFO), so the zeros come from the alias read, not from shader creation.

**Data INSIDE a command buffer is read where the CP read the buffer [iter-460, trace-verified].**
- A resolve's rectangle is a 6-dword vertex fetch (`RB_COPY` draws, `draw_util.cc:953`) that the game
  writes INLINE in the ring indirect buffer right behind the resolve packet (Xenia trace: rect at
  `1F7858A8`, packets at `1F7858C0..`; ours: `0A31E3F8` / `0A31E420..`). The live CP parses the IB
  through one window (executeGuestIndirectBuffer, ownership) while the resolve rect was fetched by a
  per-address lookup (`physToHost`) that can answer the OTHER window on a page both allocators claim:
  in `archive/p2_mtr2/live_454107ED_2400.xtr` 19 of 26 rects decoded as `empty_rectangle` and the
  other 7 as garbage (e.g. `(0,0)-(8,8)`), so the scene resolve copied nothing and the composite
  sampled stale textures (the title's red / tan / black slabs). Fix: `physwin::noteCommandBuffer` +
  `commandBufferWindow` (`SR_CB_WINDOW`, default ON) - the last 256 command buffers' windows, consulted
  first by the CP's `physToHost`, `gpuPhysToHost` and the shared-memory page source. After it every
  rect in the live traces is `complete`, `(0,0)-(1280,720)` etc.
- **The 224 MB world pool straddles 0xA0000000.** It is one physical grant at VA 0x9D76xxxx-0xAB75xxxx:
  its part below 0xA0000000 is 0x8-window VAs (phys = va & 0x1FFFFFFF, i.e. 0x1D76xxxx-0x1FFFxxxx) and
  its part above is 0xA-window VAs (phys 0x0000xxxx-0x0B75xxxx). foldPhysAlias's per-page owner
  counters (`pageAdjustLocked`) chose ONE side from the grant's start VA and clamped the phys span at
  0x1FFFFFFF, so the pool's whole upper part was never counted on the 0xA side: the game's
  write-combined stores (0xC alias) of an index buffer at phys 0x0A70D8A0 folded to 0x8 while
  `gpuWindow` (a VA range check) sent the renderer to 0xA - 591 zero indices for the title sky strip
  (VS 96C0048F, draw 2075 of `archive/p2_cbw1/live_454107ED_2400.xtr`; Xenia's own trace has the same
  strip at 1B68D8A0 with real indices). Fix: the counters split a grant at 0xA0000000 and count each
  part on its own side at its own phys.
- **The wrap itself is the trunk defect [iter-460, `[IB_RANGE]`-verified].** Because
  `MmFreePhysicalMemory` never reclaimed the 299 MB probe, the pool was placed after it and its
  physical range wrapped past the 512 MB end onto phys 0x0000xxxx-0x0B75xxxx = the virtual heap's
  physical addresses. Every page there is double-booked (`gpuGrantFlags`=5: heap grant at 0x8 +
  pool at 0xA), which the console cannot do. Measured with `pm4NoteIbRange` (an index buffer whose
  indices run past every vertex buffer the VS fetches): 73 such index-buffer reads per title roll,
  ALL on double-booked pages, the 0x8 (heap) bytes garbage (max index 65534), the 0xA (pool) bytes
  in range (max 63..663); Xenia's own title trace 0. Those reads drew the screen-sized tan / brown /
  gray slabs. With `SR_PHYS_LIFO_FREE` (probe reclaimed, pool at 0xB0008000, no wrap) the hits are 0
  and the title city renders clean (`archive/p2_lifo5`, `p2_lifo8`, `p2_lifo9`). Both LIFO and
  `SR_MEMCPY_GUARD_TOP_FIX` are default ON from iter-460; `pm4GuestWritableWin` makes the renderer's
  guest-memory WRITERS (shm readback, CPU resolve) and the scan-out use the owning window too (with
  grants starting at phys 0, an unconditional 0x8 write lands in the low virtual region).
- **LIFO moved the double-booking, it did not end it [iter-462, Frida + trace verified].** With the
  pool at VA 0xB0008000 (phys 0x10008000-0x1DFF8000), the heap's bump climbs past VA 0x90008000 by the
  title city, and every PHYSICAL request of 256 KB .. 64 MB that the `sub_82467F40` override serves from
  the heap (`noteHeapRoutedGrant`) aliases pool pages. The renderer's contested-page rule ("0x8 unless
  zero") then serves the heap object's bytes as the pool's texture. Witness: the title highway
  (t0 = k_DXT1 512x512 at phys 0x12CC8000, draws b2921-b2936 of `archive/v1_cap1/.../live_454107ED_2160`):
  `[TEX_PROBE]` owner=0, 0x8 head `0000002C 00000003`, 0xA head `94B183EE EBE355CA`; Frida dumps of both
  windows decoded as DXT1: 0xA = the concrete highway with lane markings, 0x8 = unrelated data
  (`scratchpad win_both_decoded.png`, iter-462). A census over the traces: ~20 corrupt textures in the
  title city, 14-19 in gameplay, 0 before LIFO (the triage workflow's `page_sources.txt`). Same class:
  the rooftop green checker, the confetti ground/meteor texels, the facade speckle.
  **Fix = the guest's own physical path + alias holes:** `SR_GPU_PHYS_ROUTE=1` (HR1) stops the override
  serving physical requests from the heap once the ring is claimed (the guest's `sub_82468008` ->
  `MmAllocatePhysicalMemoryEx` runs, one allocator, unique physical pages). The one heap-routed grant left
  is the ring (0x8A16B000 + 0xC0000, claimed before the route); `guestPhysReserveAlias` records its 0xA
  alias as a hole and the LIFO bump steps below it (`[PHYS_HOLE]`), otherwise a later physical grant lands
  on the ring's pages and the title dialog art reads ring bytes (`archive/v1_hr1a` vs `v1_hr1c`).
  Result on `archive/v1_hr1c`: the legal screen and the shield movie draw (black before), the title sky is
  lilac with clouds as in Xenia (flat tan before), the highway is concrete, the dialogs are clean.

**WHERE the vfetch patch happens [iter-458, IDA-verified] — and why our HLE
VS hash is UNPATCHED.**
- **The call chain.** The draw emitter `D3DDeviceEmitDrawVertices_sub_820FBBF8` (and the indexed twin) calls
  `GpuEmitDrawState_sub_82108668`, which calls one of two patch paths:
  1. `D3DVsPatchFetchesInPlaceIfIdle_sub_82108520`: if the VS's per-variant binding cache
     (`vs+416*variant+40`: decl id + stream strides) differs AND the GPU is done with the ucode, it patches the
     ucode **in place** via `ShaderAssembleVertexFetches_sub_82107A30(vs, ucode+physOfs, decl, dev+12256)`.
  2. `GpuLoadShaderImmediate_sub_82108300`, when the ucode is still busy: it copies the ucode into the ring and
     patches the COPY (IM_LOAD_IMMEDIATE).
- **Our side.** `publishDrawState` hashes the VS BEFORE `__imp__sub_820FBBF8` runs, and caches the hash per
  (obj, ucodePtr). The cache key and the `.vfetch` sidecar therefore describe the UNPATCHED container: every
  fetch reads slot 95, offset 0, stride 0, format 0.
- **Consequence.** OFFPAIR's premise ("offsets are exact") is false for those VS: every input decodes the
  POSITION bytes. Measured on `archive/iter458_hwin4.srht`, only 9 of our 33 draw VS appear in Xenia's 25023,
  against 50 of 53 PS.
- **THE PAIRING RULE** (`sub_82107A30`):
  - Each container vfetch-table entry carries (usage, usageIndex).
  - It takes the FIRST decl element with equal (usage, usageIndex), on any stream.
  - Patch: slot = 95-stream, offset = the element's byte offset, format = type & 0x3F, signed = type bit 8,
    integer = type bit 9, stride = `strides[stream]`.
  - Unmatched: slot 95, offset 0, **k_8_8_8_8 unsigned normalized** (dword1 `&0xBFC0CFFF|0x60000`), stride
    `strides[0]`.
- **Code:** `gfx/vfetch_pair_rule.hpp`, `SR_S6_PAIR_GUEST` (default OFF, experimental).
  `difftest --vfetch-pair-test` passes 21/0 and kills 7/7 mutants.
- **Replay results, offline:**
  - decoded invariants go from 10/86 to 76/86 unit-length normals/tangents, 0/6 to 6/6 integer bone
    indices, and 0/4 to 4/4 weights summing to 1;
  - the character becomes recognizable, but the world stays far from 25023 (Astra 049: **not** the dominant
    world defect);
  - the title backdrop changes, and neither side matches Xenia.
- **REJECTED, H-MS** (the stretched spikes = 5920's stream-1 POSITION1 dummy). With
  `SR_S6_PAIR_FOREIGN_POS0=1`, POSITION1 decodes to POSITION0; the activation witness is `[S6_PAIRDUMP]`. The
  frames are pixel-identical on 8/8 presents.

**NATIVE N0: the true PM4 packet count, the XDK brackets and the frame split [MEASURED iter-464, observe-only].**
The native plan's acceptance metric is "0 PM4 packets interpreted per frame" (docs/NATIVE_ROADMAP.md §1
item 1). `Pm4Engine::packets` counts `execute()` calls; the true count is `gfx/pm4_coverage` (gate
`SR_PM4_COVERAGE=1`): every packet the CP interprets, counted once at its decision point in
`Pm4Engine::execute` (type-0 by 256-register page with exact dword split, type-3 by opcode with body dwords,
predicated skips, type-1, type-2, zero dwords, truncated packets) plus the ring-level INDIRECT_BUFFER
descriptors the CP thread dispatches itself (`kernel/gpu_command_processor.cpp`). A frame ends at
PM4_XE_SWAP (0x64). Log: `[PM4_COV] F` per frame, `[PM4_COV] CUM` every 256 frames / 20 s (uncapped counter
VALUES: read the last line, never count lines); report tool `tools/pm4_coverage_report.py`.
- **Exactness check:** on every replay fixture the per-packet total equals the replay's own packet-record
  count (the Xenia trace stores one packet per record): menu 28,314 type-0 + 6,688 type-3 + 1 predicated
  skip + 28,051 type-2 = 63,054 = `[GPU:REPLAY] packets`.
- **Packets per frame on the Xenia fixtures** (`python tools/native_replay_check.py ... --gates-b
  SR_PM4_COVERAGE=1`, archive/v1_native/replay/n0_cov*):

  | Fixture | Packets | Type-0 (register dwords) | Type-3 (body dwords) | Type-2 | Draws (0x22 + 0x36) |
  |---|---|---|---|---|---|
  | menu PRESS START (Xenia) | 63,054 | 28,314 (1,723,766) | 6,688 (49,302) | 28,051 | 3,370 + 131 |
  | gameplay gp1 25023 (Xenia) | 69,238 | 30,906 (1,882,735) | 7,523 (57,858) | 30,807 | 3,749 + 131 |
  | gameplay gp2 28050 (Xenia) | 66,755 | 30,364 (1,705,565) | 7,782 (68,035) | 28,607 | 3,733 + 131 |
  | gameplay lg1 (our live trace) | 29,672 | 23,161 (1,333,664) | 6,509 (60,900) | 0 | 2,915 + 131 |
  | gameplay lg2 (our live trace) | 36,337 | 28,763 (1,824,446) | 7,572 (60,201) | 0 | 3,692 + 131 |
  | FMV eahd / t21 / t25 | 1,955 | 1,437 (8,213) | 417 (1,464) | 101 | 20 + 131 |
  | FMV t16 | 2,682 | 1,705 (8,482) | 676 (3,013) | 301 | 20 + 131 |

  Per gameplay frame the type-3 mix is ~3,700 DRAW_INDX, ~600-900 LOAD_ALU_CONSTANT, ~550-700 IM_LOAD,
  ~330-390 IM_LOAD_IMMEDIATE, ~330-400 INVALIDATE_STATE, ~700-840 SET_BIN_MASK_LO and ~670-780 _HI (the bin
  mask is rewritten around ~20% of draws, and 1-2 packets per frame are predicated away: bin-mask predication
is live in gameplay as on the menu; whether it is per-TILE replay is roadmap Q4), ~50
  WAIT_REG_MEM, 31 EVENT_WRITE, 20-28 EVENT_WRITE_SHD, 2 INTERRUPT, 1 XE_SWAP. Our own live traces carry no
  type-2 packets while Xenia's carry ~30K: the live trace writer (`gfx/pm4_live_trace`) records only
  type-0/1/3 (P-defect of the trace writer, not of the stream). 0x45 COND_WRITE x256 appears only in t16.
- **Brackets (`SR_NATIVE_BRACKETS=1`, `kernel/native_brackets.{hpp,cpp}` + generated
  `kernel/native_brackets.gen.cpp`):** observe shims on all 368 rows (213 entries, 146 table-only setters,
  9 device internals - `GpuCmdBufAllocate_820FCDF0` added by native/residue-census; 324 generated, 44 inside
  existing overrides via `SR_NATIVE_SCOPE`, the pump driver 82110840's in kernel/native_pump.cpp included since
  native/residue-census - the generator passes). The outermost
  call per thread opens a bracket keyed by (device from the VdGlobalDevice cell, thread); it records the
  command-buffer range it wrote from the cursor `dev+0x28` (limit `dev+0x2C`). **The cursor points at the
  LAST dword written** (stwu idiom: `GpuPresentEmitCommandBlock_82111EC8` writes its first dword at
  cursor+4; `GpuCmdBufBeginNewSegment_820FCF90` sets `dev+0x28 = segment - 4`, IDA 0x820FD0EC), so a piece
  covers `(before, after]` = host offsets `[before+4, after+4)` (iter-464 d9c125d). Pieces are split at
  `GpuCommandBufferFlush_sub_820FD8C0` and `GpuCmdBufBeginNewSegment_sub_820FCF90` (the only `dev+40`
  rewriters besides the monotonic emitters), and `GpuRingUpdateWritePointer_sub_820FD218` stamps the ring
  dwords `[dev+10820 before, after)` (mask `dev+13480`). Keys are host offsets (guest base relative), so
  the XDK writer and the CP reader agree whatever window/alias the VA uses. The replay has no XDK calls:
  every packet there is UNSTAMPED by construction.
  **On the segmentless transport (native/residue-census, 2026-10-14):** the CP runs a segment's guest PM4 from
  the close's host COPY (a kind-1 gap part); `cpSegment` / `bracketStep` hand the census the part's guest VA
  (`SegPart::va`) and the stamps the close took with the copy (`SegParts::stamps`), so gap packets are attributed
  like in-place ones (`pm4cov::setCopyWindow`). Every stamp also carries the INNERMOST table scope open when its
  bytes were written (`Stamp::inner`; a piece is cut at each nested scope's enter and leave): `[PM4_COVMAP] ...
  inner=<VA>` lines (the producer) beside the `owner=` lines (the outermost call). **Stamp lifetime:** BORN at a
  piece cut - only when the device was POSITIVELY the bracket's thread's at both of the piece's cuts (`dev+0x2A08`
  == KTHREAD+332, the SDK's own owner test 82468EF8, read fault-free: a bracket on a loader thread or the pump
  writes no command bytes and cuts none of the owner's cursor; the released device - `dev+0x2A08` = 0, D3DDevice_
  ReleaseThreadOwnership 820F3658 - and an unknown identity own nothing; a cut that finds another owner - an
  AcquireThreadOwnership 820F3618 there, which steals without waiting - drops the piece; and the OWNERSHIP EPOCH,
  moved at the enter and leave of every scope that writes `dev+0x2A08` (820F3618, 820F3658 and D3DDeviceCreate
  820F34C0 - its 82106010 / 82106280 are the only other guest writers; no host code writes it), must be unchanged
  between the two cuts, so an A -> B -> A change between cuts that both read A drops the piece too; a cut reads
  epoch, owner, cursor, epoch, owner, so a takeover between its owner read and its cursor read reads as not owned;
  a piece that would overlap another thread's stamp - only an owner change no scope sees could do that, none in the
  guest - is refused whole. So no bracket spans another owner's writes; `[NATIVE_BRACKETS] ... foreign= ownerLost=
  ownerShift= conflict=` count each case. With `SR_KPCR=0` no thread has an identity: no piece and no block is
  stamped, ring stamps remain) - and at a `820FCDF0` hand-out inside a
  bracket whose thread owns the device then: the block `[r3, r3 + 4 * r4)` is stamped with the allocating scope (owner = the
  bracket's entry, producer = the innermost scope below 820FCDF0, `[PM4_COVMAP] kind=block`), so the SDK Z-pass
  kick's blocks (82110C78 via 8210E458 under EndZPass / EndTiling) and the guest close's flush block are
  attributed; MOVED with a native close's copy (`takeIntervals`; the open piece restarts past the closed range and
  past a chained close's list node, `restartPiece`); ENDED when either XDK allocator hands the memory out again -
  `820FCF90` (the new segment `[base, dev+0x2C + 60)`, at its scope's leave) and `820FCDF0` (erased, then born
  again if inside a bracket); ring dwords taken on read. Both allocators return only memory the GPU has consumed,
  so no live stamp is erased; memory handed out outside any bracket reads UNSTAMPED, never a previous owner. A
  chained native close's 16-byte software-list node (820FD800 -> 820FCA80, the pump's to read) is left UNOWNED
  on purpose and marked: a CP packet read there counts in `[PM4_COV] CUM ... chain=N` (part of `unst=`), so the
  unstamped total reconciles. Ring stamps do not depend on device ownership: the pump driver 82110840's 820FD218
  ring writes (its N0 bracket on the pump thread) stay the driver's. INCOMPLETE ATTRIBUTION, kept visible: the
  pump's bin-select packets, which `82110140` writes to the ring directly (not through 820FD218; 8 of the 14 ring
  dwords of the suite's kick), carry no ring stamp and count as RING_UNSTAMPED, never anyone's. Offline proof:
  `tools/difftest/native_census_run.py` (`--native-census-test`; the pump through the production driver on a
  second guest thread with its own KPCR and identity, the released device, an unknown identity, an ownership steal
  inside an open bracket, A -> B -> A between two cuts via 820F3618's scope hook, a takeover inside a cut's reads,
  a raw owner write under another thread's stamp, a foreign allocation).
- **Segment flush continuation [verified IDA iter-467 flicker]:** `GpuCommandSegmentFlush_sub_820FD660` closes `[dev+13508, cursor+4)` (mode C: after `GpuBuildFenceWritePacket` appends the fence at cursor+4, the IB is `[segStart, fenceEnd)` via `GpuRingAppendJumpSegment`; mode A, byte dev+10808 & 0x80: `{mangledPhys, dwords}` appended to the table at dev+13160 only when `*(dev+13140+152) == 0` and the segment is non-empty; mode B, dev+12944: the scratch list dev+13008). Then the SAME buffer continues: the next segment starts at `v12 = (end + 31) & ~31` (`dev+13508 = v12`, `dev+40 = v12 - 4`) when `v12 <= dev+48`, else `GpuCmdBufBeginNewSegment_820FCF90` (`dev+40 = buf - 4`, limit `+44`, threshold `+48 = limit - 160`). The bytes between a segment's end and the next 32-byte boundary are never written or executed. No XDK path patches ring dwords in place after emission (the only candidate, the per-draw scratch slot list in `820FC000`'s tile-predication branch, byte dev+10808 & 0x02, is never taken in this game). `GpuCmdEmitRegisterRunChunked_82107058` writes what fits, sets dev+40, flushes, continues.
- **The shader bind `GpuEmitDrawState_sub_82108668(dev, d2)` [verified from the asm, iter-467 native-a P1 rows 1+2; host port = `kernel/native_flush.cpp Driver::bindState`, proven by the dry CHECK and the CPPM4 dense freeze]:** d2 is a 64-bit dirty mask (the decompiler shows 32-bit RORs; read the `extldi`/`sldi`): bit 49 = `GpuCmdUpdateSurfaceState_82107110` (pure device-field logic on dev+0x28B4 / 0x2D3C>>20 / 0x28C0 and the PS header bit 27; byte dev+0x2A39 bit 2 = a pending EVENT_WRITE 15, emitted after a kick check when 0x28C0 bit 1 ends set; returns d2 | 0x100); bit 52 = PS: `GpuRingEmitFetchConstants_82108470(dev, ps+0x28, ps[0x18])` then a kick check, `d2 |= 1<<46`, IM_LOAD `{phys(psVar[0] + ps[0x18]) & ~1 | 1, psVar[4] >> 2}` (psVar = ps + ps[0x40] + 0x28), dev+0x28A8 = psVar[0x18], and `d2 |= (1<<51)|8` with dev+0x28D4 low 3 bits := 4 (5 with no PS); bit 51 = VS: `82108470(dev, vs+0x368, vs[0x20])`, then either the pointer path (`D3DVsPatchFetchesInPlaceIfIdle_82108520(dev, vs, decl=dev+0x2D10, variant)` = 1: IM_LOAD of the variant; the second call with variant 1 when the 0x28D4 mode changed and vs flag 0x20: byte 0x2A3A |= 0x40 and a PREDICATED IM_LOAD of variant 1) or `GpuLoadShaderImmediate_82108300` (when the byte 0x2A3A bit 7 is set, or the VS/PS fetch signatures `(psVar[0x14] ^ rec[0x14]) & 0x7FFFF` (`rec[0x14] & 0x1F` without a PS) differ, or 82108520 returned 0, or dev byte 0x2A38 bit 7). The variant record is `rec = vs + vs[0x380 + 8*variant] + 0x368` {+0 ucode offset, +4 size bytes, +8 / +0xC the 0x2180 / 0x2181 values (ORed with psVar[8] / psVar[0xC]), +0x14 signature}; variant 1 is chosen with no PS when vs flag 0x20 is set. After the VS part (`d2 & 3<<51`): dev+0x28A4 = rec[0xC] | psVar[0xC], dev+0x28A0 = local | psVar[8] (the local = rec[8], updated by 821081C0 on the immediate path), `d2 |= 3<<47`, and with byte 0x2A3A bit 6 the predicated SET_CONSTANT values from variant 1's rec[8] / rec[0xC]. **Z-pass emission** (byte 0x2A38 & 0x40; its own 0x2208 mode in dev+0x3088, `d2 |= 8` when it changes) after ONE kick check: SET_BIN_MASK_LO 0x15555555, SET_BIN_MASK_HI 0, [d2 bit 3: type-0 0x2208 = dev+0x28D4, bit cleared], [SET_CONSTANT pred 0xC0012D01 {0x40208, mode}], [d2 bit 48: type-0 0x2180 x2 = dev+0x28A0/A4, `d2 &= ~(3<<47)`], [SET_CONSTANT pred 0xC0022D01 {0x40180, a, b}], [IM_LOAD pred variant 1, then with a plain variant too: SET_BIN_MASK_LO 0x2AAAAAAA + IM_LOAD pred of it] else [IM_LOAD of the variant], SET_BIN_MASK_LO dev+0x3090, SET_BIN_MASK_HI dev+0x3094. Outside the Z-pass: the plain IM_LOAD after a kick check. The -1 sentinels are compared with `cmpwi -1`. `82108300`: reserve `size/4 + 5` dwords against dev+0x2C through `D3DDeviceReserveCommandSpace_820FC910` (flush, then `820FCF90`; 0 = nothing emitted), INVALIDATE_STATE {0x100}, IM_LOAD_IMMEDIATE hdr `0xC0002B00 | ((n+1) << 16)` {0, (size & 0xFFFF) >> 2}, the ucode memcpy'd from `phys(ucodeVA) - 0x40000000` into the ring, dcbf over the source, [patch && (rec+8 byte & 7) != 7: `ShaderPatchVertexFetches_821081C0(rec, copy, &local, psVar)`], byte 0x2A3A bit 7 = patch, `ShaderAssembleVertexFetches_82107A30(vs, copy, decl, dev+0x2FE0)`, dev+0x2D20..2F = dev+0x2FE0..EF. `82108520`'s decision: the per-variant cache at `vs + 0x1A0*variant` {+0x28 decl id vs decl[0x30], +0x30/+0x38 strides vs dev+0x2FE0/E8 under the masks decl[0x20]/[0x28], +0x40 fence}: same -> 1; differs and `(dev+0x2A1C - fence) < (dev+0x2A1C - *dev+0x2A10)` (unsigned) -> 0 (busy); else patch in place, cache := current, 1. Every return-1 path writes cache+0x40 = dev+0x2A1C and dev+0x2D20..2F = dev+0x2FE0..EF.
- **Native-side contract (the S2a dedup mirror, kernel/native_const_dedup + native_flush::constRuns) [iter-467 flicker root cause]:** the mirror of "what the stream last wrote" may only be updated once the emission that carries those bytes is COMMITTED. A planned-but-aborted section (the segment-limit fallback to the guest leaf) that had already stored its groups made the leaf and the following draws skip groups nobody emitted: stale constants for one present (cars vanish, a black sky, the scene's lighting off). Store at commit; on abort the mirror must be untouched or invalidated.
- **The fence/ring waits are pure spins:** `GpuBlockOnFence_sub_820FD9A8`, `GpuRingWaitForSpace_sub_820FCB30`,
  `GpuRingWaitReadPtrWrap_sub_820FD150` and `GpuWaitTilesComplete_sub_821102B8` all loop on
  `GpuRingWaitProgress_sub_820F33E8` (no sleep, no kernel wait; it only calls the hang trap after 0x1388 ticks
  without progress). So a CPU blocked on the GPU shows up as XDK time on the waiting thread, never as an idle
  thread.
- **THE RING / FENCE / INTERRUPT CONTRACT [verified iter-467 P2 research: IDA asm/decompile, golden `sound_gameplay` / `sound_menu` (dev 0x40002880), Xenia source; full map in NATIVE_ROADMAP "P2 CONTRACTS (research)"].**
  - **The GPU write-back surface is two blocks.** `D3DDeviceInitRingBuffer_820FDE90` allocates both:
    - **Fence block** fb = `*(dev+0x2A10)` (96 bytes):
      - +0 completed fence;
      - +4 progress word `(gen & 3) | cursor`;
      - +8 the `VdSetSystemCommandBufferGpuIdentifierAddress` target, passed as VdSwap's r5 and never written;
      - +0x3C the CP read index in dwords (`VdEnableRingBufferRPtrWriteBack(phys(fb+0x3C), r4 = min(log2(ringBytes>>9), 19))`).
    - **Scratch block** pc = `*(dev+0x2A14)` = SCRATCH_ADDR, set by guest type-0 PM4 in the init block; SCRATCH_UMSK
      0x00020033:
      - +0 REG0, the pending CPU mask;
      - +4 REG1, the vblank gate;
      - +0x10 / +0x14 REG4 / REG5, callback and argument (0x0BADF00D when idle).

    Golden gameplay: fb = {0x10F8F3, 0xBFA8C403, 0, .., +0x3C 0x3380}; pc = {0, 1, 0, 0, 0x0BADF00D, 0x00010008}.
    Nothing else on the steady-state path is GPU-written. Exceptions: query slots, plus the perf / timestamp slots,
    which are off in this game.
  - **Fence tail** (`GpuBuildFenceWritePacket_820FD580`, every unpinned segment close): `C0003B00 7FFF`,
    `C0025800 {3, phys(fb+4)|2, progress}`, `C0025800 {3, phys(fb+0)|2, S}`, then `dev+0x2A1C += 2`. Fences are odd
    and step by 2. `dev+0x2A1C` is the open submission's id. Xenia writes EVENT_WRITE_SHD when the CP reaches it
    (translation order, not host-GPU completion; `pm4_command_processor_implement.h:884`).
  - **Pending test** (`GpuBlockOnFence_820FD9A8`, `D3DDeviceIsFencePending_820FDD90`, QueryGetData,
    `82108520`): `(S-F) < (S-fb0)`, unsigned. F == S flushes first, except while pinned (`dev+0x3290 != 0`, inside a
    Z-pass bracket), when GpuBlockOnFence returns at once.
  - **Binds stamp the OUTGOING resource:** `hdr+8 = dev+0x2A1C` at UNBIND time (SetStreamSource / SetIndices /
    the shader binds / SetTextureFetchConstant). A resource's last-use fence is the close after it was replaced.
  - **Each flush appends an 11-dword coherency IB before the segment** (`GpuBuildFlushMemoryPackets_820FD3B8`):
    COHER_STATUS_HOST 0x03000000, `COHER_SIZE/BASE` for the deferred CPU-write range `dev+0x2D18`, and
    `WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}`.
  - **The swap throttle is N-1.**
    - D3DDeviceSwap saves F(N) = S at 0x821126E0, then flushes.
    - Then `GpuBlockOnFence(dev, F(N-1) = dev+0x34D0, 3)` at 0x8211271C, and `dev+0x34D0 = F(N)`.
    - The copy of this block in `GpuCmdBufBeginNewSegment_820FCF90` is skipped: the behavior flags `dev+0x4FC0`
      are 0x04000001 (bit 0 set; golden).
    - Golden caught the render thread in this wait.
  - **The Z-pass bracket is RECORDED and replayed by the pump thread, not by the GPU.** This corrects the "mode B
    inactive" reading of the segment-flush bullet above: mode B is every gameplay Z-pass bracket.
    - `GpuReplayListBeginRecording_82110B30` pins `dev+0x3290/0x3294`; closes then append
      `{0x82000000|n, phys}` records with no fence tail.
      **Scratch allocation units rechecked 2026-10-12 [IDA assembly + allocator body]:**
      `820FC978` calls `820FCDF0(dev, 0x1080, 0x80)`. The size argument is in
      dwords, so the request is 16,896 bytes before any allocator minimum;
      the list descriptor nevertheless uses cursor `block+4` and limit
      `block+0x107C`: a 4,224-byte block extent with a 4,216-byte usable
      record span, `[block+4, block+0x107C)`. The IDA comment describing
      a 4,224-byte allocation is inaccurate. Begin's 63-dword header occupies
      252 bytes, and its returned r3 is the memcpy destination `header+12`.
      Exact bodies and assembly: `archive/v1_release_codex_20261012/bracket_sdk_contract/`.
      **Non-tiling Z-pass replay boundaries rechecked 2026-10-12 [IDA `82110460`]:** the complete
      software list is not replayed twice wholesale. Marker `0x84` starts the
      depth range; `0x85` clears its depth flag and selects `0xFFFFFFFF`.
      Any IBs between that first `0x85` and `0x86` execute once, with that
      neutral selection. The first `0x86` selects the colour pass and jumps
      back to `0x84`; the colour pass's `0x85` jumps directly to `0x86`,
      skipping the intermediate IBs. The second `0x86` returns to neutral
      selection, and subsequent list work executes once. Native ownership
      must preserve these boundaries and both passes' record lifetimes.
      **Generated-call ABI rechecked 2026-10-12:** the current
      `guest/ppc/ppc_recomp.2.cpp` body keeps the interpreter's cursor in a
      local `r31`; its `820FD218` call does not copy that local into
      `ctx.r31`. Reading `ctx.r31` inside the ringless override cannot
      recover the list-record identity. A native pump must hand over the
      actual record cursor/token explicitly, rather than infer it from
      matching physical addresses and dword counts.
      **Native CPU marker checkpoint, 2026-10-12:** `kernel/native_pump.cpp`
      implements the tested 80-8C control flow behind
      `SR_NATIVE_REPLAY_MARKERS=1` (default OFF). It preserves full-width
      return cursors and scopes the actual software-record cursor through
      `820FD218`, restoring an outer scope after nested publication. V4's
      actual SDK / strong-dispatch / direct-admission bank passes 134/0 with
      11 compiled defects, mutant gate-off controls and exact restoration.
      SDK PIX callbacks independently witness the scope inside publication.
      Caller-owned reachable-node and mapping/protection lifetimes remain a
      precondition; only initial inputs are validated for admission. This
      does not supply immutable generation binding or outer-driver sealing.
      Remaining helper dependencies, newest-physical deferred lookup and
      fixed pass expectations still prevent full queue / Native0 acceptance.
      Evidence: `archive/v1_release_codex_20261012/native_pump_v4_controls/`
      and `astra_native_pump_v4_review.md` (bounded Astra PASS).
      **Outer replay driver, 2026-10-12:** `SR_NATIVE_REPLAY_DRIVER=1`
      (default OFF) converts `82110840` CPU scheduling in the same module.
      It preserves the shared repeat counter, software lock, callback switch,
      marker rendezvous, terminal delegation and paused traversal resume.
      SDK `82110928-82110934` captures the callback address before unlocking;
      both native call sites do the same. The callback returns with `S+0`
      reacquired; the driver does not acquire it again on callback return.
      In the controlled two-participant schedule, the first finisher resets
      `S+18` before the marker rendezvous and the other repeats iteration one:
      actual SDK callback indices are `0,1,1`, not two callbacks total.
      This CPU conversion does not bind generations or prove final sealing.
      Tiled replay can enter directly in colour and masks the bin selection;
      this non-tiling sequence must not be applied unconditionally.
    - `D3DDeviceInvokeRenderPass_8210E458` -> `GpuReplayListEndRecordingKick_82110C78` appends the 23-dword kick
      block (`GpuEmitCpuCallbackPackets_820FDA80`, cb `GpuIsrKickReplayList_82110B00`) to the ring and sets
      `dev+0x2A74` = 1.
    - The ISR (CPU 2: 82110B00 picks the pump event by KPCR.cpu) wakes `VdInterruptPumpWait_82110A30`.
      `GpuReplayMarkerInterpreter_82110460` then writes the bracket IBs twice through `820FD218`, between
      `GpuRingEmitBinSelectLo_82110140` packets (`SET_BIN_SELECT_LO` 0x15555555 / 0xFFFFFFFF / 0x2AAAAAAA /
      0xFFFFFFFF, written straight into the ring). These are P0's 4 UNSTAMPED packets per frame.
      **Generation boundaries rechecked 2026-10-12:** `82111798` is the
      PM4/PIX tracer, not the software-list submitter. `82110B30` copies the
      five-word chain descriptor from `dev+32C8` to `dev+32DC` before
      initializing a fresh recording list. `82110C78` terminates the recording
      lists, then appends coherency and kick blocks through the copied old
      descriptor. `82110B00` delivers the kick root to `S+58`. A pending kick
      does not replace an existing `S+50` pause continuation. EndSync returning
      zero can mean paused or non-last participation; it cannot mean sealed.
      Last-participant unpaused terminal confirmation under the actual
      `S+3C` Kf lock is a seal candidate only: producer/queued-kick ownership
      must also exclude further extension/revisit. The exact final sealing
      transition is still unproven. IDA bodies: `archive/v1_release_codex_20261012/`
      `native_driver_{begin,kick,isr}_sdk_contract.txt`.
    - Golden ring: only IB descriptors (5182) and bin selects (419).
  - **Present pacing.**
    - `GpuPresentEmitVblankGate_82111EC8`: SCRATCH1 = 1; WAIT pc+4 == 1; callback `GpuPresentPacingUpdate_82111E38`
      with arg `(vblanks<<16)|pct`; WAIT pc+4 == 0.
    - The ISR's vblank branch (source 0, MMIO 0x7FC86544 bit 0) counts `dev+0x3B14`. The countdown `dev+0x3B1C`
      reaching 0 opens the gate.
    - 82111E38 reads MMIO V_COUNTER 0x7FC86530 and the viewport height 0x7FC86584.
    - Our zero-backed MMIO page made line% = 1, so the gate released at once. Xenia saturates the line%, so its
      gate waits one more vblank.
      - `SR_VD_SCANOUT_REGS` (default ON) writes V_COUNTER += 720 per console vblank and the viewport as
        0x050002D0. Under `SR_HIGHFPS` the scanline is 0.
      - A/B on the console path (gameplay, intro movies, title): no pacing change, 2.0 vblanks/frame either way
        (NATIVE_ROADMAP P2.9 item 2).
  - **The Vd exports the ring path depends on [verified iter-467: IDA call sites, Xenia `xboxkrnl_video.cc`, roll
    p2fix1].**
    - `VdGetSystemCommandBuffer(r3 = a 0x94-byte descriptor, r4 = a dword)`:
      - Its only caller is D3DDeviceSwap at 0x8211228C (r3 = sp+0xC0, r4 = sp+0x74).
      - Swap copies descriptor +4 to `dev+0x4E80` and +8 to `dev+0x4FB8`, and gates its scaler / gamma
        display-register emission on +8. `*r4` goes to VdSwap as r7.
      - Xenia zeroes the descriptor and stores 0xBEEF0000 / 0xBEEF0001. Ours does the same, `SR_VD_SYSCMDBUF`
        default ON. Before that, Swap read stack garbage (+8 = 0xBF800000) and emitted display-register PM4 from
        it about every frame.
    - `VdInitializeRingBuffer(r3 = the ring's PHYSICAL address, r4 = log2(bytes) - 3)`:
      - The ring is 0x10000 bytes. In roll p2fix1 it was at phys 0x1EF2E000, which is VA 0xBEF2E000 in the 0xA
        physical window.
      - Ours takes it from the argument (`SR_VD_RING_ARG`, default ON). The old route instead claimed the first
        bit-31 heap grant; that route is now master-gated (U1).
  - **Movie.** `MoviePlayerPresentFrame_82473208` caps 3 frames in flight (`player+0x170`), released by its
    `D3DDeviceInsertCallback_820FDE38` callback.
    - **The InsertCallback block [static iter-467 p2: IDA 820FDE38 / 820FDA80].**
      `820FDE38(dev, flags, cb, ctx)`: the XDK's kick test (`cursor > dev+0x30` -> `820FD8C0`), then
      `820FDA80` writes at cursor+4 and returns the new cursor, stored to dev+0x28:
      - `[000005C8 00020000]` WAIT_UNTIL = 3D idle, only when `(flags & 0xC0FFFFFF) == 0`;
      - `0001057C cb ctx` (SCRATCH4/5), `00000578 id` (SCRATCH0; id = `(flags >> 24) & 0x3F`, 4 when 0);
      - `C0043C01 13 pc id FFFFFFFF 100` WAIT mem[pump+0] == id (predicated), `C0005401 id` INTERRUPT
        (predicated), `0000045E id` CP_CPU_INT_ACK, `C0043C01 13 pc 0 FFFFFFFF 100` WAIT mem[pump+0] == 0
        (predicated), `0000057C 0BADF00D`;
      - pc = `*(dev+0x2A14)` mangled `(((va >> 20) + 512) & 0x1000) + (va & 0x1FFFFFFF) | 2`.

      **Caller distinction rechecked 2026-10-12 [IDA bodies + SDK goldens]:** the vblank gate
      `82111EC8` passes flags 0, so its callback includes WAIT_UNTIL (25 dwords); the complete
      gate is 8 + 25 + 6 = 39 dwords. The Z-pass replay kick `82110C78` passes `cpumask | 1`,
      so that callback omits the prefix (23 dwords). Older IDA comments saying both are
      prefix-free are stale; the actual caller instructions and callback body decide.
      **`820FDE38` was not observed in these sampled flows [measured iter-467 p2 + Xenia]:** 0 calls in four gated
      rolls (6/6 intro movies to EOM, title, front end); this does not establish whole-game unreachability.
      Every CP interrupt in the P2 trace (8,743) carries cb
      `82110B00` (the Z-pass replay kick) or `82111E38` (the vblank gate), once each per present; the movie
      callback `82471890` occurs in none of our traces and none of Xenia's (`data/gpu_traces/fmv_*.xtr`,
      `menu_pressstart`). Our intro movies render through `RenderYuvVideoFrameTec0_8235CA48` (the render
      script's fmvmovie pass), not `MoviePlayerPresentFrame_82473208`. The native kRkCallback item
      (`research/p2` e7a61346) was therefore not landed.
    - `MoviePlayerRenderStateTick_8235F488` calls BlockUntilIdle on every playing tick, because it uses a single Y/U/V set.
- **The XDK device register shadow is packed in register order from `dev+0x2800` [static, iter-464, inferred
  from anchors; every anchor fits]:** 0x2000-0x200F at `dev+0x2800` (RB_COLOR_INFO `+0x2804`), the scissor
  non-run `+0x2840..0x2848`, 0x2100-0x2114 at `+0x284C` (RB_COLOR_MASK `+0x285C`), 0x2180-0x2184 at `+0x28A0`
  (SQ_WRAPPING_0/1 `+0x28AC/+0x28B0`), 0x2200-0x220B at `+0x28B4` (RB_BLENDCONTROL0..3 `+0x28B8/D8/DC/E0`,
  PA_CL_CLIP_CNTL `+0x28C4`, PA_CL_VTE_CNTL `+0x28CC`), 0x2280-0x2294 at `+0x28E4`, 0x2300-0x2325 at `+0x2938`,
  0x2380-0x2387 at `+0x29D0..0x29EC`. Together with NATIVE_XDK_SPEC §3 item 4 (dirty bit -> register) this is
  the full dirty-bit -> register -> device-offset map a native projection reads. Non-register D3D software
  words the table setters also write: `+0x2D3C`, `+0x2D40`, `+0x2D44..0x2D50`, `+0x2D5C`, `+0x2D64..0x2D6C`,
  `+0x2DF0..0x2DF8`, `+0x33A4` (NATIVE_XDK_SPEC §6.1 Q-N2).
- **Native XDK leaves (iter-465, `kernel/native_leaves.cpp`) [static contracts; live differential in
  NATIVE_ROADMAP status]:** `D3DDevice_SetVertexShaderConstantF_sub_820F4920` / `SetPixelShaderConstantF_sub_820F4A30`
  (dev r3, start r4, src r5, count r6, mask r7): copy `count` x 16 bytes from the unaligned source (lvlx|lvrx)
  to `dev + ((start + 112|368) << 4 & ~15)` (= dev+0x700/+0x1700 + 16*start), `std r7,48(r1)`,
  `stw r3,-32(r1)`, then `*(u64*)(dev + 0|8) |= r7`. `GpuEmitType0RegPacketsVec_sub_821078C0` (dev r3, mask r4,
  baseReg r5, src r6): per run of leading ones after `cntlzd`, either the chunked segment-switch path
  `82107058` (when `cursor + 4*(regs+3) >= dev+0x2C`) or: type-2 NOPs (cursor&15: 4 -> 1, 0 -> 2, 12 -> 3,
  8 -> 0) so the header lands at 0xC mod 16, header `((regs-1)<<16) | reg`, 64 bytes per bit by lvx128/stvx
  from the 16-aligned shadow; `dev+0x28` = last dword written. Together 24.8% of the gameplay render thread
  (p1_prof slot17 self: 10.51 + 9.05 + 5.26%); the whole XDK range is 35.9% self there.
- **Per-binding constant upload [static + MEASURED iter-467, check 25.4M calls 0 bad]:**
  `ShaderPs_UploadAllConstants_sub_8260B660` / `ShaderVs_UploadAllConstants_sub_8260B2A0` (r3 = shader binding;
  callers ShaderPs/Vs_BindAndUploadConstants 8260B898/8260B800 when the shader changed, MeshBatchBindStreamsAndDraw
  82612ED8): four 0-terminated lists from r3+8 (PS) / r3+12 (VS), in order: F {u16 paramId, u8 reg, u8 count} ->
  Set*ShaderConstantF(dev = dword_8299ADF4, reg, src = dword_82998B00[paramId], count, mask = ((1<<63) >>arith
  (((reg+count-1)>>2) - (reg>>2))) >> (reg>>2)); I and B (same layout) -> SetPixel 820F4C40/820F4B98, SetVertex
  820F4BF0/820F4B40; then callbacks {u32 fn, u16 paramId, u8 reg, u8 count} (end: paramId 0): paramId 0x400..0x7FF
  -> fn(r3 = r1+80 -> {u8 reg, u8 count, _, _, u32 tex = *(0x829886A4 + paramId*32), u8 isVertex}), else
  fn(reg, count, isVertex). Frame 144 bytes. Native F list: `kernel/native_leaves.cpp` (SR_NATIVE_CONST_UPLOAD).
- **What the XDK draw computes beyond the shadow [static + MEASURED iter-465, N1 rolls c3-c5, NATIVE_ROADMAP
  status]:** (a) SQ_PROGRAM_CNTL = vsHdr[+8] | psHdr[+8], SQ_CONTEXT_MISC = vsHdr[+12] | psHdr[+12],
  SQ_INTERPOLATOR_CNTL = psHdr[+24] (`GpuEmitDrawState_82108668`; vsHdr = vs + *(vs+896) + 872, or *(vs+904) with
  no PS and vs[872]&0x20; psHdr = ps + *(ps+64) + 40) - 0 mismatches over 8.1M draws; (b) the shaders' LITERAL
  constants: `GpuRingEmitFetchConstants_82108470` LOAD_ALU_CONSTANTs table entries {u16 vec4 index, u16 dwords,
  u32 offset} (table = a2 + *(a2+20), entries from +20, size at +16; VS a2 = vs+872, base *(vs+32); PS a2 = ps+40,
  base *(ps+24)) when dev+0x10 bit 51 (VS) / 52 (PS) is flushed, AFTER the dirty shadow groups (draw order in
  `820FC000`: 821078C0 x2, then 82108668), so later dirty uploads overwrite literals; (c) the VS fetch patch:
  `D3DVsPatchFetchesInPlaceIfIdle_82108520` patches in place only if the object's fence passed, else
  `GpuLoadShaderImmediate_82108300` IM_LOAD_IMMEDIATEs a patched ring copy (29% of gameplay draws), so the bound
  object's bytes are NOT the program the GPU runs; (d) Z-pass draws (inside `BeginZPass_8210DF38`..`EndZPass_8210E1A0`,
  44% of gameplay draws) execute first in the Z-pass and are replayed in colour (the replay re-runs the whole IB with only
  bin select changed: 11-13 IBs x 2 executions per gameplay frame, window offset / scissors identical - NO screen tiling,
  tile census e3 2026-10-08; byte[10808] bit 0x02 is tile auto-predication from GpuSetTilePredicationMask 8210D400, never
  set here; single-tile native tiling REJECTED, NATIVE_ROADMAP); BeginZPass sets dev.byte[10808]
  |= 0x40, which makes GpuEmitDrawState pick depth-only (5) PER DRAW (no PS, or a PS that neither writes colour,
  psHdr[+28] & 0x30, nor alpha-tests, dev+10428 & 8) - else colour (4); (e) the VS program the XDK IM_LOADs is
  `[a2, a2 + *(hdr+4))`, a2 = *(vs+32) + *(hdr+0), hdr = vs + *(vs + 8*(variant+112)) + 872 - not the container-table
  section; DrawVerticesUP/BeginVertices put their stride/4 in stream 0 (dev+12256) for the call. Host port of the
  patcher: `gfx/native_vs_patch` (iter-465, 1,819/1,819 in-place differentials identical; native VS = CP VS on all
  8.36M joined draws of roll c9/c10). (f) EDRAM reinterpretation on the menu fixture is needed only between
  surfaces at the SAME base (SR_NATIVE_RT=2 replay = 2.06 MAD = the full EDRAM model; =1 with no transfers 4.72).
- **`k_8_8_8_8_GAMMA` is chosen only at surface creation [static, iter-464]:** `TextureSurfaceFetchConstantBuild_
  sub_820FF978` maps TextureFormat 62 -> RB colour format 1 (table `0x820B3890`) or an 8888 format with gamma signs
  on XYZ (`0x820FFB60-B9C`); no render state toggles it, no XEX constant selects it, and the traces never draw
  to it. Xenos PWL gamma is NOT sRGB (decode up to 9.3 UNORM8 steps, encode up to 19 codes;
  `tools/native_pwl_vs_srgb.py`), so `gfx/d3d12_backend.cpp:612-613` mapping colour format 1 to
  `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` (HLE-era path) is wrong per the reference - dead today, a trap for native.
- **No harvested shader uses memexport [measured, iter-464]:** 259 valid unique programs in data/ (142 VS /
  117 PS; 352 with the archive corpora), 0 `alloc kMemory`, 0 eA/eM exports (`tools/native_census_memexport.py`;
  Xenia's own disassembly of the same traces: 0 `alloc export`). Gap: 75 of the 425 DXIL hashes have no ucode
  kept anywhere (a `SR_DUMP_SHADER_CONTAINERS=1` whole run closes it).
- **Resolution scaling is host-only [Xenia port, iter-464; docs/RESOLUTION.md]:** at `SR_RES_SCALE`/`display.ini`
  scale s the host RTs, the EDRAM buffer, the resolve destinations a resolve wrote (a separate reserved
  512 MiB x s^2 buffer) and the textures fetched from those pages are s x per axis; every guest-visible value
  stays 1280x720 (viewport/scissor registers, fetch constants, resolve rects, guest RAM). Consequence (as in
  Xenia): the guest CPU never sees a scaled resolve's pixels in guest RAM; a page the CPU rewrites drops its
  scaled mark. The Xenos param-gen pixel position is `floor(SV_Position) / s` (guest pixels; exactly 8 cached
  PS read it). The CPU EDRAM mirror / CPU resolve / EDRAM snapshot restore have no 1:1 mapping -> 1x fallback.

## 6e. GUEST MEMORY MODEL & VA ALIASING [GOLD, iter-360n — cross-checked vs Xenia `memory.h`]

**Real Xbox 360 VA space** (Xenia `views_`): virtual heaps at `0x00000000`,
`0x40000000`, `0x7F000000`, `0x80000000`, `0x90000000` (title XEX loads at
`0x82000600`); **physical heaps at `0xA0000000` (64 KB pages), `0xC0000000`
(16 MB pages), `0xE0000000` (4 KB pages)** — all three ALIAS the same underlying
RAM. Xenia folds a physical VA to a physical offset (`va & 0x1FFFFFFF`, 512 MB)
into a shared membase, so `0xA0001000`/`0xC0001000`/`0xE0001000` are the SAME bytes.

### Guest allocator (`sub_8246CA80`) — tail-clobber geometry [VERIFIED iter-364]

Our recompiled dlmalloc **zeroes the last 16 bytes of every chunk's usable region.**
Measured with `SR_ALLOC_PROBE=1` (deterministic, isolated — no FMV, no roll-dependence):

```
usable      = ((R + 31) & ~15) - 0x10     # payload starts at chunk+0x10
zeroed zone = [usable - 0x10, usable)
damage      = overlap of [0, R) with that zone
```

| R | usable | damage |
|---|---|---|
| 0x30 / 0x100 / 0x270 / 0x280 / 0x500 | = R (exact fit) | **last 16 B** |
| 0x268 | 0x270 | **last 8 B** |
| 0x271 | 0x280 | ~1 B |

⚠️ This SUPERSEDES the older "only 16-aligned requests clobber" note. It explains all
four known victims — video ASF parser `+0x260` (0x270), WMA descriptor `+4/+8`, WMV3
STRUCT_C (4 B: chunk 0x20, usable 0x10, so the zone covers the **whole payload** — why
both copies read 0), and the WMV bitreader `+0x2C` (0x30).

Correct universal fix = `R' = R + 0x10` (restores ≥0x10 slack for aligned and unaligned
R alike) — what the four gated band-aids do piecemeal. A GLOBAL bump is A/B-REFUTED
(exhausts the bounded DOGMA heap: 0/13 vs 8/8 FMV-reach), so a zero-cost fix must stop
the zeroing itself.

**The clobbering store [NAMED, iter-364, HW-watchpoint]** — `kernel/hw_watchpoint.cpp`
Dr0 write-watch (race-free; the VEH page-guard can't win vs cross-thread heap traffic):
```
memset(dest=P2-0x20, 0, 0x20)  RIP=VCRUNTIME140 memset  guestPC=0x82469B74 (sub_82469B48->sub_8246CA80)
```
It is a HOST CRT memset — the compiler collapsed a guest inline zero-store loop into it
(so no `sub_8279F290` call exists to hook; the guest-memset range-hook rightly never fired).
Geometry (exact): P1 usable `[P1, P1+0x270)`, P2 payload returned at `P1+0x280`, P2 chunk
header at `P1+0x270`; the memset zeroes `[P2-0x20, P2)` = `[P1+0x260, P1+0x280)`, overrunning
P1's last 0x10. **P1 and P2 chunks are ADJACENT, not overlapping** (so "corrupt metadata /
overlap" is REFUTED). The two EXPLICIT `sub_8279F290` calls in `sub_8246CA80` are FAITHFUL
(recomp `ppc_recomp.57.cpp:13088` dest=`r30`=`r26+0x10`=return=Ghidra `v17+8`; `:13142` is the
NtAllocate zero, size 0x30) — neither is the clobber. So the culprit is an INLINE zero-store
loop elsewhere in the alloc/split path, compiler-lowered to memset. NEXT (rigorous, hard rule):
L6 difftest `sub_8246CA80` on captured mstate+heap state, OR find the specific guest store loop
whose dest is off by 0x20 (writes P2-0x20 instead of P2 or P2's header). ⚠️ my strong-def
overrides of `sub_8279F290`/`sub_82776EB0` are silently BYPASSED (recomp reaches them by a path
that skips the override) — do not rely on hooking either; use HW watchpoint / source read.
❌ Do NOT trust the old "mstate at `0x829ac16c`, magic `EEFFEEFF`" hypothesis — golden holds
`0x40000000` there.

### ⭐ Central two-allocator overlap + the `sub_82467F40` bit31 contract [VERIFIED iter-364, 2026-07-23]

**The effect-compile clobber is NOT the intra-chunk tail-zero above — it is a TWO-ALLOCATOR VA
OVERLAP, root-fixed by `SR_DLMALLOC_FENCE`.** The `guestPC=0x82469B74` the Dr0 watch logged is the
STALE last-guest-allocator-LR before a HOST memset; the actual writer is `guestHeapAlloc`'s own
zero-fill (`kernel/guest_context.cpp`). Proven by the `SR_ARENA_TRACE` arena trace:
- `sub_82467F40(size, flags)` dispatch (disassembly is truth; **IDA's decompile is BACKWARDS**):
  `clrrwi. r11,r31,31; beq` → **bit31-CLEAR → `sub_82469B48`(a1=0) → `sub_8246CA80` = guest DLMALLOC**;
  **bit31-SET → `sub_82468008` = `MmAllocatePhysicalMemoryEx` (game pool)**. (flags `0x24810000` = bit31
  clear = dlmalloc.) The old "bit31-clear lands in the game pool `sub_82468008` / `unk_82917E60`" note
  was the backwards decompile — corrected.
- The guest dlmalloc runs on the FAKE mstate `0x82110000` (`SR_HEAP_STUB`), which is **all-zero**
  (`[MSTATE3]` a1=0, 44 header words = 0), **never calls `NtAllocateVirtualMemory`** (every `[NTAVM]`
  reserve is `lr=828A779C` = DOGMA pool `sub_828A7778`), and **never installs a segment** (`[SEGADD]`=0).
  So its wilderness sits INSIDE `guestHeapAlloc`'s bump window `[g_heapStart=0x839E0000, kPhysTop=0xBF000000)`
  but is UNTRACKED by `g_heapNext`. The monotonic CAS bump (no rewind) climbs into a live dlmalloc chunk
  (`0x8B4F08F0`) and zero-fills it → the D3DX preprocessor `+104` region-counter is wiped → `0/66` compiles.
- **FIX = `SR_DLMALLOC_FENCE` (default-ON)**: the `sub_82467F40` hook (`gpu_ring_alloc_fix.cpp`) calls
  `guestHeapReserveThrough(R+size)` (`guest_context.cpp`) after every bit31-clear chunk, atomic-maxing
  `g_heapNext` PAST it. Invariant: `guestHeapAlloc`'s frontier stays ≥ the guest dlmalloc's highest live
  chunk → the two allocators are disjoint by construction. Address-based, race-free (same thread), O(1).
  A/B: OFF `0/66`, ON `33/33` effects (K=4 det, default exe, `HEAP_CAP=0`, no boot regress). RETIRES
  `SR_HEAP_SKIP_OCCUPIED` (content heuristic, now default-OFF). `SR_HEAP_STUB=0` (real dlmalloc) does NOT
  fix it — the overlap is the VA sharing, not the mstate.

### ⭐ XMemAlloc ALIGNMENT ATTRS + the D3DX compiler pool PAGE-MASK contract [GOLD, iter-449B, 2026-08-09]

**HARD RULE: every physical-path allocation must honor the XALLOC alignment attribute.**
- `sub_82467F40(size, attrs)`: if `(attrs & 0xF000000)==0` the attr class defaults to `0x0C`;
  **alignment = `1 << ((attrs>>24) & 0xF)`** (default class → 4096). bit31-SET path passes it to
  `sub_82468008(size, -1, align, prot)` → `MmAllocatePhysicalMemoryEx(0, size, prot, 0, -1, r8=align)`.
  Console Mm results are ALWAYS ≥4KB-aligned.
- **The D3DX effect compiler's pool (`D3DXPoolPageMaskAlloc_sub_827D33B0`) finds each pool page's
  header via `*(obj & 0xFFFFF000)`** — pool pages are 4096-byte XMemAllocs
  (`D3DXPoolPageAlloc4K_sub_827C07C8`, writes `page[0]=pool`). An UNALIGNED page makes the mask land
  in a neighboring allocation → the pool "bump state" reads from garbage → the chunk allocator
  returns arena OFFSETS (0x0/0x38/0x70…) and every consumer writes into the guest null page.
- Downstream chain when violated (the iter-398d/449z black-FE wall): the new-backend mark pass
  (`NewBackendMarkPass_sub_82840D00`) BFS worklist dies silently → asm-register builtins
  (`NewBackendRegisterAsmBuiltins_sub_82840B60`: eA/eM0-4/a0/prev_scalar/lodgradientX-Y, 12 rows
  appended count+1.., each `row.w1|=2`) never registered → publish check
  (`NewBackendPublishRowResults_sub_82836128`: `if (flagArg&0xFF && !(row.w1 & 2)) X4800`) fires →
  error cascade corrupts the ctx → infinite `_strnicmp(NULL,…)` errno loop → every effect E_FAIL →
  `.rdata` debug-shader substitution → black FE.
- **Harness contract (`SR_PHYS_ALIGN_FIX`, default ON):** `guestPhysAlloc(size, align)` over-reserves
  `size+align` and rounds the downward-bump VA up; `stub_MmAllocatePhysicalMemoryEx` honors r8 with a
  4KB floor; the `SR_GPU_RES_DISJOINT` route decodes the attrs nibble. New-backend fn-row table:
  rows 40B at `ctx[3]`, `w0` bits3-16=count/low3=kind, `w1` bit0=inputs-bound bit1=inputs-published,
  `w3`=entity AST node; per-param flags array at row+28 (bit25 0x2000000=has-semantic, bits26-29=
  semantic idx, bits4-17=var idx). Compile ctx flags at ctx+36 (bit 0x40000 = builtins registered).

### ⭐ The DOGMA allocation dispatchers + the NULL-task hazard [VERIFIED iter-380, 2026-07-26]

**Two peer dispatchers share one registry** (per-context entries, 20 bytes:
`+0` key from `sub_825F70C0`, `+12` idx, `+16` pool-array ptr; lookup
`sub_8278C840` — pristine miss returns NULL since iter-379b):

- **`sub_8278D258(size, a4, r5=explicitPool, …)` — alloc via context pool.**
  Pristine order: **r5 ≠ 0 → use the caller's pool DIRECTLY** (no registry, no
  fallback — this is how the 9 pre-publish bootstrap allocs, sizes
  `1..0x1F4018`, work on console while the fallback global `0x829A93F4` is
  still 0 — its static image IS 0, IDA-verified); else registry hit →
  `array[idx]`; else fallback global. Then **calls `pool->vt[+36]` itself**
  and returns the MEMORY (it is an alloc function, not a pool getter).
  ⚠️ The iter-66 suppression of the r5 fast path (`SR_REAL_DOGMA_POOL`,
  opt-in since iter-188) was the manufactured root of the entire
  `SR_DOGMA_ALLOC_SUB` dependency — default flipped ON at iter-380
  (pristine), substitution retired default-OFF. 4/4 deep no-env.
- **`sub_8278CAD8(this, size, tagStr, flags)` — named alloc** (vt[+8] of the
  static memory-interface singleton `0x82944A9C`, runtime vtable `0x8209A3D0`,
  pool names "main memory/disposable/physical"). Same registry lookup, then
  `pool->vt[+36](pool, size, tag, flags, 0, 0, ctx2, 0)` with **NO null/
  validity check** on `array[idx]`. `[CAD8]` hook logs the chain.
- **Pool alloc `vt[+36] = sub_82182700`**: small-bin `MemPoolAllocBySize_
  sub_82180788(pool+1472)` else central grow `sub_8278DCB8` → embedded heap
  object at `pool+72` (vtable `0x820B30F8`) `vt[+12] = sub_828CEB60` →
  **the guest dlmalloc family (`HeapMallocCore_sub_828CEBC0`)**. A broken/empty
  mstate here yields tiny-integer "pointers" (`0x2B0` = chunk→mem off a NULL
  base) before yielding 0.
- **The NULL-task hazard (recomp-specific visibility trap):**
  `ResMgrAcquire_sub_826FAAC0` allocs its 80-byte 'ResourceTask' via CAD8 and
  guards only the CONSTRUCTION (`if (mem)`), not the enqueue —
  `ResSchedulerEnqueueSorted_sub_826F4830(sched, item)` then vtable-calls the
  item (`vt[+116]` priority) and inserts it. **On console a NULL item CRASHES
  at the vtable deref; our `PPC_CALL_INDIRECT out-of-range` fallback returns
  r3=0 and continues**, so the NULL lands in queue A and the drain spins
  forever (`[NULL_ENQUEUE]` hook names the call site). General rule: any
  "impossible" guest state that would crash console hardware can SURVIVE here
  via the out-of-range indirect fallback — grep `[SPIN_DEBUG]` before
  trusting a "console-impossible" framing.

**Our model** (`kernel/guest_context.hpp::aliasGuestVA` + allocator constants):
one flat `host_base + va` arena, size `kMinGuestSizeToCoverAllUsedVAs =
0xBF000000`. Our physical heap is a FLAT region `kPhysBase=0x84000000` →
`kPhysTop=0xBF000000` (grows down); the main heap grows up from `0x82AE0000`.
`aliasGuestVA` folds only:
- `va < 0xC0000000` → pass through (this INCLUDES `0xA0-0xBF`, which is our own
  flat phys heap, NOT treated as an alias).
- `0xC0000000-0xCFFFFFFF` → `va - 0x40000000` (→ `0x80-0x8F`).
- `0xE0000000-0xFFFFFFFF` → `va - 0x60000000` (→ `0x80-0x9F`).
- `0xD0000000-0xD00FFFFF` → pass through — OUR fake GPU MMIO / framebuffer carve-out
  (not a real Xbox alias; charter-allowed GPU surface).

**Why ours differs and still works:** our `MmAllocatePhysicalMemory` returns
FLAT `0x84-0xBF` addresses, so the game never sees real `0xA0/0xC0/0xE0` physical
addresses from the kernel. The fold exists to catch the game computing its own
**uncached alias** by setting the `0x40000000`/`0x60000000` bits on a `0x80`-range
address — `0x85001000 | 0x40000000 = 0xC5001000` folds back to `0x85001000`. ✓

**⚠️ LATENT GAP (documented, not yet observed live):** if the game ever forms an
uncached alias with the **`0x20000000` bit** (→ `0xA0000000-0xBFFFFFFF`) on an
address that also has a `0x80`-range view, we would NOT fold it — `0xA5001000`
passes through to a DIFFERENT host location than `0x85001000` → silent
read/write desync. Unbitten so far (this game appears to use `0x40/0x60`
aliasing). Also: the `0xD0000000-0xD00FFFFF` carve-out would intercept any real
guest use of that sub-range as `0xC0-0xDF` 16 MB physical memory. Both are cheap
to probe (log any guest access to `0xA0000000-0xBFFFFFFF` that isn't one of our
own phys allocations) if a raw-memory-corruption bug ever points here. This is
the recurring "check endianness/aliasing first on raw-memory bugs" class.

### Physical memory placement and the Warworld -> Metropolis return [iter-465 physmem; live Xenia oracle + golden sound_gameplay + Xenia source]

**The windows as they are today (for PERF2, which moves to Xenia's view model).** PHYS_UNIFY
(guest/guest_harness.cpp allocGuestMemoryUnified) maps ONE 512 MB section at four windows:
`0x80000000` (XEX image at 0x82000000, fallback + primary heap from ~0x839E0000), `0xA0000000`,
`0xC0000000` (minus the private fake-device carveout `[0xD0000000, 0xD0100000)`) and
`0xE0000000`; `[0, 0x80000000)` is private memory (Xenia's v00000000/v40000000 heaps,
kernel/ntavm_xenia_heap). Physical address = `va & 0x1FFFFFFF` in every window. The title's
`MmAllocatePhysicalMemoryEx` is served by the LIFO allocator in `[0xA0000000, 0xBF000000)`
(guestPhysAlloc, kernel/guest_context.cpp; offsets count down from `kPhysTop = 0xBF000000`;
floor `0xA0000000` = SR_PHYS_FLOOR; the 1 MB main stack `[0xBE000000, 0xBE100000)` and the
alias holes of heap-routed GPU grants are skipped). Every grant is returned as an 0xA-window
VA. Divergences from Xenia that remain: the top 16 MB `[0xBF000000, 0xC0000000)` is unused
(host plausibility checks still treat >= 0xBF000000 as outside: kMinGuestSizeToCoverAllUsedVAs,
csCanonicalKey, dogma_hash_hooks, fm_world_ptr_patcher, alloc_overlap_watch, memcpy guard);
4 KB-page grants are NOT returned in the 0xE window and 16 MB-page grants not in 0xC (Xenia
does both); and the 0x8 window ALIASES the same physical pages as the 0xA window (in Xenia
the XEX heaps are separate memory), so a LIFO grant low enough would share pages with the
image / 0x8 heap - only the top-down placement keeps them apart today.

**Xenia (the oracle).** One parent page table over physical `[0, 512 MB)`, 4 KB pages; the
0xA (64 KB pages), 0xC (16 MB) and 0xE (4 KB, +0x1000 host offset) heaps all allocate in it.
`xeMmAllocatePhysicalMemoryEx` (xboxkrnl_memory.cc:415): page size from protect
(`X_MEM_LARGE_PAGES` 0x20000000 -> 64 KB, `X_MEM_16MB_PAGES` -> 16 MB, else 4 KB), size AND
alignment rounded up to it, `AllocRange` top-down = the highest free aligned spot, nothing
reserved above it. Thread stacks are NOT physical: `XThread::AllocateStack` (xthread.cc:273)
puts them in v40000000 `[0x70000000, 0x7F000000)`, bottom-up, 64 KB guard each side, freed
with the thread; the KPCR / KTHREAD are system-heap blocks at `0x3000xxxx`
(`Memory::SystemHeapAlloc`). Golden `sound_gameplay`: 29 KPCRs at 0x30006000.., stacks from
`[0x70010000, 0x70030000)` up, reused after exit (`archive/v1_physmem/xenia_golden_kpcr_stacks.txt`).

**The return, observed in Xenia** (`archive/v1_physmem/xenia_234914`, retail Canary
`--log_level=3` + the same level-skip plan through the Frida executor; `oracle_pool_lines.txt`):
boot world pool `MmAllocatePhysicalMemoryEx(0xDFF0000, prot 0x20000404, align 0x8000)` ->
ADBE0000; on the return 0x690000 -> BB5D0000, 0x3A5000 (-> 0x3B0000) -> BB220000, then the pool
-> AD230000; Metropolis plays on (`after_plan_plus.png`). Xenia's free run then was
`[0C795000, 1B220000)`, 10.5 MB spare.

**Why ours refused it (three causes, all fixed, each `=0` keeps the A/B):**
1. **Thread stacks in physical memory** (iter-388 SR_STACK_PHYS: guestPhysAlloc, never
   freed). Every cutscene starts four MoviePipeline workers (`MoviePipeline_Worker1ThreadProc_sub_8247F9D0`,
   `...FA00`, `...FA30`, `...FA60`), so 4 x 256 KB stacks landed top-down in the freed pool
   (nomodsAB1: `0xBAF36000..0xBB036010`). **SR_THREAD_MEM_XENIA** (kernel/thread_mem_xenia.*):
   stacks from `xheap::allocRange(0x70000000, 0x7F000000)`, released when the thread ends;
   KTHREAD/KPCR arena overflow from `[0x30000000, 0x40000000)`; `kthreadIsBlockVA` for waits.
2. **Placement semantics.** Ours rounded only to 4 KB and reserved `size + align` per cursor
   grant (padding nobody could reuse). **SR_PHYS_XENIA_PLACE**: Xenia's page-size rounding and
   the exact top-down cursor placement with the pages above the grant left free.
3. **A boot-time race that native speed loses.** The METROPOLISWORLDFLOW_PHYSICS group's
   temporary pool (0x690000, pool set `sub_825FA368`, freed via lr 825FA470) lives >= 4
   main-thread ticks in Xenia, so the EA_HD_logo movie's 12 frame buffers (0xE1000/0x44000,
   allocated by the movie thread) land BELOW it and its free leaves a 6.5 MB hole above the
   world pool; 33 s later the persistent boot grants (50 x 64 KB + 0x2D0000) fill that hole.
   Native code loads the group inside one tick (alloc 19.340, free 19.426), the buffers come
   after, no hole forms, the persistent grants sit directly below the pool
   (`ACF80000..AD524000`) and the return's free run is ~10 MB short. SR_READ_PENDING +
   SR_NTSTATUS_DOSERR do not change the order (rp1). **SR_PHYS_BIG_GAP**: a >= 16 MB cursor
   grant leaves 32 MB free above it, so the hole Xenia gets by timing is always there (it is
   ordinary free space the next smaller grants fill top-down). DEVIATION from Xenia by design:
   our world pool sits 32 MB lower than pure top-down would put it.

**What the fix assumes (PERF2):** the physical allocator returns 0xA-window VAs from one
top-down space ending at `kPhysTop`; the gap is an offset-space free extent, so moving the
top to 0xC0000000 (Xenia's full 512 MB) keeps it valid; 0xE/0xC-window returns for 4 KB / 16 MB
pages would change only the VA, not the physical placement. Thread stacks and KPCR/KTHREAD
overflow must stay out of physical memory whatever the view model. Diagnostics:
`[PHYS_PINS]` at a [PHYS_CAP] refusal (live bytes per kind, largest extents, the best window
and its pins), `SR_PHYS_TRACE=1` (every grant/free; callers of >= 1 MB grants).
Repro: `tools/scenarios/physmem_metropolis_return.json` + `data/devtools/plans/metropolis_return_from_new_game.txt`.

## 6h. D3D9-HLE RENDER SURFACE [GOLD, iter-421, 2026-07-30] - THE PIVOT CONTRACT

USER DECISION 2026-07-30: full pivot to an UnleashedRecomp-style D3D9-HLE renderer.
The X360 D3D9 library is statically linked (0x820F0000-0x82113FFF region, D3DX effect
strings intact); the game CALLS it (SetPixelShader alone has 19 game-code call sites).
PM4/LLE demoted to diagnostic fixture harness (TOOLKIT 1.6).

**`sub_82473208` = the MOVIE FRAME PRESENTER - the complete D3D9 sequence, verified:**

| call | role |
|---|---|
| `sub_820FFCB0(tex,0,out,0,0)` | LockRect: out[0]=pitch, out[1]=data ptr |
| `memcpy_sub_8279EC60` | plane copy into locked texture (x3, triple-buffered ring at a1+372) |
| `sub_820FFCC8(tex,0)` | UnlockRect |
| `D3DDeviceSetTextureFetchConstant_sub_82100310(dev,slot,hdr,flags)` | SetTexture; movie uses slots 0(Y) 1(U) 2(V) - LUMA IS SLOT 0 |
| `D3DDevice_SetIndices_sub_820F2C08(dev,ib)` | SetIndices (a1+60). **CORRECTED iter-440** — was "BindResourceTracked"; writes dev+12164, draw emit reads dev dword[3041] |
| `D3DDevice_SetVertexDeclaration_sub_820F5438(dev,decl)` | SetVertexDeclaration (a1+68). **iter-440**: dev+11536=decl, dirty bit 13 |
| `sub_820F5218(dev,vs)` / `sub_820F4E88(dev,ps)` | SetVertexShader / SetPixelShader (6d) |
| `sub_820F1D40` / `sub_820F1EC8` | sampler state per slot |
| `sub_820F0B60/820F04E8/820F16C8(dev,0)` | render states (blend/depth/cull class) |
| `D3DDeviceSetStreamSource_sub_820F2A50(dev,0,vb,0,0x14,4096)` | SetStreamSource: stream 0, quad VB, stride 0x14 (20B verts). **CORRECTED iter-440** — was misnamed "SetRenderTargetSurfaceTile"; decompile-proven contract (dev, stream, vb, OffsetInBytes, Stride, dirtyBit): slots dev[4*(3047+s)], stride/4 byte at dev+12256+s |
| `D3DDeviceEmitDrawVertices_sub_820FBBF8(dev,4,0,6)` | THE DRAW (primType,start,count) |
| `sub_820FDE38(dev,tag,cb,ctx)` | GPU fence + completion callback (frees the ring slot) |

**Device object state (read at draw time for HLE):** texture fetch constants cached at
`dev+1024 + 24*slot` (6 dwords each; the movie presenter pokes filter bits directly);
dirty mask at `dev+16` (64-bit); PS slot `dev+12416`, VS slot `dev+12420` (6d).

**HLE model (Unleashed): override the DRAW + fence + swap boundary kernel-side; let
state setters run their guest bookkeeping untouched; at draw time read the device
object out of guest memory and issue D3D12 using the existing XenosRecomp DXIL cache
and iter-414 texture decode. Everything below the API - PM4, EDRAM, resolves, tiling,
fetch-constant unions - is deleted from the shipping path.**

## 6g. MOVIE FRAME DELIVERY: DECODED YUV -> TEXTURE MEMORY [GOLD, iter-408, 2026-07-29]

`MovieVideoDecodeYuvToRgbLoop_sub_82480D80(r3=player, r4, r5=outDelayMs, r6=destDesc)` is the
function that hands a decoded video frame to the renderer. Verified live (SR_YUVCOPY_PROBE) and
by IDA decompile. It is a **decision tree**, and three of its four arms copy nothing:

| `r6` (destDesc) | Behaviour |
|---|---|
| `NULL` | No copy here at all. The sample is handed to the media renderer instead (vt+64 sink, `sub_82472DE0`). |
| `desc[0] == 1` | **PLANAR.** Three memcpys: Y/U/V -> `desc[3]`/`desc[4]`/`desc[5]`, pitches `desc[9]`/`desc[10]`/`desc[11]`. |
| `desc[0] == 255` | VMX YUV->RGB convert into `desc[3]`, pitch `desc[5]`. |
| anything else | Falls straight through, **copying NOTHING**. |

The planar arm is additionally guarded by **width AND height both EVEN** — an odd dimension
returns `E_NOTIMPL` (0x80004001) with no copy performed. Width = `player[58]` (+232),
height = `player[59]` (+236), both latched from the stream at loop entry.

**Live values for movie 1** (identical on 3/3 rolls): `mode=1 w=1280 h=720 rc=S_OK`,
`Y=0x8F7C0000 pitch 1280`, `U=0x8F8A1000 pitch 768`, `V=0x8F8E4000 pitch 768`. The chroma pitch is
768, not width/2=640 — it is aligned up, so **never assume chroma pitch == width/2**. The three
destinations are the k_8 (`fmt 2`) textures the GPU samples: 1280x720 luma + two 640x360 chroma.

### HARD RULE — a sampled surface the guest CPU rewrites needs explicit invalidation

The console's GPU sampled guest RAM directly, so a CPU write to a sampled surface was visible on
the very next draw and **no invalidation protocol exists in the guest to imitate**. Any host-side
texture cache is an optimisation the console never had. Ours was keyed only on the fetch constant
(address/format/size) and had eviction for GPU resolves (§6d, iter-359g) but nothing for guest CPU
writes — so a video frame uploaded once and froze forever. Fixed iter-408 (`SR_TEX_REVALIDATE`,
default-ON): hash the source span and re-upload on change **into the same slot and resource**
(a new slot per frame exhausts the 1024-slot pool in seconds and churns descriptors).

### Tooling trap: k_8 is ONE byte per texel

The movie's luma and both chroma surfaces are `fmt 2` (k_8). Any dump/inspection path that lacks a
1-byte branch and falls through to a 4-byte float read renders a real luminance plane as **black**.
This produced multiple false "the video plane is BLACK" findings (iter-406d, iter-407) before it
was caught. Verify the decoder in any new inspection tool against a surface known to have content.

### 6g.5 THE UP/INLINE DRAW PATH — AND IT IS WHAT DRAWS THE FRONT END [GOLD, iter-445 S1, 2026-08-04]

**`D3DDeviceEmitDrawIndexedGen_sub_820FB6E8(dev, primType, count, strideBytes)`**
allocates ring space for INLINE vertices and **returns the ring alloc VA in r3**
(0 on failure). ⚠ **THE CALLER MEMCPYS THE VERTICES IN AFTER IT RETURNS** — a
snapshot taken inside the hook captures the PREVIOUS frame's ring contents, i.e.
a fresh instance of the iter-444 S6j wrong-buffer bug. Contract recovered from
the deleted renderer's own hook (`git show d651b04^:kernel/hle_d3d9.cpp`) and
re-verified live.

⚠ **The shared `API_OBS_DRAW` macro mislabels this entry point** exactly as it
does DrawIndexedVertices: it publishes `start = r5` (really COUNT) and
`count = r6` (really STRIDE). Inert only because it also forces `vertBytes = 0`.

**TWO CALLING CONVENTIONS, NOT TWO DRAIN POINTS** (⚠ corrected once IDA came
back up — the probe's numbers were right, my mechanism was not). `sub_820FBBB0`
is **`D3DDevice_DrawVerticesUP_sub_820FBBB0`**, the ONE-SHOT inline-draw API,
and its decompile is three lines:

```c
D3DDevice_DrawVerticesUP(dev r3, primType r4, count r5, pVertexData r6, stride r7) {
  ring = D3DDevice_BeginVertices_sub_820FB6E8(dev, primType, count, stride);
  if (ring) { memcpy(ring, pVertexData, count * stride);
              *(dev + 40) = *(dev + 13076); }        // no EndVertices call
}
```
That independently re-confirms the BeginVertices signature above. MEASURED split
over a natural roll (`SR_S6_UPPROBE`): `begins = 19,288 = 16,819 one-shot (87%)
+ 2,469 raw Begin...End (13%)`, conservation exact, `nullAlloc = 0`,
`overwritten = 0`, `pending = 0`. That is why the pre-front-end era shows
~14,000 BeginVertices against ZERO EndVertices — those draws are one-shots.
At the latch the inline bytes are present and real in BOTH populations:
`zeroSpan = 0`, `repeating = 0`, `unreadable = 0`.

⛔ **WHICH MAKES 87% OF THE UP PATH THE CHEAP CASE.** For a one-shot the vertex
bytes are the CALLER'S OWN ARRAY at r6, fully populated **at entry** — so a hook
on `sub_820FBBB0` snapshots `count * stride` from r6 directly, with no ring
latch and no deferred drain. Only the 13% raw `Begin...End` population needs the
latch-at-Begin / read-at-End machinery.

⛔ **THE FRONT END IS DRAWN ENTIRELY THROUGH THIS PATH, AND IT IS MULTI-PASS.**
The Apt/front-end module (0x826D0000-0x826F0000) has exactly ONE function that
calls D3D9 and it calls only Begin/EndVertices (`guest/ppc/ppc_recomp.92.cpp`
:23360, :23515, :23887). Measured front-end windows (`drawV = 0`, `drawIV = 0`,
~23 UP draws/frame):

| window | UP draws | → `83A258F0` | → primary `82986F60` |
|---|---|---|---|
| 24 | 1,092 | 616 (56%) | 84 (7.7%) |
| 25 | 1,287 | 726 (56%) | 99 (7.7%) |
| 26 | 1,365 | 770 (56%) | 105 (7.7%) |

`83A258F0` is a **second FULL-SIZE 1280x720 surface** (`w6 = 0x14000500`,
fetch-constant size dword `0x384000` = 1280*720*4 — identical geometry to the
primary). So **~92% of front-end drawing goes to offscreen targets**: rendering
UP draws against the backbuffer would place 8% of the front end correctly and
paint the other 92% — a whole offscreen composition — on top of the screen.

**Primitive census over ALL THREE draw entry points:** `p4` (tri list),
`p6` (tri strip), `p13` (quad list) ONLY. **No fans (5), no points (1), no lines
(2/3)** — `hleSubmitDraw`'s supported set {4,6,8,13} covers everything the guest
actually draws.

### 6g.10 THE RESOLVE STREAM, MEASURED — and why destinations can never come from the upload path [GOLD, iter-446, 2026-08-04]

`Resolve` is now a first-class `RenderCommandType` (it was the ONE D3D9 entry
point with none). Both ends are resolved to KEYS producer-side:
`srcSurface` from the device's own slot array via `Flags & 7` (one formula
covers RT0..3 and depth, since `12168 + 4*4 == 12184`), and `destKey` =
`*(pDestTexture+32) & 0xFFFFF000` — the fetch-constant base page, the SAME key
`SetTexture` binds by.

**MEASURED, natural K=1 200 s roll:**

| | |
|---|---|
| resolves | **105,925** |
| distinct sources / destinations | **14 / 16** |
| source already in the surface pool | **105,925 — 100%** |
| destination has a host texture resource | **0** |
| depth-sourced (`Flags&7 == 4`) | 8,148 |
| clear-on-resolve (`0x100` / `0x200`) | 16,296 / 0 |

Sanity-checked against the decompile on the way past: `flags=00000014` resolves
`src=4` to `surf=82986F20`, which IS the depth surface; and consecutive resolves
to one destination carry `slice=0,1,2`, the array/cube slices §6g.4 predicted.

⛔ **`destKnown = 0` IS NOT A KEY BUG — this was checked, not assumed.** The
identical zero was iter-445's texture-identity signature (a key inserted one way
and looked up another, hidden for a whole iteration). Cross-check: of the 16
resolve destinations, **9 are keys the guest ALSO BINDS through `SetTexture`**,
inside a window where only **12 distinct texture keys were bound at all**. Nine
page-aligned 32-bit keys do not collide by accident. The two derivations agree.

✅ **THE REAL FINDING, and it reframes the resolve slice.** A resolve
destination's pixels are produced BY THE GPU. There is no `Lock`/`Unlock` for a
render product — ever — so a resolve destination can **never** arrive through
the texture upload path, no matter how complete that path becomes. It has to be
created BY THE RESOLVE and registered under its bind key. That is the missing
link between the pass graph and the sampler, and it is also part of the answer
to §6g.2's open puzzle (883 textures bound against `texCreate=104`): a share of
those "textures with no pixels" are render products the CPU never touches.

⚠ **NAMED PLATFORM GAP (iter-449o bake audit): resolve products never write
back to GUEST memory.** The host resolve path (`SR_HLE_RESOLVE`) copies into a
HOST texture registered under the guest's dest key — the guest's own resolve
destination allocation keeps whatever bytes it had. On console the resolve
writes real pixels there, so a guest CPU READ-BACK of a resolve destination
(CPU picking, screenshot code, any texture the game post-processes CPU-side)
would see stale/zero bytes under our model. No such read-back has been
observed; if one appears, this is the first suspect, and the fix class is a
host→guest write-back at resolve time (platform-fix, guest-initiated).

### 6g.11 THE RESOLVE COPY EXECUTES — the PLUMBING is complete; the CONTENT is clear-only [iter-446 R5b; ⛔ CLAIM CORRECTED by the pre-bake audit, 2026-08-05]

§6g.10 established that a resolve destination can arrive NO other way than by
being created by the resolve. It is created there now, registered under its bind
key, and the copy is recorded into the frame's open list (which is exactly what
iter-445 C4 unblocked).

**MEASURED, natural K=1 200 s roll, `SR_HLE_RT_POOL=1 SR_HLE_RESOLVE=1`:**

| | |
|---|---|
| resolves | 95,655 |
| copies EXECUTED | **69,902 (73%)** |
| destination textures created / owned | **14** |
| **guest binds of a render product** | **266,631** |
| refused: sliced / sub-rect / depth | 14,716 / 3,679 / 7,358 |
| debug layer msgs / err / warn | **0 / 0 / 0** |

**THROUGHPUT, PAIRED BACK-TO-BACK ON ONE MACHINE STATE** (three earlier
resolve-on rolls trended 6900 -> 6300 -> 5700 and looked like a cost; they were
not attributable, so they were re-run as a pair rather than guessed at):

| 200 s roll | present | feFrames |
|---|---|---|
| pool only | 6900 | 4910 |
| pool + resolve (80,637 copies) | **7200** | 4862 |

B is higher on present and level on feFrames. Across every 200 s roll of this
session `present` spans 5700-7200 REGARDLESS of gates, so only the paired run is
attributable. ⛔ The "copies" half of that cross-check is RETRACTED: backend
`copies` and executor `copies` are a TAUTOLOGY (one call site, no branch between
the increment and the return, and the return can never be 0), so their agreement
carries zero bits — it was quoted in `3f7a5da` as a self-audit pass. ✅ The
`dests`/`owned` half IS independent (a re-create bumps one and not the other),
and 14/14 stands.

⚠ EVERY resolve is accounted for: `69,902 + 3,679 + 14,716 + 7,358 = 95,655`
exactly, and the `other` bucket equals `depthSrc` exactly — every unexplained
refusal IS the named depth gap, not a silent loss.

**`sampled` is the metric, NOT `bindHit`.** Registering the destinations raises
`bindHit` BY CONSTRUCTION, so that number cannot grade this work. `sampled`
counts the actual event: the guest binding one of those render products as a
texture. It was always doing that — 266,631 times per run — and until now every
one of those binds resolved to the 1x1 white fallback or to stale guest memory.

⛔⛔ **THE CENTRAL CLAIM WAS WRONG AND IS RETRACTED HERE.** The R5b commit said
"render products become real pixels the guest can sample" and this section said
"a shadow/reflection/post product now exists as real pixels instead of stale
memory". **Both were false.** At that commit the surface pool was entirely
WRITE-ONLY: `m_hleBoundRT`/`m_hleBoundDS` were assigned and read nowhere,
`HleSurface::descIndex` was written and never read, every `OMSetRenderTargets`
site binds the legacy `m_geoRT`/`m_sceneHdrRT`, and no draw path targets the
pool. So `CopyResource` moved the pool's **deterministic black** (the heap is
zeroed — no `CREATE_NOT_ZEROED`) into 14 destination textures, and registering
them in `s_texSlot` made 315,122 guest binds resolve to BLACK rather than to the
1x1 white fallback.

⚠ **AND NO INSTRUMENT IN THE SLICE COULD HAVE DETECTED IT.** `copies`, `owned`,
`sampled`, `m_resolveCopies`, `m_resolveTexCreated` and `t.fromResolve` all
increment identically whether the source holds an image or a blank; nothing read
back a resolved texel; and the debug layer emits no message for an uninitialised
copy source (it was provably live — R6's clear-value mistake took it 0 -> 63).
The claim was **unfalsifiable by the slice's own evidence** — the rule-5 shape,
published in a GOLD entry future sessions would quote.

✅ **WHAT IS ACTUALLY TRUE.** The resolve PLUMBING is complete and debug-layer
clean: resource creation, SRV registration under the guest's own bind key, the
barrier pair, the state bookkeeping, and the copy itself. Since R6 (`d79fd3c`)
the source also carries the guest's own CLEAR VALUES — the first real reader of
`m_hleBoundRT`/`descIndex`. That is clear-only content, not a render product.

**The gap is now MEASURED rather than assumed**: `[SURF_POOL]` reports
`srcNeverDrawn=`, incremented on every copy whose source has never had geometry
rendered into it. It reads **171/171 on the fixture — 100%** — and can only fall
when the draw path moves onto the pool. `hleNotePoolDraw()` is its setter and
has NO call site today, which is precisely the point.

⛔ **WHAT THIS DOES NOT DO.** The bind MISS rate is still ~97.5%: 1,107 distinct
texture keys are bound per run and only 10-16 are render products. The remaining
miss is the GENERAL TEXTURE gap, a different slice. And this does NOT yet remove
§6g.4's "wrong by construction" property — it builds the mechanism that will,
once geometry actually lands in the pool.

**NAMED v1 LIMITS, refused rather than approximated:**
- **Depth sources refused** (7,358/run). A `D32_FLOAT_S8X24` resource cannot be
  copied into an SRV-readable texture of the same format, and faking one with a
  colour format samples garbage that reads as a shadow bug.
- **Sliced and sub-rect refused** (14,716 + 3,679). Writing slice 2 over slice 0,
  or a sub-rect as a full surface, yields a texture that samples PLAUSIBLY and
  is wrong — the worst outcome available, because it looks like progress.
- **Format is the SOURCE's**, which makes `CopyResource` legal by construction.
  A real 360 resolve also converts EDRAM tiling/format; doing that properly
  needs a full-screen resolve pass, not a copy.
- **MSAA downgraded to 1x** at pool creation (1 surface affected).

✅ **AND THE HAZARD THAT WOULD HAVE UNDONE IT.** The bind-capture path (S6g)
snapshots a texture's pixels from GUEST memory when it is bound — and a render
product's guest memory holds stale garbage, because the CPU never wrote it.
Without an ownership claim the very next bind would overwrite every resolved
image with rubbish. `s_resolveOwned` is `s_unlockOwned`'s sibling and exists for
exactly the same reason the movie planes needed one.

### 6g.14 DEPTH RESOLVE — the surface must be TYPELESS, and it was the most-bound missing texture [GOLD, iter-446, 2026-08-05]

R5b refused every depth-sourced resolve (7,982/run) and filed it as a tidy named
gap. It was not tidy: the **single most-bound MISSING texture in the frame** is
the resolved depth buffer — `key=0CC32000` on **sampler 2**, filling bind misses
#1-#8 — i.e. the guest resolves its depth buffer to a texture and samples it
repeatedly (depth fog / soft particles / SSAO / shadow reconstruction).

**WHY IT WAS REFUSED, AND THE FIX.** A fully-typed `D32_FLOAT_S8X24_UINT`
resource can be a depth target and nothing else — it cannot be SRV'd and cannot
be copied into anything a shader reads. So the depth SURFACE is now created
**TYPELESS**, which lets one resource carry three views:

| view | format |
|---|---|
| resource | `R32G8X24_TYPELESS` (fmt 1) / `R24G8_TYPELESS` (fmt 0) |
| DSV (rendering) | `D32_FLOAT_S8X24_UINT` / `D24_UNORM_S8_UINT` |
| SRV (sampling the depth plane) | `R32_FLOAT_X8X24_TYPELESS` / `R24_UNORM_X8_TYPELESS` |

The resolve DESTINATION is created in the **same typeless family** as the
source, which makes the copy trivially legal, and is SRV'd through the depth
plane. ⚠ A typeless resource still needs a TYPED optimized clear value, and a
typeless DSV must name its format explicitly — `nullptr` is an invalid parameter
there, not a default.

⚠ **A depth source returns to `DEPTH_WRITE`, not `RENDER_TARGET`.** Putting a
depth resource into the render-target state is an invalid transition.

**MEASURED**, `SR_HLE_RESOLVE_DEPTH=1` (gated apart from colour resolves so the
two can be attributed separately), natural K=1 200 s roll:

| | before | after |
|---|---|---|
| resolves executed | 75,830 | **82,741** |
| destinations | 14 | **15** |
| `skip(other=)` — the depth bucket | 7,982 | **0** |
| debug layer | 0/0/0 | **0/0/0** |

Accounting still exact: `82,741 + 3,940 rect + 15,760 sliced + 0 = 102,441`.
The depth destination is created at `dxgi=19` (`R32G8X24_TYPELESS`) under key
`0CC32000` — the very key that was missing.

⛔ **CONTENT IS STILL CLEAR-ONLY.** `srcNeverDrawn` remains **100%**: no draw
targets a pooled surface, so this resolves a CLEARED depth buffer. The plumbing
is complete and debug-layer clean; the content arrives when the draw path moves.
Do not read the executed count as depth data.

> ⛔ **SUPERSEDED THE SAME DAY BY iter-446 S5 — this paragraph was left standing
> as GOLD after the measurement that falsified it.** `SR_HLE_DRAW_TO_POOL`
> (commit `93a2b1e`) landed draws on the guest's bound surface, and
> `srcNeverDrawn` moved off 100% for the first time: **173/189 on the 16-present
> fixture**, and **77,067 / 85,177 = 90.5% on the 200 s boot roll**
> (`archive/iter447_baseline/s5_wide0_instruments.log`, final `[SURF_POOL]`
> line, `poolDraws=1273`). The correct reading is now "still *predominantly*
> clear-only", not "100%".
>
> ⚠ The 9.5% that IS drawn is the movie/UP population only. Do not read the
> movement as general geometry reaching the pool — [§6g.15](#6g15) records why
> that is a separate gap, and the honest denominator is still ~0.014% of all
> draws recorded.

⚠ **ENCODING IS NOT CLAIMED EQUIVALENT.** Xenos `k_24_8_FLOAT` is a 20e4 float;
this is IEEE 32-bit. The round trip is self-consistent — we render the depth AND
sample it in the same host format, which is what depth reconstruction needs — but
a shader reading raw depth BITS would still be wrong.

### 6g.13 COLOUR FORMATS 4 / 5 ARE SIGNED — and their ±32 RANGE is still unimplemented [GOLD, iter-446, 2026-08-05]

`k_16_16` (4) and `k_16_16_16_16` (5) are **signed** fixed point with range
**-32..32**, not UNORM. Xenia's `xenos.h` annotates "Fixed point -32...32" over
both, notes they "are signed and also have a different range" and excludes them
from `IsColorResolveFormatBitwiseEquivalent`; its own `GetColorDrawDXGIFormat`
maps them to `_SNORM`. We had `_UNORM`, so every negative value clamped to 0 on
the one measured fmt-4 surface (640x640, 4X MSAA).

⚠ **SNORM RECOVERS THE SIGN ONLY.** The host format is ±1 and the guest's is
±32, so **the RANGE IS STILL UNIMPLEMENTED** — anything `|v| > 1` clamps. The
missing piece is a per-RT output scale of 1/32 where the translated pixel shader
writes colour, with the matching x32 on the resolve/sample side (Xenia does this
as `color_exp_bias -= 5`). That belongs to the shader-translator slice. **Do not
read the SNORM row as "done".**

⚠ Identify that surface by SHAPE (640x640 / fmt 4 / msaa=2 / 0x640000 bytes),
never by VA — the address is roll-dependent (`83A32560` and `83A091A0` across
two runs are the same logical surface).

### 6g.12 ⛔ THE SURFACE HEADER IS EDRAM-TILE-PADDED IN BOTH AXES — a 4x4 target read as 80x16 [GOLD, iter-446, 2026-08-05]

§6g.9 derives height as `bytes / (pitch * bpp * samples)` because the header has
no height field. **That derivation returns the PADDED height, not the live
extent**, and the pool has been creating over-tall render targets since R1.

**THE ARITHMETIC THAT RAISED IT.** An EDRAM tile is **80x16 pixels**. Every
derived pitch is an exact multiple of 80 and every derived height an exact
multiple of 16 — and the guest's downsample pyramid for a 720p title should be
720 -> 360 -> 180 -> 90. Our derived values are `ceil(h/16)*16` of exactly that:

| true | ceil to 16 | we derived |
|---|---|---|
| 720 | 720 | **720** |
| 360 | **368** | **368** |
| 180 | **192** | **192** |
| 90 | **96** | **96** |

**MEASURED AGAINST AN INDEPENDENT WITNESS** (`[S6_VP]`, which logs the viewport
beside the decoded header of the render target bound at that instant):

```
rt0=8B63F870  derived=640x368   viewport=640x360   *** MISMATCH ***
rt0=8B63F8B0  derived=640x368   viewport=640x360   *** MISMATCH ***
rt0=82986F60  derived=1280x720  viewport=1280x720  MATCH   (x10)
rt0=83A32560  derived=640x640   viewport=640x640   MATCH
```

⚠ The two that MATCH are exactly the two whose true height is ALREADY a multiple
of 16 (720 and 640), i.e. the cases where padding is invisible. The two that
MISMATCH are exactly where it is not. That is the hypothesis predicting which
rows agree, not merely fitting them.

⚠ **The correlation is deliberately read with care**: `SetViewport` and
`SetRenderTarget` interleave, so a dump can catch a viewport set while a
DIFFERENT surface is still bound (those show as spurious mismatches on the
primary), and the `.rdata` default viewport reads `0x7FFFFFFF` = unbounded. Only
lines where a real viewport lands on a freshly-bound target are evidence.

⛔⛔ **AND THE PITCH IS PADDED TOO — this is bigger than the height.** Fixing the
destination to use the guest's declared extent produced, MEASURED:

| destination created | from source | removed |
|---|---|---|
| 640x360 | 640x368 padded | 8 rows |
| 320x180 | 320x192 padded | 12 rows |
| **64x64** | **80x64** padded | **16 columns** |
| **16x16** | **80x16** padded | **64 columns** |
| **4x4** | **80x16** padded | **76 columns, 12 rows** |

An EDRAM tile is 80 wide, so the pitch of every small target was rounded up to
80. The surfaces §6g.9 recorded as "80x16 fmt 14 (k_32_FLOAT)" are really a
**16x16 and a 4x4** — i.e. the LUMINANCE REDUCTION CHAIN for auto-exposure/
tonemapping, which is exactly what a 4x4 single-channel float target is for. We
were resolving a 4x4 luminance target as 80x16: **20x too wide and 4x too tall**,
so any exposure value read back from it would be wrong by a large factor.

**CONSEQUENCE.** For every 360/180/90-tall surface the pool allocates 8/12/6 rows
too many, and for the small targets it was up to 20x too wide. Allocation over-size is harmless in itself; what is not harmless is
that the resolve copies the padded extent into the texture the guest samples, so
normalised sampling is stretched by 368/360 = **2.2% vertically** with garbage in
the surplus rows. Nothing we had built could see this — it is the exact
"subtly wrong and invisible" class the pre-bake audit flagged as unverified.

**THE FIX, and it is measured.** The destination is now created at the guest's
declared extent and the copy is a `CopyTextureRegion` with an explicit box
(`CopyResource` cannot express it — it requires identical dimensions, and the
whole point is that the padded source and the true destination differ). The box
takes the live region and leaves the padding behind, which is what the console's
resolve does. Debug layer stays 0/0/0 across the differently-sized copies.

⚠ **REFUSED, NOT GUESSED**: 2 of 14 destinations report a size dword of 0 (keys
`0C04A000`, `0C04B000` — both 80x16-class sources). Those resolves are refused
with `BadDestSize` and counted (~6,355/run) rather than falling back to the
padded source size. **OPEN**: why those two read 0 at `+36` when their siblings
read a valid `size_2d` — possibly a non-texture destination, possibly a header
we have not located. Do not paper over it with the source extent.

✅ **THE SURFACE ITSELF CARRIES ITS TRUE EXTENT, AT `surface+36`.** §6g.9's "there
is no height field" was wrong. PROVEN by the guest's own accessor,
`D3DSurface_GetDesc_sub_82100190`:

```c
if (*(u32*)surf & 0x40000000)            // surface OF A TEXTURE LEVEL:
    ...(*(u32**)(surf+24), *(u32*)(surf+28) >> 28)   // +24 = PARENT PTR, +28 = level
else {
    desc.Width       = (*(u32*)(surf+36) >> 18) + 1;          // 14 bits
    desc.Height      = ((*(u32*)(surf+36) >> 3) & 0x7FFF) + 1; // 15 bits
    desc.MultiSample = *(u16*)(surf+24) & 3;                   // matches §6g.9
}
```

⚠ **A 14/15 SPLIT, NOT the texture 13/13 split** — and that closes §6g.6's open
puzzle. "A 1280x720 primary decodes as 5753x8161" was the RIGHT FIELD read with
the WRONG SPLIT. Same dword, correct answer under the surface layout.

⛔ **AND FLAGS BIT 0x40000000 IS A LANDMINE.** For a surface created over a
texture level, `+24` is a POINTER and `+28` is `level<<28` — decoding them as
RB_SURFACE_INFO / RB_COLOR_INFO reads a pointer as a pitch. Refused
producer-side. ⚠ Measured flags on the 15 live surfaces are `80000004` and
`00100004`, i.e. **the guard is currently UNEXERCISED** — an untested branch,
not a validated one.

⚠ **`sizeWord == 0` IS A LEGAL 1x1 SURFACE, NOT AN UNREADABLE ONE.** A 1x1
encodes `(w-1)<<18 | (h-1)<<3 == 0`, and this game HAS one — the last step of the
auto-exposure chain. Treating 0 as an error refused a real surface and ~6,355
resolves per run. Unreadability now has its own sentinel (`0xFFFFFFFF`, decoding
to 16384x32768, which no real surface can be).

**MEASURED after the fix — every surface, from its own extent field:**

| TRUE | padded | what |
|---|---|---|
| 1280x720 | 1280x720 | primary HDR, depth, LDR composite |
| 640x360 | 640x368 | x3 — the MRT pair and the fmt-6 target |
| 320x180 | 320x192 | x2 |
| 160x90 | 160x96 | |
| **64x64 / 16x16 / 4x4 / 1x1** | 80x64 / 80x16 / 80x16 / 80x16 | **the auto-exposure luminance chain** |

**10 of 15 surfaces were sized wrong.** The destination's own `size_2d`
(`*(pDestTexture+36)`, 13/13 — textures DO use the 13/13 split) remains the right
source for the resolve DESTINATION, and both are now used: the surface for the
render target, the texture for the resolve target.

### 6g.9 SURFACE DIMENSIONS — RB_SURFACE_INFO [GOLD, iter-446; ⛔ ITS 'NO HEIGHT FIELD' CLAIM IS WRONG — see §6g.12]

§6g.7 closed with "dimensions still need RB_SURFACE_INFO / the viewport". Both
turned out to be right, and the header carries what is needed.

**`surface+24` is `RB_SURFACE_INFO` (Xenos reg 0x2000).** Proven the same way
§6g.7 proved `+28`, by the register-shadow index the guest mirrors it into:
`D3DDeviceSetRenderSurfaceTileConfig_sub_820F0170` writes `surface+24` to
`dev+10240` — index **2560** — one below the **2561** that `surface+28` goes to.
Adjacent registers, adjacent header fields. Confirmed INDEPENDENTLY by
`D3DDeviceResolve_sub_8210C5F8`, which reads `*(WORD*)(surf+24) & 3` as the MSAA
mode: bits 16-17 of the dword, i.e. the big-endian HIGH half, exactly where
`RB_SURFACE_INFO` puts `msaa_samples`.

| field | where | meaning |
|---|---|---|
| `surface+24` bits 0-13 | RB_SURFACE_INFO | `surface_pitch`, in **pixels** |
| `surface+24` bits 16-17 | RB_SURFACE_INFO | `msaa_samples` (0=1X, 1=2X, 2=4X) |
| `surface+28` bits 16-19 | RB_COLOR_INFO | format (§6g.7) |
| `surface+28` bits 0-11 | RB_COLOR_INFO | EDRAM base, in tiles |
| `surface+44` | — | the surface's size in **bytes** |

⛔⛔ **"THERE IS NO HEIGHT FIELD" WAS WRONG — CORRECTED IN §6g.12.** There is one,
at **`surface+36`**, and the guest's own `D3DSurface_GetDesc_sub_82100190` reads
it there. Everything below describes the DERIVATION that was used instead, which
returns the EDRAM-TILE-PADDED extent and is right only on tile-aligned surfaces.
It is kept because `paddedRows` is still how the padding is reported — but it is
NOT the extent. Read §6g.12 before using any of it.

RB_SURFACE_INFO carries pitch and MSAA only, and the tile-config path resets the
scissor to `0x7FFFFFFF` and defers to the viewport. The old derivation was:

```
samples = 1 << msaa
height  = bytes / (pitch * bytesPerPixel * samples)
```

⚠ **The sample count is part of the footprint** — omitting it multiplies the
height by up to 4. MEASURED: surface `83A091A0` reads `pitch=640 msaa=2
bytes=0x640000`, which divides to **2560** rows without the divisor; the
viewport AND the scissor set on that same surface in the same instant both read
**640** = 2560/4. Two independent instruments against one derived number.

**MEASURED, natural K=1 roll, `SR_S6_RTCENSUS=1` — 15/15 surfaces decode valid**
and form exactly the downsample pyramid §6g.4 predicted:

| surface | decoded | fmt | note |
|---|---|---|---|
| `82986F60` | 1280x720 | 3 `k_2_10_10_10_FLOAT` | the HDR primary (§6g.7 agrees) |
| `82986F20` | 1280x720 | depth 1 `k_24_8_FLOAT` | edram base 0x2D0 |
| `83A25770` | 1280x720 | 0 `k_8_8_8_8` | the LDR composite target |
| `83A091A0` | 640x640 | 4 `k_16_16` | **msaa=2 (4X)** — the divisor case |
| `8B63F8E0`/`920`/`960` | 640x368 | 6 / 0 / 0 | MRT pair at edram 0x5A0 / 0x658 |
| `83A6FFB0` / `83A19770` | 320x192 | 12 / 0 | |
| `83A1EFD0` | 160x96 | 0 | |
| `83A653A0` | 80x64, `8B63F8A0`/`83A6EE20`/`83A65290` 80x16 | 14 `k_32_FLOAT` | |

**⚠ THE FORMAT NIBBLE IS NOT STABLE ACROSS THE CALL — and the pairing is
benign.** The boot census read the primary `82986F60` as `fmt=3`; the surface
pool, reading the same surface from the mirrored command, read `xfmt=12`.
Neither is wrong: `SetRenderTarget` READ-MODIFY-WRITES the nibble for the
`{2,3,10,12}` family, and its arithmetic is `newFmt = 2 * (fmt + 3)` — so
**2 → 10** (`k_2_10_10_10` → `_AS_10_10_10_10`) and **3 → 12**
(`k_2_10_10_10_FLOAT` → `_AS_16_16_16_16`). Both members of each pair are the
same data at the same width, so both map to the SAME DXGI format
(2/10 → `R10G10B10A2_UNORM`, 3/12 → `R16G16B16A16_FLOAT`) and a host resource
created from either side of the rewrite is identical. Do NOT "fix" one of them
to match the other.

**The viewport is a second, independent dimension source** and is now decoded
producer-side too (`D3DVIEWPORT9` = `{DWORD X,Y,Width,Height; float MinZ,MaxZ}`;
`RECT` = `{LONG l,t,r,b}`). Both used to be published to the render thread as
RAW GUEST VAs — unusable by construction. Note the default viewport/scissor at
`.rdata` `820B3910` reads `0x7FFFFFFF x 0x7FFFFFFF`: that is "unbounded", not a
dimension. ⚠ OPEN: the scratch viewport at `ABE76520` reports `minZ`/`maxZ` of
`-0.0`/`-0.0` while the one at `ABE764C0` reports a proper `0.0`/`1.0` — not yet
explained, and not blocking (0..1 is the safe default).

### 6g.8 CLEAR IS A WRAPPER, NOT A PEER — and BOTH clears skip a GPR for `Z` [GOLD, iter-445; ⛔ CORRECTED iter-446, 2026-08-05]

`D3DDeviceClear_sub_82101998(dev, Count, pRects, Flags, Color, Z=f1)`, from the
decompile:

```c
vupkd3d128 Color -> v31 (float4, via the scale/bias pair at 0x820BA520/0x820BA530)
if (!pRects) return D3DDeviceClearF_sub_82101A58(dev, Flags, 0, v31, Z);
do { D3DDeviceClearF(dev, Flags, rect, v31, Z); rect += 16; } while (--Count);
```

⛔ **TWO CORRECTIONS, both of which change how the clear slice must be built.**

1. ⛔ **THE STENCIL ARGUMENT EXISTS — iter-445 GOT THIS HALF WRONG AND SHIPPED
   THE WRONG HALF AS GOLD.** It correctly rejected `r8` (it held a guest
   pointer, `0x829A165C`) but drew the wrong conclusion from it. **Under the
   PPC64 ABI a float/double argument travelling in an FPR STILL CONSUMES ITS
   POSITION IN THE INTEGER PARAMETER SEQUENCE.** Both clears take `Z` in `f1`,
   so both skip exactly one GPR:

   | | | | | | | `Z`=f1 skips | Stencil |
   |---|---|---|---|---|---|---|---|
   | `Clear` | r3 dev | r4 Count | r5 pRects | r6 Flags | r7 Color | **r8** | **r9** |
   | `ClearF` | r3 dev | r4 Flags | r5 pRect | r6 pColor | — | **r7** | **r8** |

   PROVEN from the recompiled prologues, not the decompiler and not the XDK
   header: `ClearF`'s prologue does `mr r20,r8` (latching r8 into a
   callee-saved register) and **never touches r7**; the `Clear` wrapper does
   `mr r27,r9` and then `mr r8,r27` before EACH tail call — it moves its own
   `r9` into `ClearF`'s stencil register.

   ⚠ We published `ClearF`'s stencil from `r7` for the whole of iter-445 and it
   read as **0 and 42** — exactly the "plausible small integer" trap this very
   section names. Live re-measure after the fix: stencil reads 0 on every
   sampled clear. **GENERALISE THIS**: any guest function with a float/double
   parameter skips the corresponding GPR. Check the prologue, always.
2. **`Clear` TAIL-CALLS `ClearF`.** It is a wrapper that unpacks the D3DCOLOR to
   a float4 and forwards, once per rect. Because our hooks DELEGATE, **every
   guest `Clear` re-enters our `ClearF` hook**. An implementation must therefore
   live in `ClearF` and ONLY there — arming both would DOUBLE-CLEAR every
   wrapper call.

   ⚠ This REFINES the Phase-0 note "Clear needs BOTH hooks": true for
   OBSERVATION (the title calls both directly), false for IMPLEMENTATION.

3. **THE FLAGS ARE THE 360 LAYOUT, NOT THE PC ONE**, and `ClearF` is what proves
   it: it splits `Flags & 0xF` for the target clear and `Flags & ~0xF` for the
   depth/stencil clear, invoking `D3DDeviceClearImpl_sub_821013D0` separately
   for each half. ⛔ **CORRECTED iter-467 native-b (6g.8c): only on the
   predicated-tiling path. Untiled, `ClearF` calls ClearImpl once with all the
   flags.** The bit layout below stands.

   | bits 0-3 | bit 4 (0x10) | bit 5 (0x20) |
   |---|---|---|
   | `TARGET0..TARGET3` | `ZBUFFER` | `STENCIL` |

   ⚠ MEASURED live: `0x3F` (all four targets + Z + stencil) and **`0x30`
   (depth+stencil ONLY, no target)**. Under PC D3D9 values
   (`TARGET=0x1, ZBUFFER=0x2, STENCIL=0x4`) `0x30` ANDs to **zero**, so every
   depth-only clear would be silently dropped by a "correct-looking"
   implementation. The wrapper forwards its flags unchanged — observed live as
   `Clear(wrapper) flags=0x30` immediately followed by `ClearF flags=0x30`.

MEASURED per 60-present fixture trace: `Clear=37`, `ClearF=185` — so ClearF
carries both its own direct calls and the wrapper's forwards.

### 6g.8a THE CLEAR'S GPU FORM: one shader pair, rect lists, a colour clear written as depth [VERIFIED, iter-467 native-b, 2026-10-07]

From the retail decompile (IDA) and the lg1 trace (`archive/p3_gptrace3/live_454107ED_3400.xtr`):
- `D3DDeviceClearImpl_sub_821013D0` emits two IM_LOAD_IMMEDIATE packets: VS = the 27 dwords at
  `0x820BA0D0` (XXH3-64 of the BE bytes `0A6D1DD7767FDF27`), PS = the 9 dwords at `0x820B59DC`
  (`2E372EA28CC404B7`). The 9 rect-list clear draws of lg1 frame 0 all use this pair.
- `D3DDeviceEmitClearRectVertices_sub_82100A28` writes 3 vertices a rect into the command buffer
  (`GpuCmdBufAllocate_sub_820FCDF0`, 7 floats: x, y, z, r, g, b, a), points vertex fetch 0 at them
  (type 3, endian 8in32), sets RB_SURFACE_INFO / RB_COLOR_INFO / RB_DEPTHCONTROL / RB_MODECONTROL and
  draws an auto-indexed RECTLIST (VGT_DRAW_INITIATOR prim 8, 3n indices). x / y are surface pixels
  minus 0.5 when the device's pixel-centre flag is clear (lg1: -0.5 .. 639.5).
- `D3DDeviceClearSurface_sub_82100F70`: depth + stencil clears use RB_DEPTHCONTROL `0x8777` (z
  ALWAYS + write, stencil ALWAYS, z-pass REPLACE) and RB_STENCILREFMASK `0xFF0000 | stencil`. A
  COLOUR clear of a 32 bpp format (not 5/7/15) whose rect is at least one tile packs the colour
  (`SurfaceClearColorPackByFormat_sub_8210C090`) into a depth value + stencil byte and writes it as
  DEPTH (RB_DEPTH_INFO = the colour surface's base, mode 5).
- **The pack itself, `SurfaceClearColorPackByFormat_sub_8210C090` [VERIFIED iter-467 p2: L6 golden-call vs the
  Xenia-mode interpreter, 44 edge vectors per format; every clear in 44 traces].** r3 = the EDRAM colour format
  (jump table `byte_820BA2A0`), v1 = the colour (x, y, z, w), r5 / r6 = the out words, returns 32 / 64.
  - **fmt 6 `k_16_16_FLOAT`** (`loc_8210C3A4`) runs `vpkd3d128 v0,v1,3,1,3`: type 3 FLOAT16_2, i.e. word =
    `half(y) << 16 | half(x)` after the body's halfword swap, with Xenia's `float_to_xenos_half` (truncate,
    denormals -> 0, `|v| >= 0x47FFE000` incl. inf / NaN -> 0x7FFF, sign kept).
  - XenonRecomp has no type-3 lowering (the pristine output traps; this is the binary's only type-3 site).
    The old hand splice packed it as D3DCOLOR, which maps every colour below 3.0 to 0: 2/44 vs Xenia.
    `SR_SPLICE_VPK_F16` (default ON; `=0` = the old body) gives 44/44. The ONE predicate is
    `sr::kernel::spliceVpkF16()` (kernel/guest_context.hpp): the splice and every native port of the callers (strand
    B's `rs_colorPack`) read it, so record mode and the guest body cannot disagree. Cases:
    `scripts/difftest/golden_call/gen_vpk_f16_cases.py`. (The bake's flicker gates `archive/v1_checkroll/p2vpk` and
    `p2vpk3`, cited as PASS in d20953cd / 10fd3d3d, FAIL under the gate's world rule: their spans are black, scene
    luma 0.1 / 0.3; they predate the startup-race attach line (6g.8d), so the race is the likely but unproven cause.
    The splice rests on the L6 and census evidence here, not on them.)
  - **Consumers.** ClearSurface's depth-written fast clear of the soft-particle max/min RT (EDRAM `0x5A0`, 6g.8b),
    twice a frame. In every trace (8 Xenia, 36 of ours: FMV, title, menu, tutorial, frozen-meteor IGC) its colour
    is (0,0,0,1) or (0,0,0,0), so the word is 0 with either body: vertex z `0xB1800001`, stencil ref 0.
    RB_COLOR_CLEAR is never written for a fmt-6 surface. **The fix is latent in all 44 traced scenes**; it
    matters for any clear with x or y nonzero (mods, unvisited scenes).
  - **fmt 7 `k_16_16_16_16_FLOAT`** (`loc_8210C3C8`, type 5 FLOAT16_4) uses XenonRecomp's own lowering. It
    truncates, keeps denormals and saturates above 65504, where Xenia rounds to nearest even: 18/44 vs Xenia.
    The traced clears never reach it: the fast clear skips 64-bpp formats, and the traces' resolve clears all have fmt 0 / 4 sources. The binary's other type-5 site is `sub_823AE810`,
    called by `TexEncode_Float16x1_sub_823A8BD8` / `TexEncode_Float16x2_sub_823A8C68`. It is an open emitter item.
- `D3DDeviceClearRectTileSplit_sub_82100D20` splits the rect: the tile-aligned interior is drawn
  through a 4x-MSAA view of the same EDRAM at half the pitch (coordinates >> 1, RB_SURFACE_INFO MSAA
  bits set), the remainder as 1x strips (lg1 #2969: rows 352-360 of a 640x360 surface).
Consequence for the EDRAM model: every clear re-keys its surface (C -> Dm -> C), which is where most
ownership transfers of a frame come from. `SR_NB_CLEAR_ELIDE` (docs/NATIVE_ROADMAP.md B1a) uses this
contract.

### 6g.8c THE ROW-9 XDK ENTRIES AS PORTED: the shared dirty flush, ClearF's real flag handling, the GPR flush [VERIFIED, iter-467 native-b, 2026-10-09]

From the asm (IDA) and the recompiled bodies, ported to host code in `kernel/native_rec_b.cpp` +
`kernel/nb_rec_{resolve,clear,gpr}.inc` (P1 row 9). The dry CHECK (`SR_NB_REC_CHECK=1`) runs each port
on a device copy before the guest body and compares the ring bytes, every device dword and the guest
writes; its result is quoted in docs/NATIVE_ROADMAP.md (strand B, row 9).
- **One dirty flush, three copies.** Resolve `8210CA58`, ClearImpl `82101554` and ClearFullSurface
  `82100498` run the same instruction sequence: if `q10` (`dev+0x10`) has bit `0x100`, HiZ through
  `821072F8`; then the type-0 runs of `82107520` for `q10` bits 0..11 (`0x2200`, shadow `0x28B4`), `q10`
  bits 44..48 (`0x2180`, `0x28A0`), `q18` (`dev+0x18`) bits 2..22 (`0x2100`, `0x284C`), 23..38 (`0x2000`,
  `0x2800`), 39..59 (`0x2280`, `0x28E4`) and `q20` (`dev+0x20`) bits 0..37 (`0x2300`, `0x2938`), each mask
  cleared after its run.
- ⛔ **RB_HIZCONTROL `0x2203` IS WRITTEN TWICE when its dirty bit is set.** `821072F8` writes
  `{0x2203, hiz}` and returns `q10 & ~0x100`, but the caller ORs that return back into its unchanged `q10`
  (`or`, not a store of the return), so bit `0x100` survives and the `0x2200` run writes `0x2203` again
  from the shadow. Harmless on hardware (same value), but a byte-exact model must emit both.
- ⛔ **CORRECTS 6g.8 point 3: `ClearF` does NOT split the flags on the normal path.** Untiled it calls
  `D3DDeviceClearImpl_sub_821013D0` ONCE with all the flags. ClearImpl clears each requested and bound
  colour target through `ClearSurface` (`82100F70`), and depth/stencil ride on the first of them (`Flags &=
  ~0x30` after it). With no colour target requested it calls `ClearSurface(-1)` for depth/stencil alone.
  With no DS bound it drops `0x30` first. The `Flags & 0xF` / `Flags & ~0xF` split exists only on the
  predicated-tiling per-tile path (`82101C2C..82101DC4`), which this game never takes (6g.4: 0
  BeginTiling).
- ⛔ **`D3DDeviceClearFullSurface_sub_82100488` IS A GPR FLUSH, NOT A CLEAR.** Its only caller is
  `SetShaderGPRAllocation` (IDA `GpuRingEmitRegisterBlock_sub_820F5570`, r5 = VS GPRs, r6 = PS GPRs).
  - 820F5570 stores `v = ((r5 & 0x7F) << 12) | ((r6 & 0x7F) << 4)` (0 -> `0x40400`) at `dev+0x2A28`, then
    writes `{0x05C8: 0x20000}`, eight `{0x0D00 SQ_GPR_MANAGEMENT: v | 1}` and `{0x0D00: v}`, then calls
    82100488.
  - 82100488 runs the shared flush, loads the clear shader pair (6g.8a) and the default state block
    `8210C518`, and sets RB_DEPTHCONTROL 0 and RB_COLOR_MASK `0x2104` 0. It then makes two passes, each
    drawing 12 auto-indexed single points (`DRAW_INDX_2 0x10081`) from a zeroed 7-dword allocation, and
    finally restores the window scissor.
  - With the colour mask and depth control at 0, no surface is touched. The draws only push the new GPR
    split through the shader pipe.
  - 820F5570's callers: InitDefaultRenderStates, MovieCapture, RenderScriptRunTechnique. It is ~80 of the frame's
    CP draws (NATIVE_ROADMAP census), none of them visible.

### 6g.8d EYE ADAPTATION: the log-average luminance chain and its self-sustaining history [VERIFIED, iter-467 native-b, 2026-10-09]

Source: our live traces (`xtr_draws.py --resolves`, `--draw N --textures`), the cached DXIL of the passes
(`data/shader_cache/dxil/*.ps.dxil`, `dxc -dumpbin`), and the black-world hunt (NATIVE_ROADMAP "P1 row 9", job55-63).
- **The chain.** Each frame, after the scene and the particles:
  1. PS `1E70EB95` downsamples the HDR scene texture (1280x720 `k_16_16_16_16_FLOAT`, resolved from the 7e3 RT) to
     a 320x180 fmt-3 RT. That RT is resolved to a 16_16_16_16_FLOAT texture.
  2. Four `k_32_FLOAT` passes follow: PS `651FD4D1` (64x64), `B39672E7` (16x16), `28113F6D` (4x4), `AA511729`
     (1x1). Each is resolved to its own texture.
     - The first takes `log2(|L| + 1e-4)`, clamped to +-FLT_MAX, over 16 taps.
     - The rest sum 16 taps.
     - The 1x1 result is the frame's average luminance.
  3. PS `040D6633` is the adaptation: `out = hist + (cur - hist) * (1 - exp2(-0.87 * c))`. Here t0 = the frame
     average, t1 = the history, and c is one cbuffer constant. Its 1x1 result is resolved back over the history.
- **Addresses (2026-10-09 dev layout).** History 1E246000, frame average 1E247000, 4x4 1E248000, 16x16 1E249000,
  64x64 1E24B000, 320x180 input 1C2AC000, scene texture 1CDEC000. Older builds differ (the P2 agent: 1E306000 /
  1E307000 on 740f0768). Find them through the adaptation draw's t0 / t1: `archive/v1_nativeb/xtr_adapt.py`.
- **The history is read BEFORE it is written each frame**, and nothing ever resets it. A single non-finite frame
  average therefore poisons it for good:
  - cur = +inf makes hist inf;
  - the next frame computes inf - inf = NaN, and NaN stays NaN.
  - Exposure NaN draws the 3D world black under an intact HUD ("the black world": owner and coordinator 2026-10-09;
    P2's traces 4200 / 4800 / 5400 / 6000 hold 0x7FFFFFFF at 1E246000; the drawn twin holds 0.159).
  - A NaN input to the 64x64 pass is harmless (the clamp makes it -FLT_MAX). A +inf input overflows the 16-tap sum
    to +inf.
- **FMV and logo frames run the whole post chain** (151 draw packets, 26 resolves; the adaptation is draw #111). So
  the history is live from boot. In the black bracket roll b62_1 it was already NaN at swap 3000 (an intro FMV
  frame), before the title menu, which draws through the same exe normally.
- **Xenia traces are no oracle for these values.** Xenia's trace writer records CPU memory for resolve targets, so
  every Xenia trace (fmv_eahd, menu, the two gameplay oracles) holds 0 at the history and the average (1EEF6000 /
  1EEF7000 in its layout).
- **ROOT CAUSE of the black world: a startup race, not the chain** (job64 / job65, 2026-10-09). main() starts the guest
  thread before `d3d12.init()` (6-8 s on this machine) and attaches the live backend after it (`setPm4LiveBackend`).
  - A CP that starts first parses the boot frames without performing a draw or resolve, and its draw-command queue
    (8192 entries) fills.
  - When it saturates (8,191 queued after 55 guest swaps), the late drain poisons the history from the first frames:
    - b64_2's traces at swaps 2-16 carried no backend state;
    - job65: black in 2 of 2 saturated rolls, drawn in the roll that attached at 3,024.
  - The fix: `SR_PM4_WAIT_BACKEND` (default ON) makes cpMain wait for the attach. That gives 0 swaps / 0 queued, drawn
    3 of 3, the same boot-to-title, and the EA logo movie whole.
  - check_roll guards it: `[DISPLAY] PM4 live backend attached: guest VdSwaps before it 0`.
  - 2026-10-14 (`render/boot-attach`, NATIVE_ROADMAP "Boot attach"): the shader cache load leaves the backend init.
    It loads on its own thread (init keeps the probe, one directory listing, and the thread's start); the CP waits for
    the installed cache before its first command (WP1), and every lookup waits for the stage its answer needs (WP2).
    The CP's wait for the attach has no time limit and never runs the ring without a backend: an explicit D3D12 init
    failure is FATAL (exit 3 and a message box); a late attach is waited for, with a Warn every 10 s and the total
    wait logged, in CP phase 5 (host wait). There the hang watchdog sets aside only the CP's own stall trigger (H3):
    H0-H2 fire as usual, their [GPU_HANG] report annotated with the wait. It escalates a wait of 60 s, then every
    5 min, as a `[HOST_WAIT_STALL]` block (a milestone tag) naming the backend init's step and thread and the shader
    loader's progress.
  - Open (ledger, 2026-10-09): in job67's span roll the CP waited 23.2 s, boot-to-title took 49.8 s and the first
    movie logged as a 1-frame GAP with the next at 1.27x media speed (`archive/v1_nativeb/fg67_race.txt`); movie
    pacing after a long backend wait is unexamined. That roll's flicker-gate span landed on the title, not in play.
- **Host half semantics (a separate, open question; not the black world's cause).**
  - Xenia samples `k_16_FLOAT` / `k_16_16_FLOAT` / `k_16_16_16_16_FLOAT` as IEEE half, as we do (d3d12_texture_cache.h:
    raw copy). An exponent-31 half is inf / NaN on the host, while Xenos 16-bit floats are finite up to 131008.
  - Guest memory near these targets holds byte-repeated garbage: 0xFFFF / 0xFEFE / 0xFCFC halves in the 0x5A0 resolve
    region.

### 6g.8e CHAPTER-18 STORM LIGHTNING: a guest-side light-rig flip that looks like an inversion [VERIFIED vs Xenia, lane H lightning, 2026-10-17]

- **What it is.** In the chapter-18 storm the guest writes a second light rig in ~1 s bursts every 5-14 s: the world PS
  `63FE9C492BBE69EA` light rig (gKey1-3, gMid4, gFil5-7, gEnvMapTint, gKADkmf, gOverBright) and VS `gLightPos` (c230,
  mirrored: x +-9.8e4, z -+3.0e4) blend toward the lightning rig and back several times per burst, beside LIGHTNING_xx
  sound enqueues (forcing lane 13b ledger, `_worktrees/forcing/archive/v1_forcing/HANDOFF.md`). On screen: warm dusk
  facades go near-black navy, glass curtain walls and Superman light up cool blue-white, for 1-16 presents at a time.
  Players read it as "the world colours invert" (owner 2026-10-11 and 2026-10-17).
- **It is the game.** Live Xenia (stock oracle build) at the same Midtown spawn, loading our ch18 seed: the same flip,
  bursts every 5-13 s, 0.5-1.1 s, block-luma correlation of a flash vs its neighbours 0.20-0.76 (ours 0.25-0.82 with
  SR_MEM_RESOURCES=1, 0.29-0.79 with =0). Blind Astra: the same effect. Evidence `_worktrees/lightning/archive/v1_lightning`
  (lh_mr1, lh_mr0, xenia1), measured with `tools/perf/flash_scan.py --max-run 16`.
- **Sim-clocked.** At ~95 presents/s the rig hops ~19 times per 1.01 s burst whether the burst holds 61 or 162 presents
  (clight ledger, `clight_bursts.txt`): not re-rolled per render frame, so high frame rates do not speed up the strobe.
- **REJECTED (do not re-chase):** shader drops (hold 12), native rows (group B off), native resource routing (=0 flips
  the same), eye adaptation / exposure (the flip is spatial; a scalar exposure cannot swap lit and shadowed faces, and
  Xenia shows the same frames).

### 6g.8b SOFT PARTICLES: the half-res max/min depth RT, and the half-float pack it needs [VERIFIED, iter-467 freezefx, 2026-10-07]

From live traces of the frozen-meteor IGC (`archive/v1_freezefx/r2/traces/live_454107ED_57xx.xtr`,
Xenia trace dump + `tools/edram_bisect.py` draw by draw):
- The full-res depth (`k_24_8_FLOAT`, EDRAM 0x2D0) is raw-resolved to `1C77C000`.
- PS `059EEADD` (VS `871A3860`) downsamples it into the `k_16_16_FLOAT` RT at EDRAM **0x5A0**
  (the `8B63F870` surface of 6g.7): x = max depth, y = min depth; the SKY is 0.0. It resolves to
  `1E39A000`.
- The particle PS `9BAFE30B` draws at half res (MRT `k_8_8_8_8` at 0x5A0 / 0x658, z off) with
  `fade = saturate((maxD - particleD) * k)`, `oC0.w = alpha * fade`; PS `C6518757` / `AF8203A4`
  composite it onto the full-res scene.
- ⛔ **HALF-FLOAT PACK RULE.** Every GPU float32 -> float16 packer we compile with **FXC**
  (`D3DCompile`: `kPm4EdramHlsl FloatToHalf`, `native_resolve.inc NatFloatToHalf`) must compare
  its signed exponent against a DECIMAL bound. FXC types an unsuffixed hex literal as uint, so
  `int e; if (e >= 0x1F)` compiled to `uge` and every e <= 0 (0.0 and |f| < 2^-14) packed as
  +-inf (0x7C00). DXC (XenosRecomp, `shader_xlat_core`) compiles the same text as signed (`icmp
  sgt`). Symptom: the EDRAM store of the 0x5A0 RT turned the sky's 0.0 into +inf, so `fade` hit 0
  wherever sky was behind a particle, and the frost cloud was cut to the building silhouettes.
  Fixed iter-467 (`SR_PM4_HALF_SIGNED`, default ON; `=0` compiles the old compare): MAD vs Xenia
  6.82 -> 1.30 (trace 5776), 22.99 -> 1.75 (5740).
- ⛔ **QUAD-LIST SPLIT RULE (fixed iter-467, `SR_PM4_QUAD_STRIP`, default ON).** The particles are a
  `kQuadList` (prim 13) of camera-facing billboards whose corners the VS gives NON-AFFINE attributes
  (o1 = (dz/2, 1/dz, clip z, 1/w) for the soft fade) and, when `t1.z >= 0.5`, collapses corners
  closer than `c8.x` (3 units) to (0,0,0,0); others reach w < 0. With such inputs the quad's
  diagonal decides the interpolated fade. Xenia's D3D12 backend draws quad lists through a GS
  emitting the strip v0 v1 v3 v2 = triangles (0,1,3)(3,1,2) (pipeline_cache.cc kQuadList); we used
  (0,1,2)(0,2,3). Trace 5794 draw #3309 (5 quads straddling the camera): a homogeneous-barycentric
  simulation of the VS + PS fade predicts Xenia's touched pixels 100.0% with Xenia's split and
  our old output 99.2% with ours; the replay with the split moves 5794 from MAD 4.74 to 1.41.
  The same diagonal fixes non-planar cloth: the title-screen cape silhouette (menu fixture, 558
  px, error 24.6 -> 3.0). The vertex order inside each triangle does not matter (5 rotations,
  byte-identical). Inputs ruled out first: depth texture t15 equal to Xenia within 1 ulp (250 of
  230,400 words), t0 the same DXT5 that draws #3305/#3308 use with identical results, blend
  `010B0706` / alpha ref 0 / constants / vertices read from the trace, z test off.

### 6g.7 THE SURFACE HEADER'S FORMAT FIELD — surface+28 [GOLD, iter-445, 2026-08-04]

The open question left by §6g.6 ("render-target surfaces do not decode under the
texture layout — find the surface-header builder") is ANSWERED, and not by
finding a builder: `D3DDeviceSetRenderTarget_sub_820F2CA0` gives it away by
what it READS AND REWRITES.

**`surface+28` is the render target's COLOR/DEPTH INFO register** (Xenos
`RB_COLOR_INFO` class). SetRenderTarget proves it three ways: it MIRRORS the
dword into the device register block at `dev + 4*(v5+2561)`, `v5 = index ?
index+1 : 0`; it TESTS bits 16-19 against **{2,3,10,12}** — exactly the Xenos
`2_10_10_10` family that needs the `_AS_` variants — plus bit 19; and it
READ-MODIFY-WRITES only those bits back
(`*(surf+28) = (w & 0xFFF0FFFF) | newFmt<<16`).

| field | meaning |
|---|---|
| bits 16-19 | **FORMAT** — colour enum for RTs; ⚠ DEPTH surfaces use the DEPTH enum (0 `k_24_8`, 1 `k_24_8_FLOAT`), so never read a depth surface with the colour table |
| bits 0-11 | **EDRAM base**, in tiles |

**DECODED FROM THE LIVE `[S6_RTHDR]` DUMPS** (`hdr[7]` IS `surface+28`):

| surface | what | fmt | name | edram | bytes |
|---|---|---|---|---|---|
| `82986F60` | primary RT0 | 3 | `k_2_10_10_10_FLOAT` (**HDR**) | 0 | 0x384000 |
| `83A258F0` | 2nd full-size RT0 | 0 | `k_8_8_8_8` (**LDR/composite**) | 0 | 0x384000 |
| `83A32560` | 640-class | 4 | `k_16_16` | 0 | 0x640000 |
| `8B63F870` | — | 6 | `k_16_16_FLOAT` | 0x5A0 | 0xE6000 |
| `8B63F830` | small | 14 | `k_32_FLOAT` | 0 | 0x1400 |
| `83A26D90` | 320-class | 3 | `k_2_10_10_10_FLOAT` | 0 | 0x3C000 |
| `82986F20` `83A0A9A0` | depth | 1 | `k_24_8_FLOAT` (depth enum) | 0x2D0 | 0x384000 |

✅ **CROSS-VALIDATED FROM THE OPPOSITE DIRECTION.** iter-438c recorded, without
knowing the field's origin, that black-era draws targeted only "HDR RTs
(**colInfo 3/4**)" while all 2,317 display resolves read "the **8888 RT
(colInfo=0)**". Same field, same values. That also names the composite: the
game renders HDR and resolves to the `k_8_8_8_8` full-size target `83A258F0` —
which is exactly where the front end sends 56% of its UP draws (§6g.5).

⚠ STILL OPEN: the surface header carries no width/height in a decodable form
(a 1280x720 primary reads as 5753x8161 under the texture split). What IS
readable is this format field, the EDRAM base, and the byte size at `surf+0x2C`.
Dimensions must come from `RB_SURFACE_INFO` / the viewport.

### 6g.6 THE TEXTURE FETCH CONSTANT, FROM ITS BUILDER — AND WHY SURFACES ARE NOT TEXTURES [GOLD, iter-445, 2026-08-04]

Read off the assignments in `TextureFetchConstantBuild_sub_820FF668` (the
`XGSetTextureHeader` core), which writes the 24-byte fetch constant at
`hdr[7..12]` = hdr+28 — the block `SetTextureFetchConstant` lifts to
`dev+1024+24*slot`:

| dword | contents |
|---|---|
| `fc[0]` | low 2 bits = fetch TYPE (**2 = texture**, 3 = vertex) |
| `fc[1]` | FORMAT in bits 0-5; pixel base page above (`>>12`) |
| `fc[2]` | **the size field — layout depends on DIMENSIONALITY** (below) |
| `fc[4]` | `(mipLevels-1) << 6`, bits 6-9 |
| `fc[5]` | dimensionality bits 9-10; packed-mips flag bit 11 |

**`fc[2]` by dimensionality** (`v47`, from the resource type: 3/19 → 2D,
17 → 3D, 18 → cube, 20 → 1D):
* **2D and cube:** `(w-1)` bits 0-12, `(h-1)` bits 13-25, `(d-1)` bits 26+
* **3D/volume:** `(w-1)` bits 0-10, `(h-1)` bits **11-21**, `(d-1)` bits 22+
* **1D:** `(w-1)` bits 0-23 only

✅ This CONFIRMS the runtime census decode (`w = (fc[2] & 0x1FFF)+1`,
`h = ((fc[2]>>13) & 0x1FFF)+1`) — **for 2D only**. ⚠ It silently MISREADS volume
textures, which use 11-bit fields, and nothing currently reads the
dimensionality bits at `fc[5]` 9-10 to tell them apart.

⛔ **AND IT DOES NOT APPLY TO RENDER-TARGET SURFACES — do not reuse this decode
for the RT slice.** Measured `[S6_RTHDR]` dumps of real bound RT surfaces refuse
it: `82986F60`, a known 1280x720 primary, has `hdr[9] = 0x13FC1678`, which under
the 13/13 split reads as **5753 x 8161**. What IS readable is `hdr[11]` as a
plain byte size — `0x384000` = 1280*720*4 on both primaries, and `83A26D90`
`0x3C000` = 320*192*4 with the height tile-rounded 180→192. So `D3DSurface`
headers use a DIFFERENT layout from `D3DTexture` headers; the render-target
slice must locate the surface-header builder (`XGSetSurfaceHeader` / the
CreateRenderTarget path) instead of assuming. **OPEN.**

#### 6g.3c THE FETCH SHADOW RIDES IN THE SetRenderState PAYLOAD [GOLD, iter-457, 2026-09-01]

The SRHT `SetRenderState` payload is **1004 bytes / 251 big-endian dwords**, and
only the first 59 are the register runs (0x2100 run = 26 dwords, then 0x2200 at
index 26). **Dwords 59.. are the TEXTURE FETCH SHADOW: 16 slots x 6 dwords,
base 59, stride 6**, in sampler-slot order.

    fc[k] for sampler slot s  =  dword[59 + s*6 + k]

**Measured, not assumed:** at draw record 9587 all ten nonzero binds match on
`fc[1] >> 12 == boundKey >> 12` (10/10), and across 400 draws **4000/4000** bind
pages match. A wrong base or stride cannot produce that.

This makes texture DIMENSIONALITY readable offline from a trace alone, which the
`setTexture` record itself cannot give (it carries only `{device, sampler,
texture, dirty}`):

    dim = (fc[5] >> 9) & 3      0 = 1D, 1 = 2D, 2 = 3D, 3 = CUBE

Census over the frozen title fixture (`archive/iter456_title_fresh.srht`), bound
slots at admitted draws: **2D 48,383 / 3D 8,747 / CUBE 3,360**.

⚠ Use `fc[2]` for extents ONLY after reading this dimensionality — 2D and cube
pack 13/13, volumes pack 11/11/10, and nothing in the runtime reads the bits to
tell them apart (see the warning above this section).

### 6g.4 RENDER TARGETS, RESOLVE, AND THE FRAME'S PASS STRUCTURE [GOLD, iter-445, 2026-08-04]

**`D3DDeviceResolve_sub_8210C5F8`** — 360 has NO StretchRect; **Resolve IS the
RT→texture copy**, so it is the API-level end of every offscreen pass.
Args **verified against the recompiled PPC prologue** (`ppc_recomp.2.cpp`,
`__imp__sub_8210C5F8`), not assumed from the XDK header:
`(r3 dev, r4 Flags, r5 pSourceRect, r6 pDestTexture, r7 pDestPoint,
r8 DestLevel, r9 DestSliceOrFace, r10 pClearColor, f1 ClearZ, [SP+0x5C] ClearStencil,
[SP+0x64] pParameters)`.

⛔ **CORRECTED iter-467 native-b (2026-10-09), two points:**
- **Two more arguments, on the stack.** `ClearZ` in `f1` still takes GPR slot 8 (the 6g.8 rule), so
  `ClearStencil` (slot 9) and `pParameters` (slot 10) are past `r10` and come from the caller's
  parameter save area at `SP+0x14+8*slot`: **`SP+0x5C` and `SP+0x64`**. The recomp reads them as
  `r1+460` / `r1+468` after its `stwu r1,-368(r1)`.
- **A NULL `pClearColor` is a ZERO vector on the stack, not an .rdata default.** At `8210C68C` the body
  loads the float at `0x820BCB98` (= 0.0), stores it four times at `r1+176..188` and points `r21` there.

Both points are proven by the row-9 native port (`kernel/nb_rec_resolve.inc`) and its dry CHECK.

**SURFACE SLOT LAYOUT, proven by that same prologue** (it selects the source
with `Flags & 7`): **RT0..3 at `dev+12168+4*idx`** (`4*(3042+idx)`) and the
**depth-stencil surface at `dev+12184` = slot 4 of the same array**. `Flags & 7
== 4` therefore means "resolve the depth buffer". Flags `0x100`/`0x200` =
clear-RT / clear-depth on resolve.

**MEASURED (iter-445 census, natural K=1 200 s roll, `SR_S6_RTCENSUS=1`,
boot+FE+movie era):**
| number | value |
|---|---|
| RT binds | **33-35 / frame** (`setRT` 308K/run) |
| DS binds | **~12 / frame** |
| Resolves | **~13 / frame** |
| DISTINCT (index,surface) RT bindings, whole run | **16** |
| DISTINCT depth surfaces | **3** |
| DISTINCT resolve destinations | **16** |
| BeginTiling / EndTiling | **0 / 0 — the frame is NOT predicated-tiled** |

Consequences, all load-bearing for the HLE renderer:
- Resolves are **passes, not tiles** (no tiling), so ~13/frame is the real pass
  count. The pass graph is small and enumerable.
- **MRT is in use**: RT index 1 binds a real surface (`8B63F8F0`) and 4,086
  resolves have `Flags&7 == 1`. Slots 1/2/3 are also explicitly bound to NULL
  65K times each (MRT teardown).
- **Depth is resolved into a texture** 8,172 times/run (`Flags=0x00000014`).
- **Array/cube slices are in use**: consecutive resolves hit the same dest with
  `DestSliceOrFace` 0,1,2.
- Surface sizes run from a 1280-class target down to 80/128-class ones — a
  downsample/bloom pyramid. Raw surface headers for 16 distinct surfaces are
  banked in the iter-445 run log as `[S6_RTHDR]` (`w6` = packed dims, `fc` =
  the 6-dword fetch constant); **decode them in the slice that consumes them,
  against a known surface, rather than trusting an inline guess.**
- ⛔ **Every resolve destination is a texture the guest later SAMPLES.** We do
  not execute resolves, so those 16 destinations can only ever hold stale guest
  memory — general geometry that samples a post/shadow/reflection product is
  wrong *by construction* until the passes execute, no matter how correct the
  vertex path becomes.

### 6g.3 SHADER IDENTITY AT THE D3D9 API BOUNDARY [GOLD, iter-442, 2026-08-03]

**Proven live.** The shader identity computed from the bound shader OBJECTS
resolves to the offline DXIL cache — no ring, no PM4, no offline authoring.

Device shadow: **PS object at `dev+12416`, VS object at `dev+12420`.**
Container walk: header at `obj+0x28` (PS) / `obj+0x368` (VS); ucode pointer at
`obj+0x18` (PS) / `obj+0x20` (VS). The cache key is
`XXH3-64` over the **ucode section located by the container's shaderOffset table**
(`hdr+0x18` → `{physOfs, size}`) — **NOT** the `physicalSize` field at `hdr+8`,
which yields a different hash for the same shader (`70A2EA59B3006903` vs the
stored `20A5E5204FD23BE2`).

⚠ **Guard trap:** the original helper bailed out unless `shaderOffset < 0x1000`
and then fell back SILENTLY to the wrong `+8` span. Real containers carry
offsets into the thousands. Widen the bound, validate the table's size field,
and log which path was taken — a silent fallback produces a plausible-looking
hash that simply never hits the cache.

**Live movie draw (first draws of the boot, stable across a run):**
`prim=13 (quadlist), count=4, start=0` — issued through
`D3DDeviceEmitDrawVertices_sub_820FBBF8`. Recomp tail of
`RenderYuvVideoFrameTec0_sub_8235CA48`: `li r6,4; li r5,0; li r4,13`.
(An older comment claiming "prim=4, count=6" is STALE — it survived into two
independent analyses before the recomp source settled it.)
`vsObj=83A16DB0 → 8C029BAD1E4F75CD` (hit), `psObj=83A10C90 → 20A5E5204FD23BE2` (hit).

**The YUV shader's own plane contract** (read out of
`data/shader_cache/dxil/20a5e5204fd23be2.ps.dxil`): fetch slot **0 = Y**,
**1 = U/Cb**, **2 = V/Cr**; all three sample the same `TEXCOORD0.xy` and use
only the `.x` component, then apply BT.601 limited-range
(bias `-16/256` on luma, `-0.5` on chroma; rows `1.16438, 1.59603` /
`1.16438, -0.39176, -0.81297` / `1.16438, 2.01723`), alpha hard-wired to 1.0.
⇒ `DXGI_FORMAT_R8_UNORM` + the DEFAULT component mapping is correct; **adding an
R8→RRR1 swizzle would be a change with no consumer.**

### 6g.2 D3D9 TEXTURE RESOURCE MODEL UNDER DELEGATION [GOLD, iter-442, 2026-08-03]

Measured live (`[RESMODEL]` probe, 48 samples, movie planes). This is the contract the
HLE renderer must obey **because we DELEGATE to the guest's own D3D9** (every hook calls
`__imp__`). UnleashedRecomp *replaces* its D3D9, so its resource model — host-owned
staging, host-chosen 0x100-aligned pitch, "the host object IS the guest handle", no
VA→record table — is a consequence of replacement and **does NOT transfer to us**.

**Header** (`sub_820FFF08` return, 52 B) vs **pixel memory**: separate allocations.
Live: header `0x8E928BC0`, pixels `0x8F807000` — ~15 MB apart. The header is NOT a
container for the bits, so a **VA→record side table is mandatory** for HLE.

| Field | Meaning (verified) |
|---|---|
| `hdr[0]` bits 0-3 | resource type nibble (`3` for these textures) |
| `hdr[0]` bits 8-11 | **lock count**; `GpuResourceUnlockRestore_sub_820F3C18` flushes on the `0x100→0` transition |
| `hdr[1]` (+4) | **refcount** (confirmed 1 while held) — same offset as the reference |
| `hdr[5]`/`hdr[6]` (+20/+24) | per-lock dirty range, `{HIWORD=start, LOWORD=end}` in 128 B units, `0xFFFF0000` = empty |
| `hdr[7..12]` (+28) | the 24 B texture fetch constant `SetTextureFetchConstant_sub_82100310` lifts into `dev+1024+24*slot` |

**Fetch-constant dword0 encoding (decoded from live values):** bits 0-1 = type (`2` =
kTexture), **bits 22-30 = pitch in 32-byte units**, **bit 31 = tiled**. Live luma
`0x0A000002` → 40×32 = **1280**; chroma `0x06000002` → 24×32 = **768**; bit31 = 0 on
both. dword1 = `base | 2` — the SAME VA the Lock returns.

**The four rules this establishes (each one refutes a reference-derived assumption):**
1. **PITCH IS GUEST-FIXED — never host-chosen.** Luma 1280×720 → pitch 1280 (tight);
   chroma 640×360 → **pitch 768, not 640** (aligned up to 128 B). Upload at the
   *reported* pitch; a tight-pitch upload shears every chroma plane.
2. **These surfaces are LINEAR** (tiled bit clear), so the untiling path is not needed
   for movie planes. (Render targets are a separate question — do not generalise.)
3. **The pixels are the GUEST's allocation**, pointed at by both Lock and the fetch
   constant, persistent across frames (same `pBits` every lock). We upload FROM guest
   memory; we never own or relocate it.
4. **The guest does NOT report what changed on this path.** The dirty range read
   `0xFFFF0000` (empty) at every Lock *and* at every Unlock **sampled before
   delegating**, across all 48 samples. So Unlock must upload the full locked
   subresource (or hash it); there is no free dirty-span shortcut here.

Sanity anchor: once decoding starts the planes carry real video — Y ≈ `0x21` (dark),
U ≈ `0x7C`, V ≈ `0x78` (near-neutral 128) — so a probe reading zeros is sampling before
the first decode, not proving a black plane (cf. the §6g k_8 tooling trap).

**The movie-frame unit, MEASURED end to end** (iter-442 S4.2, `[S4_SNAP]`/`[S4_STAGE]`,
200 s roll): one movie frame = **exactly three plane Unlocks then one quad draw**.

| Plane | Create dims | Lock pitch | Snapshot bytes |
|---|---|---|---|
| Y (luma) | 1280×720 | 1280 | 921,600 |
| Cb | 640×360 | 768 | 276,480 |
| Cr | 640×360 | 768 | 276,480 |

Over the whole run **`snaps == 3 × movieQuads` exactly** (2,184 == 3 × 728) — the
invariant that makes the movie frame a well-defined render unit rather than an inference.
Plane handles are **not stable for the run**: 104 `TexCreate`s occur, so the VA→record
table must survive re-creation (a per-movie plane set, not one global triple).

**Movie quad VERTEX STRIDE = 36, resolved [iter-442 S4.3]** — this settles a conflict in
these very notes. The `SetStreamSource` example below (§ table, `stride 0x14` = 20 B) is a
DIFFERENT draw's stream; read at the movie draw itself from the device shadow
(`stride/4` byte at `dev+12256+s`, VB pointer at `dev[4*(3047+s)]`) it is **`0x09`×4 = 36**,
on 16/16 sampled movie draws: `vb0=8E928CB0 stride0=36 raw=09000000`. That matches the
iter-427 `[YUV_VB36]` layout exactly — **pos `float3`@0, uv `float2`@12 (V flipped:
(0,1)(1,1)(1,0)(0,0)), const `float4`(1,0,0,0)@20** = 12+8+16 = 36 — so the API path and
the old ring path agree, and the whole quad is **4 × 36 = 144 bytes**. Do not take a
stride from a `SetStreamSource` line without checking it belongs to the draw you mean.

**VERTEX DECLARATION FORMAT [GOLD, iter-443 S6c]** — object pointer at `dev+11536`,
element count at `+0x18`, element array at `+0x34`. **Elements are 12 BYTES, not 8.** The PC
`D3DVERTEXELEMENT9` is 8 bytes and that is where the wrong number comes from; the 360 form is
three dwords:

| dword | meaning |
|---|---|
| 0 | `stream << 16 \| byteOffset` |
| 1 | packed `D3DDECLTYPE` — low 6 bits are the Xenos format (`0x2A23B9`=FLOAT3, `0x2C23A5`=FLOAT2, `0x1A23A6`=FLOAT4) |
| 2 | `usage << 16 \| usageIndex` (0=POSITION, 5=TEXCOORD, 10=COLOR) |

Live movie decl (`decl=8E928B50`, nElem=3, 36 B):
`{0, 0x2A23B9, POSITION} {12, 0x2C23A5, TEXCOORD} {20, 0x1A23A6, COLOR}` -> stride **36**,
which reproduces the fetch-constant-measured stride from a completely independent route.

⚠ Sizing the capture at `nElem*8` **silently truncates the last element** — no error, just a
missing attribute and garbage geometry downstream. Always `nElem*12`.

**THE LOW 6 BITS ARE LITERALLY `ucode::VertexFormat` [GOLD, iter-444 S6e]** — not an
encoding that resembles it, the same enum. Verified against all three live elements:
`0x2A23B9 & 0x3F = 0x39 = 57 = k_32_32_32_FLOAT` (FLOAT3), `0x2C23A5 & 0x3F = 0x25 = 37 =
k_32_32_FLOAT` (FLOAT2), `0x1A23A6 & 0x3F = 0x26 = 38 = k_32_32_32_32_FLOAT` (FLOAT4). So a
declared attribute feeds `xeDecodeAttr` directly — no separate decl-format table is needed,
and none should be written. The remaining high bits (swizzle/sign/normalise) are NOT decoded:
signedness and normalisation are taken from the **translated VS's own `.vfetch` sidecar**,
because those describe how the *shader* interprets the fetch, not where the bytes are.

**THE DECL↔VS PAIRING RULE [GOLD, iter-444 S6e, first proven iter-435]** — the guest's vdecl
author order equals the translated VS's input-register order, so element *i* feeds input *i*,
and **the VS is authoritative for HOW MANY**. The movie quad declares THREE elements but its
VS signature has TWO (`[S4_PSO] sigN=2 stride=32`), so COLOR is simply never fetched. A decl
with FEWER elements than the VS consumes is *unknowable*, not merely lossy — refuse the draw.
Expanding the first N reproduced the previously hardcoded pos/uv mapping byte for byte
(`v0=(-1,-1,0,1) a1=(0,1)`), which is what retired the hardcode.

**THE D3D9 DEVICE SHADOW IS A DENSE 1:1 REGISTER ARRAY [GOLD, iter-444 S6f]** — verified
against the recompiled PPC, not inferred. `sub_82107520(dev, dirtyQword, baseReg, shadowPtr)`
computes `reg = baseReg + bitIndex` and reads `shadowPtr[bitIndex]`, emitting a PM4 TYPE0
header `((count-1)<<16)|reg` followed by consecutive shadow dwords. The run bases are proven
by the draw-time state flush in `sub_820FC000`:

| dev offset | Xenos reg | what, and how it was proven |
|---|---|---|
| `dev+10240` | `0x2000` | run base |
| `dev+10332` | `0x2104` RB_COLOR_MASK | four COLORWRITEENABLE setters write nibbles 0-3/4-7/8-11/12-15, each gated on its render target at `dev+12168/12172/12176/12180` |
| `dev+10376..10396` | `0x210F-0x2114` | `PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET}` — SetViewport writes `W*0.5 / X+W*0.5 / -(H*0.5) / Y+H*0.5 / MaxZ-MinZ / MinZ`. The `D3DVIEWPORT9` struct itself is shadowed separately at `dev+12376`, the scissor RECT near `dev+124xx` |
| `dev+10420` | `0x2200` RB_DEPTHCONTROL | three separate render-state setters write its documented bitfields (e.g. `sub_820F0B60` ZENABLE) |
| `dev+10424` | `0x2201` RB_BLENDCONTROL0 | the ALPHABLENDENABLE=FALSE path writes **`0x00010001`** (src=ONE, op=ADD, dst=ZERO, alpha the same `<<16`) here and to blend1/2/3 at `dev+10456/10460/10464` — the exact constant the HLE path currently hardcodes |
| `dev+10444` | `0x2206` PA_CL_VTE_CNTL | `sub_820F16C8` writes **1087 (0x43F)** = full viewport transform when enabled, **1024 (0x400)** = VTX_XY_FMT only when disabled, and simultaneously sets `PA_CL_CLIP_CNTL.clip_disable` (bit16) at `dev+10436` |
| `dev+1792` / `dev+5888` | `0x4000` / `0x4400` | VS / PS ALU float constants (see below) |

⚠ **`sub_820FC000`'s real signature is `(dev, primType, baseVertexIndex, startIndex,
indexCount)`** — r5 SIGNED, r6 in INDEX UNITS, r7 in indices. ✅ iter-449: the hook now has a
REAL PRODUCER (`SR_S6_IDX`, default OFF) that gathers vertex `(baseVertex + idx)` per index at
the hook and publishes genuinely-named fields (`start=0`, `count`=vertex count,
`vertBytes>0` = the executor's trust witness). ⚠ The gate-OFF census mirror still publishes
the RAW r5/r6 as start/count with `vertBytes=0` — those records stay refused by the
field-trust guard; anything else reading them for an indexed draw is reading the wrong thing.
The index buffer's address/format/endianness come from the IB object at `dev+12164`:
format = bit31 of `ib[0]` (set = 32-bit), endian = bits 29-30 (measured live: all 16-bit,
endian 1 = 8in16, across 2M draws/run), base = `ib[+24]` (a preformatted fetch-constant
dword0 — mask `& ~3`) + `startIndex * indexSize`. MEASURED refusal classes (full3 roll):
~33% of indexed draws carry the all-ones STRIP-CUT sentinel (restart-carrying strips — a
gathered stream cannot express a restart), ~24% reference a vertex range outside the
stream-0 fetch constant's declared size (multi-stream indexed geometry is the lead suspect).

**ALU CONSTANTS ARE BIG-ENDIAN IN THE DEVICE SHADOW [GOLD, iter-444 S6e]** — VS floats at
`dev+1792` (256 float4 = 4096 B, Xenos reg 0x4000), PS floats at `dev+5888` (224 float4 =
3584 B, reg 0x4400). They are guest CPU writes, so they are BE and must be dword-swapped
before the translated shaders read them; the PM4/xlat path never faced this because it read
already-host-order *register* values. Settled by probe, not derivation, because the wrong
direction is SILENT — it yields finite-looking denormals, not an error:
`raw c0 = -3.27e-33` vs `swapped c0 = 0.133333` (`0.133333f = 0x3E088889`, byte-reversed
`0x8988083E` = -3.27e-33 — an exact round trip, so the reading is not a judgement call).
The host-order files land at CB `+0` (VS) and `+4096` (PS), byte-identical to the layout the
xlat path already builds, so the same translated shaders read them the same way.

**VERTEX BUFFERS SPLIT EXACTLY LIKE TEXTURES [GOLD, iter-442 S4.3]** — and the split is
the trap. `dev[4*(3047+s)]` (what `SetStreamSource` stores) is a **resource HEADER, not
the vertices**: measured, its `+20` dword reads the same `0xFFFF0000` dirty sentinel as a
texture header and its `+8` dword counts up per frame. Reading vertices from it yields
garbage that decodes to a degenerate quad — i.e. a black screen with no other symptom.

The vertices come from the **VERTEX FETCH CONSTANT**, 2 dwords at `dev+1024+8*slot`
(the same fetch file as textures, which use 6 dwords at `dev+1024+24*slot`):

| Field | Encoding |
|---|---|
| `d0` bits 0-1 | type — **3 = kVertex** |
| `d0` bits 2-31 | **physical** base (mask `& ~3`); host VA = `0x80000000 \| (phys & 0x1FFFFFFF)` |
| `d1` bits 0-1 | endian — **2 = 8IN32**, so the guest's big-endian floats need a 4-byte swap |
| `d1` bits 2-25 | size in **dwords** |

Live movie quad: slot 95 = `{0BC868B3, 10000092}` -> phys `0x0BC868B0`, `(0x10000092>>2)&0xFFFFFF`
= 36 dwords = **144 B = 4 verts x 36**, matching the measured stride exactly. Prefer
**matching a kVertex slot whose declared size equals `count*stride`** over hardcoding 95 —
95 is one VS's binding, not a device-wide truth.

⚠ **The 0x8-window read is ALL ZERO** (iter-424/425 defect, still live): the guest wrote
this VB through an `0xA`-class VA and our harness does not fold physical windows. Reuse
the existing read-side accommodation (`SR_VB_ALIAS_PROBE`, default ON): if the `0x8`
window is zero, read `0xA0000000 | (va & 0x1FFFFFFF)`. Console holds identical bytes in
every window, so this is console-consistent; read-side only, so none of iter-373's
write-folding regression risk.

DECODED RESULT (the acceptance evidence): `(-1,-1,0|0,1) (1,-1,0|1,1) (1,1,0|1,0)
(-1,1,0|0,0)` — a full-screen NDC quad with **V flipped**, confirming the iter-427
`[YUV_VB36]` layout from the API path independently.

⚠ **`sub_820FFCC8` (Unlock) is passed ONLY the texture handle** — no pitch, no pointer.
The pitch/`pBits` pair therefore has to be **latched at the matching Lock**; at the one
moment a snapshot must be taken they exist nowhere else. (The reference never faces this:
it owns the staging memory, so its Unlock already holds everything.)

### 6g.2b THE UNHOOKED TEXTURE-CREATE PATH — `sub_82616C40` bypasses the identity registry [WITNESSED iter-456, 2026-08-30] [UNVERIFIED pending director]

The Section-F exact-object witness (natural forcing-OFF boot, 6520 events,
`archive/atlas_witness/k1-r5` in the jim-glyph worktree) partitions every Lock/Unlock in
the run: **all six unknown-identity textures — the run's ONLY S6/S7 `known=false`
objects — were created by `sub_82616C40`, none by the hooked TexCreate entry.**

- **`sub_82616C40(devHolder, w, h, levels, usage, fmt)`** = an IN-PLACE texture-header
  constructor: allocates a **56-byte** record via `sub_8279F290(heap, 0, 56)` (heap from
  `lwz r29,0(r25)` off arg0), computes the mip chain itself (halving w/h per level),
  masks usage bits. It NEVER passes through the API entry the HLE registers identities
  at (`tid::domain::registerCreate` in the TexCreate hook), so its products are
  invisible to the identity registry — their glyph-pixel Lock/Unlock writes classify
  unknown and draw-time binds resolve to the **white fallback**. This is the
  white-placeholder-boxes mechanism (H2 "unhooked create entry" from iter-455n),
  witnessed object-exactly.
- **Sole witnessed caller: `sub_822F9918`+296 (lr `0x822f9a40`,
  ppc_recomp.39.cpp:12865)** — the atlas-manager-class constructor site; 6 calls, 6
  textures (`0x83a59570 0x83a252a0 0x83a1c150 0x83a7ff80 0x83a09770 0x83a23380`).
- **Lock/Unlock dispatchers** (how the misses were caught): generic Lock
  `sub_8261BF60` reads type bits at `obj+48` (rlwinm 23,30,31): type3→`sub_820FFDA0`,
  type2→`sub_820FFF00`, else→`sub_820FFCB0` (call site lr `0x8261BFF4`). Generic
  Unlock = `sub_8260A590` (lr `0x8260A610`). Distinct from the 52-byte
  `sub_820FFF08` header path of §6g.2 — two different header shapes coexist.
- Hooked-path S1 creators seen the same run: `sub_8236A0B0` (early) and
  `sub_8277B0A8` (late, the `[APTTEX]` fn) — both register fine; the gap is ONLY the
  `sub_82616C40` path.
- **RESOLVED same day (iter-456 fix, live [TEX1640] census + recorded roll r2 —
  supersedes the "NOT yet proven" scope above):** the six are NOT the atlas pages —
  they are six 32bpp TGA UI textures (`TgaTextureCreate_sub_822F9918` reads a raw TGA:
  w/h little-endian at hdr+12/+14, pixels at `a4[*a4+18]`, byte-swaps 4*w*h, fmt dword
  0x18280186 = k_8_8_8_8; live dims 128x128, 64x64, 256x64, 16x16, 256x128, 64x64).
  **The GLYPH ATLAS is the ctor's SEVENTH product**: `[TEX1640] #10 128x128 lv=1
  fmt=04900002 (k_8) lr=82624F4C -> hdr=88ED7E10 pix=ABA5A000` — created via
  `D3DDynamicTexCreateInPlace_sub_82624EB0(outObj, w, h, fmtDword)` (vtable method,
  vt 0x820D78A0; device CS 0x8299B07C; stores header at outObj+8; call at +0x9C).
  That is THE long-hunted 0x88ED7E10 (Toby E4: "constructor NOT established" — now
  established) and its pixel key 0BA5A000 is the iter-455n replay-proven dialog-atlas
  key. **FIX (baked default-ON, `SR_HLE_TEX1640_REG`, =0 A/B):** the kernel override
  of `sub_82616C40` registers its returns in `s_texMap` identically to the TexCreate
  hook. Consequence chain, all witnessed live: atlas known -> k_8-eligible -> Lock
  latches (pitch 256) -> EVERY Unlock snapshots+uploads fresh under key 0BA5A000
  (iter-455n's live-producer leg, finally live) -> title-era text (press-start,
  copyright) renders as real glyphs on BOTH captures (GPU readback authoritative).
  The TGA six ride bind-time capture as KNOWN (honest TEXUNLOCK_SKIP). Ledger:
  `iter-456-tex1640-fix`. Still open: dialog-era text unexercised (no-input roll),
  TGA-six visual audit, backdrop/chroma classes unchanged.

### 6g.1 DECODED-FRAME RING + THE TWO FALSE-EOM PATHS [iter-440q audit, 2026-08-03]

**Ring object** (`v85` in the consumer; obtained via player[11] vt+68): `+48` item array
(60-byte stride, items are refcounted objects), `+52` capacity, `+56` rd cursor
(consumer-owned, plain stores), `+60` wr cursor (producer-owned, plain stores),
`+68` fill count (**lwarx/stwcx atomic**). Ops: `sub_822E1BA8`=read fill,
`sub_8247E178`=consume-peek items[rd] (+AddRef), `sub_8247E260`=rd++ & atomic fill--,
`sub_8247E1C8`=write-slot items[wr] (+AddRef), `sub_8247E2C8`=wr++ & atomic fill++
(**the publish**), `sub_8247E330`=reset all cursors.

**Race-immunity is PROVEN, not assumed** (three independent legs, iter-440q):
1. Producer order (`MoviePipeline_SubmitWorkerLoop_sub_82480290`): E1C8 → item vt+56(1) →
   decoderNode vt+72(node,item) → `buf`=item vt+84 → `rc`=node vt+120(node, **buf-8**, &more)
   → only `rc>=0` publishes via E2C8. **Attach precedes publish in the guest's own order.**
2. Our recomp emits `stwcx.` as `__sync_bool_compare_and_swap` (= `lock cmpxchg`, full fence
   + compiler barrier) — verified `ppc_recomp.59.cpp` `__imp__sub_8247E2C8`. x86 ordering here
   is STRONGER than console, so "add a release barrier" can never be the fix.
3. SPSC discipline: each cursor has exactly one writer; fill is CAS-updated on both ends.

⇒ If a null-sample item is ever dequeued, the defect is **upstream of the ring**: either
vt+120 returned success without attaching a picture (decoder warmup / starved demuxer
feed — the iter-364 family), or the item was recycled sample-less (vt+84==0 → the
iter-397e-l "buf-8 = -8 read as huge unsigned" finding is literally this line with a
NULL buffer), or the consumer re-read a slot via the drop path below.

**EOM `0x16660026` has TWO entry paths in `sub_82480D80`** — any probe must distinguish them:
- (a) **status path** `0x82481130`: `(player vt+260() & 6) != 0` → EOM **without ever
  consulting the queue** (a stop/error flag raised elsewhere, e.g. the audio side);
- (b) **null-sample path** `0x824810bc`: fill>0 → E178 → item vt[88]() == 0 with mode
  bit1 clear. A consumer-side probe on E178 alone CANNOT tell (a) from (b) — zero E178
  hits before an EOM means path (a), so the EOM site must also log the vt+260 status word.
- With mode==0 (bit0 clear — the game's own choice, iter-440o) an EMPTY queue never
  produces EOM at all: the pump loop at `0x82481054` spins vt+176 until fill>0. Starvation
  alone = hang, not EOM.

**AV-sync HOLD path** (`0x82481330` guard; ⛔ CORRECTED iter-464 - this paragraph used to
call it a "late >38 ms drop", the sign is the other way): returns WITHOUT calling
`sub_8247E260` — rd not advanced, fill not decremented — after item vt+72(item, sample-8),
when the frame is EARLY by more than 38 ms (or status bit 4). The same slot is re-offered on
the next call. A LATE frame is never dropped. The full pacing contract is §6g.1b.

### 6g.1b MOVIE PACING CONTRACT — one frame per render-loop pass, no catch-up [VERIFIED iter-464: IDA + live probe]

- **Who asks for frames, how often:** `RenderYuvVideoFrameTec0_sub_8235CA48` (the
  render-script `fmvmovie` pass, §6h natural chain) runs once per render-loop frame: Lock
  Y/U/V, `MoviePlayer_GetNextFrameThunk_sub_824707B0` -> player vt+76 =
  `MovieVideoDecodeYuvToRgbLoop_sub_82480D80` (desc mode 1, planar), Unlock x3, draw `tec0`.
- **The clock:** player[11] (+0x2C) is a `CCalChannel` (vtable 0x8204FFB8).
  `CalChannel_AdvanceClockGetDrift_sub_82476408` (its vt+0x4C): `now =
  CalChannel_GetQpcTimeMs_sub_824765C0` (QueryPerformanceCounter / (QPF/1000), i.e. mftb at
  the 50 MHz timebase, see the §6b iter-463 amendment `SR_TIMEBASE_50MHZ`), `clock(+0x120, u64 ms) += (now - last(+0x128)) *
  rate(+0x154, float)`, `drift(+0x138) = sampleMs(player+0x120) - clock`. A wall clock, not the
  audio clock: the movie audio and the video are both slaved to real time independently.
- **The decision, per call (after the planes are already copied):** drift > 38 ms -> HOLD (the
  frame is offered again next call, out-delay 0); else consume (`AdvanceRead`), present
  bookkeeping (player vt+124 / vt+200), and if drift > 5 ms `Sleep(drift - 5)` ON THE CALLING
  RENDER THREAD (`sub_82469870` -> `SleepExAlertable_sub_8246A898` -> KeDelayExecutionThread).
- **Consequence:** there is no late-frame drop. The video pace is min(the movie's frame rate,
  the render loop's rate). A render loop slower than the movie stretches the movie (every frame
  is shown, late); a faster one is throttled by the hold + sleep. The title movie
  (`fmvlegal.AST` @0x40, WMV3 1280x720, 934 frames, 30.0 fps constant, 31.13 s) therefore
  needs a >= 30 Hz render loop; the shorter logo movies are variable-rate (e.g. 106 frames in
  4.73 s) and hide a slow loop.
- **Oracle:** Xenia (canary_experimental@0bd090dbe, archive/v1_audio/oracle_fmv) calls the
  frame-get ~29.8 times a second through every intro movie; the title movie lasts 31.33 s
  EOM-to-EOM (935 calls).

## 6f. GUEST-INITIALIZED KERNEL DISPATCHER OBJECTS (KEVENT) [GOLD, iter-399, 2026-07-28]

**The title initializes kernel dispatcher objects itself, in guest memory, with plain stores —
no `KeInitializeEvent` import call.** A host-side event mirror created lazily at the first
`KeWaitForSingleObject` is therefore born with the WRONG state, and any signal the guest
established before that first wait is lost. This is a whole CLASS of platform bug, not one site.

`DISPATCHER_HEADER` layout at the object VA (16 bytes, big-endian):

| off | field | notes |
|---|---|---|
| +0 | `Type` byte | **0 = NotificationEvent (MANUAL-reset), 1 = SynchronizationEvent (AUTO-reset)**, 5 = semaphore |
| +1..+3 | `Absolute`, `Size`, `Inserted` | ⚠ NOT written by inline guest init — heap garbage; never test them |
| +4 | `SignalState` | **the source of truth**; 0/1 for events |
| +8/+12 | `WaitListHead.Flink/Blink` | **self-linked (== VA+8)** — the reliable structural type-marker |

Verified instance — `MoviePipeline_InitWorkerThreads_sub_82480598` inline-inits EIGHT events
embedded in the movie pipeline (which IS the player object returned by XMVCREATE):

- **Family A, work/stop events** at `+0x54/0x64/0x74/0x84`: Type=1 (auto-reset), SignalState=0.
  Waited via thunks `8247FAD0/FAE8/FB00/FB18`.
- **Family B, idle/ACK events** at `+0x94/0xA4/0xB4/0xC4`: Type=0 (manual-reset), **SignalState=1
  — PRE-SIGNALED at init.** Waited via `8247FB30/FB48/FB60/FB78` (vtable `0x82050758` slots
  +180/184/188/192); signaled via `8247FBD0..FC00` = `KeSetEvent(evt, 1, FALSE)`.
- All wait thunks tail-call import **`0x828EE45C` = `KeWaitForSingleObject(obj, 3, 1, 0, NULL)`
  = INFINITE**; the set family tail-calls **`0x828EE40C` = `KeSetEvent`**.
- Workers are created CREATE_SUSPENDED (`sub_828A7768`/ExCreateThread flags=4), entries
  `sub_8247F9D0/FA00/FA30/FA60`, handles at `pipeline[66..69]`, priorities via `sub_82469F28`.

**The bug this caused (iter-398f→h "movie-2 wedge"):** `MoviePipeline_ShutdownWorkerThreads_sub_82480888`
runs movie-1's teardown ON THE RENDER RUNNER'S TICK and waits family B ("is worker N idle?").
Those events are pre-signaled, so on console the waits return instantly. Our old kernel had never
seen the objects, auto-created a NON-signaled AUTO-reset host event at the first wait, and blocked
FOREVER — taking the render runner hostage, which killed every downstream render/technique pass.

**Host contract (implemented iter-399, `SR_KEVT_GUEST_TRUTH`, default-ON):** for objects passing
the structural marker, the guest `SignalState` word is authoritative — waits observe and (auto-reset
only) consume it, set/reset write it, and the host event is only a wake token that gets drained when
a wait is satisfied from guest state. Kernel-range VAs (0, `0xD00…` GPU fence slots), Nt handle IDs
and semaphores keep the legacy host-only path, because the GPU IRQ thread and VdSwap signal those
handles host-side. `KeInitializeEvent`/`KeInitializeSemaphore` now also write the real guest header
(`SR_KEVT_INIT_HDR`) — our old stubs touched no guest memory at all.

## 6i. SAVE STORAGE - the storage manager `*0x8299FB18` and the `rmcsave` package [iter-463; IDA + difftest, no Xenia save trace yet]

Host side: `kernel/xam_content.{hpp,cpp}` (SR_XAM_CONTENT_FS, default ON) and
docs/CUSTOM_FRONTEND.md section 8 part B. Evidence: IDA decompiles below, the play-session log
archive/v1_tape3/run.log (saves were never persisted: CreateEx was a generic stub and
`NtCreateFile "rmcsave:/Superman Returns"` -> FILE_NOT_FOUND), and
`difftest --xam-content-test`, which drives the real wrappers named here.

**Manager layout** (`mgr = *0x8299FB18`, verified in the disassembly):
`+0x8` user index; `+0xC` three 52-byte file slots (slot+0x2C in-use byte, slot+0x30
handle); `+0xA8` WIN32_FIND_DATA for the content file list; `+0x1E8` XCONTENT_DATA (0x134:
+0x1E8 DeviceID, +0x1EC ContentType, +0x1F0 wide display name, +0x2F0 file name); `+0x31C`
device id from the selector; `+0x32C` content enum handle; `+0x330` find handle; `+0x334`
FindFirst out-entry; `+0x338` FindFirst out-name slot; `+0x33C` XOVERLAPPED (selector only);
`+0x35C` kind-1 path prefix; `+0x360` {device id, kind, ?} copy (the golden dump has kind 0, so
the HDD always takes the XamContent path).

**Open** `SaveMgr_OpenSaveFile_sub_826446B0(mgr, devRef, req{+0 fileName, +8 -> wide name},
outFile, flags)`: flags 0x200 = create (fills XCONTENT_DATA: device = ref id or mgr+0x31C,
type 1, display + file name; content disposition 2 CREATE_ALWAYS, CreateFileA disposition 2),
else OPEN_EXISTING (3) with the XCONTENT_DATA the last enumeration left. Access 1 ->
0x80000000, 2 / 0x200 -> 0xC0000000. Mount = `sub_828A8218(user, "rmcsave", mgr+0x1E8,
flags)` -> `sub_828A8170` (returns 87 itself if DeviceID==0 or (flags&0x90)==0x90; ORs 0x10
for ContentType 1 without 0x80; copies its 9th argument pOverlapped to its own `[r1+0x54]`)
-> XamContentCreateEx. Then `CreateFileA_sub_824687F8("rmcsave:\<file>", access, 0, 0,
disp, 0x80)`; on failure `XamContentClose("rmcsave", 0)`. Mount rc map: 0 ok, 3 -> 9, 1110 /
1167 / 1617 / 1627 -> 2, 1392 -> 8, else 1. CreateFileA GLE map: (2 or 123) with
OPEN_EXISTING -> 9, 1110 / 80 -> 2, else 1.

**CreateFileA** (sub_824687F8): Win32 -> NT dispositions 1->2, 2->5, 3->1, 4->3, 5->4
(5 needs GENERIC_WRITE); access |= SYNCHRONIZE|FILE_READ_ATTRIBUTES (0x100080); CreateOptions
at `[r1+0x54]` (built from the flags: 0x20 synchronous, 0x40 NON_DIRECTORY in the common case,
0x1000 DELETE_ON_CLOSE plus DELETE access for FILE_FLAG_DELETE_ON_CLOSE 0x04000000); OA root -3,
attributes 0x40. Failure: SetLastError from the
status via RtlNtStatusToDosError, then 0xC0000035 -> 80, 0xC00000BA -> 3 or 5. Success GLE:
183 when CREATE_ALWAYS overwrote (IOSB.Information 3) or OPEN_ALWAYS opened (1), else 0.

**Write / read / close / delete**: `SaveMgr_WriteSaveFile_sub_82644C28` -> WriteFile
sub_824692B0 -> `NtWriteFile(h, 0, 0, 0, &iosb, buf, len, NULL)` (IOSB.Information = bytes;
short -> 1; 1110/1617 -> 2). `SaveMgr_ReadSaveFile_sub_82644CC0` -> ReadFile sub_824685C8:
must read exactly the requested nonzero size, else 10. `SaveMgr_CloseSaveFile_sub_82644B38`:
NtClose, then XamContentClose("rmcsave"), then XamContentSetThumbnail if the slot has one.
`SaveMgr_DeleteSaveFile_sub_82644D68`: mount OPEN_EXISTING + DeleteFileA sub_82469588
(`NtOpenFile(0x10000, share 7, options 0x4040)` + `NtSetInformationFile` class 13 {1} +
NtClose) + close. `SaveMgr_DeleteSaveContent_sub_82644F58`: XamContentDelete(user,
mgr+0x1E8, 0).

**Existence / listing**: `SaveContentFindFirst_sub_82645510` = XamContentCreateEnumerator(user,
[mgr+0x360], type 1, flags 0, cItem 1, pcb, phEnum = mgr+0x32C in r9) + XamEnumerate (wrapper
sub_828A8258: buf mgr+0x1E8, cb 0x134, ovl 0); needs rc 0 AND count != 0 -> 11 and publishes
mgr+0x1F0; else NtClose + 12. FindNext (sub_82645600) loops while rc == 0, so exhaustion MUST
be 0x12. Save and load both match an existing save by DISPLAY NAME (`wcsncmp(.., 42)`,
sub_827A5998) against `L"Superman Returns: The Videogame"` (off_82913628), so the package
`.header` is load-bearing. `SaveMgr_ContentFileFindFirst_sub_82645098` mounts OPEN_EXISTING,
runs FindFirstFileA sub_828A7D50 -> sub_8246A640 (splits at the last '\': NtOpenFile(dir,
0x100001, share 3, options 0x4021) + NtQueryDirectoryFile(h, 0, 0, 0, iosb, buf, 328, &mask
ANSI in r10, restart at `[r1+0x54]`); a mask of `*.*` becomes Length 0) and then
XamContentGetCreator(user, mgr+0x1E8, &isCreator, 0, 0). FIND_DATA consumer sub_828A8640:
+0 attrs, +4/+12/+20 FILETIMEs, +32 nFileSizeLow, +44 cFileName.

**Package**: file name `"Superman Returns"`, display name `L"Superman Returns: The Videogame"`,
one file `"Superman Returns"` inside (ProfileMgr_BeginSaveDeviceSelect_sub_822A7CC8); size
88 + 96 * AptComputeDisplayListCount (sub_82165000 / sub_82181DF8; inferred, not yet checked
against a Xenia save). Host `.header` = Xenia's XCONTENT_AGGREGATE_DATA byte for byte:
XCONTENT_DATA 0x134, pad, be u64 xuid @0x138, be u32 title_id @0x140, pad, 0x148 total.

**LIVE save sequence [VERIFIED iter-465, dev-tool-assisted roll archive/v1_save/rolls/n1].** MSTobj03
prompt button 0 -> AS `Main_Hud.ShowSaveSlots` -> `SaveLoadSlots.Start(0)` ->
`LoadSlotInfo(true)` -> `XamShowDeviceSelectorUI(flags 0x300)` (device 1; XN_SYS_UI 9,1 / 9,0) ->
4 x `XamContentCreateEnumerator(dev 1, type 1)` + `XamEnumerate` (rc 18 = none) -> AS
`FillSlotInfo` -> A on a slot -> `SaveTask` (`sub_822B56E8` -> `sub_822B4D08`: profile name
check, overwrite prompt if in use, else `sub_822AA968` = WriteSlot + storage task 8) -> one more
enumeration -> `XamContentCreateEx(rmcsave, flags 0x12 = CREATE_ALWAYS|0x10, no overlapped)` ->
`NtCreateFile("rmcsave:\Superman Returns", disp 5)` -> `NtWriteFile` 28 + 88 + 19872 ->
`XamContentClose` -> `XamContentSetThumbnail` (3178 B) -> SAVE SUCCESSFUL popup -> OK ->
`SaveComplete(true)`. The container magic is `'MC02'` (0x4D433032, `lis 0x4D43; ori 0x3032` at
0x8263EF48). Load: `natural_load.json` seeded with that save -> `LoadComplete` -> main_hud
(rolls/l2). Matches Xenia's semantics (`xam_content.cc` CreateEx disposition = flags & 0xF;
`xam_ui.cc` headless device selector: XN_SYS_UI true, 100 ms, false).

2026-10-12 classification correction: the retained `n1/attempt1/provenance.json`
enables `SR_DEVTOOLS=levelskip` and its save carries a `dev-tool-assisted`
marker. This proves the game's storage sequence after tool-assisted traversal;
the old scenario name `save-natural-mstobj03` does not prove natural progression.

**Apt: a Main_Hud function the C++ calls cannot call into an InitAction-defined function
[OBSERVED iter-465, rolls p1-p3; mechanism not traced].** `ShowSaveSlots` / `SaveComplete`
prepends calling `_global.SRAutoSave.Hud(...)` never reached it, while the same call from
`HudLoaded` (AS-called) works, inline `GetURL` in those prepends works, and `_global.SRAutoSave`
plus the disc's own pool names resolve there. Mods: keep C++-called functions to inline-string
fscommands; do the work in functions the C++ calls directly (a popup hook, an instance member).

**Boot-time enumeration is a guest-stack hazard** (IDA, iter-463 review): sub_8263FEE0 runs
`Str_WideToAsciiEscaped_sub_82642388(v49, displayName)` into a 32-byte stack buffer; the
escaper has no bound (copies until a NUL; each wchar >= 0x100 becomes 6 bytes `\xNNNN`). The
game's own name (31 chars) just fits. So the host validates every `.header` before listing it
(exact size, NUL within 128 wchars, file name == folder, type) and skips folder names that
cannot round-trip (> 42 chars).

**RtlNtStatusToDosError is its own platform fix** (`SR_NTSTATUS_DOSERR`, needs
SR_XAM_CONTENT_FS): the generic stub returned 0, so every guest GetLastError after a failed
Nt* call read 0; Xenia's table (xboxkrnl_error.cc) returns the real code. It changes boot
behaviour title-wide (archive/v1_tape3/run.log: BaseSetLastNTError lr=82469A98 at boot with
0x80000006 disc FindNext, 0xC0000034 missing files, 0xC0000002), so it waited for natural
A/B rolls. **Baked default ON 2026-10-18** with step4/audit fix batch 1 (docs/LOGIC_AUDIT.md
2.0d: ROG flicker gate + R0-R2 with the batch ON); `=0` restores the generic answer.
SR_READ_PENDING, which needs it, was not part of that bake and stays default OFF until its own A/B.

**Automated rolls are hermetic**: a found save changes the front-end path (FindFirst 11 vs 12,
SAVE -> overwrite prompt that waits on `dword_8299FB1C` 10/11 - review reading of
StorageTask_WriteSave_sub_8264AE68, not re-verified), so run_recomp_x / sr_drive
give every run its own save root and the exe gives any SR_INSTANCE_ID process a fresh one
(docs/CUSTOM_FRONTEND.md section 8 part B).

## 6j. HAVOK WORLD MEMBERSHIP + SUPERMAN MOVER HOOK POINTS [iter-463; IDA, golden-call harness]

These contracts are what `kernel/collision_watch.cpp` hooks. The mover gates are in
`archive/v1_invest/collision/` (mover_syn "map" and "verify"). Every contract below was
re-derived in IDA. The mover hooks were also run on the ours_midtown image through the
golden-call harness, where their output was identical with the watch on and off.

- **`hkWorld_addEntity_sub_82596508(world r3, entity r4, activation r5)`**
  - If `world+0x8C` (the critical-op lock count) is non-zero, it queues op 1 on `world+0x84`
    and returns 0. Otherwise it adds the entity to its island and to the broadphase
    (`sub_825AFAC0`), fires the callbacks, and returns the entity.
  - The queue replays the op from `HkWorldOpQueue_ExecuteAllPending_sub_825ACF20` at
    `0x825AD068`, with the world unlocked.
  - It is the only way into the broadphase. The only other caller of
    `addEntityBP_sub_825AFAC0` is `sub_8259C5C8`, which is `hkRigidBody::setShape`
    (rigid-body vt+8) and re-adds an existing body.
  - Callers: `PhysWorld_AddEntityIfNotInWorld_sub_822BE680`, the ragdoll code, and the world
    ctor.
- **`hkWorld_removeEntity_sub_825967C0(result* r3, world r4, entity r5)`**
  - It returns a bool through `r3`. It queues op 2 when locked.
  - Every remover goes through it: `PhysWorld_RemoveEntityIfInWorld_sub_822BE6B8` (which
    checks `entity+8` = world), the broadphase-border cull
    `PhysBroadPhaseBorder_MaxPositionExceeded_sub_822C6870`, ragdolls, the op queue, and the
    world teardown `sub_825974A0`.
  - The remove can drop the last reference, so read the entity BEFORE delegating.
- **Entity fields:**

  | Field | Offset | Notes |
  |---|---|---|
  | world | `+0x08` | |
  | collidable | `+0x14` | shape ptr at `+0x14`, motion/transform ptr at `+0x1C`, translation at `+0x30` of the transform |
  | filterInfo | `+0x30` | layer = low 5 bits; buildings are layer 7, Superman's phantom is layer 2 |

- **Actor vt+20 `sub_821E3CF8` SetTransform(actor r3, `{vec4 rot, vec4 pos}*` r4):**
  copies both vectors to `actor+0x10` / `actor+0x20` and never touches the character proxy.
  That makes it a silent teleport; TELEPORT catches it only as a position mismatch at the next
  mover tick.
- **The mover's own write-back** of the entity position (actor vt+12, `sub_821AA218`) comes from
  `0x821C1490`, so lr = `0x821C1494`. Any SETPOS with an lr outside `[0x821C0C58, 0x821C1718)`
  is a real direct write.

### 6j.1 Live verification on bc1f8fc - building collision holds in flight [iter-463; 5 rolls + golden-call]

Rolls: exe snapshot `64af19c4de72-collroll` (bc1f8fc, HEAD when rolled; the later 50 MHz clock
merge 94ce03b does not touch the fixed 1/30 sim step), `tutorial_tape_full.json` with
`SR_INPUT_REPLAY` = the user's own tape (`roll1`, `roll1b`) and three generated tapes
(`sweep.inpr`, `hop.inpr`, `dive.inpr`); artifacts in `archive/v1_invest/collision/route/`.
- **Coverage after the main_hud anchor:** 15.9 min of flight above 20 m/s, 4.8 min of it below
  300 m (building height), 735 fast ticks with a contact in the manifold.
- **Pass-throughs: 0.** The analyzer raised 30 flags (17 HIGH). Every one was re-traced with
  `tools/collision_flag_rays.py` (castRay of each travelled tick against the building's own
  shape on the golden Xenia image; recomp == clean-room interpreter, `divergent=0` in all
  sets): 29 CLEAR (no geometry on the path), 1 CONTACT (a roof landing), 0 CONFIRMED.
- **Not streaming:** no layer-7 body was added or removed with Superman within 50 m of its AABB
  during fast flight. Closest: an add 108.7 m from the box at 44.7 m/s (2.4 s of margin);
  removes are 580 m or more away.
- **Not a writer:** each run has exactly 2 TELEPORTs, both at the gameplay spawn: the
  placement (815-904 m) and a 4.4 cm settle 17 ticks later. Each run also logs 3 cycles of
  DEACTIVATE, layer 11 and back to layer 2, all on the spawn tick. During flight there are 0
  SETXFORM and 0 NO-INTEGRATE, every `[COLL_PATH]` tick reads `path=integrate`, and the phantom
  layer is 2.
- **Verdict:** the defect reported on 60a90bf does not reproduce on bc1f8fc. 04bce60 (the phys
  allocator double-booking fix) is the leading explanation, but this cannot be proven without
  rolling 60a90bf with the same watch.

Verified mover/flight facts from these rolls (live log fields, plus the golden-call for the
last):
- **The actor position (entity `+0x20`) is the FEET.** The golden-call of a roof graze rests it
  exactly on the roof face (y 1148.427 = the castRay hit). Probe the body upward from it, not
  around it.
- **The FE background flyby is the player actor.** The main-menu background flight runs the
  same Superman actor through the same mover, on a scripted loop that repeats its positions.
  Ticks before the `main_hud` APT request are not player flight.
- **Flight controls** (from the HUD prompts and these rolls):
  - Hold Y 1.2 s on the ground: power launch to about 253 m, then level flight at 44.7 m/s.
    RT does not raise the speed.
  - Tap Y on the ground: launch into slow flight, at most about 10 m/s.
  - In flight: tap Y lands; hold Y is a power drop, about 123 m/s straight down.
  - Right stick +Y pitches up. Full -Y pitches down. Flight follows the camera, so right
    stick X turns him; a left stick X pulse does not change the heading for good.
- **Bodies are added with layer 3.** `[COLL_WORLD]` ADD lines show `L=3` for buildings that
  later show `L=7` on remove; the layer is switched after the add. Layer 3 still collides with
  Superman's layer 2 (golden GameGroupFilter: layer 2's disabled pairs are 1, 5, 8, 11, 15, 19
  and 20), so this is not a collision gap. It does mean the watch's "every layer-7 ADD" rule
  only logs adds within its 300 m radius.
- **AABB slack is large.** Tower Hull00424 (golden `0x022D09C0`) is about 30 m narrower per side
  at 254 m altitude than its AABB. At that height its footprint is x [-2189.6, -2122.9],
  z [-315.7, -249.0], against an AABB of x [-2220.3, -2092.9], z [-346.4, -219.0]. An
  AABB-depth flag alone is never evidence of a pass-through.
## 6k. FRAME LOOP - sim tick, render tick, present pacing [iter-464; IDA + golden sound_gameplay + difftest --highfps-test]

The contract the high-frame-rate PORT FEATURE rests on (full write-up: docs/HIGH_FRAMERATE.md).

- **Four GameKernelLoop services** (`FSGameApp_Init2_StartServices_sub_8218CFE8`), each a
  "GameKernel::Loop" thread running `GameKernelLoop_Run_sub_82720B50` on a 176-byte mgr.
  Rates from the byte table `0x82911888` (4 bytes per `g_FeFlowConfig+420` mode, all three
  modes `3C 1E 1E 3C`): FSGameCore `*0x8296D8D8` 60 Hz fixed, **FGGameRender `*0x8296D8DC`
  30 Hz VARIABLE step**, **FGGameSim `*0x8296D8E0` 30 Hz FIXED step** (33,333,334 ns),
  FGGameSound `*0x8296D8E4` 60 Hz variable. The render mgr is built only by
  `StartService_FGGameRender_sub_82177F90` -> `FGGameRenderMgr_Ctor_sub_82166178(mgr, 2, f1 =
  rate)`.
- **Mgr fields:** +0x34 step mode (0 fixed dt = period; 1 variable dt = elapsed; 2 min(elapsed,
  cap +0x58)), +0x3C rate (float), +0x48 period ns (`(float)(1e9f/rate) + 0.5f`, fctidz:
  30 -> 33,333,334, 60 -> 16,666,668, 120 -> 8,333,334), +0x50 loop time (i64 ns), +0x60 last
  clock sample, +0x70 debt (clamped to one period: a loop never runs faster than its rate to
  catch up), +0x78 dt ns, +0x80 tick counter, +0x84 dt seconds (float), +0xA0 average tick
  cost, +0xA8 ticks/s. Clock = `GameKernelClockNs_sub_827203A0` = (KeTimeStampBundle ms + 1) x
  1e6: every loop paces on a 1 ms grid. Golden sound_gameplay: sim mgr `0x05B47780` +0x50 =
  2385733381048 = exactly 71572 x 33,333,334.
- **Sim publishes, render reads (EA::StateStream, `EA::StateStream::Manager` = `*0x829A93C8`).**
  `FeMasterTick_821494C0` (the sim tick, all game logic, cameras, Havok, Lua, Apt advance)
  brackets its work with `StateStreamGroup_BeginWrite_sub_82787650(group0, sim+0x50)` /
  `EndWrite_sub_82787730`: one frame per sim tick, stamped with the sim time. The render tick
  `FSGameRenderMain_Tick_sub_821491A0`: `StateStreamGroup_BeginRead_sub_82787520(group0,
  render+0x50)` picks cur (newest ready, stamp <= time) and next (oldest, stamp > time), blend
  t = (time - cur)/(next - cur) at group+0x2C, read stamp at group+0x30; then `render+0x50 =
  read stamp`, renderer Draw, EndRead; `PostTickStats` adds the render dt. The render clock
  chases the newest sim frame (at most one sim tick behind; snaps back when it overtakes).
  Group: +0 CS, +0x2C t, +0x30 read stamp, +0x38 next idx, +0x3C cur idx, +0x40 stamp being
  written, +0x48 last published, +0x50 write idx, +0x54 stream count, +0x60 stream array;
  stream: +0 stamp, +8 data, +0xC state (0 read, 1 written, 2 ready, 3 fresh).
- **The renderer Draw ignores dt** (`*0x829A9384` vt+0xC = `sub_8230C038`; golden vtable
  `0x82098120`): BeginFrame / DrawFrame get t = group0+0x30 x 1e-9 = the SHOWN game time in
  seconds; renderables blend through `StateStreamGroup_GetReadBlocks_sub_827877A8` (23 readers:
  camera/view nlerp `sub_8277A048`, anim channels, cloth, VFX, shadow segments).
- **Present pacing:** every present goes through `D3DDeviceSwap_sub_82112050`; the PM4 block
  (`GpuPresentEmitCommandBlock_sub_82111EC8`) holds the flip until >= PresentationInterval
  (dev+0x33A4, golden 1 = D3DPRESENT_INTERVAL_ONE) vblanks since the last flip (vblank ISR
  `GpuInterruptCallback_sub_820FDBA0` source 0 counts dev+15124). So the console cap is
  min(render loop 30 Hz, vblank 60 Hz / interval).
- **Movies** are paced inside the render loop (section 6g.1b of the movie lane): one frame-get
  per render pass, hold when > 38 ms early, `Sleep(drift - 5)` on the render thread.
- **Verified by:** `build/difftest.exe --highfps-test` (the real ctor / SetTickRate /
  TimeGate / AdvanceTickTime / PostTickStats / StateStream functions on a simulated 1 ms guest
  clock), IDA comments on the functions above.

## 6l. KERNEL / XAM ANSWERS THE GAME READS [VERIFIED step4/audit 2026-10-18; IDA + Xenia 0bd090dbe source]

From the logic audit's static inventory (docs/LOGIC_AUDIT.md 2.0, full table docs/LOGIC_AUDIT_IMPORTS.md):

- **RNG seed.** The global RNG pair `*0x8296D8C4` (generator A +8, B +24; created by sub_82176AE0) is seeded once by
  sub_82164440 from sub_82775468 = `49710 * local seconds-of-day (_strtime sub_827A47C0 -> GetLocalTime sub_828A8350)
  XOR QueryPerformanceCounter low word (sub_824680D0 = mftb)`. Traffic placement, lane choice, audio randomisers and
  the Lua `GetRandomMiniEventBasedOnWeights` (generator B) draw from it: random per run on the console and on Xenia.
- **XUserGetXUID** sub_82467EF0 = `XamUserGetXUID(user, 7, pxuid)` (r5 = the out pointer); RefreshSignedInUser
  sub_825655C8 compares the XUID's low dword with obj+20 and on a change stores it at obj+16, renames, sets obj+24 = 1.
- **CreateEventA** sub_824693D8 = `NtCreateEvent(&h, attrs, type = !bManualReset, bInitialState)`; the XMV media stream
  InitStreamBuffer_sub_824828F0 asks for manual reset, initially signalled (handle at stream+0x58).
- **ExGetXConfigSetting** callers: D3DDeviceCreate (3, 0xA) VideoFlags -> dev+0x3B24 (no reader found); the PAL-50
  check Xapi_Pal50IncompatibleCheck_sub_82468B40 ((2,2) AV region, (3,0xA)); GetTimeZoneInformation's helper
  sub_828A89D0 ((3,1..7) bias / names / DST dates) -> local time. Xenia's values: docs/LOGIC_AUDIT.md 2.0a.
- **XamGetSystemVersion** readers: XMPOverride/RestoreBackgroundMusic sub_82470628 / sub_82470700 (`(v & 0xFFFF00) >=
  0x8A100` -> XMPSetPlaybackController via XMsgStartIORequestEx 0x7001A; else the old path: XMsgInProcessCall(0xFA,
  0x7001B) + the override flags 0x82976AF4 / AF8 under CS 0x8291D230) and the WSAStartup / XNetStartup wrappers
  sub_8246F3B0 / sub_828A8D30 (`< 0x200A3200` -> NetDll_*; else XexGetModuleHandle("xam.xex") + XexGetProcedureAddress,
  pointer pre-zeroed). Xenia returns 0 (the old branches).
- **XapiThreadStartup** 0x828A8C08 (r6 of XapiCreateThreadEx's ExCreateThread): XapiCallThreadNotifyRoutines(1),
  start(ctx), XapiCallThreadNotifyRoutines(0), ExTerminateThread(ret). The only registered routine is the CRT's
  sub_827AF280 (Crt_MtInit): create = an empty callback, exit = _freeptd sub_827A57A8 (frees the thread's _tiddata).
- **Batch 1 answers are BAKED (default ON 2026-10-18, docs/LOGIC_AUDIT.md 2.0d; `=0` per gate = the old answer):**
  SR_XAM_XUID_ABI (XamUserGetXUID's r5 out pointer), SR_XCONFIG_XENIA (Xenia's XConfig table), SR_XAM_SYSVER_XENIA
  (XamGetSystemVersion 0), SR_XMP_APP_XENIA (the XMP app's 0x7001A / 0x7001B), SR_NT_EVENT_TYPE (NtCreateEvent's
  type / initial state, NtClearEvent), SR_NTSTATUS_DOSERR (Xenia's NTSTATUS -> Win32 table). With SR_XCONFIG_XENIA on,
  the DST helper reaches RtlTimeFieldsToTime, still a generic FALSE until batch 2's SR_RTL_TIMEFIELDS_XENIA, so local
  time stays UTC + bias (no BST), as it was before the bake.
- **Batch 2 answers are BAKED (default ON 2026-10-20, docs/LOGIC_AUDIT.md 2.0f; `=0` per gate = the old path):**
  SR_RTL_TIMEFIELDS_XENIA (RtlTimeFieldsToTime: the DST helper now finds London's cutovers, so local time follows BST
  as on Xenia), SR_RTL_MBCS_XENIA (the two Rtl MBCS converters; the title never calls them on the routes), SR_XNOTIFY_XENIA
  (Xenia's notification queue: area / version filter, priming from the current UI / XMP state, waitable listener
  handles), SR_XAPI_THREAD_STARTUP (ExCreateThread enters XapiThreadStartup 0x828A8C08 with (start, context), so the
  CRT's thread notify routine runs and the exit code is the start routine's).
- **Batch 3 is BAKED (2026-10-20, docs/LOGIC_AUDIT.md 2.0g; `=0` = the old answer):** SR_IMPORT_FIX_ATTRS
  (NtQueryFullAttributesFile by real existence: NO_SUCH_FILE for a path nothing resolves, the info filled for one
  that resolves). Its only guest caller is GetFileAttributesA 0x82469720, called by the D3DX compiler's one-time
  `ShaderDumpxe:\CompareBackEnds` probe (CompileShaderCompareBackEnds_sub_823B3370; a hit sets byte_82976270 and every
  later run-time HLSL compile runs twice, the old back end too) and the effect preprocessor's file #include
  (EffectPreprocHandleInclude_sub_8236DD90). Off, the generic SUCCESS turns the compare mode on. The run-time
  compiles' output is identical either way (the A/B, s4b3dev).
  Its second item DELETED the host's optional-data alias (NtCreateFile: `DATA/objtiv1.AST` -> `objtiv.AST`, `sound1`
  -> `sound`, `fmv1..5` -> `fmv`): the retail disc holds none of the seven names, so the title opens each, fails
  (OBJECT_NAME_NOT_FOUND), retries once and goes on, as on Xenia; before, ours mounted seven extra Rtam storages.

## 7a. APU / XMA - how the game decodes XMA, and the native port [iter-463; IDA + golden dumps + offline decode]

**2026-10-13 private native candidate amendment:** application builds exclude the
XMA register dispatcher and SR_XMA_NATIVE_FRONT A/B seam. The register oracle is
test-only; shared game import glue uses macro-independent `xma_api.hpp`.
Actual 50-check PCM parity and named object / link / PDB exclusion pass bounded
Astra review in `archive/native_xma_oracle_separation_v2/`. Historical measurements
below remain historical. Whole-game HAL absence, audio parity and complete
release composition are still owed.

**Two XMA paths are linked; only one of them runs.**
- **EA DECEAXMA (0x82658xxx-0x82659xxx) is LIVE.** It drives XMA only through the 16 `XMA*` kernel
  imports: Create, Release, Initialize, SetInputBuffer0/1, SetInputBuffer0/1Valid, IsInputBuffer0/1Valid,
  IsOutputBufferValid, SetOutputBufferValid, Get/SetOutputBufferReadOffset,
  GetOutputBufferWriteOffset, Enable and Disable.
  - It creates exactly 256 contexts (`dword_8292F0C0`), and in the oracle they are indices 0-255.
  - It uses one packet per input buffer, refilled in0/in1 alternately, with a 24-block (6144 B)
    output ring and subframe_decode_count 4.
  - Progress is kick-driven: the DAC re-kicks through `XMAEnableContext` at 0x826595B4.
- **The XDK XAudio XMA HAL is DORMANT.** It is linked, but it never runs in the oracle.
  - It is the XAudio *XMA source voice*: ctor `XmaSourceVoiceCtor_sub_821236E0`, whose primary
    vtable is **0x820BC978**. That ctor is reached only from `AudioCreateObjectByType_sub_8211B070`
    for voice type 4.
  - The HAL calls `Xma_AllocContextBlock_sub_82119748` and then `Xma_CreateContexts_sub_82119980`.
    It gets its contexts from the same `XMACreateContext` import, then writes the XMA registers
    directly with stwbrx to 0x7FEA1xxx: Clear at 0x82119A18, Lock at 0x82119BF8 and Kick at 0x82119F74.
  - **Evidence that it is dormant (golden.py `findva`):**
    - Neither sound golden (`sound_menu`, `sound_gameplay`, oracle 0bd090dbe, full ISO) holds a
      single object whose first dword is 0x820BC978.
    - Positive controls: the one live PCM source voice (vtable 0x820BCA90, ctor
      `AudioVoice_ctor_sub_82126750`) is found at 0x400991D0 in both dumps, and the XAudio engine
      (vtable 0x820BC50C) is found at 0x4009447C.
    - XMA contexts 256-319, the ones a HAL voice would receive after EA's 256, are never
      initialised (`archive/v1_audio/oracle/findings.json`).
  - **Consequence (NATIVE rule):** only the imports need a native implementation, and there is no
    register emulation. `kernel/xma_hal_observe.cpp` is the witness on our runs. It wraps the ctor
    and `Xma_CreateContexts` as pass-throughs and logs `[XMA_HAL_LIVE]`. Log gate:
    `SR_XMA_HAL_OBSERVE`, which defaults to `SR_XMA_NEW`.
  - If that line ever appears, the three HAL functions (0x82119980, 0x82119BA8, 0x82119E68) must be
    replaced at function level, never by emulating MMIO.
  - The residual gap is a HAL voice created and released between the two dump times. Only the
    witness closes it.
- **N6 native audio (iter-467 sound lane, all default ON, `=0` keeps the A/B):**
  - `[AUDIO_NATIVE]` (kernel/audio_native_census.cpp, every 30 s, kept in the player log) counts
    entries into all eleven XMA-register / HAL functions: the six that touch 0x7FEA0000+
    (`sub_82116800` reg 0x600 lwbrx, Clear 0x82119980, Lock 0x82119BA8, WaitIdle 0x82119C28,
    Poll 0x82119CE8, Kick 0x82119E68 -> `guest_mmio_fns`) and the five register-free ones
    (Alloc 0x82119748, Release 0x82119A50, Submit 0x82119AC8, ConsumeOutputRing 0x82119FA8,
    ResetSlot 0x8211A160 -> `xma_hal_rest`; IDA: every code caller is an XmaVoice* method or
    another HAL function), plus the XMA device's register-file writes (`xma_regfile_writes`).
  - `SR_XMA_ARRAY_NATIVE`: `sub_82116800` stores the native device's context-array PA in
    `dword_829AC0E8` (what Xenia's register 0x600 returns) and tail-calls `sub_82117018` with the
    original registers; no lwbrx.
  - `SR_XMA_NATIVE_FRONT`: `XMAInitializeContext` / `XMAEnableContext` / `XMADisableContext` call
    `XmaDevice::clearContext` / `kickContext` / `lockContext` by context; the Xenia register path
    (`StoreXmaContextIndexedRegister` -> `writeRegister`) stays as the oracle (difftest T5n /
    T5n': the same PCM bit for bit, 0 vs 145 register-file writes).
  - Measured on B6 (dev 85e82831 + audio/fix3): the player whole run w1b (boot, front end,
    MODS, RESTART NOW, the relaunched game, NEW GAME, 4 min of gameplay) and the o11 intro roll both
    end on `guest_mmio_fns=0 xma_regfile_writes=0 reg600=native:1E301000 xma_hal_voices=0
    xma_hal_rest=0`, in both processes of the whole run. The struct is readable from code
    (`audioNativeCensus()`, kernel/audio_native_census.hpp) for the native meter, and
    tools/check_roll.json grades the line in every combined roll.

**How EA stores XMA on the disc (corrects the design pass).**
- `sound.AST` (BGFA1.05) holds EA Audio Core **SNR headers with raw SNS blocks of 2048-byte XMA
  packets**. They are *not* RefPack/zlib-compressed.
  - SNR: be32 `ver<<28 | codec(3 = EA-XMA)<<24 | (ch-1)<<18 | rate`, then be32
    `type<<30 | loop<<29 | num_samples`, plus loop_start when looped.
  - Each SNS block: be32 `flag<<24 | size`, be32 samples, a be32 sub-stream header with low bits
    0x012, then the packets. The last block has flag 0x80.
- `tools/ea_snr_xma.py` carves 779 of these sounds. It was verified by finding the oracle's live
  in0 packets byte-for-byte in `sound.AST`.

**Oracle facts.**
- The context array is at VA 0xFFCA9000, PA 0x1FCAA000, and the host is membase+VA+0x1000.
- `MmGetPhysicalAddress` follows Xenia `memory.cc`: VA - heap_base, +0x1000 for the 0xE heap,
  UINT32_MAX when no heap owns the VA. The export maps that to 0.
- The D3D ring init computes the same +0x1000 inline (0x820FDF00).

**The native port (default ON since the iter-463 rolls; =0 keeps each A/B):**
- `SR_MM_PHYSADDR` (`kernel/mm_phys.*`).
- `SR_XMA_NEW` and `SR_XMA_DEDICATED_THREAD`, default 1 (`kernel/apu/`). This is a 1:1 port of
  Xenia's XmaContextNew and XmaDecoder from 0bd090dbe, including 6bef1fb, 7015473 and aee0871.

**Offline proof (no game process).** Repro: `tools/ea_snr_xma.py --wav-wrap`, then
`build/xma_offline.exe list <disc>/list.txt <wav> --be x`, then `tools/xma_golden_extract.py`,
then `tools/xma_offline_check.py`.
- All 779 disc sounds decode through the port in EA's geometry: about 2085 s of audio,
  duration-plausible and non-silent.
- Stock FFmpeg's independent xma1 decoder matches within 1 LSB after a constant 576-sample
  codec-delay offset. That constant holds for all 779 sounds, so no frame is dropped or
  duplicated.
- Against **the Xenia oracle's own output rings** (golden dumps): 5688/5688 (gameplay) and
  5928/5928 (menu) 256-byte PCM blocks are found **bit-exact** in our decode of the same disc
  sound.
- `difftest --xma-test`: 45 passed, 0 failed, with 5/5 mutations caught.

**Live (iter-463 rolls, archive/v1_audio/rolls):** with the port on, `[XMA_SUM]` reads
kicks = works (7.4-8.4K per 300 s intro boot, ~770K over the 1800 s tutorial tape), decode_fails 0,
phys_miss/alias 0, `[XMA_HAL_LIVE]` 0 (the HAL stays dormant live too), 2 kick timeouts in the
long roll (one during a GPU-hang dump). Disc sounds are found by waveform in the output
(`tools/audio_sound_presence.py`: 6 sound.AST sounds at peak correlation 0.62-0.945 in the attract
demo of a_all_e1).
**Review (iter-463, archive/v1_audio/review) - what the live evidence does and does not show:**
- The intro-movie correlation (speed 1.0000, window corr 1.000) proves the OUTPUT path (render
  driver, pacing, 6->2 downmix), not the XMA port: `[XMA_SUM]` reads inits=0 / kicks=0 for the
  whole intro in every boot (f_main_final2, a_all_e1). The movie audio is decoded by the game's own
  code and reaches XAudio as PCM.
- In an intro boot, XMA runs ONE context, from the title (~115 s) for ~80-86 s at ~85 frames/s
  (a real-time stream), then stops. The sound.AST hits in the attract demo (f_main_final2: s0708,
  s0734-s0736 at dump 226-266 s, peak 0.60-0.93) come AFTER that stream ended, so they are not
  attributable to the XMA port. The title stream's content is not checked against the disc.
- The one hit that can be XMA's: b_long1 s0242 (peak 0.62, dump 124.7 s), next to the first
  multi-context inits (inits 1 -> 7 at 10:38:20-30).
- The decode itself is proved offline (bit-exact vs the oracle's output rings, above).
- Negative controls: four fmv.AST cutscenes the boot never plays, and three time-reversed intro
  refs, are NOT matched by `audio_movie_xcorr.py` (one fmv.AST ref cleared the 0.3 coarse bar with
  0/169 good windows; the tool now requires >= 3 good windows and median >= 0.5 for "found").
  `audio_sound_presence.py` on the time-reversed f_main_final2 dump: 0 of 779, best null peak 0.442
  (threshold 0.6). Positive control: a synthetic 1.5 %-slow copy with an 80 ms gap reads speed
  0.9850, 1 gap (80 ms).
- Tool blind spot fixed: a dropout longer than the +-60 ms local search used to read "gaps 0"
  with every later window failing. The review roll r_review_logfix1 (host under load: intro EOM
  132.7 s, 13 GPU_HANG) has one 190 ms dropout 13 s into the title movie (no sink underrun, so the
  silence came from upstream of the output path: the movie voice starved); the tool now
  re-acquires the waveform (wide search +-1.5 s, accept >= 0.8) and reports "gaps 1 (190 ms)",
  the speed fitted over the gap-free steps. f_main_final2 is unchanged (5/5, gaps 0).
- The two b_long1 `[XMA_KICK_TIMEOUT]`s each began 250 / 314 ms after an `[XMA_SUM]` stamp (~170
  summaries in the run). The summary was written by the decoder worker between the kick's wake-up
  and the scan, through the synchronous logger. Fixed: the worker no longer logs; the kicking
  thread writes the summary after its kick completes. Boot roll of the fix (r_review_logfix1, no
  gates): E1 PASS, 0 ICALL_FAIL, `[XMA_SUM]` still written while XMA is active (kicks = works 8841,
  decode_fails 0), 0 kick timeouts, the five voiced intro movies MATCH at speed 1.0000. One boot
  cannot show the rare timeout gone; the mechanism is removed by construction.
**SR_XMA_NEW=0 (the legacy "always empty/complete" fakes) is the root of D-AUDIO-CMDBUF:** with
every other audio gate on, c_ab_xma_new0 stops at `[ICALL_FAIL]` call_site 823399D4 (the
AudioGetOrCreateNamedAsset vt[+4] site the burndown's Frida capture named) ~95 s in, as do the
all-off default (c_r0_default, site 8234AEC0) and the worker-less-wait A/Bs below; every roll with
the port on has 0 ICALL_FAIL (8 intro boots + the 1800 s tape).
**Still open.**
- The EA drain loop's phantom-6144 reading on a starved context (critique B8/T11) is
  Xenia-faithful and unverified live. Offline (difftest --xma-test T3p, iter-463 cmdbuf2): a
  context starved on input with write == read and valid = 1 is stored back byte-identical (the
  ring is empty, not full; xma_context_new.cc @0bd090dbe Work), so our port cannot hand EA a
  phantom 24-block read that Xenia would not.

**D-AUDIO-CMDBUF recurrence with SR_XMA_NEW on: the DAC thread wedges on a DEAD stream reader
[iter-463 cmdbuf2; IDA + recomp source + roll logs; root cause NOT yet found]:**
- Roll archive/v1_invest/collision/route/roll1 (exe bc1f8fc, 1 of 5 gameplay rolls): the DAC
  thread's first three `[CS_GARBAGE]` of the run (15:50:29.691, caller_lr 825FC788, cs 4, 4, 3);
  it never waits again, `[AUDIO_RMS]` goes to 0, the queue overruns in ~8.2 s, ICALL_FAIL at
  823399D4 (object 87337770, vt 8264CE28). 3 of the 4 healthy rolls take the same path with cs=4
  only, thousands per burst (lower bound: `[CS_GARBAGE]` is count-unsafe), and survive.
- `EaStrm_DequeueReadyBlock_sub_825FC760(reader)`: owner = reader+4; lock =
  `CsEnterWithDeadline_sub_825FC108(owner+4, &dword_82074190)`, which on success increments
  owner+36 (CS+32) and the function decrements it on both exits (balanced); pending = reader+12
  (0 -> return 0); else block = reader+16, pool entry = 312 * (u8)block[1] + *(owner+48), and the
  walk `v=*v until v[5] == reader+8` at 0x825FC810 can loop forever on garbage. So with owner 0
  the guest touches only VA 4 (CS key) and 0x24; with owner 0xFFFFFFFF (cs=3) and pending != 0 it
  writes through garbage and walks garbage.
- Our RtlEnter/LeaveCriticalSection are HOST locks keyed by address (stubs.cpp ~5289): they never
  write the guest CS bytes. With owner 0 the path's only guest writes are the +36 inc/dec at VA
  0x24. Open hypothesis H-DL (unverified): if `CsEnterWithDeadline` ever returned -2 (deadline
  global != -1; both are -1 in the image and in the golden dumps, and IDA shows no store among
  dword_82074190's 28 xrefs - all address loads - so H-DL has a LOW prior) the increment is skipped,
  the decrement leaves VA 0x24 = 0xFFFFFFFF, and a reader at 0x20 would then read owner
  0xFFFFFFFF (cs=3) - roll1's exact 4, 4, 3. `[EA_RDR]` logs the reader address and both deadline
  globals to decide it. Otherwise another writer changed reader+4 within the same millisecond.
- Callers of 825FC760: `sub_82654BE0` (stream player, reader = request+64; call sites by return
  address: CB0 82654D24/82654DA8, DD0 82654EBC/82654FB4, FF0 826550B8, player 82655564),
  `FmvDecodeFrameSubmit_sub_8235C460` (lr 8235C47C), the SCHl feeder `sub_826577F8` (lr 826578E4,
  its stream table dword_8299FDF0 is empty in both sound golden dumps). Request byte +74 = mode
  (0 resident, 1 streamed, 2 resident head + streamed tail); `sub_82654CB0` calls 82654BE0 for
  mode 2 once +40 >= +48, `sub_82654DD0` re-arms the reader with `sub_82607968(*(req+64), ...)`.
- Oracle: Xenia runs protect_zero = true (memory.cc:235, the user's config line 428), so any
  null-page reader read is fatal there; the oracle played ~40 min without one. Our writable null
  page hides the divergence until garbage lands there.
- Probes and diagnostics: kernel/audio_cmdq_obs.{hpp,cpp} - `[RWAC_Q]` (1 Hz), `[RWAC_Q_STALL]`,
  `[EA_RDR]` / `[EA_RDR_REQ]` / `[EA_RDR_DEQ]` (SR_RWAC_Q_OBS, default ON, observe only); S1
  SR_PROTECT_ZERO=1 (+ SR_PROTECT_ZERO_DELAY_S), S2 SR_DIAG_NULLPAGE_POISON=1|N (forcing;
  roll ALONE with SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_DIAG_NULLPAGE_POISON - plain
  SR_ALLOW_FORCING=1 also starts cfgSoundOffThread and mutes the audio path), S3 SR_DIAG_DAC_WAKE_DELAY_US=N with control SR_DIAG_IO_DELAY_MS=N
  (SR_DIAG_IO_DELAY_MATCH, default sound.AST). Banned as fixes (forcing): a queue bounds check, a
  host drain, a NULL/owner check added to 825FC760.
- `gen_override_manifest.py` scans `kernel/*.cpp` only, not subdirectories, so overrides must stay
  in the kernel/ root.
- **STATUS 2026-10-02 (lane memfix, handover):** WORKS - root cause named in 7d010fe: the misparsed
  resident request comes from `ParseTagStreamAndAlignData_sub_82330CA8` aligning with a SIGNED
  `p % 16`, wrong for our 0x8xxxxxxx NtAllocateVirtualMemory bump placements (Xenia/console give
  base==0 NtAVM positive VAs from v00000000/v40000000); difftest C5 runs the real recompiled fn on
  roll A1's bytes. UNPROVEN - the fix SR_NTAVM_XENIA_HEAP (default ON since 795320e) has 1 clean
  natural fix-on gameplay roll (B2, 27 min, bad=0) and 1 fix-on roll N2 that EXITED code 1 at
  611 s (cause not yet read); Q1-Q5 are SUSPECT (RAM window). NEXT - read N2/C2/C5 exit cause,
  then clean A/B >= 3+3 on tools/scenarios/tutorial_tape_full.json + SR_INPUT_REPLAY tape3.
- **PROVEN on dev b5249527 (iter-467 sound lane, archive/v1_audio3; the fix reached dev with the
  d6ad9bb5 integration, default ON).** Same tape (archive/v1_tape3/tutorial.inpr), same exe:
  - fix OFF (`SR_NTAVM_XENIA_HEAP=0`, tools/scenarios/audio_gameplay_short.json): 2/2 rolls show the
    witness `[EA_RDR] ... FIRST` 90 s after main_hud (o8: RWAC bad +6268, survived the 4 min; o9:
    `[CS_GARBAGE]` cs=4,4,59821554 -> `[RWAC_Q_STALL]` drains +0 -> write offset 0xECA18 of 0x100000
    in 7 s -> `[ICALL_FAIL]` target C003000C site 8271EE08 = the D-AUDIO-CMDBUF chain).
  - fix ON (default): 3/3 ~13-min tutorial rolls (o1 dev log, o6 player log + WASAPI, o7 player log +
    uncapped fps) have 0 `[EA_RDR] FIRST` lines, RWAC bad 0 (o1), 0 `[ICALL_FAIL]`; with the cmdbuf2
    lane's B2/N2/C2/C5/C6 that is 8 fix-on gameplay rolls without the witness.
  - The cmdbuf2 lane's fix-off rolls A1/C1/C4 showed it 60-91 s after main_hud as well (3/3).
  - N2's exit code 1 (611 s) had a healthy queue (bad +0, drains normal to the last line): not this
    defect; cause still unread.

### 7a.1 Render-driver output, process type, KeWaitForMultipleObjects [iter-463; IDA + Xenia 0bd090dbe]

**The output contract (verified in IDA, the recomp and Xenia):**
- `XAudioRegisterRenderDriverClient(pClient {+0 callback, +4 arg}, pDriver)`. The one client is
  `XAudioRenderDriverCallback_sub_821209F0`, which ignores r3 and calls
  `XAudioServiceSync_sub_82117F10`.
- Each mixed frame reaches `XAudioSubmitRenderDriverFrame(r3 = handle from client+16, r4 = frame)`
  through `XAudioRenderClientSubmitFrame` 0x8211B680, a tail branch; the import sees lr 0x821209D4.
  The frame is 256 samples x 6 channels of planar big-endian float (FL FR FC LFE BL BR, 6144 bytes).
  The old "not imported" comment in stubs.cpp was wrong and has been removed.
- Xenia paces each client by a 5333 us deadline (400 us slack) and uses 64 credits as
  back-pressure. The host device gives one credit back per frame it plays. A frame with no credit
  at its deadline is skipped, not queued. The stereo downmix (the AMD64 path) is
  L = (FL + ch4 + FC/2)/2.5 and R = (FR + ch5 + FC/2)/2.5, with LFE dropped.
- `XAudioGetVoiceCategoryVolume` writes 1.0f and `...ChangeMask` writes 0. The generic stubs
  wrote nothing.

**Process type (Xenia xthread.cc:207-217, xboxkrnl_threading.cc:429-437):**
- Every KTHREAD carries its process type at +0x72 and +0x73. Title threads are TITLE (1); Xenia's
  "Audio Worker" host thread is SYSTEM (2).
- `KeGetCurrentProcessType` returns KPCR+0xC while `dpc_active` is set, and KTHREAD+0x73 otherwise.
- `AudioBuildSpeakerConfig_sub_82117DF8` builds the XAudio worker config ({1,1,0,0,0,0,1,0},
  golden engine+304 = 1) only when the type is 1. The old constant 0 gave {0,1,0,0,0,0,4,0}: no
  worker, and ServiceSync mixed inline on the callback thread.
- Heap owner check `sub_8246BD18` bugchecks F4 when a flagged heap (+20 & 0x40000) is used by a
  thread whose type differs from the heap's creation byte (+379).

**`KeWaitForMultipleObjects`:**
- The only caller is ServiceSync at 0x82117FC0: WaitAny, infinite, on the worker's done and exit
  KEVENTs 0x829AC094 and 0x829AC0D8, with result == 1 meaning "skip the device submit".
- It runs only when engine+304 is set (a worker exists), so SR_PROCTYPE needs SR_KE_WAITMULTI. The
  generic stub returned 0 at once, which lets the mixer race the worker.

**KeResumeThread (Xenia xboxkrnl_threading.cc KeResumeThread_entry) [VERIFIED iter-463, live].**
The XAudio worker `AudioWorkerThreadMain_sub_82117810` is created CREATE_SUSPENDED
(flags 0x10000001) and resumed by `KeResumeThread(KTHREAD*)` from AudioBuildSpeakerConfig
(lr 0x82117C88). The argument is the KTHREAD pointer (our arena, e.g. 0xD0026E00), not a handle.
With the generic stub the worker never ran, and ServiceSync's WaitAny on 0x829AC094/0x829AC0D8
blocked the render callback on its first call (a_all_1: 0 frames submitted).
**The pump <-> worker handshake.** ServiceSync (pump thread): engine+300 = [r13+0x100], KeSetEvent
0x829AC0B8, WaitAny{0x829AC094 done, 0x829AC0D8 exit}. Worker: KeWaitForSingleObject(0x829AC0B8);
engine+300 == 0 at wake means EXIT (release 0x829AC0A4, set 094, leave the loop); else swap child
buffers + voice volumes, set 094. All three are auto-reset KEVENTs in .data. A spurious wake breaks
it; our KeSetEvent wrote SignalState before setting the host token, so a waiter's fast path between
the two left a stale token (a_all_r2 deadlocked ~3 min in with all states 0; difftest W13: 3-10
extra wakes per 200K kicks). SR_KEVT_SET_ATOMIC makes state+token and consume+drain atomic.

**The native port (default ON since the iter-463 rolls; =0 keeps each A/B):**
- `SR_AUDIO_OUT`: `kernel/apu/audio_system.*`, a port of Xenia's AudioSystem and SDL driver.
  - Handle 0x41550000|index and a wrapped-argument cell.
  - Its unregister lock order is fixed, and it has an unregister-pending flag so a long callback
    cannot starve the unregistering thread.
  - `SR_AUDIO_SINK=sdl|null`, and `SR_AUDIO_WAV=<path>` for an observe-only float WAV.
  - Log lines: `[AUDIO_RMS]` (1 Hz), `[AUDIO_OUT_SUM]` (5 s) and `[AUDIO_CB_NOSUBMIT]`.
- `SR_PROCTYPE` (`kernel/kpcr.*`): type bytes are written at KTHREAD init, and the XAudio worker's
  own KTHREAD is SYSTEM.
- `SR_KE_WAITMULTI` (stubs.cpp): built on the single wait's object model (guest-truth KEVENTs,
  arena KTHREADs, host handles).
- `SR_KE_RESUME_THREAD` (kernel/kthread_objects.*, stubs.cpp): ExCreateThread records KTHREAD ->
  duplicated host thread; KeResumeThread resumes it, unknown KTHREAD -> 0xC0000008.
- `SR_KEVT_SET_ATOMIC` (stubs.cpp): KeSetEvent's state write + token set vs a waiter's
  consume + drain under one lock (single wait and WaitMulti).
- The Sound flag: cfgSoundOffThread is master-gated (burndown round 2, `SR_CFG_SOUND_OFF` named
  diagnostic); the natural game keeps the flag at 1 and builds the mixers.
- Tests: `difftest --audio-out-test` gives 28/0, including a silent SDL disk-driver check.
  `--ke-waitmulti-test` gives 16/0 (W13 handshake x200000, W14 KeResumeThread; W13 fails with
  SR_KEVT_SET_ATOMIC=0). `--proctype-test` gives 8/0 and drives the real 0x82117DF8.
- **Output health vs the Xenia oracle (iter-467 sound lane, archive/v1_audio3; TOOLKIT 1.15).**
  Both sides captured at the submitted frame (`SR_AUDIO_FRAMES`; Xenia: Frida on 0x8211B680):
  - Content: the intro movies, the title and the attract demo are bit-exact (2 s waveform corr
    1.000, gain 0.0 dB, constant lag; the 3D-panned attract demo included); the opening cinematic's
    start 1.000; tutorial music 0.97-0.99 at 0.0 +-0.2 dB; the SFX/VO left after removing the shared
    music (tools/audio_residual.py) is within 0.4 dB of the oracle (natural) and of the natural roll
    (uncapped `SR_HIGHFPS`, 175 render ticks/s).
  - Timing: in the PLAYER log profile over WASAPI (silent "Steam Streaming Speakers" endpoint) the
    render callback never ran late (> 20 ms) in a 5 min boot or a 15 min tutorial, natural or
    uncapped; the 4-6 sink underruns every boot used to log were all in its first 5 s, the silence
    before the guest's first frames: `SR_AUDIO_PREROLL` (default ON) starts the stream on the first 3
    queued frames, so they read 0 (difftest A17). The 70 ms - 1 s callback gaps the dev-profile
    rolls showed were the instruments: synchronous WAV / frame dumps (the render thread caught inside
    WriteFile by `SR_AUDIO_CB_PROBE`) and the dev logger. Observers are asynchronous now; measure
    output timing with `SR_LOG=player`.
  - Movies (player profile, `[MOVIE_PACE]`): the six intro movies play at speed 0.986-0.999 and the
    title movie in 31.12 s for 31.10 s of media, so the old title-movie A/V desync (video 25 % slow)
    is gone on dev.
  - Second level (Xenia `tools/xenia_audio_warworld.sh` session `archive/v1_audio3/xww` vs our
    level-skip roll o10): the opening cinematic is bit-exact for 105 s; the Warworld arena plays
    at -21.8 dB on both sides, and where its music dominates (2 s chunks at corr 0.91-0.94) the
    gain is -0.4 dB and every band up to 10 kHz is within 0.9 dB. The rest of the arena differs
    only as the idle fights differ (no input on either side).
  - Front end (the game's own pause menu, START, DOWN, DOWN, UP, UP, A; Xenia xww vs our o12b):
    neither side makes a cursor sound that stands above the level audio (event-locked average of
    the four cursor presses: within +-3 dB of the pre-press level on both, broadband and 2-12 kHz;
    `tools/audio_events.py`), so there is nothing missing to find there.
    Confirm / back (Xenia xcb vs our o14: 7x DOWN, A, B, B): no cursor, confirm or back sound
    stands above the level audio on either side either (the 7 DOWN presses' event-locked average
    stays within +3 / -3 dB; A opened Xenia's QUIT GAME warning and our AUDIO OPTIONS - without a
    profile Xenia greys SAVE / LOAD out and the cursor skips it - and B closed our pause menu,
    after which the level audio returns, while B does nothing on Xenia's warning).
  - VO: the opening cinematic carries the centre channel (dialogue) for 76 of its 104 s, louder
    than the fronts, and is bit-exact against Xenia. Over the tutorial the centre-channel energy is
    -37.0 dB ours vs -37.2 dB Xenia (`tools/audio_channels.py`, centre = VO in this mix).
  - 3D positioning (5.1 shares, `tools/audio_channels.py`): tutorial x2 vs o6, whole signal
    FL/FR/FC/LFE/BL/BR 26.7/24.0/0.4/0.2/25.7/23.0 % vs 26.8/24.0/0.3/0.2/25.9/22.7 %; Warworld
    arena fronts and surrounds within 0.2 dB, centre -0.7 dB. The LFE carries 0.2 % in the tutorial
    and nothing in the arena, on both sides.
- **Device handling (SDL 3.4.18 stream API since RELEASE_PLAN step 3, verified in its source;
  SDL2 2.32 before):** the sink opens `SDL_OpenAudioDeviceStream` on the chosen device or
  `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`, 6 ch F32 48 kHz, 480-frame (10 ms) buffers; a STEREO device
  gets our 2 ch Xenia downmix, every other device our 6 ch (SDL converts), as under SDL2. A stream
  opened as the default follows Windows' default output (`SDL_DefaultAudioDeviceChanged` migrates
  it; a lost default stays on SDL's zombie device until then, which keeps pulling on its own clock).
  A device opened by NAME (PC SETTINGS > OUTPUT DEVICE) that is lost posts
  `SDL_EVENT_AUDIO_DEVICE_REMOVED` (from the main-thread event pump); the sink's event watch marks
  it STOPPED. `SR_AUDIO_DEVICE_GUARD` (audio_system.hpp; default ON since iter-467, `=0` for the A/B)
  re-opens a STOPPED device (chosen one if listed, else the default), falls back from a chosen device
  that will not open, and bridges a device that pulls nothing for 250 ms at the console rate (the
  guest clock rides on our consumption; without it a stalled pull stops the render callbacks at the
  64 credits and the RWAC queue overruns). A harness `SR_AUDIO_DEVICE` endpoint never falls back
  to the default (= the owner's speakers). Lane E E0 (2026-10-22, `chooseAudioDevice` in audio_system.hpp):
  `SR_AUDIO_DEVICE` with no match = the null sink whatever device PC SETTINGS stored (before E0 the stored
  device opened instead); a harness run (`SR_INSTANCE_ID`) never uses the stored device; on a real driver
  (any SDL driver but disk / dummy) a harness run and every `SR_AUDIO_DEVICE` choice open only a device on
  `SR_AUDIO_DEVICE_ALLOW` (default "Steam Streaming Speakers"; `*` = any), so the system default is refused
  (null sink) there. The legacy driver (`SR_AUDIO_OUT=0`) runs the same rule (it never read the stored device;
  an `SR_AUDIO_DEVICE` pick opens that device by id) and now honours `SR_AUDIO_SINK=null` (before E0 it opened the
  system default whatever the sink setting). One `[AUDIO_OPEN]` receipt per open, written as soon as SDL has opened
  the device (before any setup that can close it), with `target=default` (SDL's system default) or `target=named`
  (a listed device by id), and one `target=none name='null' driver=null` receipt when a client or the legacy driver
  opens nothing, read by `tools/audio_safety_check.py` (a game log with no receipt is NO-EVIDENCE, never PASS).
  New with SDL3: `SR_AUDIO_DEVICE_FOLLOW` (default ON) re-applies
  the stereo / 6 ch rule when the device's channel count changes and returns to the chosen device
  when it is listed again; `SR_AUDIO_REOPEN_RESTART` (default ON) works around SDL 3.4.18 failing every
  in-process re-open of a WASAPI endpoint that was disabled and enabled again ("WASAPI can't
  initialize audio client"; SDL2 re-opened it ~1 s after its return): when the device is listed but
  will not open and this sink is SDL's only audio user, the audio subsystem is restarted on SDL's
  main thread (inside the event pump), then the device opened. Proof: difftest A14 / A15 / A20-A23
  (silent disk driver: stalled, STOPPED, REMOVED event, open rule for 1/2/4/6/8 ch, the bridge's
  partial frame); `tools/audio_device_cycle_test.ps1` on a real silent WASAPI endpoint (Steam
  Streaming Speakers disabled + enabled: PASS; restart off: FAIL = the SDL bug; SDL2 control: PASS);
  one in-game cycle (bridge ON 253 ms after the loss, re-opened by the restart, bridge OFF after
  7.9 s = 1485 frames at the console rate, 0 underruns; lane sdl3-audio, 2026-10-18). Real devices
  (headset / speakers / 5.1 receiver) are an owner check: automated rolls never play through them.
- **Gate dependencies (rolled A/Bs, archive/v1_audio/rolls/c_ab_*):** SR_PROCTYPE=1 creates the
  worker, which needs SR_KE_RESUME_THREAD (else silent + ICALL_FAIL) and SR_KE_WAITMULTI (else
  silent + ICALL_FAIL). SR_PROCTYPE=0 (inline mixing) also plays correctly. SR_AUDIO_OUT=0 runs
  the game without ICALL_FAIL but outputs silence. SR_MM_PHYSADDR=0 and SR_XMA_DEDICATED_THREAD=0
  played correctly in one intro boot each.

## 7. Threading & harness rules (HARD RULES)

1. **The VEH makes any host AV fatal BEFORE SEH filters run.** Host diagnostics touching possibly-dead memory must be fault-FREE (VirtualQuery), never fault-and-catch.
2. Side threads must call `sr::kernel::setGuestMemory` (via `getGlobalGuestMemory`) before any guest read/write — else silent no-ops.
3. Guest calls from host threads: own guest stack (`guestHeapAlloc`), `r14=0x829C0000`, `ExportRegistry::setGuestCallContext(true/false)`, `__try`-guard. Register the thread: `setCurrentGuestContext(&ctx, offsetof…)` +
3a. **Calling a guest helper from a HOST port of its caller [iter-467 native-a, roll p1a_cppm4]: read the ARGUMENT REGISTERS AT THE CALL SITE IN THE ASM, never the decompiler's prototype.** The decompiler drops register arguments it cannot type: `GpuLoadShaderImmediate_82108300` shows `ShaderAssembleVertexFetches_82107A30(vs, copy, decl, dev+0x2FE0)` but the asm sets `mr r7, r23` (the VS variant) first, and the helper indexes `vs[0x380 + 8*r7]`. Called from host code with r7 left over from the draw's arguments it walked a wild vfetch table and overwrote a GameKernel runner's saved r31 on the guest stack (ICALL_FAIL target 0 at boot, 40 s in). Set every r3..r10 the asm reads; the host port's own by-reference locals need a guest frame (`Driver::scratch`: r1 - 0x100, the local at +0x50 as the XDK kept it). `setGuestThreadName` → visible to `[THREAD_SAMPLER]` + `frida_threads`.
4. The fab pump's tick counter STALLS minutes/iteration once the mgr exists — never clock retries or late gates off it; use a dedicated wall-clock thread.
5. Transient publications (`0x829A17BC`-class): latch at the publication site (hook), not by polling.
6. Fabricated heap objects consumed by refcounting code: initialize refcount high (0x10) so the pool free never runs on our allocations.
7. Concurrent FMUI-ctor instances deadlock — one in flight, retry only after completion.
8. Structural type-markers beat range checks (e.g. resMgr+4 == 0x82088654); ASCII garbage passes range checks.
9. `guest/ppc/**` is **gitignored** (generated) — hand-edits there persist locally but are NOT version-controlled; keep kernel-side overrides when possible.
10. **DROPPED-INSTRUCTION CLASS [iter-434e, 743168e]:** the generation that produced our
   guest/ppc silently skipped instructions it didn't recognize — the comment line prints,
   NO code follows (`recompiler.cpp` prints the mnemonic THEN `default: return false`).
   Found: 39× `vsel128`, 20× `vandc` (repaired by `tools/regen/patch_dropped_vmx.py` —
   idempotent, RERUN AFTER ANY REGEN); trap family (`twllei/twlgei/tdllei/tdlgei/twi`,
   ~950 sites) is always-dropped upstream behavior (retail null-check traps), left alone.
   Detection recipe: scan for `\t// <mnemonic>` immediately followed by another comment
   or label. Proven kill: the dropped `vsel128` in `CameraCollisionSweepTest_sub_82179230`
   left a vcmpeqfp MASK as the sweep length → its fcmpu-EQUALITY recursion terminator
   never converged → unbounded self-recursion (`bl 0x82179230` @ 0x8217B36C) →
   0xC00000FD (32 MB host stack) on GameKernel runner 49 at the movie→FE transition =
   the iter-434 wedge crash variant. When an oracle disagrees with a VMX-heavy function,
   check for dropped instructions BEFORE deriving semantics.
11. **GameKernel runner handle map [iter-434e]:** pool handles 48/49/50/51 = the four
   `GameKernel::Loop` runners (mgr 89668480/89668530/896685E0/89668690; THCREATE
   jobFn=825FF4F0, inner=mgr). Loop = `GameKernelLoop_Run_sub_82720B50`, runs while
   mgr+0x42; vtables 0x82005B94/BBC/BE8/C10. The FE flow's condition evaluation ticks on
   runner 49's manager — if 49 dies, FSMCOND/LOADU polling stops entirely.

12. **Wait / semaphore / file-read contracts follow Xenia [iter-463, difftest-verified].**
   Each is its own gate, default ON, `=0` restores the old path:
   - `SR_WAIT_FULL_TIMEOUT` - finite Ke/Nt waits honour the full guest timeout (old: 5000 ms cap; Xenia xobject.cc:199-205).
   - `SR_WAIT_ALERTABLE` - NtWaitForSingleObjectEx waits alertably only when the guest's r5 says so (old: always, so a non-alertable wait could return USER_APC 0xC0; xboxkrnl_threading.cc:1024-1030).
   - `SR_THREAD_WAIT_STATUS` - a thread-handle wait returns STATUS_SUCCESS on exit, honours timeout + alertable, and the exited thread stays signalled for every waiter (old: 0xC0, 5000 ms, auto-reset). The movie-worker joins (e.g. handles 69-72, 90-93 in archive/v1_waitfix/boot_on1) now return 0.
   - `SR_NTWAIT_ESCAPE_SCOPE` - the 8 s INFINITE-wait USER_APC escape only under SR_ALLOW_FORCING and only on `isMainGuestThread()` (`=0` restores the old handle<=2 scope, still only under SR_ALLOW_FORCING).
   - `SR_SEM_STATUS` - NtReleaseSemaphore: LIMIT_EXCEEDED 0xC0000047 when the host release fails (over max, count <= 0), INVALID_HANDLE for an unknown handle, count never coerced (xboxkrnl_threading.cc:807-832).
   - `SR_ATOMIC_READ` (now default ON) - NtReadFile = per-handle I/O lock (`fileHandleIoLock`, Xenia XFile::file_lock_) + positioned `ReadFile(OVERLAPPED)` on the FILE*'s OS handle, position = offset + n. NtQuery/SetInformationFile and the xam_content write paths hold the same lock. The old seek+fread shears ~20 of 6000 concurrent 64 KB reads.
   - `SR_READ_EOF_STATUS` - a read starting at/after EOF returns STATUS_END_OF_FILE, IOSB {EOF, 0} (disc_image_file.cc:26-27; xboxkrnl_io.cc:151-153); any other failed host read is END_OF_FILE too (host_path_file.cc:33-43), an unmapped buffer is ACCESS_VIOLATION (xfile.cc:124-156), a failed read's IOSB is {status, 0}; the completion APC is queued only on SUCCESS. Only ByteOffset -1 means the current position (xfile.cc:114-117).
   - `SR_READ_PENDING` (**default OFF**, `=1` enables; bake ON only together with `SR_NTSTATUS_DOSERR` - with the generic RtlNtStatusToDosError stub returning 0 the stream readers see GetLastError 0 != 997, treat every read as failed and the intro movies are skipped: archive/v1_waitfix/e1_on1) - a handle opened WITHOUT FILE_SYNCHRONOUS_IO_ALERT/NONALERT (NtCreateFile stack arg 9 / NtOpenFile r8) gets STATUS_PENDING from NtReadFile unless the read hit END_OF_FILE; the IOSB keeps the real status + count, the event is set, and the file object is waitable (auto-reset event set after every read = Xenia XFile async_event_; xboxkrnl_io.cc:94-96, 167-169; xfile.cc:23, 198). The guest's XAPI CreateFileA sets FILE_SYNCHRONOUS_IO_NONALERT only when FILE_FLAG_OVERLAPPED is clear (0x824688DC), so the overlapped stream readers (CalAsyncFileStream 0x824776F0 / 0x82477328 / 0x82477B68, all tolerating ERROR_IO_PENDING 997) now see ReadFile -> FALSE + 997 as on the console. Deviation kept on purpose: NtOpenFile reads its options from r8 (NT order); Xenia's NtOpenFile_entry takes r7 (the share mask).
   - `SR_ASYNC_READ` (iter-466 stutter lane, **default ON**, `=0` = synchronous completion) - a real-convention NtReadFile with an explicit offset on a non-synchronous handle (no FILE_SYNCHRONOUS_IO_*, the flag recorded at NtCreateFile) completes on one FIFO worker: the caller gets STATUS_PENDING with IOSB {PENDING, 0} and the event + file object cleared; the worker reads under the per-handle lock, writes the IOSB, then sets the event and the file object and wakes the issuer. A read at/after EOF still answers at once. RtlNtStatusToDosError maps exactly 0x103 -> 997 under the gate (the stream readers need ERROR_IO_PENDING; SR_NTSTATUS_DOSERR keeps its default). The console completes overlapped reads asynchronously; Xenia does not (`if (true || is_synchronous())`). Proof: `difftest --async-read-test` 7 checks; docs/PERFORMANCE.md 7.3.
   - `SR_KESEM_STATUS` - KeReleaseSemaphore passes the adjustment through (<= 0 or past the limit adds nothing) and returns the previous count (xeKeReleaseSemaphore :743-758); NtCreateSemaphore with an impossible count/limit returns INVALID_PARAMETER with handle 0 (:785-792). Old: adjustment coerced to 1, return 0, limit <= 0 -> 1024.
   - Semaphore release failures log as `[SEM_RELEASE_FAIL]` with their own counter (first 64, then powers of two), so a zero is a real zero for the whole roll.
   - GPU_SEM auto-track (render-thread timed waits auto-released every 16 ms) is FORCING: only under SR_ALLOW_FORCING (`SR_GPU_SEM_AUTOTRACK=0` switches it off inside a forced roll).
   Proof: `build/difftest.exe --wait-sem-test` (22 checks) and `--atomic-read-test` (12 checks); each gate =0 fails exactly its own checks.

13. **Per-thread KPCR (real r13) and the Ke/Kf spinlocks follow Xenia [iter-463, difftest-verified; `kernel/kpcr.{hpp,cpp}`].**
   - `SR_KPCR` (default ON, needs `SR_OB_KTHREAD`; `=0` = r13 0 everywhere as before). Every guest-running thread gets an X_KPCR in the harness carve-out `[0xD00E0000,0xD0100000)` (stride 0x400; the KTHREAD arena now ends at 0xD00E0000). Fields (Xenia xthread.cc:399-409 @0bd090dbe): +0x100 prcb.current_thread = the thread's KTHREAD, +0x10C current_cpu (mirrored to KTHREAD+0xBF), +0x150 dpc_active 0, +0x18 irql, +0x30 be64 pcr_ptr, +0x70/+0x74 stack base/end, +0x2A8 prcb pointer. CPU = Xenia GetFakeCpuNumber: `creation_flags >> 24` mask (highest bit), 0 = the creating thread's CPU, else round-robin 0..5; main thread = CPU 0. **Oracle check (iter-463 review):** a structural scan of the live Xenia dump `sound_gameplay` (X_KPCR = be64 +0x30 == self and +0x2A8 == self+0x100; 29 KPCRs at 0x3000xxxx) gives: main XThread (start 0x82468D10, flags 1) cpu 0; 0x827A7420 workers cpu 5 (KeSetAffinityThread 0x20); 0x82110A30 (flags 0x04000080) cpu 2; 0x82566760 / 0x825671A0 (flags 0) cpu 0; 0x8247F9D0/FA00/FA30/FA60 cpu 3/1/3/2; XAudio worker 0x82117810 (flags 0x10000001) cpu 4 - all matched by our run except 0x82117810, which our game never creates (audio-arc divergence, not a KPCR defect). Xenia also sets +0x0 tls_ptr (the XEX static-TLS block); we leave it 0: no recompiled function reads r13+0 (TLS goes through KeTlsGetValue). KeSetAffinityThread is real (prev mask out, SetActiveCpu); the game calls it with masks 0x20/0x04/0x01 on its worker KTHREADs.
   - r13 is set on: the main thread, ExCreateThread threads (KPCR built on the creator's thread, bound in runGuestThreadEntry), every `GuestCaller` host thread (VD_ISR, GPU CP, rtam/fe_flow drivers), the render-driver callback thread. APCs run on the target thread's own context (its r13); the game imports no DPC API (KeInsertQueueDpc); the GPU hang watchdog runs no guest code. The import glue publishes the caller's r13 to exports (`ExportRegistry::getCurrentR13`).
   - **`SR_KT_BIND_VA` (iter-463 review fix, default ON; `=0` = the old handle lookup):** a thread created running (flags 0) binds to the KTHREAD block ExCreateThread built, not by handle. The flags-0 workers 0x82566760 / 0x825671A0 (created next to the NetDll_XNetStartup / WSAStartup calls) start AFTER their creator closed the only handle (`[OBREF] close ... last handle of a LIVE thread` precedes `[THREAD] STARTING`), so the handle lookup missed: each ran on an anonymous KTHREAD with a second, round-robin KPCR (cpu 3/4 instead of the inherited 0 that the Xenia oracle shows), r13 named a KTHREAD the guest never saw, and the real block was never marked exited. Fixed: for the same 37 created threads, kpcr_census_boot1 built 43 KPCRs / 6 anonymous KTHREADs, kpcrfix_boot1 41 / 4 (the two duplicates gone), and both threads keep cpu 0. Review rolls (natural, archive/v1_audio/kpcrfix_*): boot2 PASS (6 EOM, PRESS START 147 s, 0 GPU_HANG), boot4 PASS (PRESS START 178 s, one GPU_HANG H1 episode, 3 s flat at 222 s during the front-end load), boot1 and boot3 - each the first roll after a relink - were slow from the start (first present 41 / 76 s vs 14-32 s in every other boot, uniformly slow with no stall >2 s): boot3 timed out at 5/6 EOM still presenting (0 crash, 0 GPU_HANG), boot1 reached 6/6 EOM but not PRESS START by 240 s and also overlapped another workflow's 24-process job. Host state, not a stall, but unproven beyond that. The SR_KT_BIND_VA=0 A/B boot did not hit the race that roll (thread bound before the close), so T7 is the deterministic witness; every pre-fix KPCR roll (boot1-3, census, nospin1-2, long2-3) shows the two handle-miss anonymous KTHREADs. Note: the frames_sheets `t=` labels assume a 2 s capture cadence; long-roll frames are every 120 presents (~12 s at 10 p/s), so `dur=280s` in checklist.txt is not the roll length.
   - Guest readers (recomp census): +0x100 in 13 functions (RtlSet/SetNt/GetLastError 0x82469A70/A88/AC0 -> KTHREAD+0x160, so **GetLastError is now per-thread**; the D3D ring waits GpuBlockOnFence 0x820FD9A8 / GpuRingWaitReadPtrWrap 0x820FD150 / GpuRingWaitProgress 0x820F33E8 read KTHREAD+0x58 as the hang-trap timing seed - it is never written (Xenia leaves it 0 too), so the D3D hang trap D3DGpuHangHandlerTrap_sub_82106BA0 cannot fire, as in Xenia; XAudio 0x82117810/8D8/F10, 0x82118600), +0x10C in 10 (KeGetCurrentProcessorNumber 0x82469FD8, the GPU ISR's `1 << cpu` pending bit, XAudio's per-CPU worker slot), +0x150 in 3 (last error is not written inside a DPC). The +31164/+31172/+30848/+30472/-32584 hits are data disassembled as code.
   - GPU interrupts impersonate a DPC as Xenia EmulateCPInterruptDPC does: SetActiveCpu(n), irql 2, dpc_active 1, processtype_value_in_dpc TITLE, restored after (`KpcrDpcScope`), on the CP thread (cpu from the INTERRUPT mask) and on VD_ISR (cpu 2; VD_ISR no longer mirrors the main thread's r13). The review also put VD_ISR's legacy source-1 deliveries (only when the async CP is off) inside the scope: every interrupt callback is a DPC in Xenia.
   - `SR_KE_SPINLOCK` (default ON; off or `SR_KPCR=0` = the generic stubs): KeAcquireSpinLockAtRaisedIrql / KeReleaseSpinLockFromRaisedIrql / KfAcquireSpinLock / KfReleaseSpinLock / KeRaiseIrqlToDpcLevel / KfLowerIrql per xboxkrnl_threading.cc:1184-1350 - CAS 0 -> BE r13 on the lock word, same-CPU owner -> Sleep(0) else yield, release = store 0, IRQL in KPCR+0x18. With r13 = 0 the stubs gave NO exclusion, and XAudioWaitVoiceIdleUnlessAudioThread_sub_82118600 let every thread pass as the audio thread.
   - Live (natural boots, archive/v1_audio/kpcr_*): the only contended lock is XAudio's 0x829AC148 - holders 0x8212279C / 0x8211FCB4, waiters 0x82118480 (the acquire in AudioProcessVoiceQueueLocked_sub_82118428; the voice-idle wait XAudioWaitVoiceIdleUnlessAudioThread_sub_82118600 acquires at 0x8211872C, lr 0x82118730) / 0x8212279C; ~7-10K acquires and 70-150 contended per 5 s, < 1 ms spin per 5 s (`[KE_SPIN_SUM]`). No `[KE_SPIN_LONG|RECURSE|FOREIGN_RELEASE|NOPCR|BADLOCK]`.
   - Rolls (natural, archive/v1_audio/): intro boots kpcr_boot1-3 + kpcr_census_boot1 all PASS (6 intro EOM + PRESS START), as do SR_KPCR=0 and SR_KE_SPINLOCK=0 A/B boots. Long tape rolls (tutorial_tape_full, 1800 s): kpcr_long1 (partial, killed ~25 min by a user interrupt, 191 frames), kpcr_long2 (141 frames), kpcr_long3 (138 frames) - all reach gameplay with the HUD and keep presenting to the end, 0 crashes, 0 `[KE_SPIN_*]` anomaly lines; GPU_HANG episodes: only transient ones (long2 H3 at 5 s boot; long3 one H1 3 s flat at 1433 s, recovered). Contention over a whole long roll: ~1.1-1.3K contended of ~120-150K acquires, 6-8 ms total spin; one extra contended lock seen once (0x839F52F8, holder 0x820FD840, waiter 0x82110788, D3D). long2/3 present rate (~10 p/s) is below long1/gb_long2 (~15 p/s), but the same-session SR_KPCR=0 A/B (kpcr_off_long1, 161 frames) is no faster and has a 6-min ~2 p/s slow era, so the rate drop is host state, not the KPCR. The ~27-37 s present stall at the post-intro front-end load (CS 0x85E40068 wait at lr 0x822EB3D4, `[CS_WEDGE]`, sometimes a GPU_HANG H1) predates this change (gb1_boot2, gb_long2) and occurs with SR_KPCR=0 too.
   Proof: `build/difftest.exe --kpcr-test` (30 checks since the review: T7a/b = a thread whose only handle is closed before it starts still binds its own KTHREAD + KPCR and inherited CPU, T7m = the old bind-by-handle is seen running it on another KPCR; the original 27 checks: real sub_82469A70/AC0/FD8 and the real import glue; 4 host threads x 20000 through the glue = 80000/80000). SR_KPCR=0 fails 23, SR_KE_SPINLOCK=0 fails 4 (T3b/T3c/T4a/T4b); the in-test mutations (r13 = 0 -> last errors collide; stubs -> lost updates) are detected.

## 8. Techniques that work here

- **Golden-dump differential** ★: find a blob in `data/xenia_canary.DMP` (host = `0x100000000 + guestVA`; search unique bytes, `.writemem` the region), diff against the raw file — relocated dwords reveal pointer fields and object graph. Runtime objects (holders, TOCs) sit near the data and confirm contracts. See `tools/golden.py`.
- **K-parallel roll beating**: DOGMA makes everything roll-dependent; `tools/run_recomp_x.ps1 -K 4` + aggregate columns; a correct change may need 2-4 rolls to show. NEVER conclude from one run.
- **Trunk-first**: when a metric won't move across attempts, stop forcing sub-steps and question the framing (iter-294: the game loop itself was skipped). See memory `feedback_trunk_first_when_circling`.
- **Fabricate at the natural builder's shape**: read the natural ctor/builder first (`sub_827878C8` just zeroes 8 bytes → trivially fabricable; the ring was NOT — decompile the consumer too).
- **Drive verify-retry with fresh buffers**: failed guest calls only dirty private copies if each attempt allocates fresh inputs.
- Gate convention: `SR_*` env gates default **ON** once proven (`return !e || e[0] != '0'`), `=0` to A/B off.

## 9. Toolkit upgrade backlog (researched 2026-07-02 — community state of the art)

Sources: XenonRecomp/UnleashedRecomp (local mirror `reference/UnleashedRecomp/`),
ReXGlue interview, N64Recomp/Zelda64Recomp. Ranked by leverage:

1. **ADOPT MID-ASM HOOKS** ★★★ — XenonRecomp natively supports `[[midasm_hook]]`
   (TOML: name, address, registers[], return/return_on_true/jump_address/
   after_instruction). Native side = `void MyHook(PPCRegister& r3)` — the
   recompiler emits the call at that exact instruction. **UnleashedRecomp uses
   191 of them; we use 0.** Replaces/derisk: transient-global latching (hook the
   exact store), instruction-granular tracing without Frida or strong-def
   overrides, surgical branch fixes. ~~BLOCKER: requires regenerating guest/ppc~~
   **UNBLOCKED (iter-296, commit b45779d): the regeneration hygiene pass is DONE**
   — `tools/regen/` pipeline (README there) regenerates guest/ppc from the XEX +
   deterministic transforms; `verify.py` proves per-function byte-parity. New
   hooks: add `[[midasm_hook]]` to `tools/regen/superman_regen.toml`, re-run the
   pipeline. Migration debt: shrink the 287-body splice list into hooks/kernel
   overrides over time.
2. **XenonAnalyse switch-table pass** ★★ — DONE (iter-296): fresh XenonAnalyse
   reproduces the checked-in 555 switch tables EXACTLY; the 0x828EE region
   (worker drain `sub_828EEBDC`) genuinely has no jump table — that lead is
   closed, the drain's opacity is real (`halt_baddata` is data-in-code, not a
   missed table).
3. **Xenia kernel/GPU as a service layer** — EXCLUDED by direction (2026-07-02):
   the project stays fully native, no emulation layer. **XenosRecomp** (shader →
   HLSL static recompile) remains acceptable — it is static translation, not a
   service layer — and is the tool of choice when the render frontier arrives.
4. **Study the local UnleashedRecomp mirror before inventing** ★ — same engine
   generation, same recompiler lineage: their `UnleashedRecomp/patches/*`,
   `kernel/`, `gpu/video.cpp` show shipped idioms for hooks, kernel imports,
   and guest/host interop (`guest_stack_var`, byte-swapped guest types). We
   mined "4 lessons" once (docs/OTHER_RECOMPS_LESSONS.md); the full source is
   the reference for every new subsystem we touch.

## 10. Ghidra hygiene

When a contract is verified, immediately: `mcp__ghidra__rename_function_by_address` to a
`ProperName_sub_XXXXXXXX` (keep the address in the name — logs/recomp still say sub_XXXX)
and `set_plate_comment` with the one-line contract + ENGINE_MAP section ref. Decompiles
then teach the next session for free.

## §6g.15 The Apt aux UP-draw chain (16/24-byte strides) — NOT the blackout draw [GOLD chain / WITHDRAWN blackout claim — iter-455l, director verdict 035a3ee]

⛔ **WITHDRAWN (director verdict 035a3ee): this chain is NOT the blackout quad's
chain.** The measured blackout producer (iter-454 trace + replay, runtime-bound
prov-roll-2) is `DrawVerticesUP` **primitive 13, count 4, STRIDE 8** (POSITION@0 +
COLOR@4; raw color bytes 00 00 00 FF = BE dword 0x000000FF), while
`sub_826E4698` emits ONLY strides 0x10/0x18 (ppc_recomp.92.cpp:23348/:23503/:23875)
and the HLE UP hook preserves guest stride unchanged (hle_api_observe.cpp:1260).
Shader identity alone does not bind a draw to an emitter — the stride contract
must match. The blackout owner search continues among stride-8 emitters of
`sub_820FB6E8` (sole literal one found by the director: `sub_8231AEE8`, r6=8,
LR 0x8231AF1C).

The chain below REMAINS verified ground truth for the Apt auxiliary
16/24-byte-stride UP-draw provider (every hop IDA + golden-dump verified; no
unresolved virtual until the display-list walk):

1. `AptFrameRender_sub_822E9E68` — the Apt frame render: AptCore vcall +0x20 begin →
   `sub_826DC518(auxVB, 0, 0)` reset → **`g_AptExecutor` vcall +0x10 (executor, -1)** =
   render ALL Apt movie layers → `AptAuxVBFlushNotify_sub_826DE588` → debug console.
2. Apt shapes tessellate into **AptAuxVertexBuffer `dword_8296E9D0`** (ctor
   `AptAuxVertexBufferInit_sub_826DC568`, vtable 0x8208914C, 32-byte-stride arena
   "AptAuxVertexBuffer", cap 256; +32 = matrix/state obj `dword_8295F0F4`, +36 = draw
   provider `dword_8295F0F0`).
3. `RenderStateApplyQueued_sub_826DC650` drains the queue at :0x826DC6BC —
   **vcall provider vtable +0xC** with (provider, mode=aux[7], vtxArena=aux[10],
   count=aux[3], texObj=aux[6]).
4. Provider = **`dword_8295F0F0`**, the Apt vertex-array draw provider: 12-byte object,
   ctor `AptVtxDrawProviderCtor_sub_826E4550` (vptr 0x8208935C; base-class vtable
   0x8208934C; slots +4/+8 are literal `blr` stubs; +0xC = the draw), created by
   `FmuiGlobalInit_821516E0`, destroyed by `FmuiModuleShutdown_sub_82150EE0`; sole
   live instance 0x05070368 in the golden dump.
5. Slot +0xC = **`GfxDrawVertexArray_sub_826E4698`** → `D3DDeviceEmitDrawIndexedGen`
   UP draws (ret 0x826E4718 / 0x826E4820 / 0x826E4A94; strides 0x10/0x18; branch 3
   toggles render-state 212 vs `dword_8297B454==15`).

⚠ The .pdata entry at 0x820DC778 is the ONLY static xref to sub_826E4698 — the binding
above lives in .data vtables + runtime objects, invisible to naive xref walks.
(An observer spec targeting this chain for the blackout quad was REJECTED with the
withdrawal — wrong stride/color contract, and a drain-time stack walk names the
drain machinery, not the display-list producer. See ledger 035a3ee.)

## Additive costume animation associations (2026-10-02, static contracts)

This is decompiler/recompiled-code evidence, **not** a proven runtime hot-swap.
Authoring detail: `the owner's private mod project/research/costumes/ANIMATION_BINDING.md`.
Retail costume Lua consumes `SetCostumeNum` and calls ReplaceEntityVisRep:
`821CCED0 -> 821C7510 -> 821D9638 -> 821C6C00`. The visitor reads only `visrep`;
actor `+0x6C` changes, AnimSys `+0x64` does not. A new ENT's `animsys` field is
therefore insufficient to activate a source-proportioned suit's own banks.

Full entity construction uses `821D9B70 -> factory vt+52/821BF178 -> 82146938
-> 82143388`. Both `project` and `statemgr` attributes are required (the latter
may be empty). Projects must already be registered in `8295C608`; missing
project leaves controller's player pointer `+20` null. `82146FE0` parses project
name/rcbfile/bank references; `82146AD8` acquires type `C9A0B102` banks (`.bnk`).
Actor setter `821C6940` destroys and replaces AnimSys, but its only code caller
is actor construction `821D0DD0`; no gameplay hot-swap precedent is established.
Owner setter `8213D440` does not reconnect FSM/actions or start a clip.

The narrower same-controller/player migration remains an experiment:

- Controller: player `+20`, actor `+24`, cloth `+28`, aimer `+32`, project hash
  `+36`, bone count `+40`, cached value nodes `+100`; derived power vector
  `+112/+116`. Destroying through `82146800` tears down those resources.
- Bare bind `8213D1F0 -> 82709E38 -> 8270D170` does **not** clear old trees.
  Player active tree `+500`, state `+504`, transition states `+136/+188/+240`
  (records start `+132/+184/+236`), and layer chain `+572` need lifecycle work.
  Evaluation through `8270CBF0/8270D8F0` can still use the old trees.
- Reset `82709CB8` clears trees/transitions/layers AND ownership/backlinks
  `+12/+16/+20/+24`. Do not silently discard or invent those links.
- **Double-free hazard:** `821487B0` frees current/transition state without
  nulling it; reset then frees transitions again. Candidate fixture sequence:
  `8270EE50(player+504)` frees-and-zeros current state, then reset owns clearing
  transitions. This sequence now passes the bounded fixture below, not a live
  migration transaction.
- **List hazard:** `82140230` removing a project-list head writes project+32=0
  even with a successor at player+16 (also checked in generated PPC). An initial
  migration fixture must refuse shared-head removal. Detach also invokes
  `8213DFA8` for aimer overrides. `82142318` rebinds aimer bone indices;
  `8213D5E8` reallocates pose storage only when bone count changes.
- Cached nodes use generation player+516 through `82145D28/821404E0`; this is
  not proof of all FSM references surviving a project change.

Next bounded test: retained/preloaded projects with identical supported bone
ordering/count but own rest offsets; quiescent player, no layers/active powers/
cached nodes/unrelated overrides; validated old membership and exclusive head
or non-head removal. Refuse missing/incompatible/busy inputs before mutation.
Demonstrate the three hazards above as negative controls before implementing
detach/unregister, free/reset, reattach/register, owner/pose/aimer restoration,
constructor-equivalent `8270ABE0(player,0)` and return-to-retail. Preserve actor,
controller, player and FSM identities; prove balanced resources and no stale
tree pointers. Subsequent playback must come from the game's own request.
Natural spawning with an own preloaded project and live A -> B -> retail
continuity remain separate acceptance; do not fall back to stretching meshes.

### Bounded lifecycle execution and preload composition (2026-10-02)

`build/difftest.exe --costume-animation-test` runs 39 passing checks through the
real recompiled bind/attach/detach/reset/destructor/register/pose/pause bodies.
Only three virtual leaves are substituted: the heap free recorder and runtime
register/unregister recorders. Original globals, dispatch table and fixture
memory are restored. The three negative controls above execute and reproduce
their stated defects, rather than assuming them from decompilation.

The local candidate refuses missing/incompatible/busy/ownership/shared-head
inputs before mutation. In its restricted 103-bone, empty-bank, no-cloth/aimer
fixture, retail -> A -> B -> retail retains actor/controller/player/FSM pointer
identities, drains old trees, requests each of 12 granted buffers' frees once,
and balances three unregister/register pairs. Real `8270A6D8` then returns the
chosen fixture skeleton. Already-selected is a callback/allocation-free no-op.
It does not evaluate animations; preserved pointer identity is not proof that
real FSM/action references remain valid. Action-boundary and own-offset checks
are explicit stand-ins. The transaction is not connected to the shipping bridge.

Project preloading is content composition only (`docs/MODS.md`, own-animation
checkpoint), opt-in via exact `SR_COSTUME_PROJECT_PRELOADS=1`. It retains the
retail preload document and adds included, namespaced projects without replacing
SupermanAnims. Registry creation is atomic with the costume table. Runtime bank
registration, native material/cloth rendering, real action-boundary selection,
rollback and live retail/own/own/retail continuity are still required.

### Bounded read-only animation registration observer (2026-10-02)

`tools/mod_animation_probe.py` snapshots the append-order, unsorted project
registry at `*8295C608`, descriptors, circular bank lists, RTAM records, bank
header/table identities and the CCT skeleton/maps. Reads are range/count/cycle
bounded (64 KiB per read, 8 MiB total) and reread for snapshot stability. Live
mode uses Frida volatile reads only: no hooks, writes, guest calls, suspension
or polling. It requires `--pid` and an authoring `--descriptor`; expected names,
source SHA-256 and bank header hashes are checked. Missing/incomplete/shared
CCT records, roots or mutable maps refuse acceptance. JSON output is exclusive.
RTAM state uses the real `826E4D80` halfword masked-equality contract: `0x10`
means failed, `0x08` no-source; `0x18` is not automatically a failure.

Golden retail-only audit (the dump alias is absent from this worktree):

```powershell
python tools/mod_animation_probe.py --golden <repo>/data/xenia_canary_sound_gameplay.DMP --out archive/mod_animation_probe_golden_audit.json
```

Choose an unused output filename. The earlier golden receipt
`archive/mod_animation_probe_golden_final.json` passes: retail SupermanAnims,
six banks, 103 bones and 184 clip-map entries. `python
tools/test_mod_animation_probe.py` passes 15 synthetic controls, including
independent/shared resources, missing/wrong banks, bounds/cycles, snapshot races
and missing registry/input. These are **not live own-project registration,
actor binding, animation playback or costume-switch acceptance**.

## Fortress COSTUMES and the costume switch (iter-465; retail facts static, live results at the end)

**Retail (IDA + the player's own Lua/DAS/Apt, archive/v1_fortress):**

- Reaching it: ScrMap Y (FORTRESS) refuses during an objective (`Map_Fortress_UnavailableObj`)
  and during the first hour (`Map_Fortress_UnavailableHour`); `StateAdapter.IsFirstHour`
  (sub_822A1028 -> sub_822265B8) = objective status bit 6 (WARobj02) NOT set. The main menu
  (ScrMainMenu: METROPOLIS / FORTRESS) appears after a load only when IsFirstHour is false.
- ScrCostumes (ui.AST SCRCOSTUMES) InitGui: `RecordAdapter.GetCostumeData` (sub_822AF910)
  builds exactly 3 records: Name `Costume_Name_<i>`, Description `Costume_Desc_<i>`,
  ThumbFile `artassets/Costumes/thumbs/CostumeThumb_<i+1>.swf`, FullSizeFile
  `artassets/Costumes/fullsize/CostumeFullSize_<i+1>.swf` (the ui.AST movies COSTUMETHUMB_n
  128x128 / COSTUMEFULLSIZE_n 512x512: one image char, 128-byte header + DXT5 8in16 linear),
  Locked = !(BaseWorldFlow `costumeUnlocked` bit i) (sub_82206F08). Records 0..2 =
  "Superman Returns" / "Pod Suit" / "Golden Age Superman". ThumbViewer: 1 row x 3 columns,
  scrolls rows (mTotalRows/ScrollThumbs) when there are more records.
- HandleInput: A = ShowFullSize, X (SQUARE) on an unlocked record =
  `RecordAdapter.SetActiveCostume(i)` (sub_822B29E0, i < 3 only): FindFlowByName(mgr,
  "SupermenActorWorldFlow") (821F6850), flow+0x68 DAS `SetCostumeNum` := i+1 (821E6F90),
  then the popup sub_822B18E0(*0x8296EE90, off_82913718 `Costume_Wearing_MessageBox_Body`,
  off_82913714 `_Title`) = "NEW COSTUME SELECTED!". `GetCurrentCostume` (sub_822A3EE8) is a
  stub returning 1; nothing saves the choice (SetCostumeNum is DAS category 0).
- SupermenActorWorldFlow attributes: SetCostumeNum (Integer 0), Costume1 SupermanEnt,
  Costume2 SupermanGrayEnt, Costume3 SupermanClassicEnt, Costume4 SupermanCrazyEnt (no such
  entity exists). Its only FSM state S_60265281 (process): SetCostumeNum != 0 -> reset to 0,
  `ReplaceEntityVisRep(GetIDFromBinIndex("SUPERMAN",0), Costume<n>)` - the model only.
- Resets: BaseWorldFlow S_7E412353 (unload: load from an objective, quit, a loss without
  retry) requests Costume1; LoadMetropolis (S_4982930A, arena -> Metropolis) does
  ReplaceEntityVisRep(SupermanEnt). Story overrides: WARobj01/02 and the Lois / Mongul
  cinematics ReplaceEntityVisRep(SupermanGrayEnt). Superman is spawned from SupermanLayer
  (entityGroup fileName="SupermanEnt") by SpawnWorldFlowEntities (S_68 boot, S_266D0038 after
  the S_360D0B49 reopen). LuaDebugPrint is stripped (LuaStripDebugCalls_sub_82167A50).
- Whole-character precedent (Playable Bizarro, PLAYABLEBIZ): Setup DisableVisRep + PauseFSM
  Superman; SwapInBizarro ClearFilter("SUPERMAN", supes), SpawnEntityActive(entity, pos, ori),
  exit SetActorToDefaultController + UnpausePlayerControl; MainLinkToSubGraph exit
  ClearFilter + DespawnEntity(biz), AddFilter/EnableVisRep/ResetActor/SetCameraHomeEntity/
  SetActorToDefaultController on Superman. ScriptCmd_SpawnEntityActive 821D96E0 ->
  LuaSpawnEntityImpl 821D8A10 -> EntitySpawner_SpawnSingle 821D7830 is synchronous (owner
  WORLDFLOW<i> of the calling flow) and returns the new id to Lua. ApplyPowerups(id) is the
  Lua native the game uses after (re)spawning Superman (BaseWorldFlow S_69, WARobj01).

**Port contract (port feature, `SR_NATIVE_COSTUMES`, kernel/mod_costume_data.cpp) - the switch
contract the costume lane's packs match (asset contract 2026-10-03):**

- **Animation families.** An own-proportion suit animates on its OWN banks, and those load only as
  the six `SUPERMAN_*` bank entries inside `baseworl.AST` BASEWORLDFLOW_ANIM (loose `.bnk` banks or
  a second animsys project overflow the animation budget and hang the game; costume lane, Xenia
  and our build). The entity keeps `animsys project="SupermanAnims"` + the retail CCT / joint
  aimer / `clothanimdef SupermanCape`. **BASEWORLDFLOW_ANIM is read once per process, at boot**
  (before the title: one 2,436,819-byte NtReadFile of the baseworl.AST handle; none through new
  game, five objective loads and chapter transitions - archive/v1_fortress ng_hold2 log). So one
  game session animates ONE family: `retail` (the disc banks) or `banks_<16 hex>` (sha256 of the
  pack's bank set: every suit shipping the same bank bytes, e.g. nine suits of one pack series, is one
  family).
- **Pack form.** mod.json costume `"animation_banks": {"SUPERMAN_FLIGHT": "banks/x.bin", ...}`
  (1..6 of the six names; files outside DATA; each a Superman-skeleton bank: +04 kind 9, +0C
  0xE3D7527A; each no larger than its retail slot). A pack never ships a whole baseworl.AST.
- **Boot.** The worn suit (`<save root>/settings/costume.ini worn=`, the player's choice) picks
  the session family. For an own-bank family the exe writes the banks into the retail slots of
  the winning baseworl.AST IN PLACE (TOC size field updated, rest of the slot zeroed, no offset
  moves - the patch_ast_banks.py scheme; `modPatchBaseworldBanks`) and serves that copy from the
  generated registry layer. The autosave port feature is unaffected (it replaces the nested
  BASEWORLDFLOW_SCRIPT bundle as a loose file, entry S_69 only). Any failure (missing file, a
  bank larger than its slot, a non-bank) keeps the retail banks AND the retail default suit.
- **DAS** (SupermenActorWorldFlow, generated): `SRCostumeFamily<n>` per suit (retail 1..3 =
  `retail`), `SRActiveFamily` (constant, this session's), `SRWornCostume`, `SRAppliedTo`.
- **Switch.** The actor flow's costume state is the family-aware switcher: same consumer of
  SetCostumeNum; a suit of `SRActiveFamily` gets the retail `ReplaceEntityVisRep` at once (its
  VisRep + cloth def; the session's banks already fit it). A suit of another family is NOT
  switched (it would be stretched or squashed): WEAR remembers it (costume.ini) and the
  description field says "Superman wears this suit from the next start of the game"; the next
  start loads its family's banks and dresses Superman in it. The same holds for a retail suit
  in an own-bank session (SRCostumes.Retail -> `SR_CostumeWorn` replies queued 0). A Superman
  the game spawns itself (SupermanLayer: SupermanEnt) is noticed by its id (`SRAppliedTo`) and
  dressed in `SRWornCostume`. No whole-actor rebuild: a second project cannot be resident.
- Persistence (port feature, not retail): S_7E412353 no longer resets to Costume1, and
  LoadMetropolis keeps its own synchronous `ReplaceEntityVisRep(supermanID, <suit>)` right after
  `LoadWorldFlow` (the retail order) with the worn suit's entity instead of SupermanEnt (the actor
  flow's DAS is read only under `IsWorldFlowRunning`, the retail S_7E412353 guard). The worn suit's
  stable id is `<save root>/settings/costume.ini worn=` and becomes SRWornCostume at the next boot.


## Superman pose compose, the animation heap and the Bizarro switch (iter-465 costume lane, verified 2026-10-04)

- **Pose compose** `ComputeInstanceTransformsQuat_sub_8213FB90(out r3 {ptr, count, done@+8}, AnimPlayer r4,
  pose r5)` is the only reader of the blended local pose: bone b at pose+32+32b (quat x,y,z,w then translation
  x,y,z, BE floats), validity bitmask pointer at pose+3344 (sub_8270C4A0), bone info 16 B {parent, restIdx, ?,
  flag} at `*(skelEntry+8)` with skelEntry = `*(*(player+28)+28) + 20*(s16)player[524]` (count +4); flag 1 =
  from the pose, 2 = rest from `*(player+28)+88`, 3 = copy. Translations x the actor scale (player+20 = AnimSys,
  AnimSys+24 = actor, actor vt+36('ASCR') -> +80). Bone overrides (joint aimer / IK, AnimPlayer_FindBoneOverride
  8270A660: player+496 count, 12-byte records at +292) are applied inside the same call after the compose.
  Callers: the tail call at 82142F3C (r3 = *(*(*(player+20)+20)+24)) and ListInsertAllocEntry 82146710 (a stack
  player whose +20 is its arg2). Real-body fixture: `difftest --runtime-retarget-test` (branch feat/runtime-retarget).
- **AnimSys/actor links:** RegistryEntrySetChildSlot100 821C6940 (only from actor ctor 821D0DD0) stores AnimSys at
  actor+0x64 and sets AnimSys+24 = actor (8213D440); ReplaceEntityVisRep (821CCED0 -> AptDispObjBindVisitor
  821C7510(visitor, entity, name)) writes only actor+0x6C (821C6C00), inside 821C7510.
- **Animation heap:** DOGMA heap `HeapAnimation`, pool global 0x8296D8A0 ("Animation Memory allocator" in
  ResourceSystem_Ctor 8218F068), size = dword 0x829118A0 = 4 MiB in the 11-entry heap table at 0x8291189C
  (24-byte records {pool*, size, extra, name*, skip, index}: Animation 4 MiB, Gameplay 7, Physics 24, Render 19,
  Sound 26, UI 3.49, Vfx 4, Strings 0.73, EASTL 2.49, EngagingContent 0.73 (skipped), Main 11), built once by
  MemoryPoolSystemInit 82186500 as TibMem HeapGeneral (8278E1F8; dlmalloc-layout chunks, size flags in the high
  bits too) on a guest VirtualAlloc core (8278E018). Xenia golden `sound_gameplay` / `sound_menu`: core
  0x00070000, 3.09 MiB used, 0.91 MiB free (largest free 945,840 B); BASEWORLDFLOW_ANIM resident 2,436,840 B.
- **Bizarro switch (PLAYABLEBIZ):** PlayableBizarroEnt keeps `animsys project="SupermanAnims"` (X2_bizarro on
  Superman's skeleton; filter adds FEARSOURCE|BIZ, own powersys/targetsys); the mission's ANIM bundle adds
  BizarroExtraAnims (BIZARRO_EXTRA 286,880 B) and Superman_MIM_BIZ (134,228 B) to SupermanAnims. Swap Lua
  (PlayableBizarroFSM): copy Superman's position/orientation, ClearFilter("SUPERMAN"), move him away,
  SpawnEntityActive("PlayableBizarroEnt" | "BizarroRampageEnt", pos, ori), SetActorToDefaultController,
  UnpausePlayerControl; back: DespawnEntity(Biz), AddFilter/EnableVisRep/PauseFSM(false)/SetEntityPosition/
  SetEntityOrientation/DASPosture 0/ResetActor/SetCameraHomeEntity/SetActorToDefaultController on the saved
  `supermanID`. An own-proportion character on its own project = docs/MODS.md "Character packs".

## The pause map (ScrMap / MapAdapter) (iter-466 map lane; static IDA + golden sound_gameplay dump)

- SCRMAP (class ScrMap): the city is the 3D renderer through the MAP CAMERA (camMgr `*0x8296D86C`
  +0x1060, vt 0x82005944, tick `MapCam_UpdateTick_sub_8216D540`: yaw 0, pitch by height, pan / zoom
  by the sticks, clamped to the `mapCamLibDefinitions` "default" def from `MapCamDefsXML`:
  mapAreaMin (-8000,0,-7000), mapAreaMax (5000,4000,3000), fov 1.7). Apt draws only the frame, the
  hints, `mcObjText`, `mSupermanIcon` and `mcReticle`; the reticle is never shown in retail
  (`_FindObjective` / `_TargetObjective` have no caller; their strings are absent from the xex).
- MapAdapter natives (`MapAdapter_RegisterNatives_sub_822B8D20`): GetObjectiveDetails 822AC5E0
  (available TYPE_OBJECTIVE triggers + TYPE_MINIGAME with the objectiveFound bit + persistent
  minigames; title/desc from objectiveName/TextLocalizationKey), UpdateObjectiveDetails 822A6B78,
  TargetObjective 822A6CD8 (never called in retail), RegisterListener 822A0568, TransitionMapIn
  822A05A0 (camera state 6, 2 s blend), TransitionMapOut 822A0698, SetPauseMapActive 822A0738
  (`byte_8296EC55`), unused GetMapMode / GetThreatDetails / UpdateThreatDetails / TargetSuperman.
- Per frame: `PauseFlowStateTick_sub_82151298` case 4 -> `PauseMap_TickAnalogInput` ->
  `sub_822AC538` calls `ScrMap.SetSupermanIcon(x, y)` (AptExecutor vt+0x20) while the map is active.
- World -> stage: `PauseMap_WorldToStage_sub_822A6A28` through the live camera frame
  `V = *(camMgr+0x28)` (vt 0x82087820: +0x04 fov horizontal, +0x08 aspect, +0x14 tan flag, +0x20
  pos, +0x30 Euler, `AptBuildTransformMatrix_sub_826CBF70` R = Rz*Ry*Rx): x' = view.x / (z tan),
  y' = view.y / (z tan / aspect), stage = W/2 (1 - x'), H/2 (1 - y'), W/H = u32 at
  `*0x829A9384 + 0x3C / 0x40` (1280x720). Positions: entity ACTR +0x20, Y up; north-up map
  (screen up = +Z, screen right = -X). Host replica + self-check: kernel/map_markers.cpp, docs/MAP.md.
- **Apt numbers are float32 in this runtime (VERIFIED iter-466, race96_L frames vs the [MAP] stage
  log).** A value an AS script does arithmetic on keeps 24 bits of mantissa: a host reply packing
  (x, y) as `(x+16384)*32768 + (y+16384)` (up to 2^30) came apart with y off by up to +-32 px
  (logged (445,366), drawn (445,384); float32 rounding predicts +18), and `icon << 24 | colour` lost
  the colour for every icon >= 1. Rule for every fscommand bridge (mod_bridge replies, FlowMgr.SR_*):
  **any number the screen computes with must stay below 2^24**; send separate arguments instead of
  packing. (A host-side self-check cannot catch this: it never sees the screen's decode.)
- **Nested BGFA entry sizes use the header's width** (`wSize` at +0x24): 2 bytes in SCRMAP (65,535
  max), 3 in 308 retail movies (MAIN .apt 105,010). `tools/apt/bgfa.py` widens it when a patched
  entry outgrows it (SCRMAP .apt 69,932 with the race trail, docs/MAP.md 4.5).

## Shader constant library entry (2026-10-12; actual SDK and offline execution)

- **`GpuCmdEmitShaderConstantRuns` / `821075E0` [DECOMP + SDK unit oracle]:**
  a nonzero 64-bit mask selects groups of four shader registers, MSB first, starting
  at register `0x2388` and device bytes `+0x27A0`. The entry first emits a fixed
  52-byte synchronization preamble, including device `+0x281C`. A fast run fits
  only when `cursor + 16 * groups + 4 < dev[+44]`; its header is aligned to
  4 modulo 8. The `82107058` chunk helper splits at whole groups of four without
  that padding, flushing segments as needed. The SDK returns `r3=3` when no chunk
  helper ran; otherwise it leaves the last chunk helper's cursor in `r3`.
- **Dispatch [source + SDK unit oracle]:** production `initializeDispatchTable`
  maps `821075E0` to the strong replacement. Direct calls, the actual generated
  indirect-call macro and scoped SDK-oracle calls are checked independently in
  `tools/difftest/shader_const_runs_test.cpp`. Fourteen actual `820FC478`
  tessellated-caller fixtures use zeroed primitive tables; they prove this caller's
  state/constant path, not a rendered tessellated draw or live caller distribution.
- **SDK fallback while holding an Entry [unit oracle]:** the arena mutex is
  nonrecursive. Both `Driver::flushState` and `Entry::guest` scope nested SDK work
  with `ShaderConstRunsOracle`, preventing a changed A/B gate from re-entering
  the native leaf while the mutex is held. The held-Entry SDK test completes;
  the intentionally unscoped negative control acquires the lock and times out.
- The isolated 23-process positive matrix and lock control are recorded in
  `archive/v1_release_codex_20261012/shader_const_sdk_v2_unit_matrix.json`.
  `SR_NATIVE_SHADER_CONST_RUNS` is default ON since the construns bake (2026-10-14; =0 keeps the SDK body).
- **Caller domain (IDA, verified 2026-10-14):** 821075E0's four callers (820FB6E8 / 820FBBF8 / 820FC000 /
  820FC478) each call it as `rldicr r4,r27,9,5` behind `and r11,r27,63<<49 ; beq`: the device's six dirty bits as
  the mask's top six bits, at most six groups, never zero. Its entry flushes only when cursor > dev+48, and
  GpuCmdBufBeginNewSegment 820FCF90 sets dev+48 = dev+44 - 160 on both branches, so at the worst entry every caller
  mask takes the fast path and never 82107058's chunked one (`--shader-const-runs-test bake`; per the decompile, mask 0 would
  underflow the SDK body's group loop, so the tests hand an empty mask only to the native leaf, which declines it
  before any write). The recording-pin fixtures
  prove real SDK internal flushes without a GPU; arbitrary SDK admission modes,
  live ring/backpressure, whole-game fidelity and release composition remain
  separate obligations in `docs/native_ledger.json`.

## Complete present-gate entry contracts (2026-10-12; IDA bodies and assembly)

- `82111EC8` skips the whole block for interval `0x80000000` or no previous
  front buffer, always sets device `+0x2A39 |= 0x20`, and preserves the incoming
  64-bit `r3` on the skip path. Otherwise its sequence is performance timestamp
  begin, a cursor/threshold kick check, the 39-dword gate, and performance
  timestamp end. Interval 0/1 maps to one vblank, 2 to two, and 4 to three;
  other intervals load an uninitialized stack word and are outside the defined
  contract. These are actual caller/body instructions, not older IDA comments.
- `8210BB08` emits only when device `+0x4E70 == 2` and the next slot differs
  from reader `+0x4E74`. Next slot is writer `+0x4E78 == 3 ? 0 : writer + 1`.
  The four dwords are EVENT_WRITE_SHD, `0x80000003`, physical timestamp address
  with endian bit 2, and `0xDEADBEEF`. The address derives from `+0x4E6C` plus
  `4 * (2 * nextSlot + (begin ? 0 : 1))`. This helper does not advance the writer.
  Each emitted timestamp has its own `cursor > dev[+48]` kick check. The slot
  offset is computed before that kick, but the timestamp base at `+0x4E6C` is
  read after it: a recording allocator can change the base during the flush.
- The gate restores incoming `r3` before its final timestamp call. If that
  final helper does not kick, it leaves the incoming 64 bits unchanged; if it
  kicks, the SDK returns the new segment cursor from the flush, before the
  16-byte timestamp emission. Preserving incoming `r3` unconditionally would
  therefore be incorrect for full performance-capture coverage.
- **Implementation [bounded SDK unit oracle]:** the default-OFF native leaf
  now implements the defined begin/gate/end sequence, including capture and
  real SDK recording-pin flushes. Unsupported intervals, segmentless admission
  modes and malformed allocation state decline before mutation. Nested SDK
  work is guarded against native reentry while the nonrecursive Entry mutex is
  held; an explicitly scoped SDK gate oracle also keeps its nested flush on the
  original SDK path.
- **Dispatch and evidence [offline]:** leaf, direct, production-table indirect
  and scoped SDK routes each run 28 fixtures in record, literal and reconstructed
  modes. They compare full device/segment state, emitted bytes or scratch-store
  ordering, full-width return and preserved registers, and two native transport
  replays. Actual SDK allocator callbacks change timestamp base and writer/reader
  during begin/end kicks; moving the base read before the kick fails all four
  dynamic-base fixtures. A separate wrong-return control fails the four final
  kick cases. V5 records 594 passing assertions in 17 positive processes at
  `archive/v1_release_codex_20261012/present_gate_full_v5_unit_matrix.json`.
  This proves these defined recording-pin contracts, not live pacing, arbitrary
  SDK modes, GPU backpressure, whole-game fidelity or release composition.

## SDK software submission and park pair (2026-10-12; actual SDK differential)

- `820FD800` appends a software-list node through the inline `820FCA80`
  sequence when device `+0x2A74` is nonzero. Both the producer and `82110768`
  consumer use the actual Kf spinlock at device `+0x2A78` (consumer state
  `+0x3C`). The count and terminal marker must be read again under that lock:
  the consumer can park before producer acquisition, or the producer can link
  before consumer acquisition.
- Chaining initializes all four node words before publishing the old tail
  link, increments the count under the lock, and returns incoming full-width
  `r4 + 16`. The direct branch increments outside this lock, passes a caller-
  stack `{dwords, physicalAddress}` descriptor to `820FD218`, and returns
  incoming full-width `r4`. Preserve that SDK ordering.
- `82110768` follows a linked marker with `uint64_t(marker) + 4`. For the
  `0xC0000000` terminal it decrements nesting, handles the pause/completion
  event, records the park position, releases the spinlock, then waits. A
  parked position is not an automatic restart of a new list.
- The default-OFF `SR_NATIVE_TRANSPORT` pair uses the actual guest imports and
  lock owner/IRQL protocol. Invalid primary alignment, mappings or guest owner
  decline before imports/mutation; an invalid tail found under the lock
  releases it before declining. Stable caller-owned mappings and storage
  lifetimes remain a precondition. Direct publication still calls `820FD218`.
  V6 also required a writable EndSync marker even though the SDK only reads it.
  V7 uses a fresh readable permission check while retaining writable state,
  alignment, guard/no-access rejection and the locked marker read. The host
  bank passes 106 checks; eight compiled defects include restoring writable-
  only marker admission, which fails two explicit native-admission assertions
  before SDK fallback. Direct-entry and production-dispatch comparisons cover
  readable-only terminal and linked markers against the actual SDK. Astra
  accepts this bounded correction. New sampled decline categories identify
  owner, append input, tail input, EndSync state or marker rejection; these
  independently sampled counters need not add up atomically.
- V6 host evidence passes 77 checks through production dispatch and the
  actual SDK oracle. Controlled two-host-thread races cover both winners;
  seven compiled defects fail their named checks with passing gate-off
  controls, exact source restoration and a restored build/test. Astra accepts
  this bounded checkpoint. This proves CPU SDK submission/park behavior,
  not the full pump, deferred identity or arena lifetime, gameplay fidelity,
  Native0 or removal of emulation from the shipping package.
- V14 live sampling observes both submission routes and park/follow activity,
  but declines are nonzero (last samples: 842 plain, 761 CPPM4). Eight observed
  original-EndSync calls have aligned inputs and matching bound owners; their
  page permissions were not captured. The readable-only limitation is not the
  proven cause of these declines. Preserve the strict decline-zero requirement.
- V15 replay and an uninstrumented plain probe complete. Replay is 9/9 unchanged;
  six of seven declared items pass, and activity fails with 465 sampled declines
  (append-input 347, prior-tail 118). Owner, EndSync state and marker categories
  remain zero through the last sample. This narrows the current probe to two
  admission categories, not a failed subpredicate or V14 cause. No CPPM4 freeze,
  stress, flicker gate, visual/pacing, default or full cutover acceptance follows.
- EndSync's state block (2026-10-14, read from the recompiled 82110768 and the
  fixture layout): r3 = `dev+0x2A3C`, so the lock `+0x3C` is `dev+0x2A78`, the
  nesting count `+0x30` is `dev+0x2A6C`, the pause pointer `+0x50` is `dev+0x2A8C`
  and `+0x38` is the chain count `dev+0x2A74`. At a terminal marker the pump
  decrements nesting. Only when nesting reaches 0 with no pause does it drain
  `dev+0x2A74` by one and set the event at `+0x40`. Otherwise it resets that event.
  D3DDeviceBlockUntilIdle 820FDCB8 flushes, waits on its fence (reason 4), then
  spins until `dev+0x2A74 == 0`. It returns only with the count drained.
- V16's 16 reject samples (8 node, 8 prior-tail) are all committed 0xA-window
  pages with PAGE_READONLY: the GPU write tracker's validation protection, which a
  guest store releases through the tracker's VEH. V17 (12343e71, tracker-owned
  read-only pages admitted) records decline 0 in all 124 samples. Attributing the
  V14 / V15 declines to this tracker-admission gap is an **inference**, not a
  proof. V14 has no reason counters. V15 has only the input / tail categories, with
  no permissions. V16's permission snapshots were taken after the rejection, so
  they do not prove ownership at that moment. The fix and V17's zero declines
  support the attribution without proving every historical decline. Either way the
  declines were an admission property, not a property of the scenario.
- Admission A (tpair-fix lane, 2026-10-14, after the live run tpl1): the pair's admission is arithmetic, with no
  OS query. The fresh permission queries of V7-V17 (QueryWorkingSetEx, VirtualQuery and the tracker query) stalled
  the pump's EndSync and the render thread's append for milliseconds live. VirtualQuery walks the resident pages of
  the marker's whole same-protection region; protection churn adds tails. A range is admitted when it maps
  contiguously into the registered guest arena and touches no declared foreign-protected range.
  - The arena is one committed read-write allocation.
  - The GPU write tracker's PAGE_READONLY pages are stored into as a guest stores: the first store faults into the
    tracker's handler, which releases and invalidates the block.
  - Every other host protector of guest pages declares to `kernel/guest_protect.hpp` (image watchdog boot page: a
    range; its opt-in modes and SR_PROTECT_ZERO: exclude all). The pair declines every entry, counted `exclusion=`,
    while a page-protecting diagnostic is requested or declared.
  - `tools/difftest/guest_protect_census.py` checks that list textually. It reads the exe's ninja build inputs, guest/ppc
    and FFmpeg included, and fails on any protection, allocation, mapping, release or resolver reference it has not
    classified, and on link flags or pragmas it does not model. It does not preprocess, so token pasting and raw
    system calls are outside it.
  - A declaration publishes; it does not quiesce. A protector that can start while the pair runs needs pre-exclusion
    through a listed gate, or a drain of the calls in flight.
  - The arena's lifetime: every native entry holds a `guest_protect::ArenaLease` across its guest accesses, the
    park's wait included. The teardown release (`releaseGuestArena`, from freeGuestMemory) retires the arena
    first: later entries decline (`retired=`) and leases in flight drain within 250 ms. Otherwise the release is
    skipped and the arena stays mapped. Scope: a declined entry falls back to the SDK body, so this covers the native
    admission only, not the pre-existing teardown race of guest threads that outlive the release.
  - Readable-only, guard and no-access pages are no longer queried. Outside the declared protectors, nothing in the
    port makes guest pages so.
- Per-present A/B (transport-pair lane, 2026-10-14): `SR_AB_TOGGLE=SR_NATIVE_TRANSPORT`
  with `SR_AB_PRESENT=1`. Each entry reads the window once, before any read, import or
  write. An OFF window runs the SDK body whole and is counted `abOffAppend` /
  `abOffEnd` on the census line, not as a decline. Mixed per-call traffic is exact
  because both bodies leave identical state and imports on the shared lock.
