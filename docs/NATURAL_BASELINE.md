# Natural Baseline — the one true progress number

**The sole measure of progress.** How far the game gets on its OWN, with `SR_ALLOW_FORCING`
off (zero forcing). It advances ONLY when the game genuinely does more by itself. Every entry
is reproducible: run the default build and look. See `docs/NATURAL_PATH_CHARTER.md` §4.

**How to reproduce the current baseline:**
```powershell
# SR_ALLOW_FORCING defaults OFF. Solo rolls only; repeat sequentially for confidence.
.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 240   # no forcing env set
```

Record for each entry: date · commit · furthest NATURAL point reached · how measured · notes.

**Saves are part of the repro (iter-463).** Real save storage is on by default, and the natural
game enumerates saves at boot: a found save changes the front-end path. The roll harnesses give
every run an EMPTY save root of its own (`run_recomp_x.ps1`: `archive/content_rolls/<stamp>/run<i>`,
`contentDirs` in `last_run_x.json`; `sr_drive.ps1`: `<attempt>/content`). A seeded roll passes
`SR_CONTENT_DIR=<seed>` (copied in, never written). Each entry states "saves: empty" or the seed.

---

## Log (newest first)

### 2026-10-09 - iter-467 p2: the forcing audit is clean - the last ungated forcing (U1, the ring allocation) is replaced by the platform fix (frontier unchanged)

**Not a frontier move** - the natural game still runs NEW GAME to the ending (2026-10-05 entry below). What changed is
the caveat every entry since 2026-10-01 carried: `audit_forcing.py` UNGATED 2, the `sub_82467F40` override that served
pre-claim physical requests from the guest heap and published the first grant as the ring (U1). Dev **f7c28315**:
VdInitializeRingBuffer takes the ring from its physical argument like Xenia (`SR_VD_RING_ARG`, default ON, with the
real MmGetPhysicalAddress `SR_MM_PHYSADDR`), and the heap route runs only under `forcingAllowed()`;
VdGetSystemCommandBuffer gets Xenia's fill (`SR_VD_SYSCMDBUF`; before, Swap emitted display-register PM4 from stack
garbage) and the scan-out registers answer (`SR_VD_SCANOUT_REGS`).
- **Audit (dev f7c28315):** `python scripts/audit_forcing.py` -> "RESULT: no UNGATED, non-allowlisted forcing sites,
  and no registry-classified band-aid active by default"; UNGATED 0; host launches FAIL 0; generated guest/ppc 444
  hand edits, 0 ungated. Still listed (not failures): 37 DEFAULT-ON own-gated sites - registry-classified platform
  fixes and native replacements.
- **Natural roll on the merged exe (no gates set):** title-era roll `SR_HIGHFPS=0` 300 s - ring at VA 0xBEF2E000 from
  the argument, Xenia's fill on every swap, every intro movie to its end, no GPU hang, no ICALL_FAIL; replay 9/9 vs
  dev b5ddf7226ad0; flicker gate PASS (`archive/v1_checkroll/p2merged/flicker`). Open: two 3 s GPU-watchdog
  episodes in that span roll's every-present BMP dump window (57 s vs the usual 12-13 s, overlapping an archive-wide
  disk scan) - believed disk contention, unproven; watch for H1 outside dump windows.
- Saves: empty.

### 2026-10-05 - iter-465 playthrough: the natural game runs from NEW GAME to the ending (dev-tool assisted)

**Natural code path, dev-tool assisted.** Port features off, no mods, forcing off, saves empty. Roll
**nat1** (`archive/v1_playthrough/rolls/nat1`, exe snapshot `7a3032393921-playthrough2` = c75a5ba6): the
game's own front end (START -> LOAD A GAME? -> START NEW GAME), then the level-skip dev tool walks the
whole story spine from the game's data: MSTobj01-03, Warworld (WARobj01/02), CinematicLuthorSteals,
chapters 3-18 with their cutscenes and transitions, MTOobj09 (Metallo), BIZobj03 / BIZobj15 (Bizarro),
TWSobj04 (the Twister), and **CinematicLexIsland** (snapshot `S21_won_126`): the ending FMV plays (Earth from
space, the kryptonite island lifted away, the Daily Planet's "WHY THE WORLD NEEDS SUPERMAN", Metropolis at
sunset) and the game returns to free roam with its post-game metro events. One process, RESULT SUCCESS,
**0 `[ICALL_FAIL]`, 0 crash markers, no hang** (the known audio crash 823399D4 did not occur here, nor in any
of the 8 port-feature rolls of the same lane: 0 in ~3.5 h of gameplay). Frames:
`rolls/nat1/drive/attempt1/frames` (ending 162-172), sheet `rolls/nat1/sheet_end.png`. The port-feature twin
**pf6** reached the same ending (docs/PLAYTHROUGH.md).

**What the dev tool did (not the game):** `pass` (objectiveComplete/objectiveWon) on each objective after a
~60 s gameplay hold for the four bosses; `reach` (a trigger-radius poke) for triggers a player would fly to;
15 `xp_to_cap` writes where the real game gets hero points from metro events; FastFlyer races 95/96/97 ended
by `pass` as the state-105 carriers of those writes (a harness script flies Superman off each race start);
`metro_lock on` for the idle Twister hold. Everything else - loads, transitions, cutscenes, chapter advances,
the ending - is the game's own code. So this proves the game RUNS start to finish on our build; it does not
prove a player can win each fight (that needs played rolls).
**Repro:**
```powershell
py -3.13 tools/devtools/skipplan.py plan --target 126 --image xenia:sound_gameplay --carrier auto --out t126.txt
python tools/devtools/playthrough_plan.py t126.txt nat.txt
# beside the roll: python tools/controls/harness_play.py --instance playthrough --log superman_returns_playthrough.log --script tools/scenarios/playthrough_carrier_move.txt --out <dir>\play.jsonl
.\tools\sr_drive.ps1 -Scenario tools\scenarios\playthrough_chain_nat.json -Out <dir> -TimeoutSeconds 4800 -Gates "SR_INSTANCE_ID=playthrough,SR_PORT_FEATURES=0,SR_MODS=0,SR_DEVTOOLS=levelskip+cheats,SR_DEVTOOLS_CMDS=<abs nat.txt>,SR_DEVTOOLS_OUT=<dir>\devtools,SR_LEVELSKIP_DECLINE_SAVES=1,SDL_AUDIODRIVER=disk"
```
(archive/v1_playthrough/roll.ps1 wraps both.) One roll is a sample; pf6 is the second, port-feature run.

### 2026-10-04 - iter-465 physmem: the Warworld -> Metropolis return no longer kills the natural game

Platform blocker removed on the natural code path (no mods, port features off, forcing off).
Before: after WARobj02 the game re-requests its 224 MB world pool and our physical allocator
refused it ([PHYS_CAP] -> [ICALL_FAIL] 8260527C), 2/2 on dev (nomodsAB1, base3). After
(SR_THREAD_MEM_XENIA + SR_PHYS_XENIA_PLACE + SR_PHYS_BIG_GAP, default ON): the pool is
re-granted, Metropolis reloads, CinematicLuthorSteals is won and gameplay continues (gap1,
gap2 on the branch build, baked1 on the dev-merged build with no gates). Xenia oracle: the same
return passes (`archive/v1_physmem/xenia_234914`). PROGRESS 2026-10-04 [physmem], ENGINE_MAP 6e.
**How measured - dev-tool assisted:** the level-skip plan
`data/devtools/plans/metropolis_return_from_new_game.txt` drives the game's own objectives
to WARobj02 (tag: dev-tool-assisted, never baseline evidence by itself); everything after the
WARobj02 pass - the return, the reload, the cinematic - is the game's own code. A player who
reaches WARobj02 by playing now gets back to Metropolis. Saves: empty.
Repro: `.\tools\sr_drive.ps1 -Scenario tools\scenarios\physmem_metropolis_return.json -Out <dir> -TimeoutSeconds 1500 -Gates "SR_MODS=0,SR_DEVTOOLS=levelskip,SR_DEVTOOLS_CMDS=<abs plan>,SR_DEVTOOLS_OUT=<dir>,SR_INSTANCE_ID=physmem,SDL_AUDIODRIVER=disk"`.

### 2026-10-02 - iter-464 directory platform correction (mods/ship lane)

Natural storage-registration evidence, **not a further gameplay/visual milestone**.
With no mods/port features, forcing/dev tools off and empty saves, the actual
DATA FileStorage table contains 3/12 disc ASTs on the old packing path and 12/12
with Xenia-faithful one-record directory answers. All 12 IDs match the Xenia
sound-menu dump. A rebuilt default-on launch without `SR_NTQDIR_XENIA` also
contains 12/12. All three completed the same no-input FE readiness +90s scenario.
Fix gate defaults on in this lane; `=0` remains a developer comparison.

Repro/receipts: `docs/MODS.md` section 12, scenario
`tools/scenarios/mod_directory_natural.json`, read-only actual-table probe
`tools/mod_storage_probe.py`; main `archive/directory_natural_20261002/` rolls
off/on/default, lane JSON snapshots under that same relative directory. A/B exe
`49660f7bf193-native-costumes-dircheck`, default exe
`1ae84e34f9c3-directory-default` (c99aa26 plus the directory-default source edit).
No gameplay, audio or renderer correctness claim; inherited forcing-audit count
remains two. Dev promotion and the broader mod/character acceptance are open.

### 2026-10-01 - iter-463 correction: D-AUDIO-CMDBUF is NOT closed - it recurred on HEAD bc1f8fc with the XMA port on

**What happened:** the collision rolls (archive/v1_invest/collision/route; exe snapshot `64af19c4de72-collroll`,
HEAD bc1f8fc, no gates beyond the collision watch and present dumps, `SDL_AUDIODRIVER=disk`, saves: empty) ran the
user's tutorial tape. Roll `roll1` stopped at `[ICALL_FAIL]` call_site **823399D4** (AudioGetOrCreateNamedAsset
vt[+4]) 4.6 min after launch, ~100 s into gameplay, with `SR_AUDIO_OUT=1` and `SR_XMA_NEW=1` both logged. Same object
`r31=87337770` and `r27=8A620253` as the pre-port crashes (burndown2 `diag_audio`); this time its vtable word is
`8264CE28` (a code address, RwacCmd_ExecSetParamF) instead of 0. The other 4 rolls of the same exe ran their full
dwell with 0 ICALL_FAIL (roll1b: the same tape, 21 min; roll2/3/4: synthetic flight tapes).
- So "0 `[ICALL_FAIL]` in every roll with the XMA port on" (entry below) held for its sample but not in general:
  the port made the crash rarer (1 of 5 gameplay rolls here), it did not remove its cause. Status: **open**.
- Not caused by the collision watch: the same site and object crash in builds without it (burndown2 `diag_audio`,
  `diag_audio2`, `iso_heapzero` have no `[COLL_WATCH]` banner), and the 1800 s `b_long1` gameplay roll ran with
  the watch on and no crash.
- The crash the user saw on screen at ~13:17 was a different exe: the devtools worktree roll (`sr-wt-devtools`,
  ebd429a, which does NOT contain the audio merge b829918) hit the same 823399D4 stop - expected there.
- **Repro:** `.\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 1800 -Gates "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr"`
  (one roll is a sample; repeat sequentially).

### 2026-10-01 - iter-463: the natural game has sound, and it runs past the title again (D-AUDIO-CMDBUF closed by the native XMA port)

**What changed** (branch audio/part2, merged b829918): the audio arc steps 4-8, all default ON. A native port of
Xenia's AudioSystem drives XAudioSubmitRenderDriverFrame (`SR_AUDIO_OUT`); Xenia's "new" XMA decoder
(xma_decoder=new, dedicated thread; FFmpeg XMAFRAMES fork as ffmpeg_xenia.dll) replaces the XMA fakes
(`SR_XMA_NEW`); MmGetPhysicalAddress, KeWaitForMultipleObjects and KeGetCurrentProcessType are Xenia-faithful
(`SR_MM_PHYSADDR`, `SR_KE_WAITMULTI`, `SR_PROCTYPE`). The rolls found two more platform gaps, both fixed:
KeResumeThread was a generic stub, so the CREATE_SUSPENDED XAudio worker never ran (`SR_KE_RESUME_THREAD`), and
KeSetEvent could leave a stale wake token that deadlocked the pump/worker handshake (`SR_KEVT_SET_ATOMIC`).
No forcing: the Sound flag is the game's own (burndown round 2) and nothing writes guest state for it.

**Natural point reached (default exe, forcing off, saves: empty):** intro movies WITH their audio, the title,
the attract demo, and (tape) the tutorial and free flight - the pre-burndown reach, now with the audio subsystem
live instead of muted. D-AUDIO-CMDBUF does not occur: 0 `[ICALL_FAIL]` in every roll with the XMA port on.

**How measured** (archive/v1_audio/rolls; no audible sound: `SDL_AUDIODRIVER=disk` + the in-process
`SR_AUDIO_WAV` dump of exactly what the device pulled):
```powershell
.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 300 -Gates "SDL_AUDIODRIVER=disk,SDL_DISKAUDIOFILE=<dir>\sdl_disk.raw,SR_AUDIO_WAV=<dir>\out.wav"
python tools\audio_movie_xcorr.py --dump <dir>\out.wav --ref <movie>.ref48k.wav ...   # refs: the disc's fmvlegal.AST ASF audio, ffmpeg -ar 48000
```
- Merged main build, no gates (f_main_final2): E1 PASS (6/6 intro EOM, PRESS START 104 s), 0 ICALL_FAIL,
  0 GPU_HANG, 236/279 non-silent seconds, XMA kicks = works 8164, 0 decode fails.
- **Oracle comparison, intro:** each of the five voiced intro movies' disc audio is found in our output at speed
  1.0000, median 0.5 s-window correlation 1.000, no gaps (f_main_final2, a_all_e1/e2, a_all_r1). Movie 02 has a
  silent audio track on the disc.
- **Gameplay** (b_long1, tutorial_tape_full, 1800 s, all audio gates on): RESULT SUCCESS, 180 frames, contact sheet
  shows every era (intro, LOAD A GAME?, HUD gameplay, map, flight); 1408/1440 non-silent seconds; XMA ~770K kicks
  = works; one H1 GPU_HANG episode at 1226 s, recovered.
- **Attribution (one gate =0, the rest on):** SR_XMA_NEW=0 -> ICALL_FAIL 823399D4 (the D-AUDIO-CMDBUF site) - the
  fakes were the cause. All gates off (c_r0_default) -> ICALL_FAIL ~150 s, silent. SR_AUDIO_OUT=0 -> runs but silent.
  SR_KE_RESUME_THREAD=0 or SR_KE_WAITMULTI=0 (with SR_PROCTYPE=1) -> silent + ICALL_FAIL.

**Still broken / unverified:**
- **A/V sync in the long title movie:** its audio (31.1 s, at the right speed) ends ~11-14 s before the video
  (EOM after 42-45 s; Xenia 31.3 s). Video decode is still ~25 % slow (pre-existing, audio-independent).
- **Gameplay audio content is not oracle-checked.** Waveform search of all 779 sound.AST sounds finds 6 in the
  attract demo (peak 0.62-0.95) but only 1 in the gameplay dump: 3D mixing/pitch defeats the matcher. No live
  Xenia audio capture was taken (user asleep; loopback needs an unmuted endpoint).
- The SDL disk driver is not a real-time clock: under host load it pulls slower than 48 kHz (a_all_e2 played
  236 s of audio in 295 s), and the game's credits throttle with it. A real device does not.
- 2 XMA kick timeouts (250 ms) in the long roll, one during a GPU-hang dump.
- SR_KEVT_SET_ATOMIC=0 deadlocked 1 of 3 boots; the fix is proved by difftest W13, not by roll counts.

### 2026-10-01 - iter-463: kernel burndown round 2 rolled; the default exe forces nothing, and the natural game now stops at the title on an audio defect it used to be kept away from

**What changed** (branch burndown/round2, merged into dev): items 1-12 and U1 of the round-2/3
kernel list (docs/BANDAIDS.md, burndown round 2). Three things matter for this number:
- **The Sound flag is the game's own.** `cfgSoundOffThread` no longer runs by default, so FMSoundSystem builds its
  pools and mixers as on the console: the mixer globals 0x82976040..74 match the sound_menu golden at the same heap
  offsets.
- **No fabricated indirect-call returns.** A NULL, out-of-range or unmapped bctrl target logs `[ICALL_UNRESOLVED]` and
  stops with `[ICALL_FAIL]`. It no longer returns r3=0.
- **A real bug that fail-stop found on the first boot, now fixed (e54568d).** The spliced FM init-graph walk
  `sub_8271E630` declared a variable after a goto. clang dropped the empty-array exit, so every boot ran the walk body
  on ysRagdoll's empty dependency array: a NULL vtable call, a recursion on address 0, and FmModuleInitOnce on vtable
  words. `-Werror=microsoft-goto` now guards the whole tree.

**Natural point reached (default exe, forcing off): intro movies, then the title (PRESS START / `main.apt`), then
`[ICALL_FAIL]` 2.5-3.5 min in.** The previous entry reached HUD gameplay and the tutorial. That progress was **forced**: it
depended on the Sound-flag zeroing, which kept the audio subsystem from running at all. With the flag natural, the
pre-burndown exe never presented a frame (ab_main_sndon: 0 VdSwap in 300 s). The burndown exe gets through the intro to
the title and then hits the open defect below.

