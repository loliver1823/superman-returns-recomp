# EA Apt / ui.AST — External Research Dossier (menu blocker)

**Date:** 2026-06-12 (post-iter-190)
**Target problem:** frontend menu never constructs. Chain (per iter-190): clean Apt globals → FMUI ctor → flow factory (`sub_826D4930`) → g3 flow object → frontend movie loads from **ui.AST** → Apt ActionScript interpreter ticks → ≥2 execution targets on stack (`0x829A19B0/B8`) → g1/g2 → menu. This doc maps **external** resources onto that chain.

---

## 1. Confirmed locally (this session)

Probed `Superman Returns (USA)/DATA/ui.AST` (89,595,420 bytes):

- Magic: **`BGFA1.05`** — "BigFile Archive v1.05" confirmed (matches the wave-3 RAG finding).
- Contains **820 `Apt Data\0` blobs** (first at offset 0xCC686) and 10 `FWS` (uncompressed SWF) signatures, 0 `CWS`, 0 `DDS `.
- Header u32 at +0x0C = 0x33C = **828 ≈ entry count** (820 Apt + ~8 others) — consistent with the madden-file-tools layout below.

**Implication:** the menu UI is **EA Apt format** — the same Flash-derived `.apt`/`.const` system used by SAGE-engine games (BFME, C&C3, RA3) — *not* raw SWF. The wave-3 note "ui.AST contains SWF blobs" was imprecise. This unlocks the entire SAGE-modding/reimplementation ecosystem as reference material.

---

## 2. External resource map

| Resource | What it gives us |
|---|---|
| [OpenSAGE Docs — APT format spec](https://opensage.readthedocs.io/file-formats/apt/apt.html) ([repo](https://github.com/OpenSAGE/Docs)) | Reverse-engineered APT format: `Apt Data\0` magic, Movie root character, characters, `.const` companion file (constants + initial character offset), AS bytecode layout |
| [OpenSAGE engine](https://github.com/OpenSAGE/OpenSAGE) (`src/OpenSage.Game/Gui/Apt/`) | A working C# **ActionScript VM + Apt player**: execution contexts, display list, frame advance, sprite/clip script execution. The reference for what our interpreter's "execution-target stack" entries are and when they exist |
| [feliwir/libapt](https://github.com/feliwir/libapt) | C++ Apt parser/renderer; `docs/` includes the **SWF v9 spec PDF** the format derives from; notes EA's custom bytecodes beyond standard AS2 |
| jonwil's `apt2xml`/`xml2apt` (via [Revora modding threads](https://forums.revora.net/topic/74801-editing-apt-files/)) | Original community format knowledge; converts apt/const ↔ XML for inspection |
| [bep713/madden-file-tools](https://github.com/bep713/madden-file-tools) (`streams/ASTParser.js`) | **The AST container layout** (same Tiburon lineage): 0x30 header (8-byte sig, block/file counts, TOC offset/len, offset-shift, desc-len), self-describing variable-width TOC fields (`id, startPosition, fileSize, uncompressedSize`), padding = `(shift - (size % shift)) % shift`, entries are compressed streams (community uses OffZip-style zlib scan) |
| [withmorten/big4f](https://github.com/withmorten/big4f), [Experiment5X/BIG-File-Extractor](https://github.com/Experiment5X/BIG-File-Extractor) | EA BIG-family context (BIGF/BIG4). Our container is BGFA, not BIGF — these are background only |
| [GHFear/RW4ArchiveTool](https://github.com/GHFear/RW4ArchiveTool) | RenderWare 4 archive tooling — relevant to the `ea_aptaux_rw45` glue layer (Apt-on-RW4.5) |
| [Xenia compat issue #1468 — 454107ED Superman Returns](https://github.com/xenia-project/game-compatibility/issues/1468) | **Ground truth that the menu is reachable**: "loads into the menu where you have to press start, then Xenia crashes." The healthy boot→menu sequence is capturable in Xenia |

---

## 3. How this maps onto the iter-190 open items

**(e) ui.AST read = success metric → make it a *verified input* too.**
Write `tools/extract_ast.py` from the madden-file-tools layout (§2) and extract the frontend `.apt`/`.const` pair. This (1) verifies our extracted ui.AST is intact, (2) names the frontend movie assets so we know exactly which `NtReadFile` offsets into the ui.AST handle to watch for, (3) gives us inspectable AS bytecode (via apt2xml/libapt) for the frontend movie — i.e., we can read *what the menu's own scripts will do* before we can run them.

**The ≥2-execution-target gate — semantics confirmed by OpenSAGE's player model.**
In the Apt/Flash execution model, an execution context exists per *executing movie/clip timeline*: the root Movie plus each active sprite running script. Two entries ≈ root movie + frontend clip executing. This independently confirms the iter-190 agent recon ("entries exist only while a loaded Apt movie's script executes — no missing push to patch") and tells us what plausible fabricated entries must look like if we take the diagnostic host-push shortcut (`sub_826C2FD8(0x829A19B0, root)` ×2): entries are alive-flagged (bit27@+4) object pointers from the Apt factories, not bare cookies.

**Healthy-boot ground truth: capture a verbose Xenia run.**
Compat issue #1468 confirms boot→menu works in Xenia. Our existing [docs/_attic/xenia_canary_log.txt](_attic/xenia_canary_log.txt) is only 132 KB (module load + 1 NtCreateFile line — default log level). Re-run Xenia Canary with kernel logging high (`--log_level=3`, optionally `--log_file`) and capture splash→menu: the exact file-open/read ordering (when ui.AST opens relative to splash end), thread creation, and XAM calls. That sequence is the reference our self-driven boot should converge to, and directly serves the OTHER_RECOMPS_LESSONS Finding-1 investigation (does the streamer use Event/APC completion?).

**Long-term "menu visible" Apt stub (wave-3's 5–8 day estimate).**
If driving the game's own interpreter stays blocked, OpenSAGE's Apt player + libapt's renderer are working reference implementations of the *consumer* side: load frontend .apt, run AS2 VM, render. The wave-3 stub strategy can crib the player loop structure from OpenSAGE rather than deriving it from Ghidra alone.

**(a) script-engine junk-state source — external help is limited by design.**
The junk-state family (bytecode-VM `sub_827657C8` walking never-constructed state) is our-harness-specific; the structural fix remains OTHER_RECOMPS_LESSONS §4 (allocator hooks). One external datum helps validate: in a real boot the Apt allocator vtable (`DAT_829a23e4`) and singleton (`0x829A23E0`) are written during FMUI init *before* any interpreter tick — OpenSAGE's init ordering (create player/env → load movie → tick) matches; any tick observed before env init in our logs is by definition the corruption family, not legitimate traffic.

---

## 4. Suggested artifact order

1. `tools/extract_ast.py` (BGFA reader per §2) → extract + name frontend .apt/.const from ui.AST. *Small, immediately verifiable.*
2. Verbose Xenia Canary capture of boot→menu → file/kernel sequence reference log into `docs/` (replaces the 132 KB stub log).
3. apt2xml/libapt pass over the frontend .apt → document movie name, exports, first-frame AS (what "ActivateUI"/flow commands the script actually calls — cross-check against the ~70 flow-script commands Function_822B62E0 registers).
4. Keep the OpenSAGE AS VM repo path pinned for the eventual interpreter-semantics questions (bit27 alive flag, target-stack push/pop timing).

## 5. Artifact results (2026-06-12, same session)

**Artifact 1 — DONE.** [tools/extract_ast.py](../tools/extract_ast.py) carves all blobs by magic scan (the BGFA TOC's 41-byte records resisted field decoding; magics are version-suffixed `Apt Data:1:7:4\x1a` / `Apt constant file\x1a`, NOT NUL-terminated). Result: **820 .apt + 820 .const pairs, strictly alternating, stored uncompressed**, extracted to `data/ui_ast_extracted/` with TOC names paired in order (heuristic but credible: `SCRPAUSEMENU`, `SCRAUDIOOPT`, `SCRCONTROLLEROPT`, …). TOC layout for the record: 16 bytes fields + 25 bytes NUL-padded name, 41 bytes total, after a 16-byte ` GNP…/ COL…` TOC header; field encoding still undecoded (didn't block anything).

**The frontend movie is `0038_ICONMENU`** (.const @ container offset 0xE5C20, 64 KB string table, 935 unique strings). Its script vocabulary confirms the whole engine-side chain read from Ghidra:
- Screens: `screens/bootflow/ScrPressStart` (the press-start screen Xenia reaches), `screens/frontend/ScrMainMenu`, `screens/misc/ScrSaveLoad`, `screens/misc/ScrPauseMenu`.
- Flow idiom: `_global.FlowMgr`, `ChangeInterfaceState`, `CurInterfaceState`, `StateAdapter` — matches the binary's `g3->vt[+28]("ChangeInterfaceState", 0, "FE", 1, "_global.FlowMgr")` exactly.
- Engine-command counterparts: `StartGame`, `LoadHUD`/`UnloadHUD`, `LoadOverlayScreen`/`UnloadOverlayScreen`, `Notify_LoadMainFinished`, `SetInterfaceStateBG/FG` — cross-check against Function_822B62E0's ~70 registrations.
- Full main-menu icon set (`IconMetropolis`, `IconFortress`, `IconUpgrades`, `IconCostumes`, …) — useful later for verifying menu render output.

**Artifact 2 — DONE.** Verbose Xenia Canary capture (`--log_level=3`, 180 s, GPU rendering active during capture) → `logs/xenia_verbose_boot.log` (62 MB). Ground-truth findings:

1. **The healthy boot performs a single-threaded open+query sweep over ALL .AST packages back-to-back** (thread `F800004C`): `TEXT → parammap✗ → arenas✗ → xml✗ → pus✗ → textures✗ (dev files, open fails cleanly with 0xFFFFFFFF handling) → lunits → sound/sound1 → voice → _workinp → metropol → fmvlegal → objtiv1/objtiv → ui → baseworl → fmv`. Per file: `RtlInitAnsiString` → `NtCreateFile` → `NtQueryInformationFile(class 0x22 FileNetworkOpenInformation, len 0x38)` — **no content reads inside the sweep**. ui.AST is opened in this early sweep, NOT lazily at menu time.
2. **Our divergence point is iteration 2 of that sweep.** In our build (run M): TEXT.AST opens (caller_lr=0x8246894C), class-0x22 query succeeds (our stub's layout matches Xenia's call exactly), then the game immediately content-reads TEXT.AST through worker-queue jobs (64 B, 128 B, `NtSetInformationFile` class 0xE seeks) and drifts into DOGMA list ops — parammap.AST is never attempted. The sweep loop's continuation condition is the thing to find (decompile around caller 0x8246894C's parent loop). Plausibly the same root cause as the script-engine junk-state family: the package list/registry driving the sweep may live in those degenerate structures.
3. **Zero `NtReadFile` calls appear anywhere in the 62 MB healthy log** (the one match is the XEX import-table dump). Either Xenia filters that import's logging or boot-time reads use another mechanism — do not assume Event/APC semantics are exercised at boot; the OTHER_RECOMPS Finding-1 (async completion) stays deprioritized.

---

## 6. Round 2 (2026-06-12 late): external research on the streaming-pool spawn

**Frontier (per [[reference_apt_extraction]] / 190n-p probes):** the streaming thread pool (6× each of `0x8247F9D0/FA00/FA30/FA60` + 3 singles, from the descriptor table at `0x820D18xx`) never spawns in our build — a DOGMA worker (`0x827A7420`) in a healthy boot executes a posted job that spawns it; the job is never posted in ours. Open question: *who posts it* (suspected FM module init — FMStreamingCore/FMTaskManager — that never registers self-driven).


### Closed lead

No GDC talk / Game Developer Magazine postmortem for this title exists in the indexable record (multiple targeted searches; only retrospective forum threads). Engine internals (`DOGMA`, FM* framework, tibmem) have zero public documentation — the debug build is strictly better evidence anyway.

---

## 7. Round 3: retail-log thread names crack the spawn chain (2026-06-12, late)

Xenia implements the MS SetThreadName protocol (`RtlRaiseException` 0x406D1388 — **which our harness no-ops**, iter-189) and attaches the game's own thread names to XThreads. Both retail logs contain them. Mined from `logs/xenia_verbose_boot.log` (+ `logs/xenia_retail_mar24_named_threads.log`, preserved from the Xenia folder before overwrite):

### Healthy-boot thread map (creator → thread)

| Creator | Thread (handle) | Name | Note |
|---|---|---|---|
| — | F8000008 | **Main XThread** | |
| Main | F800003C | **ResStorageManager** | string @ `0x8208A878`, referenced from descriptor data @ `0x826F960C` (FMUI-pool neighborhood; not in lifted code — likely a thread-spawn descriptor `{name, entry, …}`) |
| Main | F8000048 | **OnLoad** | |
| Main | F800004C | *(empty name)* | **the AST open+query sweep thread** (§5) |
| Main | F80000CC/DC | (unnamed) / **RealMemCardThread** | |
| Main | F80000E8/EC/F0/F4 | **GameKernel::Loop** ×4 | = the `0x827A7420` DOGMA-worker family. **Healthy boot: 4. Our harness: 24 (!)** — investigate where our 24 comes from (descriptor-table over-spawn?) |
| **F80000F0** (GameKernel::Loop #3) | F8000148/14C/150/154 ×4 batches, F8000128–134, F800020C–218 | (anon) | **the entire streaming pool** — every pool thread is spawned by this ONE worker |

### The init job, fingerprinted (verbose log line ~452248+)

On `F80000F0`, immediately before the first pool spawn:

```
XGetVideoMode(…)
MmAllocatePhysicalMemoryEx ×5  (sizes 0xC, 0x78, 0x60, 0xB8, 0xB8; align 4,4,32,32,32; flags 0x404) → FEED4000…FEED0000
ExCreateThread(stack 0x10000, xapi 828A8C08, entry 8247F9D0, param 4009E030, CREATE_SUSPENDED=1) → F8000148
ExCreateThread(…, 8247FA00, 4009E030, 1) / (…, 8247FA30, …) / (…, 8247FA60, …)
```

### Next steps this enables (short Ghidra exercise)

1. Xref `__imp__XGetVideoMode` → the candidate that also calls `ExCreateThread` 4× = **the init-job function**. Xref *that* function's address as data (job descriptors hold fn pointers) → **the poster**. Then the FMUI playbook (register/activate or host-drive the posting module).
2. Decode the descriptor at `0x826F960C` (name ptr for ResStorageManager → adjacent fields = entry VA etc.) — likely an instance of the same spawn-descriptor shape as the `0x820D18xx` table.
3. Implement SetThreadName capture in our harness (we already intercept `RtlRaiseException` 0x406D1388 and log p1 — store the *name* per thread and emit it in logs) → free thread attribution forever after.
4. Chase the 4-vs-24 GameKernel::Loop discrepancy — our 24 workers may be a descriptor-table over-spawn that itself perturbs boot.

---

## 8. Round 4: §7 lead CLOSED — the quad threads are the FMV decode backend, not a package-mount gate (2026-06-13)

The §7 "short Ghidra exercise" is done. Full chain, mapped via recomp call graph (Ghidra's decompiler chokes on the `__savegprlr_*` prologues for most of these; the lifted source was the reliable path):

```
sub_822E5100  — FMV player tick; reads player global @0x829761E0; fn-ptr table @0x82098120 (after "main","ui" strings)
  └─ sub_8235F488  — player state machine (state @player+4, 0..5 jumptable)
       state 1: if backend@0x829761D4 == 0:
       └─ sub_824715F0([0x829761D0], params from player+536/544, &out@0x829761D4)   — "CreateBackend"
            └─ sub_82470F28 → sub_82470A58 → sub_82472068 (Initialize: vt[+0x28], this[+0x38]=env)
                 └─ Function_82471A50 — XGetVideoMode + allocs 0xC/0x78 + vt[+0x60] → ExCreateThread ×4 (8247F9D0/FA00/FA30/FA60)
```

**Conclusions that change the plan:**

1. **The 8247-quad threads are per-movie streaming/decode workers, created lazily by the movie player's state-1 (open) transition — one batch per FMV played, torn down between movies.** The healthy log shows 6 batches (legal/logo/attract FMVs), with the same backend object (param 41A62DC0) re-created 4×. They are NOT a boot-time "streaming pool" and NOT a package-mount prerequisite. They never spawn in our build because the host splash driver fakes FMV completion (mgr+0x230=-1/+0x234=1) without ever running the player. Expected, not a bug to fix.
2. **The package mounter does not have its own thread.** `sub_8236EBD0` never appears as an `ExCreateThread` entry in the healthy log. Mount work runs as jobs on the `827A7420` (GameKernel::Loop/DOGMA) workers — which our build has (24 of them). Healthy-boot 827A7420 roster: ResStorageManager (param 400025B0), OnLoad (40002690), the unnamed AST-sweep thread (40002770) — all three created back-to-back at boot — plus 8 more later incl. the 4 named GameKernel::Loop.
3. **The "sweep stalls at iteration 2" theory (§5.2) is dead.** Run 190p shows our build does the package-registry discovery via **`NtQueryDirectoryFile` enumeration of DATA/** (2 dir opens, all packages + sizes returned, incl. ui.AST) instead of Xenia's per-file open+stat sweep. Data-equivalent route; registry inputs exist. Total NtCreateFile in 190p: 3 (2 dir + TEXT.AST via the device-vtable open `sub_82601FD0` = vt[+0x0C] of the fs-device class, vtable @0x82074C10).
4. Therefore **the menu-critical path is unchanged from iter-190's open items (a)/(b):** clean Apt globals (kill/catch the script-engine scribbler poisoning 0x829A23E0/E4) → re-roll `SR_DRIVE_FMUI_CTOR` → real flow manager (g3) → frontend movie (ui.AST mount) → AS ticks → screen stack ≥2 → g1/g2 → menu. ui.AST mount remains the success metric (§3e).

Side notes for later: the FMV backend chain above is exactly what `"PlayFMVs"` (0x822A2640) ultimately drives; if we ever switch from faking FMV completion to running real FMVs, state-1 of `sub_8235F488` is the entry, and result codes land at 0x829760BC with error sentinel -1 at 0x829761D8. SetThreadName capture in the harness (§7.3) is still worth doing for free thread attribution.

**Same-day addendum (iter-191 runs a/b/c):** the watch window from §8.4 caught the corruption root in one run — the FMUI pool pop-compact (`sub_826F9CD0`) executing on a scribbled queue whose `{begin,end}` were overwritten by a `vector::insert` grow-copy backed by a **garbage DOGMA grant** (`sub_8278D258` → 0x7D8802A6). With the compact guarded (kernel/cs_timeout_underflow_fix.cpp) and the FMUI-drive hook made idempotent, run 191c had **g3 + the Apt flow singleton alive and stable for the entire run** (singleton refcount 0xA — ten consumers) and produced the first new file request ever past TEXT.AST: `fonts.AST` (absent on disc, clean FILE_NOT_FOUND). The menu-chain frontier moves to: (a) DOGMA garbage grants (168 REWINDs/run), (b) with g3 stable — does the flow reach `ChangeInterfaceState("FE")` → ui.AST mount → Apt execution-target pushes (g1/g2 gate). Full detail in PROGRESS.md iter-191 entry and RAG #116.

---

## Sources

- [OpenSAGE APT docs](https://opensage.readthedocs.io/file-formats/apt/apt.html) · [OpenSAGE/Docs repo](https://github.com/OpenSAGE/Docs) · [OpenSAGE engine](https://github.com/OpenSAGE/OpenSAGE) · [OpenSAGE blog — year in review 2018 (APT work)](https://opensage.github.io/blog/year-in-review-2018/)
- [feliwir/libapt](https://github.com/feliwir/libapt) · [Revora: Editing APT Files](https://forums.revora.net/topic/74801-editing-apt-files/) · [Revora: APT UI (BFME1)](https://forums.revora.net/topic/88086-apt-ui/)
- [bep713/madden-file-tools](https://github.com/bep713/madden-file-tools) · [withmorten/big4f](https://github.com/withmorten/big4f) · [Experiment5X/BIG-File-Extractor](https://github.com/Experiment5X/BIG-File-Extractor) · [GHFear/RW4ArchiveTool](https://github.com/GHFear/RW4ArchiveTool)
- [Xenia game-compatibility #1468 — Superman Returns](https://github.com/xenia-project/game-compatibility/issues/1468)