- **Open defect D-AUDIO-CMDBUF (audio stage):** once audio runs, a heap object is overwritten by a 16-byte audio
  command. `sub_8264CE48` appends `{fn=0x8264CE28, voice, param, value}` at `*(q+32) + *(q+40)` and advances `+40` with no
  bounds check. Frida caught the write (diag_audio2): Audio_SetVoiceParamValidated_sub_82327378 ->
  sub_8264CE48 wrote over the named-asset object 0x87337790, whose golden twin is 0x03857790
  `{vt 0x82022FC8, name, hash 0x89FA2887, ref 3}`. The next lookup in AudioGetOrCreateNamedAsset_sub_82339900 calls
  vt[+4] = 0x8264CE28 / 0xC003000C / 0x80000000, which is the ICALL_FAIL.
  - The other failure site, `FindOrCreateChildAndDispatchEvent_sub_8234ADF8` (a NULL child after
    `Audio_CreateSoundObjectByType` returned 0), is the same run of corruption until shown otherwise.
  - Most likely cause: the queue is not drained at the console's rate in our platform, so the write offset outgrows its
    buffer. Not proven.
  - Not the cause: the heap-zero removal (iso_heapzero fails the same way).
- **Everything else in the burndown, isolated from audio:** `SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF`
  re-enables only the Sound-flag zeroing (a named diagnostic, not the default).
  - Full tutorial tape: RESULT SUCCESS, 177 frames to p21120 over 1565 s, 0 ICALL, 0 GPU_HANG, 0 crash marker.
  - All 4 contact sheets viewed: movies, LOAD/NEW dialog, space cinematic, HUD gameplay, flight, meteor tutorial,
    street idle. Horizons are level.
  - Pace: VdSwap #21210 in 1565 s, against #24900 / 1602 s for the pre-burndown f42586aedcd7 exe (v1_fix1/long1) and
    #22880 / 1519 s for kpcr_long1. That is 10-20% fewer swaps in the gameplay minutes, with no gap over 3 s after
    boot. The cause is not attributed: the scheduler-throttle removal and host load from another workflow's rolls are
    both possible.
- **Intro movies:** boot1/boot2 6/6 EOM. The four Xenia trace-dump FMV oracles match p60/p1/p100/p222 exactly (MAD 0, 0
  differing pixels) in both. boot1's pre-movie boot was 25 s slower than boot2's (host contention; the slot was shared).
- **Audit:** `python scripts/audit_forcing.py` gives UNGATED 2 (`gpu_ring_alloc_fix` sub_82467F40, an open defect:
  gating it needs a real MmGetPhysicalAddress and a physical-address VdInitializeRingBuffer first), DEFAULT-ON 0,
  band-aid default-ON 0.
- **Repro** (saves: empty):
  ```powershell
  .\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 300 -Gates "SR_FMV_EOM_PROBE=1"          # default: title, then ICALL_FAIL
  .\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 300 -Gates "SR_ALLOW_FORCING=1,SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF"   # diagnostic arm
  .\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 1800 `
      -Gates "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr,SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_DIR=archive/<dir>/frames,SR_PRESENT_DUMP_SPARSE_EVERY=120,SR_PRESENT_DUMP_SPARSE_CAP=600,SR_ALLOW_FORCING=1,SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF"
  ```
- **Runs** (archive/v1_burndown2/rolls; exe snapshots burndown2_d7a4e545c0d5 / burndown2_100ae2c1cfd7; SDL_AUDIODRIVER=disk):

  | roll | arm | result |
  |---|---|---|
  | boot1_icallfail_69d9a41f | default, before the goto fix | ICALL_FAIL at 6 s: sub_8271E630 NULL call (the goto bug) |
  | boot1 | default | 240 s: 6/6 EOM, FMV oracle exact, ScrPressStart at 192 s, 0 ICALL, 1 boot GPU_HANG H3 at 5 s (presents=0; same as the baselines) |
  | boot2 | default | 6/6 EOM, FMV oracle exact, ScrPressStart 167 s, **ICALL_FAIL 206 s** (8234AEC0, NULL child) |
  | diag_audio / diag_audio2 | default + Frida | ICALL_FAIL at ~194 s / ~146 s (823399D4, overwritten named-asset object) |
  | iso_heapzero | isolate SR_REAL_HEAP_ZERO | ICALL_FAIL 177 s at 823399D4 (heap zeroing is not the cause) |
  | iso_sndoff | isolate SR_CFG_SOUND_OFF | 300 s: 6/6 EOM, ScrPressStart 154 s, 0 ICALL |
  | ab_main_sndon | pre-burndown exe, Sound flag natural | 0 VdSwap in 300 s (the old iter-316e wedge) |
  | long_iso | isolate SR_CFG_SOUND_OFF, full tape | SUCCESS, 177 frames / p21120 / 1565 s, 0 ICALL / GPU_HANG |
  | final_main_boot | default, MERGED main build (f257436) | 6/6 EOM, FMV oracle exact, ScrPressStart 140 s; GPU_HANG H1 at 166 s (presents flat 4.8 s, then no further swaps logged); **ICALL_FAIL 183 s** at 8234AEC0 (D-AUDIO-CMDBUF class) |

### 2026-09-30 - iter-463: host-launch burndown closed; the default exe runs no forcing except the one declared cfg-sound-off band-aid

**What changed:** every forcing site the host-launch audit found (0bddbcd) is now master-gated or deleted, except
one declared band-aid. Group A (de0d1db) covered the 1 Hz image restore, the MODGLOB poller, the RTAM drive threads,
the alloc probes, the legacy fake GPU IRQ / VdSwap completion and XNotify force-bootui; signalInit3Ready, the
SR_RUN_REAL_GAMELOOP=0 arm and the dead FMUI drive were deleted. Group B covered the SR_MAIN_BOOTSTRAPS splice calls
(8e03f36), the game-loop quit-flag clear (602404c, now observe-only), the CRT doexit flag writes (fe467ca, deleted) and
the OOR critical-section force-recreate and pretend-success (3af02dc). No Group B site had to be left declared.

- **Build:** `3af02dc` and later. `SR_ALLOW_FORCING` is off.
- **Natural except ONE declared band-aid:** `cfgSoundOffThread` (SR_CFG_SOUND_OFF) still zeroes the gameconfig
  Sound flag, **so the game is silent**. It retires at the audio stage: per-thread KPCR plus real Ke/Kf spinlocks
  first, then un-force (docs/BANDAIDS.md; docs/HOST_THREADS.json "declared").
- **Audit:** `python scripts/audit_forcing.py` exits 0 with UNGATED 0. Host launches: 64 sites, FAIL 0, DECLARED 1,
  ALLOW 12, MASTER 12, INERT 39. All 4 open_forcing entries read FIXED.
  - **Not covered by this burndown:** the forcing audit still lists 55 DEFAULT-ON own-gated sites, each behind a
    private SR_* env that defaults ON. Platform fixes are expected there, but no one has reviewed the list site by
    site. "Natural" in this entry means master gate off plus the host-launch audit clean. It does not mean each of
    those 55 is proven to be a platform fix.
- **Repro** (saves: empty):
  ```powershell
  python scripts/audit_forcing.py                                                    # exit 0, 1 DECLARED
  .\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 240 -Gates "SR_FMV_EOM_PROBE=1"      # boot + intro movies
  .\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 1800 `
      -Gates "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr,SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_DIR=archive/<dir>/frames,SR_PRESENT_DUMP_SPARSE_EVERY=120,SR_PRESENT_DUMP_SPARSE_CAP=600"
  ```
- **Runs** (archive/v1_burndown; all natural, one process at a time):

  | roll | build | result |
  |---|---|---|
  | ga_boot1, ga_boot2 | Group A | PASS: all 6 intro movies reach EOM; ScrPressStart at 131 s / 127 s; 0 GPU_HANG, 0 crash |
  | ga_long | Group A | RESULT SUCCESS; 199 frames over 1584 s; 0 GPU_HANG / POOL_WITNESS / HEAPCHUNK_GUARD |
  | gb1..gb4 x2 (8 boots) | each Group B item | 8/8 PASS: 6/6 EOM; PRESS START between 100.4 s and 153.9 s (e1_* baseline 106-149 s); 0 crash; one H1 GPU_HANG warning in gb1_boot2 (presents flat for 9 s during the title load, then resumed; same class as e1_on1) |
  | **gb_long** | all of Group B | **FROZE at 236 s** in early HUD gameplay; only 25 frames. sr_drive still printed RESULT SUCCESS because its patterns had already matched. |
  | gb_long2 | all of Group B | RESULT SUCCESS; 202 frames over 1565 s (baseline long_off: 200 / 1555 s); 0 GPU_HANG / POOL_WITNESS / HEAPCHUNK / IN-IMAGE, 0 crash |

- **Frames:** the whole-run contact sheets archive/v1_burndown/gb_long2/sheets (6 sheets).
  - All 6 were viewed in the Group B pass. Sheets 000, 003 and 005 were viewed again at the close, next to
    long_off sheet_00.
  - Eras shown: the EA / DC / WB intro movies; the starfield and S-logo LOADING screen; the LOAD A GAME? popup
    over the city; the space cinematic; tutorial HUD gameplay with hint text ("ON EARTH, YOU'LL HAVE THE ABILITY TO
    DEFY GRAVITY", "NAVIGATE TO THE SUPERHEARING ICON"); night flight over Metropolis with airships; high-altitude
    flight with no HUD; the idle street camera at the end.
  - The early black frames at about 24-28 s, between the WB logo and the starfield, are also in long_off sheet_00.
  - The later gameplay differs from long_off because the tape is open-loop and drifts. That is not a regression.
  - No visual change is claimed, so there was no Astra review.
- **Still broken:**
  - **Silent game** (the declared band-aid).
  - **Gameplay GPU progress-hang in gb_long (1 of 2 long rolls, UNATTRIBUTED):**
    - The render submit thread parks in GpuRingWaitForSpace sub_820FCB30 (callerLR 820FCDD0).
    - The CP is idle with the ring empty.
    - The completed fence is 0x8AAF while the guest has issued fences up to 0x8ACA; the last 25 IBs carried no fence.
    - This is not the 04bce60 phys-alias cause.
    - Group B is not ruled out: no A/B with item 4 reverted has been run.
    - The guest's own hang trap is inert with r13=0.
  - The sr_drive verdict alone is not a pass: count frames and GPU_HANG triggers.
  - The open visual defects of project_visual_defects_first, unchanged.
  - The spliced `goto loc_827A1770` in CRT doexit skips the atexit handlers on exit().

### 2026-09-30 - iter-463: the natural run no longer needs DOGMA_AUTORELEASE (deleted); boot and 26 min of tutorial + free flight hold without it

**What changed:** the declared band-aid `SR_DOGMA_AUTORELEASE` is deleted. The game releases those 12
semaphores itself: they are the benaphores of six EA::Thread::Condition objects created at boot, not
movie-worker semaphores (docs/BANDAIDS.md has the corrected row). The one roll that wedged with it off
(v1_noforce1, movie-3 decode skip-loop livelock) did not recur in 4 OFF boot rolls or the long roll.
The same series made the wait / semaphore / NtReadFile layer follow Xenia (43321b9, f37d0cf; ENGINE_MAP
section 7 rule 12).

- **Build:** band-aid off in rolls on `bec0d14`; deleted in the commit that adds this entry. `SR_ALLOW_FORCING` off.
- **Still NOT fully natural:** the host-launch audit (`python scripts/audit_forcing.py`, HOST-LAUNCH
  section; entries in docs/HOST_THREADS.json) still lists forcing sites that ship by default - cfgSoundOffThread
  (game is silent), the MODGLOB poller, the 1 Hz image restore, the SR_MAIN_BOOTSTRAPS splice calls, the
  quit-flag clear, the CRT doexit flag writes, the out-of-range CS force-recreate - plus the GPU-IRQ fence/RPTR
  advance (audit_forcing DECLARED band-aid; starts only on the legacy VdSwap path). So this entry is "natural
  except those", one fewer than the entry below.
  (Superseded: all but cfgSoundOffThread are master-gated or deleted - see the "host-launch burndown closed" entry above.)
- **Repro** (saves: empty):
  ```powershell
  .\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 240 -Gates "SR_FMV_EOM_PROBE=1"      # boot + intro movies (probe = observe-only [FMV_EOM]/[SRCREAD])
  .\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 1800 `
      -Gates "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr,SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_SPARSE_EVERY=120,SR_PRESENT_DUMP_SPARSE_CAP=600"
  ```
  (the evidence rolls below passed `SR_DOGMA_AUTORELEASE=0`, which is a no-op after the deletion.)
- **Runs** (archive/v1_waitfix):

  | roll | gates | result |
  |---|---|---|
  | e1_off1, e1_off3, e1_off4 | SR_DOGMA_AUTORELEASE=0, SR_FMV_EOM_PROBE=1 | PASS: 0 autorelease fire lines; [FMV_EOM] cursors 0-5 (all six intro movies); last MOVGATE psmCnt=FFFFFFFF; APT_LOADMOVIE("main") at 118 / 94 / 94 s; ScrPressStart at 147 / 115 / 113 s; 0 SRCREAD lines with lr=824BD7A0; 0 CS_WEDGE lines on 89678CE8 |
  | e1_off2_poolw | + SR_POOL_WITNESS=1 (the v1_noforce1 condition) | PASS, same markers; 0 [POOL_WITNESS] lines |
  | e1_on2, e1_on3 (controls) | SR_FMV_EOM_PROBE=1 | PASS, same markers (12 autorelease fires each) |
  | e1_deleted1 | code deleted, SR_FMV_EOM_PROBE=1 | PASS, same markers |
  | long_off | tutorial_tape_full, SR_DOGMA_AUTORELEASE=0 | `RESULT: SUCCESS`, 26 min (15:19:46-15:46:07); present dumps run to p023880 at 15:45:59, so presents rose to the end; 0 [GPU_HANG], 0 HEAPCHUNK, 0 [SEM_RELEASE_FAIL]; soft failures = the same 4 menu-nav pulse steps as iter463_nat1 |

  SRCREAD logs its first 64 nonzero returns and then powers of two, so line counts are not event counts: the
  highest ordinal was 48315 / 95009 in every roll, ON and OFF, against 268,451,411 in the stormed v1_noforce1.
  The SRCREAD caller mix was identical ON and OFF (1x 824BF614, 1x 824C236C, 73-74x 824C41B0).
- **Frames:** long_off contact sheets (archive/v1_waitfix/long_off/sheets, all 200 dumps viewed): EA / DC / WB / starfield /
  logo intro, the load-or-new menu over the city, tutorial with HUD and hint text, the map screen, free flight over
  Metropolis, then Superman idling on the street. It matches the ON reference archive/iter463_nat1/sheet.png era for era.
  **Still broken (unchanged, same in nat1):** the open visual defects of project_visual_defects_first (e.g. the dark top strip).
  No Astra review: this is a stability entry with no visual change claimed.
- **Also measured:** `SR_READ_PENDING` (Xenia's STATUS_PENDING for overlapped handles) stays default OFF. With it on and
  our generic RtlNtStatusToDosError stub, every intro movie was skipped (e1_on1). Bake it together with SR_NTSTATUS_DOSERR.
  (2026-10-18 note: SR_NTSTATUS_DOSERR was baked ON by itself in step4/audit fix batch 1, docs/LOGIC_AUDIT.md 2.0d;
  SR_READ_PENDING was not in that bake and still needs its own A/B.)

### 2026-09-30 - iter-463 (5780fbb): 21 MIN OF TUTORIAL + FREE FLIGHT WITH NO FREEZE AND NO HEAP CORRUPTION; the tutorial is completable

**The number moves: the game stays up.** Three blockers that ended every long session are fixed
at their root:
- 60a90bf `ObReferenceObjectByHandle`: the EA ThreadPool clones, the heap corruption, the dead
  D-pad power select and the "CS livelock" freezes;
- af895b3: the storage prompt loop;
- 04bce60: the arena GPU freeze (physical double-booking).

The user played the whole tutorial on 60a90bf (archive/v1_tape3, 04:00-04:22), including power
select, and reached Warworld; the only stop there was the freeze fixed in 04bce60.

- **Build:** `5780fbb`. `SR_ALLOW_FORCING` off. **One declared band-aid is still active:**
  `SR_DOGMA_AUTORELEASE` (docs/BANDAIDS.md; `=0` wedges the intro movie pipeline, v1_noforce1).
  Until its missing semaphore release is found, this entry is "natural except DOGMA_AUTORELEASE".
  (Superseded: DOGMA_AUTORELEASE was not masking a missing release and is deleted - see the entry above.)
  **CORRECTION (same day, host-launch audit 0bddbcd):** that list was incomplete. Eight forcing
  sites run by default, invisible to the override audit: 
  - cfgSoundOffThread, which zeroes the game's Sound flag, so the game is silent;
  - the MODGLOB poller;
  - the 1 Hz 8 MB image restore;
  - DOGMA_AUTORELEASE;
  - the SR_MAIN_BOOTSTRAPS splice calls;
  - the game-loop quit-flag clear;
  - the CRT doexit flag writes;
  - the out-of-range CS force-recreate.
  So this entry is "natural except those eight" until they are master-gated (list and fixes:
  `python scripts/audit_forcing.py`, HOST-LAUNCH section). Every earlier entry carries the same
  caveat.
- **Repro** (saves: empty; the user's full-tutorial input tape; open-loop, so it drifts from the
  user's route after the first minutes - the run is a stability check, not a route replay):
  ```powershell
  .\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 1800 `
      -Gates "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr,SR_PRESENT_DUMP_DIR=archive/<dir>/frames,SR_PRESENT_DUMP_SPARSE_EVERY=120,SR_PRESENT_DUMP_SPARSE_CAP=600"
  ```
- **Run:** `archive/iter463_nat1` (run with `SR_XAM_CONTENT_FS=0` for attribution; it was
  built before the save commit landed). `RESULT: SUCCESS`, 21 min dwell after the HUD.
  Counts: `[PHYS_DOUBLE_BOOK]` 0, `[PM4_IB_CONTESTED]` 0, `[GPU_HANG]` 0, `[PHYS_CAP]` 0,
  pool witness 0, in-image grants 0, `[CRASH]` 0. The allocator reused freed physical memory
  35 times, and retained memory stayed at 0x34000 bytes in 5 extents.
  Sheet `archive/iter463_nat1/sheet.png` covers every era:
  - the WB intro movie;
  - LOAD A GAME?;
  - the street with the HUD;
  - the map screen;
  - flight over the city, and standing on rooftops.
- **Also this day:** the Warworld arena renders its level geometry and correct tower shading
  (c3c0e6c, replay only - `archive/v1_arena/ab/`; no live arena run yet).
- **Still broken:**
  - 2 VS still drop their draws in this run (40078E73, 8B37013C; ucode not dumped yet);
  - 7 arena VS are untranslated;
  - the natural arena has not been reached again on the fixed build (the tape drifts);
  - LOAD GAME after a real save has not been verified live;
  - earlier visual defects (street darkness, facade speckling) remain.

### 2026-09-29 - iter-460 (1f0813e): THE NATURAL GAME REACHES GAMEPLAY THROUGH THE PM4 PATH AND RENDERS A BROADLY COHERENT 3D METROPOLIS

**The number moves: the world is no longer severely corrupted.** Wording by Astra's blind
review (scratchpad `astra_gameplay.txt`, four frames vs Xenia's gameplay renders
`archive/xtr_extra/`), verbatim:

> "The natural game reaches gameplay through the PM4 path and renders a broadly coherent 3D
> Metropolis, including textured buildings, roads, vehicles, pedestrians, Superman with cape,
> and HUD. The sampled views no longer show pervasive severe world corruption, although
> shading, texture and possible clipping defects remain."

- **Build:** `1f0813e` (SR_PHYS_LIFO_FREE, SR_MEMCPY_GUARD_TOP_FIX, SR_PM4_WRITABLE_OWNER,
  SR_CB_WINDOW, SR_ALIAS_OWNER all default ON; the PM4 live path and shared memory still
  opt-in). `SR_ALLOW_FORCING` off.
- **Repro (input from the outside only, no guest writes):**
  ```powershell
  .\tools\sr_drive.ps1 -Scenario tools\scenarios\frontend_navigate_pm4.json -Out archive\<dir> `
      -Gates "SR_PM4_LIVE=1,SR_PM4_SHMEM=1" -TimeoutSeconds 480
  ```
  (P3, same day: both gates are default ON, so `-Gates ""` is the same run - `archive/p3_reach1`.)
  The scenario is `frontend_navigate_hardened.json` through `select-third`; the era witness is
  the guest's own `screens/hud/main_hud` APT request (the HLE-era black-loading-screen gate
  never fires under PM4 because the load screen is drawn).
- **Runs:** `archive/p2_reach1` (hardened scenario; the user took the controls partway, so the
  flight/pause frames there are human-driven; frames `present_cl_016..031`: street, HUD,
  pedestrians, cars, pause menu at 021, tutorial prompts at 023/024/030, flight at 027 - the
  pause-menu and prompt frames were not part of the blind review). `archive/p2_reach2`: the
  human-free rerun with the pm4 scenario - `RESULT: SUCCESS after 1 attempt(s)`, every pulse
  confirmed by `[KBPAD_OBS]`, the `main_hud` request at 14:36, frames `present_cl_029..038`
  (presents 3480-4560): Superman standing on the street with the HUD, trees and buildings for
  the whole 120 s dwell; `[IB_RANGE]` 0; the title city under the dialogs at 1920-3240.
- **Oracle:** Xenia's renders of its own gameplay traces (`archive/xtr_extra/454107ED_25023.png`,
  `_28050.png`). Content classes match; moments differ.
- **Still broken (Astra + ours):** street views darker with less shadow detail; multicoloured
  speckling/striping on some facades; a flight view obstructed by an overhead surface (geometry
  or clipping, undetermined); small white triangles at the lower frame edges (at full size: textured wedges clipped by the edge, most likely the game's objective edge markers - not confirmed);
  the title city runs ~8-12 fps and the title load gap is longer under LIFO; a noisy strip along the top edge of one gameplay frame (`archive/p3_reach1` present 3360); the intro's
  attract/title timing varies per roll.

### 2026-09-24 - iter-458: THE GAME PASSES THE NEW-GAME LOAD AND PRESENTS THE GAMEPLAY HUD — the 3D world is severely corrupted

**The number moves.** Wording ruled by Astra (docs/astra/014 reply), verbatim:

> "iter-458: two sequential forcing-off runs reached gameplay HUD presentation after new-game
> navigation; one showed a tutorial prompt. The 3D world remains severely corrupted.
> Playability and reliable loading remain unproved."

- **Build:** exe `5079A387...` = `6e49b58` (the fix) **plus** the parked, uncommitted
  idxstream change in `kernel/hle_api_observe.cpp`, which is gated by `SR_S6_IDXSTREAM`,
  default OFF, and was not set. `SR_ALLOW_FORCING=0` in both runs' provenance.
- **Runs:** `archive/iter458_nat1` + `_nat1_pw`, and `archive/iter458_nat2` + `_nat2_pw`.
  - Scenario: `tools/sr_drive.ps1 frontend_navigate.json`.
  - Gates: `SR_KB_PAD_OBS=1, SR_PRESENT_DUMP=1` (sparse every 120 presents, cap 200),
    `SR_LOADSCR_OBS=1, SR_PROV_RING=0, SR_NO_MODGLOB_POLLER=1`.
  - Capture: GPU readback plus the PrintWindow recorder (`tools/record_fullrun.ps1`,
    5 fps, 540 s) with `tools/verify_run.py` sheets. Both methods agree on the features
    below. Frame-exact equivalence was not demonstrated.
- **What changed:** before the fix every run went solid black after "START NEW GAME" (the
  loading screen was never dismissed).
- **The mechanism** (docs/astra/005-012), in order:
  1. The hand splice of `sub_82767688` (Lua `luaH_mainposition`) dispatched numeric keys to
     `sub_827676B0`, whose first four instructions were the switch table's words executed
     as `lwz r19,...(r22)`.
  2. That clobbered the caller's r19, so the CinematicSupermanReturns worldflow record was
     created with auto-spawn off (+668 = 0).
  3. The cinematic was never spawned, so the load gate never opened.
  4. Regression: `tools/test_lua_hashnum_r19.py`, 33 passed / 0 failed.
- **What is visible now:**
  - starfield/black, then the in-game HUD (emblem, blue/red bars, crosshair);
  - nat1 showed a moving camera and later the prompt "TO LAUNCH, TAP (Y). TO POWER LAUNCH,
    HOLD (Y)" with correct glyphs;
  - nat2 held a slowly changing composition.
- **Still broken (Astra-graded):**
  - world geometry and colours are wrong against Xenia (stretched planes, spikes,
    overlapping polygons; no recognizable street or Superman);
  - a sharp vertical seam at x~640 in both runs;
  - horizontal striping and a dotted diagonal line;
  - an anomalous circle-grid diamond at the bottom right;
  - flat-pink frames in the movie era;
  - the menu backdrop is malformed and varies between rolls.
- "Untextured" is NOT an established diagnosis.

### 2026-08-30 - iter-456: PRESS START RENDERS AS TEXT — the white-boxes mechanism was an unhooked in-place texture create; REACH UNCHANGED

**The number does not move** (furthest natural point unchanged); this entry
retires a named known-broken element from the 2026-08-10 entry below: "PRESS
START is solid white/grey boxes; the responsible mechanism is not yet
identified." The mechanism is now identified, fixed, and the fix witnessed on
both captures: the glyph atlas `88ED7E10` (128x128 k_8, pixel key `0BA5A000`)
is constructed by the in-place header init `sub_82616C40` — never by the hooked
TexCreate — so it was invisible to the HLE texture map and every bind fell to
the white fallback. The kernel override now registers those creates (platform
fix, `SR_HLE_TEX1640_REG` baked default-ON, zero forcing: the guest performs
every create/lock/unlock itself).

**MEASURED + EYEBALLED (K=1 zero-env-class rolls, 200 s; recorded roll
`archive/iter456_r2_frames` sheets + authoritative GPU readback
`archive/iter456_r2_presentdump`, both reconciled):** title screen shows
SUPERMAN RETURNS / THE VIDEOGAME / PRESS START / (c) 2006 lines as crisp
glyphs; legal screen crisp. **Known-broken, same breath:** flat grey-brown
title backdrop with seam (backdrop class unchanged), movie-era chroma/noise
bands (~11-12 s), pink-tan inter-logo flashes, a dark stretch ~130-170 s;
dialog-era text not exercised (no-input roll). Ledger `iter-456-tex1640-fix`
[UNVERIFIED pending director].

### 2026-08-10 - iter-450i/k: NAMED TITLE UI ELEMENTS REACH THE PRESENTED FRAME NATURALLY; THE SCREEN IS NOT CORRECT

**Codex verdict:** `OVERSTATED` as originally written; corrected here. The
natural milestone is specific: the Superman logo, the "SUPERMAN RETURNS / THE
VIDEOGAME" wordmark and the copyright line render correctly over a guest-driven
3D backdrop, where the screen was previously flat maroon. The UP-endian fix is
a real platform fix and the guest reaches this era without forcing.

**Known-broken in the same frame, against the console oracle
`data/gpu_traces/menu_pressstart_oracle.png`:** PRESS START is solid white/grey
boxes; the Metropolis backdrop is large flat angular geometry in wrong
orientations; the flying Superman character model is absent; exposure is dark
instead of daylight. The responsible PRESS START draw/texture mechanism is not
yet identified. This entry records natural progression, not title-screen pixel
correctness.

**Commits:** ed6506e (window fix) + 56c215e (UP endian fix), plus 145ccb6
(WB staleness guard). Two harness bugs had been hiding the entire visible
frontend: (1) the vertex gather read physically-addressed index buffers
through the 0x8 virtual window — garbage indices refused 237K world draws
per roll as "range" (now 0; coverage 49.8%→63.4%; the world scene paints
varied HDR); (2) the DrawVerticesUP producer published guest vertex bytes
with NO endian swap — every post-chain screen quad (exposure measure,
luminance pyramid, composite, UI) rasterized as degenerate denormal
geometry since iter-447, so the presented frame stayed a flat fade tint.

**MEASURED, zero-env roll, EYEBALLED
(`archive/iter450k_upswap/title_046.png`):** the named correct UI elements are
composited over a live 3D scene. The archived roll also showed intro movies and
attract rotation, but this entry makes no new whole-run visual-cleanliness
claim; those eras require their own oracle-scoped verification.

### 2026-08-09 - iter-449B: ALL 33 RUNTIME EFFECTS COMPILE NATURALLY on the console's own (new) HLSL backend — SR_HLSL_OLDBE retired; FE era leaves black

**Commit:** b6bbee9. The iter-398d wall ("new backend errors X4800 on
console-clean input then loops forever") was a HARNESS bug: our physical
allocator dropped the XALLOC alignment attribute, so the D3DX effect
compiler's 4KB pool pages were unaligned and its page-mask header lookup
(`*(obj & 0xFFFFF000)`) silently corrupted the whole compile. Platform fix
`SR_PHYS_ALIGN_FIX` (alignment honored end-to-end); the old-backend routing
band-aid is DELETED — the default exe now compiles every runtime effect the
way the console did.

**MEASURED, zero-env (2 clean rolls + 1 OLDBE=0 roll):** FXCREATE **33/33
rc=0** (was 0/66 pre-iter-398, 71/72 only via the old-backend band-aid since),
including the inline-asm tfetch2D edge-AA, vfetch tessellation, and
`[unroll(16)]` luminance-downsample (the exposure chain) sources. Zero
backend errors; RealizeVS/PS fire. Boot health intact-or-better: present
6000-6600, FULL-INIT 1/1, movie instanced, feFrames 4145-4578, crash=0.

**EYEBALLED (`archive/iter449B_fe_dump/contact_449B.png`, in-engine GPU
readback):** full intro chain + attract second cycle crisp (EA|HD → dot
tunnel → WB shields → shield flythrough → title card). The FE era
(press-start wait) is now a UNIFORM DARK RED field instead of black — the
post pipeline processes every frame; the FE 3D scene's geometry still does
not arrive on the primary (the separate draw-coverage arc: 41% coverage,
srcNeverDrawn 77%). That is the next frontier for a lit menu.

**Reproduce:** `.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 200 -Gates ""`
— count `[FXCREATE] ... rc=00000000` (33), note zero `[BESITE]` lines.

### 2026-08-08 - iter-449t: THE TITLE MOVIE PLAYS IN FULL and the attract cycle runs — a pre-charter forcing relic deleted

**Commit:** 7512ab3. The `stub_XamInputGetState` no-controller arm had
synthesized a phantom Start+A every ~120 polls since before the charter —
ungated forcing hiding in an import stub, outside the override registry's
scan. The guest's skip-movie action check (event 187 → input query,
IDA-verified chain in the commit) read the phantom press ~2.8s into the
ONLY skippable intro movie — "0100", the long title-card FMV — and
executed it on EVERY roll. The frozen starfield frame that defined the
FE era for the project's whole recorded history was that skip. The
synthesis is DELETED; no controller now means neutral input.

**MEASURED, zero-env (single-change attribution; 2 clean rolls):**
`[EV187]` ACCEPTED-KILL 1/roll → 0; movie-5 lifetime 2.2s → **38s (the
full movie)**; mQuads ~740 → **3,730**, movie gate EXACT (11,190 ==
3×3,730); doneFlag → WorldFlow dispatch → press-start park as before —
and at +2.25min the SM loaded a SEVENTH movie (fresh 1→2, new contCb):
**the attract-mode movie starts naturally. The FE's full loop (intro →
title movie → press-start wait → attract) cycles for the first time.**

**EYEBALLED (`archive/iter449_idx/title_movie_full.mp4` +
`title_sheet0-3.png`, in-engine GPU readback):** the complete title-card
FMV — starfield, "SUPERMAN RETURNS" in gold over the red nebula,
warp-tunnel transitions, "THE VIDEOGAME" title card — correct colors,
frame after frame, through the fully natural chain.

**Reproduce:** `.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 300 -Gates ""`
— watch `[FMV_SM]` cursor=5 lifetime, mQuads ≈3,700, `[EV187]` declines
only. Disconnect any controller (a REAL Start press still skips — that is
the design).

### 2026-08-08 - iter-449o WAVE 2: THE GUEST'S OWN PRESENT CHAIN IS THE DEFAULT — every frame reaches the screen via movie/geometry → pool surface → resolve → front buffer → blit

**Commit:** the wave-2 bake (after wave 1 + two clean zero-env soak rolls).
`SR_S6_IDX` and `SR_HLE_PRESENT_FRONT` default-ON (`=0` A/B each). The
default exe now serves the indexed draw path (gather producer, restart-strip
conversion) and presents through the guest's own front-buffer chain.

**MEASURED, zero-env K=1 330 s (`archive/iter449_idx/wave2.log`):**
present=9,600; `[S6_FRONT] HIT=9600 shown=9600` — EVERY present is the
guest's own presentation path; movie gate EXACT (2343 == 3×781); crash=0.
**EYEBALLED (`wave2/contact_8.png`):** the whole intro program through the
front chain (both title cards), then the continuous FE starfield/skybox —
the attract demo's environment rendering as the default. Still absent
(honest): title-screen text, characters (morph-mesh class), correct terrain
textures — **iter-438f/g remains the reference to beat.** `SR_S6_VBCACHE`
stays opt-in pending multi-session evidence.

**Reproduce:** `.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 330 -Gates
"SR_PRESENT_DUMP=1"` — the `[GATES]` log line names the effective set.

### 2026-08-08 - iter-449o WAVE 1: the API-HLE TRANSLATION STACK is the default — draws land on the guest's own surfaces, resolves execute, state applies, NATURALLY

**Commit:** the iter-449o wave-1 bake. Nine gates flipped default-ON (`=0`
keeps each A/B): `SR_S6_WIDE/STATE/TEX/UP`, `SR_HLE_RT_POOL/DRAW_TO_POOL/
DRAW_INORDER/RESOLVE/RESOLVE_DEPTH`. The default exe now: widens the draw
gate beyond the movie quad, applies the guest's own depth/blend/viewport/VTE
state, captures general textures at bind, serves the UP/inline path, creates
host surfaces for the guest's render targets, routes draws to the guest's
bound surface in guest order, and executes the guest's resolves — all
decisions the guest's, zero forcing (7-agent charter audit, iter-449o).

**MEASURED, zero-env K=1 330 s roll (`archive/iter449_idx/wave1.log`):**
present=10,200 (full throughput), movie gate EXACT (snaps 2346 == 3×782),
rejects=0, crash=0, recorded=1,425,380 draws (18.0% of issued — wave 1 has
no indexed producer; that is wave 2). The `[GATES]` line in the log names
the live set.

**EYEBALLED (`archive/iter449_idx/wave1/contact_8.png`, 60 frames):** the
FULL intro program (EA|HD → EA → DC → WB → WBIE → both title cards) intact
and identical to the prior default via the legacy present path, PLUS content
the old default never showed: crystal-shard menu art (multiple frames) and
an aircraft silhouette in the FE era. Honest caveats: the FE era is mostly
the clear field (indexed content is wave 2; the residency cache is held);
no title-screen text, no attract demos, no characters — **the iter-438f/g
recording remains the visual reference to beat.**

**Reproduce:**
```powershell
.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 330 -Gates "SR_PRESENT_DUMP=1"
# then eyeball archive\present_*.bmp — intro logos + FE-era shard art.
```

### 2026-08-03 - iter-442: THE ARCHITECTURE REGRESSION IS PAID BACK — the API-HLE renderer puts the intro movies back on screen, NATURALLY

The iter-441 entry below recorded a deliberate visual regression (old renderer
removed before its replacement existed). **That debt is now cleared for the
movie path.** The D3D9-API-boundary renderer draws the guest's own movie quad,
from the guest's own vertices, its own three YUV planes and its own two
shaders. No forcing, no synthesis, no substitution.

**Commit:** `64f0791` + the bake. **Gate:** `SR_HLE_DRAW` BAKED DEFAULT-ON
(`=0` keeps the A/B) — the default exe renders this with no env set.

**How measured (both loops, and the second one is the honest one):**
* Fixture: `.\tools\hle_replay.ps1` reproduces
  `data/gpu_traces/fmv_eahd_golden.png` — EA|HD logo, mean RGB
  [36.5, 36.4, 36.8] (neutral), chromatic fringing and grain present.
* **Live natural K=1, NO env gates but the dump switch:** 45 backbuffer dumps,
  **10 carrying real content** (max=255) across the movie era — the animated EA
  logo and the full-colour WBIE logo. `walksRet 4`, `crash 0`,
  `GATE=MOVIE-ACTIVE`, present/swap unchanged.

**Reproduce it yourself:**
```powershell
.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 150 -Gates "SR_PRESENT_DUMP=1"
# then look at archive\present_*.bmp — the movie-era ones show the logos.
```

⚠ **Do NOT judge this with `record_fullrun.ps1`.** PrintWindow returns all-black
for our flip-model swapchain: a roll that provably rendered produced 1010
entirely black frames. Use the in-engine `SR_PRESENT_DUMP` readback. (Recorded
in that script's header; iter-438b's note named gdigrab and CopyFromScreen but
PrintWindow is blinded too.)

**Still dark:** everything after the movies. General geometry is S6; the menu
remains behind its own separate wall. This entry claims the MOVIE path only.

### 2026-08-03 - iter-441: DELIBERATE ARCHITECTURE REGRESSION of the VISUAL baseline (user decision) — old renderer removed ahead of the HLE rebuild

**Not a natural-progress entry — an honest bookkeeping one.** The old
inference-layer renderer was REMOVED (MIGRATION_API_HLE.md "PHASE ORDER
AMENDED") before its API-boundary replacement renders, so from this commit
until HLE Phase 2 first light, **no draws reach the screen**. This is a chosen
architecture cost, not lost game progress: the game itself still boots, plays
movies (decode path untouched), walks the FE flow, and presents at its own
Swap cadence — all boot metrics remain comparable. The **iter-438f/g entry
below stays the standing VISUAL baseline** (archive:
`archive/iter438g_fullrun_truetime.mp4`); HLE first light must be judged
against it. The number has NOT moved backward in game behavior — only our
window went dark while the renderer is rebuilt on the reference model.

### 2026-08-01 - iter-438f/g: THE FULL BOOT SEQUENCE IS ON SCREEN — movies, TITLE SCREEN (text readable), attract demos — with SPECIFIC corruption classes. The number moves, with honest caveats.

Commit chain `d18ecf6..438f`. Default build, no env set, K=1 solo rolls.
Evidence standard: CONTINUOUS full-run recording (PrintWindow loop, 3,934
frames / 360 s / 10.9 fps — MPO-immune), analyzed frame-by-frame
(brightness + dHash on every frame, then eyes on all flagged anomalies,
transitions and era samples). Archived: `archive/iter438g_fullrun_truetime.mp4`.

**What genuinely renders (new furthest natural point):** ident cards →
intro movies (WBIE/EA logos, crisp) → the SUPERMAN RETURNS TITLE SCREEN
with READABLE title + © 2006 Electronic Arts text (press-start-class UI on
screen for the first time) → attract-mode in-engine demos (Superman street
flight, crosswalk scene with burning car, airship dive, arena — coherent,
detailed, no seam in the 5 demo frames eyeballed across two runs).

**Corruption/slowdown classes OPEN (each seen with eyes, this recording):**
1. MOVIE FLICKER: solid-maroon full frames interleave with clean movie
   frames in clusters (t≈13-14s, 20-22s, 28-31s) — a wrong-content present
   during movies.
2. PRELOAD-ERA COLLAPSE (t≈80-215s, ~135s): 1-2 fps effective, a vertical
   TILE-SEAM at x≈640 with mismatched halves, flat untextured polygon
   fragments (solid green/maroon/blue), a full-width green rectangle at the
   bottom, stretches of near-black. This is the world/city streaming era.
3. TITLE-SCREEN ART: the S-shield texture renders as diagonally-offset
   slices (tiled-texture/untile mismatch class); title + copyright TEXT is
   double-exposed with an offset ghost copy (double-composite class).
4. UNCONFIRMED (need Xenia oracle): ~11.6s static hold on the boot card;
   movie pillarboxing (plausibly correct 4:3).

**Also open:** no-movie rolls can hold inside the boot ident (thread-50
recursive CS hold, [CRITSEC_STALL] names owners); no pad input.

### 2026-07-31 - iter-434f: THE GAME REACHES PRESS-START AND DRAWS ITS FIRST UI PIXELS. The number moves.

Same commit `743168e`, 600 s runs. Default build, no env set. **New furthest natural
point: the clean roll walks movies → transition → full world/city streaming →
requests the press-start screen by name → `ScrPressStart` resolves →
`[FE_SETSCREEN] #1` (first ever) → 5,972 UI draw calls reach the D3D9 boundary →
the frame dump shows real (untextured) UI geometry — and the game parks on the
press-start screen waiting for input.** Frame evidence:
`archive/iter434f_first_ui_pixels_run1_240s.bmp` (240s ≡ 360s, static park).

Remaining to a visible menu: Apt-UI renderer correctness (vdecl/texture/shader on
the HLE path — currently flat white), and pad input (nothing presses Start).
Roll noise at 600 s: 1/3 clean-to-press-start; 1/3 late AV in a D3D-family worker;
1/3 NtSuspendThread(0) spin after a worker-exit race (both queued for triage).

### 2026-07-31 - iter-434e: THE TRANSITION SURVIVES — the golden deep-signature is 3/3 DETERMINISTIC. The number moves.

Commit `743168e`. Default build, no env set. **The new furthest natural point, now
deterministic: the full movie program plays to its natural end, the movie→FE
transition survives with all four GameKernel runners alive, the world preload
DRAINS (`[LOADU]` busy→0), and the FE flow walks its state machine deep into
multi-state condition polling (47K+ Lua condition evaluations, refs to 0x2DA) —
with the full deep signature defExec=666 / parser=15 / aptReq=15 / clipCtor=453 /
movInst=451 on 3/3 rolls.** Before this it was one lucky roll in ~4; the rest died
at the transition instant.

The mover: 59 recompiler-DROPPED VMX instructions (39× vsel128, 20× vandc — comment
emitted, no code) repaired mechanically (`tools/regen/patch_dropped_vmx.py`). The
proven kill was the dropped `vsel128` inside CameraCollisionSweepTest's fcmpu-equality
recursion terminator → infinite self-recursion → 0xC00000FD killed the GameKernel
runner the FE flow ticks on, at the movie-end instant, in ~75% of rolls. Platform-fix
class: recompiler faithfulness (charter's canonical allowed category).

**Current natural wall:** `[FSM_WALK] fsm=893F4770 count=0` — the FE interface FSM
has no states registered; the menu-load request is never issued (iter-430 wall,
unchanged, now reachable 3/3 instead of by luck).

### 2026-07-31 - iter-432: THE ROLLS ARE STABLE — 9/9 clean by DELETING a band-aid. The number moves.

Commit `83c7af0`. Default build, no env set. **The natural boot is now deterministic-
stable: 9/9 rolls across three K=3 batches play all 6 movies with zero thread deaths,
zero lock wedges, and the flow FSM walking + executing its Lua state scripts on every
roll** (was 1-in-3 before, with roll-dependent AV thread-deaths orphaning locks).

The mover is the charter's thesis in one line: the roll instability was CAUSED by a
band-aid whose root had already been fixed. The iter-360 allocator tail-clobber (a
miscompiled boundary-tag store) was root-fixed by the iter-357k recompiler transplant,
but the compensating `SR_ALLOC_FIX` +0x10 size bump stayed default-ON — distorting heap
layout vs console and producing the roll-dependent ASF-reader corruption. Retired
(default OFF) → 9/9. New permanent instrument: `[THREAD_FATAL]` makes any guest-thread
death loud and self-attributing (guest back-chain + orphaned-CS census).

**Current natural wall (unchanged, now measurable on a stable base):** the boot flow
FSM parks on a Lua transition condition (state 1→13) that never turns true — the last
gate before the press-start/menu chain. iter-432's opening moves are banked in memory.

### 2026-07-30 - iter-429: '0100' plays to natural completion; the splash→menu handoff FIRES. The number moves.

Commit `b407fea`. Default build, no env set. **The new furthest natural point: the whole
movie program runs to its natural end** — all 5 logo movies plus the streamed '0100'
attract movie (fmvlegal.AST, 23.9MB streamed, ~47s, ends with the game's own
END-OF-MOVIE), and the instant it ends the splash sequencer advances into the frontend
flow script (PSM dispatch → sub_822AB748 → sub_822A8C68). 3/3 rolls, zero lock wedges,
zero thread deaths, movie audio callback firing 10,000+ times/run.

The mover: our `XAudioRegisterRenderDriverClient` had its two pClient fields swapped
since the beginning — no movie audio callback had EVER fired. Preloaded logo movies
survived it; the first streamed movie deadlocked on the undrained audio queue.

**Current natural wall (iter-430):** the post-splash frontend FSM (`fsm=893F4770`)
sits at `curState=0 count=0 stateObj=0` — no states registered, so the menu load is
never requested (Apt manager pump alive, `aptCtx=88B63C3C` published, `pending=0`).

### 2026-07-30 - iter-428f: ALL SIX intro movies play, EVERY roll. The number moves.

Commit `8a11f74`. Default build, no env set. **The new furthest natural point: the FULL
intro movie sequence (EA_HD_logo → EA_ID_OLD → DC_logo → WBIE_logo → DC_WB_legal → the
'0100' attract movie) plays through on 3/3 rolls** — previously 2/3 rolls froze at a random
movie transition. The freeze was OUR bug, not the game's: an ungated leftover diagnostic
(iter-397m [MPRING] hook) armed a hardware write-watchpoint whose disarm leaked armed debug
registers onto every thread; the resulting orphaned SINGLE_STEP exception silently killed
the movie presenter mid-transition while it held the FMV SM critical section. Reproduce:

```powershell
.\tools\run_recomp_x.ps1 -K 3 -TimeoutSeconds 300 -Gates ""
# [MOVIE_Q] shows 6 opens on every roll; [FMVRSM] ticks through all 6 state cycles
```

**Current natural wall (iter-429):** the '0100' attract movie streams `fmvlegal.AST`
sequentially (137×128KB reads, all succeed) and stalls at offset 17.96MB (~25s in): the
demuxer stops both requesting reads and feeding the decoder (BitReader starvation guard
trips), the whole movie crew parks on its events, the presenter holds sm+560, and the FE
service thread blocks behind it — menu never requested (aptCtx=0). No thread deaths, no
lock breaks: the wedge is honest and converges identically on 3/3 rolls.

### 2026-07-30 - iter-426/427g: THE INTRO FMVs PLAY ON SCREEN. The number moves.

Commits `0a3ab5f` + `e18573e`. Default build, no env set. **The new furthest natural point:
the intro movies RENDER AND PLAY in the window — the EA|HD logo fully composited (metallic
logo, chromatic edge tints, TV-static grain post chain), animating across frames. The user
watched the entire intro cutscene sequence play live.** Reproduce:

```powershell
.\tools\run_recomp_x.ps1 -K 3 -TimeoutSeconds 200 -Gates ""
# watch the window ~60-90s in; composite_*_n3xxx_1280x720.bmp banks the frame
```

Measured on 3/3 no-env rolls: movie draws present (78-584/roll), NDC quad corners decoded
exactly, full state carried (mask=0xF depth=0x700760 blend=0x10001), luma texture refreshed
~39 movie frames/roll with legible animation frames.

**What moved, and who did it.** The game's own presenter (sub_82473208 → DrawVertices) was
always issuing the draw; OUR side gained: (1) the D3D9-HLE boundary renderer — the draw is
synthesized from the device shadow regfile with container-hashed shaders (registered
platform-fix, SR_HLE_RENDER default-ON, =0 A/B) because the ring's copy of the runtime-
patched VS is corrupted by the flat memory model's split physical windows ([PM4_REJECT]
zero-payload class — root documented in MEMORY_MODEL_PLAN §5b, real fix = offset-disjoint
allocators + view aliasing, parked); (2) the vdecl-true vertex layout (pos f2@0, uv f2@12,
stride 36, fmt 37 — container placeholders say fmt 36 = ONE float, which decoded y=0);
(3) a content-sensitive VB dedup key (the old address-only key froze the movie quad at its
first, empty decode); (4) span-sampled window alias probes for textures (head-only probes
are blind to letterboxed frames). Known remaining gaps: presenter fence callback
(sub_820FDE38) not implemented — playback continuity rides on the game's own retries; the
menu after the movies is the next frontier.

### 2026-07-29 - iter-408/408b: THE FIRST FMV FRAME EXISTS AS A GPU TEXTURE. The number moves.

Commits `b76b69a` (iter-408) and `b8820b7` (iter-408b). Default build, no forcing env set.

**The new furthest natural point: the game's own first movie frame - the EA HD logo - decoded
from WMV3 and uploaded into a GPU texture, legible.** Reproduce it yourself:

```powershell
.	ools
un_recomp_x.ps1 -K 1 -TimeoutSeconds 200 -Gates "SR_TEX_DUMP=1"
# then open  texdump_slot*_fmt2_1280x720_0x0F7C0000_r0.bmp
```

The Y plane reads distinct=221 min=0 max=227 mean=55.1. Every earlier reading of that same
surface was distinct=2 mean=63 - but see the retraction below, because that number was the
measuring tool, not the plane.

**What moved, and who did it.** The guest was never at fault. `[YUVCOPY]` (SR_YUVCOPY_PROBE)
around `MovieVideoDecodeYuvToRgbLoop_sub_82480D80` shows the game's own planar copy landing
correctly on 3/3 rolls - mode=1, 1280x720, even dimensions, rc=S_OK, non-zero bytes at all four
sampled destinations. Two defects on OUR side were fixed:

- **Texture cache had no CPU-write invalidation** (platform fix, `SR_TEX_REVALIDATE` default-ON).
  The console's GPU sampled guest RAM directly, so a CPU write to a sampled surface needed no
  invalidation protocol; our host cache was keyed only on the fetch constant and had eviction for
  GPU resolves but nothing for guest CPU writes. A surface the guest rewrites in place - a video
  frame - uploaded once and froze. It now re-uploads on content change into the same slot and
  resource: 93/149/220 refreshes per run against ~220K unchanged-skips.
- **The BMP dumper rendered k_8 as black** (tooling). fmt 2 is one byte per texel and fell
  through to a branch that read four bytes as a float.

**RETRACTION.** Every "the video plane is BLACK" reading - iter-406d's distinct=2 mean=63, and
the iter-407 measurement that declared the physical-window hypothesis a failure - was produced by
that broken dumper. The movie's Y and both chroma surfaces are all k_8. iter-407's conclusion
(the window change bought nothing measurable) still holds on its own A/B and stays default-OFF,
but its evidence was weaker than it was written to be.

**What is NOT claimed: the frame is not on screen.** iter-408b measured the back buffer directly
(`SR_PRESENT_DUMP`) - 92 captures spanning a full run, all flat, mean (0,0,5), which is exactly
the render-target clear colour. `[FRONT_RESOLVE]` reports 0/3672 non-black texels on most
resolves (best seen 688/3672). The composited render target is essentially empty; the screen
shows it faithfully. The frame reaches GPU memory and stops there.

**Next rung:** dump `m_geoRT` just before it is copied to the back buffer, on a frame where
`[FRONT_RESOLVE]` is non-zero. Draws translate and bind cleanly (census: noVs=0 noPs=0 noLayout=0
noPso=0 prim=0), so the suspects are geometry-side - viewport/scissor, depth test, blend, or
vertex positions off-screen.

### 2026-07-28c — iter-399: a real platform fix lands; the NUMBER DOES NOT MOVE

Commit `3f98620`. A genuine platform gap was closed — guest-initialized KEVENTs (the title
inline-inits eight of them inside the movie pipeline and never calls `KeInitializeEvent`, with
the idle/ACK family **pre-signaled**) now treat the guest `SignalState` word as the source of
truth, so the teardown wait that used to block forever returns as it does on console
(`ENGINE_MAP.md` §6f, `SR_KEVT_GUEST_TRUTH` / `SR_KEVT_INIT_HDR`, both default-ON, both A/B-able).

**Why the baseline number is unchanged anyway.** The fix is proven at MECHANISM level — the exact
wedge site logs `wait-enter state=1 → wait-ok via=guest-state` and the teardown completes, versus a
control arm parked on that same wait for the rest of the run (parked-in-shutdown 0/9 vs 1/3). It is
NOT proven at metric level: the movie-chain length is roll-dominated. Identical builds produced
5/5/5 and 2/2/2 movies in different K=3 batches while the forcing-free control produced 1/6/6, and
a higher XMVCREATE count can mean movies ending SOONER, not the sequence getting further. Per the
charter, a fix with mechanism proof but no reproducible frontier movement is recorded as a platform
fix, not as progress.

**Reproduce:** `.\tools\run_recomp_x.ps1 -K 3 -TimeoutSeconds 240` (no env). Expect 3/3 full-init,
present ~6300-6900, feFrames ~4600-4900, zero crashes, and `[KEVT] … via=guest-state` lines on the
movie-pipeline event VAs. The furthest natural point remains what iter-397o recorded: the
console-true intro movie sequence plays; the chain then goes quiet with the decode workers idle-
waiting on their work events while the FE keeps ticking — that silence is the open frontier.

### 2026-07-28b — iter-398d/e: shader-compile chain LIVE (band-aid-assisted) + an honest movie-chain REGRESSION logged

Commits `844df76` + `f2335dc`. Two changes to the honest picture:

**1. The effect-shader chain works for the first time** — but band-aid-assisted, so it does
NOT move the natural number: the ancient unregistered ANIM-INIT stub of HlslCompileShader
(`sub_823C8CC0`, generated code) was deleted; the restored compiler's NEW backend loops on
console-clean input (open platform gap), so `SR_HLSL_OLDBE` (default-ON, registered band-aid,
retirement = new-backend root-fix) routes compiles to the game's OWN old backend: 71/72
compiles rc=0, holders realize, **first-ever non-NULL SetPixelShader/SetVertexShader
(216/run), two freshly game-compiled VS hashes IM_LOAD**. `SR_PM4_CHAINSKIP`/`SR_CP_INT`
(platform fixes) baked default-ON.

**2. The movie sequence REGRESSED vs 2026-07-28a and the regression is now precisely
bounded**: today the no-env default plays legal part 1 to its true end (~8.5s, MPNEXT
decode completes), creates legal part 2 — and then NOTHING: 2×580s soaks + ~8 shorter
rolls all show exactly 2 XMVCREATEs in the first 20s and zero further movie activity
(iter-397o measured 4 movies by 150s, 6/6 rolls). A same-day 2×2 A/B exonerates ALL of
today's changes (stub-replica × PM4-gates-off reproduces the wedge), so the suspect
window is the iter-397q→398 probe/gate commits (`0e9efaf`..`22f568c`), none of which
ever re-measured no-env movie health. Reproduction protocol queued: worktree anchor at
`2f78450` (re-inserting the then-present compiler stub) → verify 4 movies → walk the
probe commits forward. ALSO RE-LEARNED: the master-gate "deep" runs were
`auiNatDriveThread` (SR_AUI_NATDRIVE default-ON under `SR_ALLOW_FORCING`) hand-firing
ActivateUI — the retired metric chain, not a rescue; defExec=0 during the movie era
remains CORRECT (it is literally footnoted in the 2026-07-28a entry below).

**Furthest natural point (unchanged claim, regressed reliability): legal part 1 plays to
its real end; part 2 creates but never starts. Restore-to-4-movies is the top natural
task.**

### 2026-07-28a — iter-397o: 🏆 THE GAME PLAYS ITS INTRO MOVIES — the forcing-free default boots the CONSOLE-TRUE sequence: legal screens → intro reel, four movies back-to-back at 30fps, every transition via the movie's REAL end

Commit `2f78450` (chain: `cfd2e01` iter-395 malloc-typo kill → `beef4fe` iter-396 weld
dissolution → `b0ca697` iter-397 false-EOS dissection → this). `SR_REAL_CRT_HEAP` is now
default-ON: the game runs on its OWN dlmalloc heap (built naturally since iter-392), whose
large allocations route through NtAllocateVirtualMemory and are therefore 64KB-aligned —
the implicit alignment contract the old bump substitution broke. That misalignment was the
last blocker: the YUV texture backing must be 4KB-aligned (the GPU descriptor stores
base>>12; the texture lock returns the aligned-DOWN pointer), so on the bump the frame
copy wrote below its allocation and wiped the decoded-frame ring (HW-watchpoint-proven,
guestPC 824813E4). K=3 no-env: legal movie part 1 plays ~8.5s and ends at the demuxer's
true end-of-data, part 2 follows, then the intro reel from a second file, a fourth movie
still running at cutoff; MPGETF rc=0 at 30fps throughout, presents 3900–5700/run, zero
crashes, near-identical timestamps across rolls. **Measurement note:** the historical
"deep menu" metric is retired — iter-396 proved it measured the FMV-FAILURE bail path
(the game skipping to the menu because movies couldn't play). The baseline metric from
here: how far the CONSOLE-TRUE sequence runs (movies → attract/menu). defExec=0 on these
runs is CORRECT — the game is busy playing its intros, exactly as a console does.
Frontier: the movie chain's END (menu after the reel, or input-skip) + on-screen YUV
verification (host swapchain shows the tec0 quad; guest-FB dumps are black by design).

### 2026-07-27c — iter-391/392: ALL TEN data imports resolved + the CRT builds its REAL process heap for the first time; no-env 4/4 DEEP, strongest FE counts yet

Commits `ed0973c` (iter-391), `fc10d90` (iter-392). The VdGlobalDevice hunt exposed a
CLASS: the XEX has exactly ten kernel DATA imports, and any cell left holding its
raw token makes the guest read or write THROUGH it into pseudo-random VA space
(two prior multi-week walls: the frozen clock, the FE freeze). iter-391 resolves
the seven remaining cells per the golden-dump contract (`SR_DATA_IMPORTS`
default-ON) — most critically KeDebugMonitorData/KeCertMonitorData = NULL, which
D3DDevicePresent and GpuDriverIssueCommand check on hot paths. iter-392 fixes a
misreading dating to the earliest sessions: our `RtlImageXexHeaderField` stub's
pointer-to-zero return told the CRT "this title declares NO process heap", so the
game's CRT heap was never built at all. Console truth (the field is absent from
this XEX): the CRT builds its default 1MB heap. With the honest return
(`SR_XEX_HEAP_FIELD` default-ON) the guest now builds it through the natural path
— NtAVM reserve at 0x839F0000, commit, heap CS init — zero substitution.

Measured: iter-391 no-env K=4 → 4/4 deep. iter-392 gated arm K=4 → 4/4 deep,
clean FE tails; baked; **no-env confirm K=4 → 4/4 DEEP, 0 crashes, feFrames
1833–2721 (the strongest natural batch recorded)**. Day total on pure defaults:
12/12 deep. Follow-up queued: re-measure the iter-364 fence/fake-mstate
machinery per the iter-380 audit rule — it may compensate for this now-fixed gap.

### 2026-07-27b — iter-390: the mode-B post-flow FE-FREEZE ROOT-FIXED (unresolved VdGlobalDevice data import aliased the D3D device pointer onto the main-thread stack); no-env 4/4 DEEP, 0 freezes

Commit `3391e37`. The remaining roll-killer after iter-388 — the front-end tick
freezing permanently at the flow-completion moment while every thread stayed
alive — was a **platform loader gap**: the XEX's `VdGlobalDevice` DATA import
(cell `0x82000948`) was never resolved, and its raw token bytes read as pointer
`0xBE010100`, deep inside the main thread's stack. On console the title stores
its D3D device pointer through the resolved cell and every GPU fence check reads
it back; here it stored into accidental stack memory that worked until a deep
post-flow call chain clobbered it — then every fence wait failed forever and the
FE tick wedged inside the game's GPU fence reaper (`sub_82616208`). The fix
resolves the import exactly like the iter-303 KeTimeStampBundle clock fix:
`initKernelDataStructures` backs it with a zero-init slot at `0xD0013000`
(`SR_VD_GLOBAL_DEVICE` default-ON, `=0` reverts). The guest still makes every
decision — we only provide the kernel variable the console always provided.

Measured: A/B on the `SR_MAIN_BOOTSTRAPS=0` repro — fix OFF **6/9 rolls
sick/frozen** (that day's rate); fix ON **4/4 deep, 0 crashes, 0 freezes**.
**Natural no-env K=4: 4/4 DEEP** (defExec=666, movInst=451, feFrames
1102-2379, FE cadence a steady 30/s to timeout in every roll, 0 crashes).
The natural default exe no longer exhibits the FE-freeze class at all.

### 2026-07-27 — iter-388: the residual shallow-death ROOT-FIXED (thread stacks were granted inside live game arenas); no-env 3/3 DEEP

Commit `773391c`. The dominant residual shallow-boot mode — one resource record's
Havok physics data shredded before its parse, leaving the loading conveyor waiting
forever on a completion that could never come — was traced end-to-end to a
**platform allocator collision**: `ExCreateThread` carved guest thread stacks from
`guestHeapAlloc`'s forward bump, which marches through arenas the game's own
allocators had already carved inside the bump window (a late worker's entire 256KB
stack grant landed INSIDE the live 6.8MB metropolis package image; its stack
frames were the "corruption"). Fix: `SR_STACK_PHYS` (default ON) allocates thread
stacks from `guestPhysAlloc` — the same monotonic cursor as the game's pool
backings — so the collision class is structurally closed. Fourth confirmed
instance of the two-allocator-overlap family.

Measured: no-env (fully natural) K=3 → **3/3 DEEP** (defExec=666, movInst=451,
feFrames 1407–2107, 0 crashes, 0 Havok parse failures). On the harsher
`SR_MAIN_BOOTSTRAPS=0` repro: fix ON 6 rolls → 5 deep, 0 parse failures; fix OFF
→ the shallow-with-corruption mode returned (small N; accumulating). Forcing
audit: exit 0 (clean). Remaining known natural walls: the mode-B post-flow
FE-freeze (SVCTICK slots 2/3 never dispatch; ~1/6 of repro rolls), effects
truncated-source, resampler crawl.

### 2026-07-26f — THE FORCING AUDIT IS CLEAN (iter-382): "Default exe forces nothing (by this check)", and it boots deep 3/3

The last two audit survivors fell by DELETION of real code, not allowlisting:
the hand-rewritten main tail (`sub_821498F8`, ~263 lines of iter-26..53
host-driving — blocking-E630 replay, fallback `while(true)` with
GameShutdown-as-tick, the 2 s splash driver `sub_822A8C68`, the PSM
dispatcher) turned out to be **DEAD on every normal boot**: main already
calls the pristine `GameMainQuitSpinWait_sub_82176300` (the game's own
main "loop" — a quit-flag park; the game runs on the Init2-spawned
kernel-loop runners) and it never returns pre-quit. The complex was also
WRONG on quit (E630 replay instead of shutdown). Replaced with the
pristine tail: `GameShutdown_821863A0(app); return 0`.

Verify: no-env K=3 → **3/3 DEEP** (defExec=666, movInst=451, feFrames
2257-3166, 0 crashes). `audit_forcing.py` exit 0 for the first time.
Day total on all-natural defaults: **26/27 deep ≈ 96%**.

### 2026-07-26e — CONSOLE malloc(0) SEMANTICS BAKED (iter-381, `1c31588`): the iter-365 menu-vs-console-correctness trade-off is DISSOLVED

`SR_MALLOC0_FIX` (malloc(0) returns a valid non-null minimal block — the
console's own allocator contract) was parked default-OFF since iter-365
because it regressed deep-Apt 0/12 by shifting the FAKE-mstate dlmalloc
layout. That arena no longer exists (iter-380c). On the real heap: `=1` K=3
→ 3/3 DEEP, `malloc(0)→NULL` failures 63/run → 0, and the effect compiler
advances past init into real parse/technique stages. Baked default-ON;
no-env confirm 3/3 DEEP. Day total on all-natural defaults: **21/22 deep**.

Also measured, unchanged: `SR_DLMALLOC_FENCE=0` = boot parity + effects
unchanged (fence kept ON pending the effects arc); the WMA/FMV-tiny/
bitreader alloc-fix bundle still regresses the trunk (0/3) with no effects
benefit — stays opt-in; the remaining effects wall is a TRUNCATED-SOURCE
class ("unexpected end of file" reaching the HLSL lexer), a separate arc.

### 2026-07-26d — ALL THREE LOAD-BEARING BAND-AIDS RETIRED (iter-380c): the game boots DEEP on its OWN REAL HEAP; 18/19 deep today (~95%)

The last one, `SR_HEAP_STUB`, fell to the same audit rule: its 0/4 "real
dlmalloc death" was measured on the pre-iter-380 exe, where the iter-66
fast-path suppression was still starving early init. On the pristine exe,
K=3 `SR_HEAP_STUB=0` → **3/3 DEEP** with the guest **building its own real
dlmalloc heap** (all four handle globals 0 at boot; allocs served from the
real-heap region; zero NULL enqueues). Default flipped OFF (=1 = fake-mstate
diagnostic). Final no-env K=3 with ALL defaults natural: **3/3 DEEP**
(defExec=666, movInst=451, feFrames 1843-2839, 0 crashes).

**The default exe now performs: pristine DOGMA dispatch, zero allocation
substitutions, and the game's own heap init — no allocator forcing of any
kind.** Day total on retired-defaults builds: 18/19 deep (~95%) vs the 75%
(9/12) historical. Next: delete the retired blocks + the fake-mstate fence
machinery after a few clean sessions; residual shallow-roll mode; 29
generated-code audit sites.

### 2026-07-26c — SR_FM_ALLOC_SUB RETIRED TOO (iter-380b): both alloc substitutions now default-OFF; 9/10 deep across today's post-380 batches

Applying the iter-380 audit rule to the sibling: `SR_FM_ALLOC_SUB`'s load was
never measured alone (bundled in the iter-378 0/4 batch). Alone, post-380:
K=3 `=0` → **3/3 DEEP**, zero substitutions fired, zero null-pool hits — the
FM/Lua pool serves everything natively. Default flipped OFF (=1 diagnostic).
No-env verify (both retired): 3/3 FULL-INIT, 2/3 deep (the residual shallow
roll is the known pre-existing mode). Cumulative today on retired defaults:
**9/10 deep** vs the 75% (9/12) historical. The exe now performs ZERO
allocation substitutions of any kind by default.

### 2026-07-26b — SR_DOGMA_ALLOC_SUB RETIRED: 4/4 DEEP on the PRISTINE allocator dispatch (iter-380)

**Natural frontier MOVES — the default exe no longer substitutes any DOGMA
allocation.** The iter-66 suppression of `sub_8278D258`'s explicit-pool (r5)
fast path — on by default since 2026-05 — was the manufactured root of the
whole `SR_DOGMA_ALLOC_SUB` dependency: it starved the 9 pre-publish bootstrap
allocs into NULL and the divergent init later broke the default pool's grow
path (`AccessContextManager`→0x2B0, `ResourceTask`→0 → NULL task enqueued →
init-time drain wedge; on console the NULL enqueue would CRASH at the vtable
deref — the state is unreachable there). Fix: `SR_REAL_DOGMA_POOL` default ON
(the PRISTINE dispatcher — this deletes behavior, it doesn't add any), and the
substitution (`SR_DOGMA_ALLOC_SUB`) default OFF.

Measured (commit pending, this entry's commit): no-env K=3 220s + K=1 90s:
**4/4 DEEP** (defExec=666, parser=15, clipCtor=453, movInst=451, feFrames
287..3260), zero crashes, zero substitutions (`heap_fallback`=0,
`no_allocator`=0), zero NULL enqueues. The 1-per-batch shallow-17 roll did not
appear in this batch (small N — rate tracking continues). Evidence chain:
Dr0 slot watch (enqueue stored 0, once, guestPC=826F4A00) → `[NULL_ENQUEUE]`
hook (lr=826FAF84 = ResMgrAcquire's GAMECONFIG task) → `[CAD8]` dispatcher
watch (same pool, ON=real memory / OFF=0x2B0,0) → 9-alloc substitution census
→ the r5 fast-path suppression → one-run KILLS-IT (deep on first try).

### 2026-07-26 — DEEP-APT RATE ~21% -> 75% AFTER THE FORCING SWEEP: the shallow-death mode was largely collateral of our own hand patches (iter-370..378)

**Natural frontier MOVES — the deep-roll RATE, not just the depth.** Measured on the
bare default exe (no env), K=4 300s, three consecutive post-sweep batches:
**9/12 rolls DEEP** (defExec=666, movInst=451, feTick 3700-4200) vs the historical
~21%. The dominant shallow-death mode (3 of 4 rolls dying at feTick~17) is gone;
the residual shallow roll (1 per batch) still dies early and is the next target.

**What changed (all iter-370..378, commits a8f6cc0..a9f9f75):**
- ~5,500 lines of FORCING DELETED: the EB80 dispatcher fabricator complex
  (sub_8290E000/E040 + the 4,900-line kernel driver thread + pool seeder), the
  sub_822E77D8 splash reimplementation, GameShutdown_821863A0 restored to
  pristine from IDA (it had been hand-steered into acting as the game since
  iter-31), the sub_825F7168 semaphore fabricator (deleted after root-fixing the
  platform gap it hid: NtYieldExecution now yields; SR_YIELD_ALERTABLE pending
  attribution), forced GPU flags (bit29, frame-done, render-work bit1), vtable
  heals (now detection-only), the 32MB hash sentinel spray, a suppressed store.
- PLATFORM FIXES: the GPU command processor consumes IB descriptors at the
  CP_RB_WPTR doorbell (kernel/gpu_ring_doorbell.cpp, baked default-ON — 713KB/run
  of real submissions vs the 40 bytes the old VdSwap window inference saw);
  render-active bit set by the Vd layer; INDIRECT_BUFFER recursion bounded;
  IM_LOAD ucode structure gate; kPointList GS; atomic fallback-heap bump.
- AUDIT: scripts/audit_forcing.py now sees generated guest code (the fabricator
  lived in gitignored guest/ppc for months while the audit reported clean);
  kernel-side clean, generated-code residue 29 sites (from an initial 62).

**Honest remainder — the last default-ON compensations, each MEASURED
load-bearing (0/4 FULL-INIT without) and documented in docs/BANDAIDS.md with
retirement paths:** SR_HEAP_STUB (real dlmalloc init dies in early boot),
SR_DOGMA_ALLOC_SUB (DOGMA worker semaphore never signals -> main parks at the
ResourceScheduler drain lr=826F4740), SR_FM_ALLOC_SUB (bundled). Those two
platform gaps are the natural-path frontier now.

Reproduce: `.	ools
un_recomp_x.ps1 -K 4 -TimeoutSeconds 300` (no env), read
defExec/feTick per run.

### 2026-07-22 — D3DX EFFECTS COMPILE NATURALLY (0/66 → 33/33) AND THE FMV PLAYER CREATES: the effect/material + FMV render wall was one central allocator overlap (iter-364)

**Natural frontier MOVES.** The whole D3DX runtime effect/material path — which gates the FMV
YUV render (`FindTechnique("tec0")`) and every material — was dead: **all 66 runtime effect
compiles E_FAIL**, so `FindTechnique` dispatched through a NULL effect. Root (proven end-to-end
this iteration, not vibed): the effect-compiler preprocessor's `+104` region-counter is **zeroed
by an allocator overlap**. Our `guestHeapAlloc` (host bump, UP from `0x839E0000`) shares VA space
with the guest dlmalloc heap; a `guestHeapAlloc` grant (the effect node-allocator's block) lands
on the *live* region and its zero-fill wipes the counter → the preprocessor EOFs after the first
statement → zero techniques → every effect fails.

**Fix (platform, not forcing):** `SR_HEAP_SKIP_OCCUPIED` (default-ON, `=0` A/B). Fresh bump memory
is zero-backed; a live guest object carries non-zero bytes — so `guestHeapAlloc` scans each
candidate range's leading `0x100` bytes and **skips it if occupied**, retrying. This coordinates
the two uncoordinated allocators without a blanket gap (`SR_HEAP_GAP` regressed boot; this only
skips genuinely-occupied ranges). It implements the coherent single heap the console provides.

**How measured (deterministic A/B, same build, 2/2 identical):** default run + `SR_PASS_TRACE=1`
(logging only, no behavior change). OFF → `0/66` effects build, `1` technique node, region counter
`1→0`. ON → **`33/33` effects build (`eff!=0`), `35` technique nodes, `26` occupied-skips, and the
FMV player CREATES** (`[FMVRSM]` smState `1→2`, `player=935A0F00`, `hr=0`; was `player=NULL` /
`XMVCREATE 0x8000FFFF`), with `FindTechnique` firing and `draws=499` seen. No boot regression
(`2/2` full-init), no real crash, natural run presents (`present=7200/5400`).

**Notes:** commit `abec1c0`. Same allocator-overlap class as the VB flush-grind, WMA descriptor,
and video parser+0x260 walls — a candidate to also help those. NEXT natural frontier = the FMV
YUV→RGB blit/present now that `FindTechnique(tec0)` resolves a real effect.

### 2026-07-21 — FMV RENDER SWEEP RETURNS AND REPEATS NATURALLY: the “one frame then park” wall was the WMV bitreader's tail callback being erased by the allocator (iter-363)

**Natural frontier MOVES.** After `XMVCREATE rc=0`, the first FMV render sweep no
longer blocks forever. The game's own render runner returns from
`sub_8230C038 → sub_82776038 → sub_82776EB0`, invokes the sweep again, and drives
`sub_8235CA48` decode/draw repeatedly. No `SR_FMV_PUMP_SWEEP` or other forcing is
involved.

**The captured hang:** the XMV worker was executing `sub_824C4428` with bitreader
`cur=0x18,end=0xFFFFFFFF`, causing its unsigned byte-refill loop to scan almost
4 GiB. The preceding `sub_824C4178 → sub_827BAB80` refill had dispatched through
a NULL reader and used stale stack out-parameters (`ptr=0x18,len=-24`).

**Why the reader was NULL:** the decoder allocates a 0x30 bitreader at
`sub_824BF348`'s LR `0x824BF3DC`; `sub_824C3E48` correctly writes the real reader
object to the allocation's final word (`+0x2C`). A second pre-FMV live capture
proved that word is zero before the first `sub_824C4250`. This is the same
next-chunk boundary-tag overlap as the prior FMV allocator walls, at another
exact-tail field.

**Codegen check:** L6 compared `sub_824C4428` against Ghidra's PPC emulator on a
finite four-byte refill and an empty non-callback exit: **2 pass, 0 fail**. The
refill loop is faithful; corrupt input caused the apparent hang.

**Fix (forcing-off platform correctness):** `SR_FMV_BITREADER_ALLOC_FIX`
(default-ON, `=0` restores the captured baseline) applies the existing +0x10
tail reservation only to `{lr=0x824BF3DC,size=0x30}`.

**Measured:** gated K=4 reached `XMVCREATE rc=0`, logged repeated
`DECODE_DRAW return` and subsequent `[SWEEP_Q_RETURN]`, with zero crashes.
Baked-default K=4 completed 4/4 init with zero crashes; best roll reached
`feFrames=1797`, `aptPump=1797`. The default log shows continued video-packet
delivery and repeated natural sweeps. **Honest next frontier:** decode/draw often
returns `0x80004005` and packet delivery can report `0x80600008`; visible
multi-frame output is not yet claimed.

### 2026-07-21 — FMV WMV3 VIDEO DECODER BUILDS NATURALLY: the 0x80600006 wall is a dlmalloc tail-clobber of the 4-byte STRUCT_C extradata; extending the proven tail-protection bump to tiny allocs makes XMVCREATE succeed with zero forcing (iter-362n)

**Natural frontier MOVES.** The intro-FMV path reached `XMVCREATE` naturally but
it failed WHOLE (`hr=0x8000FFFF`) because the WMV3 video decoder returned
`0x80600006`. That wall is now GONE on the default (forcing-off) build: the WMV3
video decoder **creates successfully** (`XMVCREATE rc=0`, player non-NULL).

**The bug (platform-class, our allocator — same root as Wall 1 & the audio wall):**
the WMV3 sequence header the SIMD decoder (`sub_824C9740`) parses is the STRUCT_C
extradata (`4F F1 0A 01`), a **4-byte** allocation. It is copied TWICE — the ASF
parse's copy (`sub_82493060`, ptr@`fmt+52`) and the decoder's own copy
(`sub_824B2E78` line 50, ptr@`codec+84`, the readfn's WMV3 EOS-path return) — and
BOTH 4-byte buffers had their bytes zeroed by the **dlmalloc boundary-tag overlap**
(`sub_8246CA80`; writer confirmed `lr=0x82469B74`). Zero seq header → `a1[967]=0`
→ `sub_824C9740` rc=6 → `0x80600006` → `XMVCREATE 0x8000FFFF`.

**Proof it's data, not codegen:** writing the real `4FF10A01` into the assembled
seq buffer (`sub_824C4250`, after both clobbers) flipped rc 6→0, XMVCREATE
0x8000FFFF→0, FMV smState 1→2→5 (forced diagnostic `SR_WMV_SEQ_RESTORE`). A
per-buffer restore (`SR_XMV_EXTRA_RESTORE` of `fmt+52`) proved the two clobbers are
distinct (fmt+52 restored but `codec+84` still zeroed → rc still 6). `sub_824C9740`
codegen is faithful — EXONERATED.

**The fix (forcing-off, platform correctness):** `SR_FMV_TINY_ALLOC_FIX`
(default-ON) extends the existing +0x10 tail-protection bump down to `4 ≤ size <
0x10` for the bounded WMA/XMV caller region (only ~27 such allocs → a few hundred
bytes, no heap cost). Both STRUCT_C copies now survive (`cxData=4FF10A01`).

**Measured (default exe, `SR_LOG_BOOT=1`, no forcing):** `XMV_AFMT rc=0`,
`cxData=4FF10A01`, `XMVCREATE EXIT rc=0`, `80600006` count=0 — DETERMINISTIC across
K=2, plus the A/B (fix-off → rc=6, 2/2; fix-on → rc=0, 2/2). No boot regression
(`present=3900`, `feFrames=4818`). Clean root would be fixing `sub_8246CA80`'s
tag-write (removes all bumps) but it is faithful complex dlmalloc, not a
mis-lowering — deferred; the gated bump is the retirable step. NEXT: confirm
whether video frames then decode (packet callback register + demux delivery) or a
second delivery gap remains.

### 2026-07-19 — DEEP APT UNBLOCKED NATURALLY: an allocator-overlap platform fix (GPU resource data colliding with the game's own pool headers) lets the game reach deep Apt instantiation again — 0/6 → 2-3/6 rolls, zero forcing (iter-361, `64b86fe`)

**Natural frontier MOVES (post-burndown floor).** After the 2026-07-18 burndown
the honest floor was "init walks + FE ticks, but 0/6 reach deep Apt (defExec/
movInst)". This entry restores deep Apt to the NATURAL path via a platform fix,
not a band-aid.

**The bug (platform-class, our allocator):** `D3DVertexBufferCreate` (and the
texture-resource creators) split each resource into a HEADER (bit31-clear → the
game's `__imp__` pool) and DATA (bit31-set → our `guestHeapAlloc` override). The
`[ALLOC_COORD]` diagnostic proved the pool is a THIRD allocator the heap↔phys
cross-check can't see (`physLowVA=0xAF000000` while headers live at `0x8FFBxxxx`,
`pool-tracked=NO`), so the data overlaps a header by 0x10. The vertex/texture
memcpy then scribbles the header's flush-range field — caught EXACTLY with a
Frida watchpoint (writer lr=`0x82786acc` = the memcpy). This didn't just cause a
~88s `dcbf` flush-grind; it CORRUPTED the render path and blocked deep Apt.

**The fix (forcing-off, platform correctness):** route bit31-set GPU resource
allocs (VBs always; textures ≤256KB once the ring VA is up) to `guestPhysAlloc`
— a disjoint block ~500MB from the collision zone. `SR_GPU_VB_DISJOINT` (VBs) +
`SR_GPU_RES_DISJOINT` (resources), both default-ON.

**A/B PROVEN (2×K=6 each, zero env, forcing off):**
| gauge | fix OFF (default) | **fix ON** |
|---|---|---|
| deep Apt rolls | **0/6** | **2-3/6** |
| `defExec` | False | **666 / 813** |
| `movInst` (REAL MOVIE INSTANCED) | False | **451** |
| `clipCtor` | ≤2 | **453** |
| FE frames | (ticks only) | **2700-3800** |
| init walks | 6/6 | 6/6 |

Reproduced across two fix-ON batches. So fixing OUR allocator's overlap lets the
game's OWN code re-reach the deep-Apt frontier (the same defExec=666/movInst=451
depth the pre-burndown contaminated baselines showed, now NATURAL).

**Honest caveats:** (1) roll-dependent (2-3/6, DOGMA non-determinism unchanged);
(2) ~1/6 roll logs a recoverable guard-page `0x80000001` mid-render (process
keeps running, still reaches the frontier) — non-fatal, provenance TBD; (3) a
residual flush-grind remains at another resource (ctr=`0x823ACE00`). NOT the menu
on screen yet — draws still don't reach the ring (the separate draw-cb→DRAW_INDX
bridge gap). Contracts: memory `project_gameloop_readiness_frontier`; commits
`1f4a589` (VB fix) / `d01b99e` (coord root) / `64b86fe` (resource fix bake).

### 2026-07-18 — FORCING BURNDOWN: ungated forcing 17→0, enforced by a re-runnable checker + pre-commit hook. A live probe then RETRACTED my own "contamination" read: the gated hash band-aids were near-dead on the natural path (burndown, `audit_forcing.py`)

**This is a measurement-honesty entry, not new guest behavior — and it includes a
correction of a claim made earlier in this same entry's first draft.** The forcing
burndown (`scripts/audit_forcing.py`, structural + re-runnable) found 10
guest-state-fabricating sites that were **ungated** (a real charter violation —
active on every run regardless of `SR_ALLOW_FORCING`):

* `dogma_hash_hooks.cpp` — bucket back-fill + degenerate/spin short-circuit for
  the Robin-Hood hash family (`sub_825FDBB0/825FD958/825FDA60`).
* `dogma_pool_registry_seed.cpp` — a fabricated fallback pool + vtable at `*(0x829A93F4)`.
* `crt_file_lock_init_fix.cpp` — a fabricated critical section per CRT stdio slot.

All 10 are now behind `forcingAllowed()` (`SR_ALLOW_FORCING`, default OFF); 7
other flagged sites were mislabeled and allowlisted with written reasons
(default-off GPU diagnostics; XAudio driver platform writes). **Ungated forcing
count: 17 → 0**, enforced going forward by `audit_forcing.py --count` +
`scripts/git-hooks/pre-commit` (baseline `scripts/forcing_baseline_count.txt`, now
0). "Are there hidden band-aids?" is a command now, not a claim.

**⚠️ WHAT I GOT WRONG, then caught with a live probe (charter §3 in action).**
My first read of the gating was: "4th contamination — these load-bearing hooks
were carrying the whole Apt/FE frontier; the true natural boot is much lower."
I built that from thin K=2-vs-K=4 data (walksRet 4 one batch, 0 another) and a
mis-derived pointer-alias story. **The oracle-first rule says design experiments
from the low tiers but never conclude from them.** So I ran the decisive one —
a default-off `SR_HASH_PROBE` (pure observation, no writes) on a natural boot.
It refuted the narrative:

* **The natural hash tables are HEALTHY.** `table=0x82960838 buckets=0x82960848
  (non-NULL) cap=3072 cnt=0→5 growing.` Not degenerate. The hooks only act on
  `{buckets=NULL/cap=0}` tables — which **do not occur** in early natural boot —
  so they were **near-dead code on the natural path**, closer to iter-358f's
  deletable fake device than to a load-bearing contamination. Gating them costs
  ~nothing; my "carrying the frontier" claim is **withdrawn**.
* **No thread spins in the hash.** During the stall, `[THREAD_SAMPLER]` shows the
  init-walker threads parked at `lr=8246A410` (CRT `_initterm` / FM static-ctor
  walker) inside ctors at `ctr=826F92E0` / `828B4658` for 40+ s — **not** at the
  hash lookup PC. The "missing hash grow-path" was a red herring for this stall.
* **The bit-25 "uncached alias" hypothesis is REFUTED**: `0x82` base already
  includes `0x02000000`, so every normal data address has bit 25 set. The hook
  comment's aliasing rationale was unsupported (band-aid self-justification, cf
  the iter-358e "NULL vtable" story).

**The honest natural baseline (K=6 × 200s, forcing OFF):** `walksRet` = 4,4,1,1,1,4
— **init-walk completion is roll-dependent (3/6 here; 0/4, 2/2, 1/2 in other
batches = DOGMA non-determinism, NOT a deterministic effect of the gating).**
**0/6 reach aptCtx / clipCtor / FE ticks / Apt pump** (present 5400–6000).
`SR_ALLOW_FORCING=1` gives a steadier `walksRet=4 / aptCtx=1 / init2=1` but still
0 clipCtor/feFrames — a *different*, not strictly-further, point.

**The REAL natural frontier (unchanged, pre-existing, now correctly attributed):**
the FM-module static-ctor walk (`_initterm` family, `lr=8246A410`) parks in
constructors at `ctr=826F92E0 / 828B4658` for tens of seconds, and downstream
Apt/FE work never starts. That — not the hash — is the thing to investigate next.
Whether the ~144 pre-existing MASTER-gated forcing sites are what let
`SR_ALLOW_FORCING=1` push past it is a separate, open question.

### 2026-07-18 — 🏆 THE FMV PARSER WALL FALLS: a recompiler allocator bug root-caused + compensated; the game's XMV player advances PAST the ASF parse for the first time (iter-360, `SR_ALLOC_FIX`)

**Natural frontier MOVES.** The FMV/menu wall since iter-340 was `XMVCREATE`
returning E_FAIL 0x80004005 — the FMV state machine parked at state 1 forever,
so boot never left the movie phase. Root-caused this session to a RECOMPILER
allocator bug: `sub_8246CA80` gives 16-byte-aligned requests only R+0x10 of
chunk (front header, no room for the 0x10 back boundary tag the next chunk
writes), so that tag clobbers the live tail at R-0x10. The ASF parser is a
0x270 (16-aligned) alloc storing its demux reader back-pointer at +0x260 = the
doomed tail. (The 0x1f rounding is the ORIGINAL game constant and the game ships
on hardware → the console does not write that tag into in-use neighbors → our
recomp does. Platform-fix class.)

**A/B PROVEN (same binary):** with `SR_ALLOC_FIX=1` (default ON): 0 E_FAIL/run
(5 runs), the ASF header PARSES for the first time (`sub_82488070` rc=0 with
REAL format fields — channel count 2, width/height), `XMVCREATE` advances from
0x80004005 to a DIFFERENT downstream error 0x8000FFFF, and the FMV SM reaches
**state 4 (menu)**. Stable: no crashes, 150s, present to #6000. With
`SR_ALLOC_FIX=0`: E_FAIL 0x80004005 returns (3-6/run), header rc=3, SM parks.
Deterministic mechanism: `SR_ALLOC_PROBE` isolation (0x270 then 0x20) clobbers
6/6 off, clean on.

**What this is / isn't:** a genuine platform fix (recompiler-bug compensation),
so the advance is NATURAL — the guest's own XMV player runs correctly on a fixed
platform. It is a BAND-AID FORM (bump-the-request) pending the clean regen
root-fix of `sub_8246CA80`'s tag-write. NOT the interactive menu yet: the
downstream 0x8000FFFF (the ~28 switch-holed 0x8247xxxx decode fns) is the next
wall, and defExec/aptReq still 0. But the FMV parse — dead since iter-340 — now
runs, and the SM reaches the menu state. Contracts: memory
project_wipe_rootcause_arc §iter-360/360b; PROGRESS iter-360.


### 2026-07-17 (LATEST) — baseline HOLDS; the render path now sees the game's REAL command stream: 24 real draws + a real VS/PS pair per frame, natural, zero forcing (iter-358h, `26faf1e`)

**Guest frontier unchanged** (this entry moves the PLATFORM, not the game):
zero-env K=4×200s → 3/4 rolls full menu chain (`defExec=666 parser=15 aptReq=15
clipCtor=453 movInst=451`), 1/4 the known DOGMA roll, 0 crashes, `present=11700`
uniform, `feTick` up to 2421. Reproduce: default build, look for `[GPU:PM4]
region#N` lines.

**What changed:** the live PM4 path previously parsed the real stream in the
deleted fake device's 8-byte slot format — 6 garbage type-3 packets in ~91 MB
over 401 swaps. iter-358h root-fixed the chain (VdSwap's reserved 64-dword swap
block now written Xenia-parity; ONE shared engine `gfx/pm4_engine.cpp`, replay
parity re-verified byte-exact; slot parser + past-wptr fence scribbler DELETED,
net −502 lines). Now, every frame, natural: **24 real draws (DRAW_INDX_2),
~2.8K register writes, a real VS/PS pair via IM_LOAD_IMMEDIATE (VS decodes to
pos+color stride-7-words — solid-color quads), 12 real GPU fences executed
in-order.** `resolves=0`: the front buffer at swap is named by the VdSwap r4
fetch constant, not EVENT_WRITE_EXT — consuming it is the named next step.

### 2026-07-17 — 🏆 THE FAKE D3D DEVICE IS **DELETED** (1,953 lines) AND THE MENU CHAIN STILL RUNS. Baseline HOLDS, honestly (iter-358f)

**Read this before the `9a5d1d3` entry below — that entry's "baseline moves DOWN"
verdict is WRONG, and the reason is the lesson of the day.**

`patchD3DDeviceVtable` was gated off (default-OFF) but `pumpRenderState`,
`startRenderThreadHost` and `setupFakeGpuDevice` were **left running**. Those drive
the render state machine *through* the vtable that was no longer there — so the
half-gated build wedged at `feTick=25`, and I read that wedge as "the honest lower
baseline". It wasn't. It was a broken intermediate state of my own making.

**Deleting the WHOLE family fixed what gating broke:**

| | fake vtable ON | HALF-GATED (wrong) | **FULLY DELETED** |
|---|---|---|---|
| `defExec` | 666 | 0 | **666** |
| `movInst` | 451 | 0 | **451** |
| `feTick`  | 2977 | 25 | **2402** |
| `aptReq`  | 15 | 0 | **15** |
| `cmd43`   | 4472 | 0 | **0** |
| `present` | ~9000 | 3600 | **11400–12600** |

Two independent K=4 batches: **3/8 rolls reach the full menu chain with the entire
fake-device subsystem deleted and zero PIX-monitor traffic.** (Roll variance is the
separate, known DOGMA non-determinism problem.) Init walks 4/4 throughout.

**THE LESSON — the half-measure was worse than either extreme.** Gating left the
dead code in the tree still being *called by* its dependents. Deletion is the
charter's actual instruction (force → diagnose → root-fix → **delete**), and it is
the default from here. Gating is only for forcing that reveals a platform gap we
intend to build. A misidentified address reveals nothing — it just goes.

**What was deleted (1,953 lines):** `patchD3DDeviceVtable`, `d3dVtableStubDispatch`
(+`Ex`), `pumpRenderState` (incl. the ALLOC2/ALLOC8/VB_PROBE heap-scavenging),
`linkRenderContext`, `resetD3DBumpAllocator`, `startRenderThreadHost`,
`setupFakeGpuDevice`, the render-pump thread, `renderTestPattern`, the test
pipeline (`m_testPSO`/`m_testVBView`/`kTestShaderSrc`), `probeFromFetchConstants`,
and the `g_alloc2FloatVA` override. Also removed: the `PPC_CALL_INDIRECT_FUNC`
branch dispatching the `0xD0030000` stub range across ~2,500 generated call sites —
and the same edit in `tools/regen/hand/ppc_context.h`, so a regen cannot resurrect
it.

**Everything below about WHY it was fake still stands** (`0x8200094C` =
`KeCertMonitorData`, NULL on retail by design; real device = `VdGlobalDevice`
@`0x82000948`, four bytes earlier; `cmd43`/`cmd46` = PIX monitor opcodes 0x2B/0x2E).
`cmd43` going 4472 → **0** while the menu chain is unaffected is the proof: that
traffic was never the game rendering, and nothing of the game's depended on it.

---

### 2026-07-17 (⚠️ VERDICT SUPERSEDED — see above) — "the D3D device" is `KeCertMonitorData`; the render path has been faked for ~350 iterations (iter-358e, `9a5d1d3`)

**The IDENTIFICATION in this entry is correct and important. Its BASELINE VERDICT
("moves down") is WRONG — it measured a half-gated build, not the natural game.
Corrected in the entry above. Left in place because the misread is instructive.**

`patchD3DDeviceVtable()` claimed `0x8200094C` was "the game's D3D render device"
with "a NULL vtable because the XDK kernel code isn't recompiled". **Every clause
is false.** From Xenia's import table for this XEX
(`docs/_attic/xenia_canary_log.txt:1186-1189`):

```
82000948   1BE (446)  VdGlobalDevice      <-- the REAL graphics device
8200094C   266 (614)  KeCertMonitorData   <-- what we patch
82000900   059 ( 89)  KeDebugMonitorData  <-- kAltRenderObjPtrVA
```

The real device sits **four bytes earlier**. An off-by-one import slot that
rationalised itself into a "D3D device" story. `KeCertMonitorData` is the PIX
cert-monitor hook: **NULL on retail by design**, every guest use site guarded
`if (*__imp__KeCertMonitorData != 0)`. **The "NULL vtable" this subsystem existed
to fix WAS the correct retail behaviour.** Writing a vtable there switches on a PIX
debug-capture path the console never runs:

| what we called it | what it is |
|---|---|
| `cmd43` | `0x2B` — emit trace blob to the monitor |
| `cmd46` | `0x2E` — end-of-frame monitor notify |
| `vtable[3]`/`vtable[4]` (60/59 calls) | the monitor object's malloc/free lifecycle |

`sub_82111798` *is* a real PM4 walker — but it walks the buffer only to **stream it
to the monitor**. We were reading Microsoft's debug tracer and calling it our GPU.

**PROOF (K=4, zero env, gated off):** `cmd43_total` tag **absent**, `D3D_STUB`
vtable calls **0** — down from 4472 / 260. The entire "GPU traffic" was 100% our
own fabrication.

**THE COST, stated plainly:** with the fabrication off, the menu chain stops.

| gauge | with the fake vtable | honest (forcing off) |
|---|---|---|
| `feTick`   | 2977 | **25** |
| `aptPump`  | 2977 | **24** |
| `defExec`  | 666  | **0** |
| `movInst`  | 451  | **0** |
| `aptReq`   | 15   | **0** |
| init walks | 4/4  | 4/4 (unchanged) |

4/4 rolls, deterministic. **Today's celebrated numbers — the ones recorded in the
two entries below — were downstream of this fabrication.** That is the THIRD
contamination found today and the deepest. The honest natural frontier is: the game
completes its init walks (4/4) and ticks its FE loop ~25 times, then stops.

**The game itself does not depend on this** (retail leaves it NULL and ships). The
dependency is OURS: harness forcing further up (`pumpRenderState`, audit item F6)
drives the render state machine *through* this fake vtable, and the FE chain was
riding on that. **That chain is now the thing to dismantle, not restore.**

**THE REAL SEAM (both audits concur):** Xbox 360 D3D9 is **static-linked into the
XEX** with macro-inlined direct calls — there is no COM vtable to intercept at the
API level (`0x820BC6B8` is an internal D3D9 class vtable, never handed to game code;
its slot 4 is a format-descriptor getter, not `SetStreamSource`). The only boundary
between title and hardware is the **Vd\* kernel imports + the PM4 ring**
(`VdInitializeRingBuffer` base + `VdSwap` WPTR → `parsePM4Region`, which already
exists and already extracts RESOLVE addresses). That is where the renderer belongs.

**Also established today (still valid, and independent of the above):**
* The vertex FORMAT is **not** in the fetch constant (`xenos.h:1158-1172` = type/
  address/size/endian only). It lives in the vertex shader's `vfetch` instruction
  (`ucode.h:690-775`: format bits 20-25, signed bit 12, normalized bit 13, RF mode
  bit 14, exp_adjust bits 26-31; stride/offset in dword 2). Our observed `0x7FFF`
  is consistent with SNORM16 — but must be verified by reading the vfetch, not by
  pattern-matching bytes.
* **The golden trace is a complete, self-contained workload.** `parse=CLEAN-EOF`,
  123 MB of memory restored, 0 snappy failures, 1.74M register writes. Xenia's
  `trace_player.cc:169-232` restores memory + registers + EDRAM and re-executes real
  packets — including shader ucode (IM_LOAD writes a `WriteMemoryRead` first). The
  renderer can be built and regression-tested against it **today**, with no boot
  dependency (CLAUDE.md's "renderer work replays golden traces" rule).
* Xenia-Canary is **BSD-3** (attribution only) — portable with a notice.

### 2026-07-17 (superseded by the above) — the game's own geometry renders naturally; renderer gap named (iter-358, `9a55ca4`)

**Run:** K=4 + K=2 × 200 s, default build, **zero env, forcing off, 0 `[FORCED]` tags.**

**What the game now provably does by itself:**

```
[D3D12] Rendering guest geometry: 3454 verts, 2 batches
```

Real vertices, resolved from the guest's own draw commands and its own Xenos
vertex-fetch constants. **Deterministic** across rolls, and **staged**: 3454 verts
for ~4400 geometry resolves, then **56 verts** — a genuine transition in what the
game is drawing.

**How this became visible — four ungated forcing sites, now gated:** the render
path was the largest ungated forcing surface in the tree (exactly one site
consulted `forcingAllowed()`). `vtable[6]` fabricated a 1406-vert draw every call
*and* bumped `drawSubmits`; the `VB_PROBE` heuristic latched heap floats and
**overwrote the game's correct fetch-constant VB**; `renderTestPattern()` presented
a hardcoded debug triangle whenever `pendingSubmits > 0` (which the fabrication
guaranteed forever); `probeFromFetchConstants()` synthesized batches with no draw
command at all. **Gating them cost the boot NOTHING** (defExec 666, movInst 451,
feFrames 2439, 3–4/4 rolls unchanged) — the forcing was buying us nothing and
hiding the real result.

**ATTRIBUTION DEBT PAID (charter §3):** every `present`/`cmd43`-derived claim
about the *rendered image* predating this entry was contaminated — including
iter-357e's "present 10.5–11.4K vs ~6.2K, cmd43 ~6K vs ~250" jump tagged
**natural**, and the measurement-correction entry below. `present`/`cmd43` remain
honest *counts* (those loops run regardless); claims about the *image* do not.

**THE RENDERER GAP — named and measured, no longer a mystery.** We draw the game's
true vertices through a **placeholder pipeline**:
```hlsl
float scale = 1.0 / 10.0;                   // VS: hardcoded ortho; "scale will be recalibrated after"
float3 n = frac(abs(p.worldPos) * 0.001);   // PS: "Color by world position for visual debugging"
```
That debug shader **is** the cyan→magenta gradient in every frame dump. Nothing
recognizable appears because there is **no Xenos shader translation and no real
vertex transform** — squarely a PLATFORM implementation task (implementing what
the console's GPU provided), not forcing and not a blocker to diagnose.

**Next on this leg:** Xenos vertex/pixel shader microcode → HLSL/DXIL translation,
real vertex-format decode from fetch constants, real transform/viewport. The GPU
trace replay (`data/gpu_traces/menu_pressstart_454107ED_9415.xtr`) can drive all
of it offline without waiting on the boot.

### 2026-07-17 (earlier) — MEASUREMENT CORRECTION, *not* guest progress: the natural boot was ALWAYS doing far more than the aggregate reported (iter-358, `4400606`)

**⚠️ READ THE ATTRIBUTION FIRST (charter §4).** The baseline numbers below jump
by orders of magnitude. **The game did not change. Our instrument did.** Nothing
in this entry is new guest behavior — it is the same boot, finally measured
honestly. The baseline moves here on *measurement correctness*, which is exactly
the "reproducible, anti-gaslight" property §4 exists to protect: the previously
published numbers were **wrong in the pessimistic direction**, and a human
re-running the old aggregate would have been misled the same way we were.

**Run:** K=4 × 200 s, default build, **zero env vars, forcing off** (`4400606`).
3/4 rolls identical; run2 = a known bad roll (DOGMA non-determinism).

**What was broken:** the aggregate's `movInst`/`feFrames`/`defBuilt`/`aptTick`
columns counted `[BLOB_INJECT]` / `[FE_TICK]` / `[APT_TICK]` log lines — tags
emitted ONLY by the forcing drivers in `splash_hooks.cpp`, which sit behind
`forcingAllowed()` (default OFF). **On a natural run they never execute, so those
columns read 0 BY CONSTRUCTION regardless of what the game did.** `feFrames` had
already been recalibrated once (iter-326) onto `[XMSG_7001A] delivered` — a
premise iter-356 later REFUTED (0x7001A is the XMP music player) and whose hijack
is now opt-in, so it read 0 too. `defExec` counted capped log lines, not events.

| gauge | previously published | TRUE natural (unchanged game) |
|---|---|---|
| `feFrames`  | 0   | **1603–2314** — FE master tick `sub_821494C0`, `lr=0x8271EE0C` (the game's own loop) |
| `aptPump`   | n/a | **1603–2314** — Apt executor pump `sub_826D2718` |
| `movInst`   | 0   | **451** — clip ctors with a REAL (non-NULL) `movieDef` |
| `defExec`   | 25  | **666** |
| `clipCtor`  | 453 | 453, **of which 451 are REAL** — not empty bookkeeping ctors |
| `aptReq`    | 15  | 15 (confirmed; `[APT_REQ]`'s log was capped at 24) |

**So the corrected natural frontier is:** the game boots (4/4 init walks), runs
its own FE master tick and Apt executor pump thousands of times, requests 'main'
+15 UI movies in golden order, populates the catalog, and **instantiates 451 real
movie clips** — all by itself, zero forcing. `present` 9000–9600, `cmd43` 4472.

**⚠️ SUPERSEDED WITHIN THE HOUR — see the render entry below (`9a55ca4`).** This
entry originally read "the game submits real draws (`cmd43_total=4472`) but our
draw queue is empty at present". That was **wrong on both halves**, and the error
is instructive enough to leave visible rather than quietly edit away:

* `pendingSubmits=419` was **inflated by our own ungated fabrication** — vtable[6]
  (`guest_context.cpp:2379`) invented a 1406-vertex draw on every call AND bumped
  `drawSubmits`. Gating it dropped the figure to 347. I cited a contaminated
  metric as evidence of guest behavior, the exact failure charter §3 exists to
  prevent, in the same commit where I corrected a different dead instrument.
* `queueSize=0` at present is **normal**: the queue is drained every present
  (`d3d12_backend.cpp:1348`) while `drawSubmits` is a never-reset lifetime total.
  The two were never comparable quantities.
* The geometry **was** reaching the backend all along: with forcing gated off,
  `[D3D12] Rendering guest geometry: 3454 verts, 2 batches`.

`present` and `cmd43` remain honest COUNTS (those loops run regardless), but any
claim about the rendered IMAGE from before `9a55ca4` is void.

**Instrument contract going forward** (`kernel/natural_gauges.{hpp,cpp}`):
counters are atomic and UNCAPPED; a wall-clock thread emits one authoritative
`[GAUGE]` census every 10 s; the aggregate parses the LAST census line.
**Never count log lines again** — capped probe output reporting its own cap as
the measurement is the "sampling trap", now 6 occurrences. A gauge that can only
move with `SR_ALLOW_FORCING=1` does not belong in the baseline.

### 2026-07-17 (later) — THE MENU CHAIN FIRES: FMV ladder completes, 'main' + 15 UI movies requested in golden order, THE APT CATALOG POPULATES, defExec/parser/clipCtor move for the first time (iter-357j)

**Run:** K=2 × 500 s, default build, zero env vars, forcing off (commit `46ddff1`).

**What the game now does BY ITSELF, 2/2 identical, 0 crashes:**
1. **The splash ladder COMPLETES**: XMV player create returns (decode still
   blocked by ~28 remaining switch-holed XMV fns → movies skip-on-error),
   the FMV SM cycles error→movie-end through all 5 splashes, PSM counter
   walks 0→5→-1, **done(+0x234)=1**, and the boot flow's `AreFMVsDone()`
   Lua condition reads TRUE.
2. **The menu chain fires**: `[APT_REQ] #1 path='main'` — **movie #1, the
   Gap-A request that never happened in project history** — followed by the
   exact golden catalog order (ScrForeground_IG, **ScrPressStart**,
   ScrBackground_IG, LetterBox, SaveLoadSlots, ButtonHints, DisplayWell,
   PartText, ButtonIcon, BasicButton, PartHighlightBar, HintA/HintB…).
3. **The Apt catalog POPULATES**: `cat[0]=88AF1444` (the {value,next}
   prepend list golden shows — 0 forever until tonight); `[POSTER_FIRE]`
   registrar fires (0× in every prior iteration).
4. **Menu metrics move for the first time ever: defExec=25, parser=15,
   clipCtor=453** (all-time prior: 0/0/≤1).

**Platform fixes behind it (PROGRESS.md iter-357j):** render pass dispatcher
sub_827851E8 (29 ERROR-holed cases; 'fmvmovie' pass 0x1C is the FMV pump's
only dispatch site), XMV GetOutputFormat sub_824797D8 (12 formats),
ExCreateThread CREATE_SUSPENDED honored + real NtSuspend/ResumeThread (the
XMV creator/worker handshake), XMsg(0xFA,0x7001A) hijack gated OPT-IN.

**Remaining walls:** movInst/feFrames/aptTick still 0 (movie INSTALL into the
executor + FE frame advance); XMV decode E_FAIL (visible movie playback) —
both filed, the latter for the regen pipeline (fixes all 284 holed fns).

### 2026-07-17 — THE GAME RUNS ITS OWN BOOT SCRIPTS: PlayFMVs fires from Lua, the splash movie is queued, and the fmv archives stream-load (iter-357i: the Lua VM worked for the first time in project history)

**Run:** K=4 × 200 s, default build, zero env vars, forcing off.

**What the game now does BY ITSELF, 4/4 deterministic, 0 crashes, 0 Lua errors:**
1. **Every boot FSMA Lua chunk compiles and runs** (dostring rc=1 across the
   board; previously 200/200 failed with swallowed syntax errors — the VM had
   NEVER successfully compiled a single script on this platform).
2. **The BaseWorldFlow boot state's enter chunk executes** and calls the
   game's own `PlayFMVs()` native from Lua (`[SPLASH_TICK]` lr=luaD_call).
3. **The splash ladder advancer picks EA_HD_logo from the table at 0x8291363C
   and queues it into the FMV state machine** (state 0→1) — the "who fires the
   movies" question open since iter-350 is answered: the game does.
4. **The movie archives load naturally**: NtQueryDirectoryFile enumerates
   `fmv.AST` (996 MB) + `fmvlegal.AST`, then `BigFileAsset::Load("d:\DATA\fmvlegal")`.

**Fix classes (all platform fixes, zero forcing — see PROGRESS.md iter-357i):**
recompiler switch-holes in the Lua parser (getbinopr/block_follow + GC family,
kernel strong-defs); recompiler u64-switch-dispatch bug (~550 sites, u32'd);
our realloc guard's over-strict 8-align heuristic; the iter-190 fake-TString
string-intern band-aid nest REVERTED to pristine recomp bodies; the
audit-proven CallInfo clobber (L+40/L+20) in the spliced stack-realloc deleted.

**New frontier:** FMV SM parks at state 1 — the movie is queued and its archive
is loading at timeout; the native FMV open/decode/movie-end path is the last
wall before visible splash movies → the FMV ladder → the menu.

### 2026-07-16 — REAL TEXTURE UPLOADS + first natural clip ctor (iter-357c/d/e: TEXUP forcing escape gated, recompiler switch-hole root-fixed, hand walker deleted)

**Run:** K=4 × 240 s, default build, zero env vars, forcing off (commits `7101acb`→`a5441a8`→`d4855b3`).

**⚠️ CONTAMINATION CORRECTION (ratified by the human 2026-07-16):** every prior
entry ran with a then-unknown ungated forcing escape active — the iter-220
`sub_8261C5B0` texture-upload fake-success (default-ON under a misnamed gate,
outside `SR_ALLOW_FORCING`), silently swallowing 6 real texture uploads per
boot. The prior entries' *achievements* (WorldFlow, FSM parse, wipe-dead) were
re-confirmed on this run WITHOUT the band-aid, so they stand — but their
metrics were not clean. This entry is the first fully-forcing-free baseline.

**The charter loop closed at record speed on this one** (force → diagnose →
root-fix → confirm → delete, all in one day): gating the escape exposed the
real wall (boot wedges in the Xenos texture tiler, w=h=55904); the tiler probe
named garbage bounds = the descriptor pointer's low16; the cause is a
**recompiler switch-lowering hole** (XenonRecomp emits computed-jump switch
cases as `// ERROR` + bare return — the texture width/height getters
`sub_8260A328`/`sub_8260A3D0` returned their input pointer). Kernel strong-defs
with Ghidra-verified semantics fix the pair (`kernel/recomp_switch_fixes.cpp`);
the wedge texture is really 128×128 and tiles in 3 ms. **2,476 more `// ERROR`
case sites exist tree-wide — filed as a recompiler-class family for the regen
pipeline** (the iter-321 sweep repaired only the 30 open-frame instances).

**BASELINE ADVANCES — all natural, 4/4 rolls, 0 crashes:**
1. **All 6 boot texture uploads EXECUTE for real** (tile + upload complete,
   `[TILER] RETURNED`, correct dims) — first time ever; the iter-220
   fake-success band-aid is dead code behind `SR_ALLOW_FORCING`.
2. **Full natural init holds without the band-aid**: walksRet 4/4,
   lastInit=0x8295F1C4, WorldFlow loads, FSM parses — everything iter-350j
   recorded, now clean.
3. **present 10,500–11,400** (prior entries ~6,200) and **cmd43 5,376–6,596**
   (prior ~250) — the draw path does real work now that textures exist.
4. **First-ever natural clip ctor: `clipCtor=1` on 4/4** (`[CLIP_CTOR] #1
   movieDef=NULL node=0x88A14654 lr=0x826CB300`) — new, downstream of real
   textures; a fresh lead into the Apt world.
5. The retired FM hand-walker body (734 lines of charter-dead code, incl. the
   FMGameRtam skip-list) is DELETED; boots clean without it.

**Where it stops (frontier unchanged, now clean):** the Apt movie-def catalog
stays empty — nothing issues the movie-load REQUEST (`POSTER_FIRE=0`,
`cat[0]=0`, executor pumps 2400+ ticks on an empty queue). The requester hunt
is live (see the EB80 watchpoint probe, TOOLKIT §1.7).

### 2026-07-14 — THE FE FLOW FSM PARSES (iter-350j, init-timeout forcing retired)

**Run:** K=4 × 300 s, default build, zero env vars, forcing off.

**THE ROOT (a piece of our own forcing, hiding for ~100 iterations):** an
ungated init-timeout band-aid in the hand-edited recomp source forced
`sub_826E4D80` to return 1 after 10 s of every init walk. That function is
NOT a "sim loop done flag" (the band-aid's mislabel) — it is the RESOURCE
RECORD STATE CHECK consulted by every asset acquire. Effect: every asset
requested >10 s into a walk silently aborted as "cannot load". That single
line was the entire menu/FMV wall AND the per-roll config noise (a race
between the walks and the timer). Its condvar twin was retired to
`SR_LEGACY_INIT_TIMEOUT` in iter-297; this site was missed. Same retirement
applied (default OFF). **Deleted forcing, zero new code — the purest
charter fix possible.**

**BASELINE ADVANCES — all natural, 4/4 rolls:**
1. **The `WorldFlow` asset (the FE flow FSM config, inside `baseworl.AST`)
   LOADS**: catalog-3 open succeeds, 83 KB read, translated, handed to the
   flow parser. First time in project history.
2. **The FE flow FSM PARSES and BUILDS**: `[FSM_PARSE]` walks 256+ elements
   (fsm/states/state/connectivity/transition/attributes); the FSM table
   registers **43 states** (`stIdx=0x2B`, was 0 forever). 4/4 deterministic.
3. **The gameconfig key/value store populates every roll** (`WorldData`
   loads reliably; `CFGSTR WorldFlowXML → 'WorldFlow'` 4/4) — the 1-in-4
   empty-config roll noise is dead (it was the same root).
4. A cascade of dependent flow-asset loads follows (catalog opens for the
   flow's dependency entries) — the resource pipeline is genuinely alive
   past boot for the first time.
5. Init walks still complete 4/4 (`walksRet=4`) — the wedge the band-aid
   originally masked did NOT return (its true causes were fixed in
   iter-291/297/306/348).

**Where it stops (the NEW frontier — deterministic, 4/4):**
- The parsed FSM never ENTERS its start state: `curState=0`, FSM actor
  stack stays empty, zero `[FE_SHOWSCREEN]`/`[FE_SETSCREEN]`, screen id
  stays 0, PSM splash counter parked at -1, FMV SM state 0. The graph
  exists; nothing ticks/starts it. Next hunt: who starts/ticks the FE flow
  FSM instance (world-flow entity tick / 'ACTR' service / flow-mgr update).

### 2026-07-13 — THE .DATA WIPE IS DEAD (iter-348, NetDll no-client root fix)

**Run:** K=4 × 300 s, default build, zero env vars, forcing off.

**BASELINE ADVANCES — all of the following is new, natural, and 4/4 rolls (was
roll-dependent or never before):**
1. **The mid-boot .data wipe is GONE.** Root cause found and fixed: the game's
   own remote-debug/capture service (port 9201) was fed a PHANTOM CLIENT by our
   generic-success NetDll stubs (`accept()`→0 passed the `!=-1` check; `recv()`
   "succeeded" with an untouched buffer), so its handlers parsed stale stack
   memory as commands and resized their shared tri-buffer with garbage sizes —
   one pointer-as-size resize ran `memset(buf, 0, ~2.2 GB)` over `.data`,
   killing the splash-name table AND the FE service descriptors in one sweep.
   Fix: honest console semantics — accept/connect/recv/recvfrom/send/sendto
   return SOCKET_ERROR (-1, no client attached), WSAGetLastError=WSAEWOULDBLOCK,
   accept paces like a blocking call. On real hardware this code never runs;
   now it doesn't run here either. **Platform fix, zero forcing.**
2. **The full FE service quartet is served naturally**: SVCTICK reaches Core,
   RenderMain, **Simulation (0x8295C690 — never served before)**, and Sound,
   16K+ tick cycles, 4/4 rolls.
3. **FeMasterTick runs naturally** (its callee, the FMV state-machine update
   `sub_8235C8D0`, is invoked with a real PSM instance built by the natural
   lazy-ctor).
4. Splash-name table at `0x8291363C` verified byte-correct at end of run —
   the backfill diagnostic confirms "already correct NATURALLY" →
   **backfill band-aid now formally retirable**.
5. `present` 13.5K–16.2K/roll (up from 3.9K–9.3K), 0 crashes.

**Where it stops (the NEW frontier — deterministic, 4/4):**
- `[FMV_SM]` is called exactly 4 times, stays at state 0 (intro), `pendName`
  never set — the splash sequencer never queues the first movie name, and the
  FMV update stops being invoked after call #4. Next hunt: why the tick chain
  stops feeding it / what gates the first `EA_HD_logo` queue.
- One kernel-loop manager (late-created, e.g. `mgr=8A27EEB8`) re-enters its run
  function instantly 32K+ times (run flag on, body returns immediately) —
  benign-looking but unexplained; candidate side-quest.

### 2026-07-13 — FIRST TRUE BASELINE (iter-342, commit 53326c3)

**Run:** K=4 × 240 s, default build, zero env vars, `SR_ALLOW_FORCING` off. Verified zero
forcing leaks in the logs (no fabricator, no drives, no name substitution, no synthetic draw).

**How far the natural game gets, 4/4 rolls, 0 crashes:**
1. Full natural FM-module init — `walksRet 4/4`, all 63 modules, real objects, dep order.
2. Apt context builds naturally (`aptCtx=1`). Present pump healthy (7800–9600 presents/roll).
3. XEX image data loads correctly — the splash-name pointer table at `0x8291363C` verified
   byte-correct **naturally** (the iter-10 "loader gap" was the scribble bug, root-fixed
   iter-327 → **backfill band-aid retirable**).
4. On ~1/4 rolls the game runs its own splash sequencer end-to-end: `sub_822A8C68` →
   `sub_822B7F38` → movie-queue, 40 natural iterations. **No forcing involved.**

**Where it stops (the frontier):**
- The splash sequencer reads its movie name from the table at `0x8291363C` — and gets **NULL**.
  The table was verified correct at 03:15:58, but every natural read from 03:17:01 on saw
  zeros: **something zeroes this .data region mid-boot (~60 s after init)** and it stays zero.
  Empty name → FMV never opens → FMV SM parked at state 0 → sequencer loops forever.
- Same-region corruption also explains the historical "SM cell wiped on some rolls"
  (`0x829761E0`) and this run's `state=32767` garbage read (run3).
- No movie, no menu, `feFrames=0`, `DEF_EXEC=0`, `movInst=0` — honestly zero presentation.

**FIRST NATURAL GAP (named, real, platform-class):** a residual mid-boot scribbler zeroes
`.data` around `0x82913xxx` (iter-329's "scribble family's last breath"). NOT a missing
drive — a genuine bug to root-fix. Hunt in progress via the env-target write watchpoint
(`SR_WRITE_WATCH_VA=0x8291363C`, pure diagnostic, iter-343).

Also observed vs the forced era: `spinStorm` high on 2/4 rolls (RTAM hold dynamics changed
with the synthetic RTAM threads off), `init2=0`, `schedWork=0` — the init2/schedWork counters
were artifacts of the forced-on RTAM threads, not natural behavior.

---

*(Prior "progress" from the forced era is intentionally NOT logged here — it was measured with
forcing on and is not a natural baseline. History lives in PROGRESS.md / git.)*

---

## 2026-08-14 - iter-455m control roll (measurement correction, director-ruled wording)

On a forcing-free default-config run with ordinary host input forwarded through the guest input API, the guest traversed the storage-selector flow, rendered two storage-dialog layouts, requested `screens/hud/main_hud`, then entered sustained exact black.

Required qualifications (director-mandated):
- Roll-dependent, not deterministic.
- Storage UI is visually broken: placeholder text and malformed background geometry.
- The `main_hud` request is progression, not evidence that gameplay rendered.
- Terminal black remains the frontier.
- This does not advance pixel correctness.

Context: exe AC2942C652032C6DE6792041AF4AE94E01025E655C55975C3190652429E0C15E at commit 84cbd74 lineage (accepted 1ec7ab5 driver), scenario frontend_navigate_hardened.json (300A328A...), SR_WFFSM_OBS ABSENT (the one difference from the six gate-on misses). This entry is a MEASUREMENT CORRECTION - the capability existed in the current default binary; prior gate-on instrumentation runs did not measure it - not progress caused by a new platform fix. Evidence: archive/iter455m_control1{,_window} + ledger OUTCOME iter-455m-control1 (381bb3b).
