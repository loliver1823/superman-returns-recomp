# Playthrough: how far the game gets, start to finish (iter-465 playthrough lane)

THE GOAL says "playable start to finish". This page is the walk past the last verified point (the
Warworld -> Metropolis return after WARobj02, NATURAL_BASELINE 2026-10-04 physmem, and the
chapter-2 save, 3674dc00) to the end of the story, objective by objective, with evidence.

**How it is walked (dev-tool assisted).** The level-skip dev tool (docs/DEVTOOLS.md 3) runs a plan
made by `tools/devtools/skipplan.py --carrier auto` from a NEW GAME. A plan only writes the game's
own "objective won" stores (`pass`), a trigger-distance poke (`reach`) and, where the story needs
hero points the real game gets from metro events, `xp_to_cap` (CurrentXP := the chapter cap);
every load, transition, cinematic, chapter advance and objective script is the game's own code.
Real objectives get a ~60 s gameplay hold (`stable 1800`) before their pass. Results are tagged
**dev-tool assisted**: they show the game's code survives each step, not that a player can win
each fight. Port features ON = the player build (`SR_PORT_FEATURES=1`: custom front end,
autosave); natural = `SR_PORT_FEATURES=0,SR_MODS=0`.

## The story spine (from the game's own data)

Source: `data/devtools/objective_table_retail.tsv` / `objective_flows_retail.tsv` (baseworl.AST
trigger docs, docs/OBJECTIVE_TABLE.md). Chapter = the chapter the step is reached in. Static
model; the live rolls below are what checks it.

| # | idx | flow | kind | chapter |
|---|---|---|---|---|
| 1 | 147 | CinematicSupermanReturns | cinematic (new game) | 1 |
| 2 | 0 / 144 / 1 / 145 / 2 | MSTobj01-03 + transitions | objectives (meteor storm) | 1 |
| 3 | 143 | CinematicLoisAsks | cinematic | 1 |
| 4 | 5 / 142 / 6 | WARobj01, CinematicMongulAttacks, WARobj02 | Warworld arena (boss: Mongul) | 1-2 |
| 5 | 121 | CinematicLuthorSteals | cinematic, back in Metropolis | 2 |
| 6 | 135 | 3_4_Transition | chapter transition | 4 |
| 7 | 122 | CinematicBadNews | cinematic | 5 |
| 8 | 123 | CinematicLexsFirst | cinematic | 6 |
| 9 | 136 | 6_7_Transition | chapter transition | 7 |
| 10 | 141 | CinematicMetallo2Intro | cinematic | 8 |
| 11 | 34 | MTOobj09 | objective (boss: Metallo) | 8 |
| 12 | 124 | CinematicLexsPlot | cinematic | 9 |
| 13 | 137 | 10_11_Transition | chapter transition | 10 |
| 14 | 146 / 12 | BIZ03_Transition, BIZobj03 | objective (boss: Bizarro, sets ch 11/12) | 11-12 |
| 15 | 148 / 44 | BIZ15_Transition, BIZobj15 | objective (boss: Bizarro 2, sets ch 13) | 13 |
| 16 | 128 | CinematicLoisLasso | cinematic | 13 |
| 17 | 125 | CinematicPrison | cinematic | 14 |
| 18 | 138 / 139 / 140 | 11_12, 15_16, 16_17 transitions | chapter transitions | 15-17 |
| 19 | 129 | CinematicSupesDecides | cinematic | 18 |
| 20 | 22 | TWSobj04 | objective (boss: the Twister) | 18 |
| 21 | 126 | CinematicLexIsland | cinematic, last static step | 18-19 |
| - | 38 / 3 | LEXobj01 / LEXobj02-03 | the Lex island finale | needs bits 11 / 36 that no static writer sets |

Between story steps the planner uses `PlayableBIZ` (idx 67, the repeatable Bizarro rampage) as
the state-105 carrier for `xp_to_cap` (an assumption the rolls check). After 126 the static model
has nothing reachable: the finale (LEX objectives, CinematicSupesSaves 134) depends on bit 11,
which no Lua or trigger in baseworl/objtiv/metropol writes (OBJECTIVE_TABLE.md 3). What the game
does after 126 is observed live below.

## Results

**The story completes (dev-tool assisted, port features ON).** Roll **pf6** (dev exe
`7a3032393921-playthrough2` = c75a5ba6, saves empty, plan `tools/devtools/playthrough_plan.py --drop-pad`
on the `skipplan.py --target 126 --carrier auto` plan): NEW GAME -> every story step of the spine
-> **CinematicLexIsland won** (`levelskip done at k=86`, snapshot `S21_won_126`), the ending FMV
(Superman under water / in space, the robot montage, Metropolis at sunset), then free roam in the
sunny Metropolis with the post-game metro events ("FORCE OF NATURE, ENEMIES REMAINING 15") for the
180 s hold. One process, 31 minutes from launch to the ending, **0 `[ICALL_FAIL]`, 0 crash markers,
no hang**. Frames: `archive/v1_playthrough/rolls/pf6/drive/attempt1/frames` (ending = 198-228),
`rolls/pf6/sheet_end.png`. pf5 (the same spine, chapter numbers below) reached the Twister and was
stopped by the game's own GAME OVER during an idle hold (metro destroyed); pf6 locks the metro for
that hold only (`metro_lock on`, tool cheats).

**Natural twin: nat1** (port features OFF, no mods, the game's own front end, exe
`7a3032393921-playthrough2`): the same spine to **CinematicLexIsland won** and the ending FMV (Earth from
space, the island lifted away, "WHY THE WORLD NEEDS SUPERMAN", Metropolis at sunset), then free roam
with metro events. RESULT SUCCESS, 0 `[ICALL_FAIL]`, 0 crash markers. Frames
`rolls/nat1/drive/attempt1/frames` 162-190, `rolls/nat1/sheet_end.png`. NATURAL_BASELINE 2026-10-05.
pf6 caveat: another lane started two extra game processes at 00:13-00:15, over pf6's first minutes; its
boot-time timing is not evidence (the ending result is confirmed by nat1, run clean).

### Step 4 (2026-10-18): start to finish on the step-3 exe pair, on the ROG

The make2 recipe (`tools/devtools/chapter_seeds.py make --chapters 17`: NEW GAME through the custom front end, then
SKIP CHAPTER 17 times, every autosave captured) through the ROG wrapper (`Invoke-AllySet.ps1 -Tool chapter_seeds`),
graded by `tools/devtools/wg_report.py`, native_zero.py, run_health.py and terminal_snapshot.py. Dev-tool assisted.

- **Dev-default 976b35b97d7d, set `s4h_wg_dev1`** (`_worktrees/dev/archive/saves/_rolls/s4h_wg_dev1`): one process,
  13:26:25-13:59:43, sr_drive SUCCESS, 17 of 17 tickets done (the ending CinematicLexIsland in ticket 16, the post-game
  in 17), 17 valid saves (sha256 in `report/saves.json`), no crash / abort marker, 0 ICALL_FAIL / GPU_HANG /
  THREAD_FATAL, run_health HEALTHY, graceful quit with a complete terminal snapshot (stranded 0). **Terminal native
  meter: pm4 / edram_res / edram_xfer / edram_load / xma / dispreg / shm all 0**, nrec 73,409,540 over 46,622 frames
  (native_zero PASS). Contact sheets per chapter (`report/sheets/ch*.jpg`), all viewed: the world drawn in every
  chapter, from the tutorial spawn and the Warworld arena through the night chapters to the ending and the sunny
  post-game; black frames only at boot and at two cinematic cuts.
- Process commit (10 s samples beside the run): median 9,264 MB in chapter 1 rising to 9,663 MB in chapter 19, never
  falling between chapters (the monotonic-growth rule of FABLE_RULINGS 2: reported as a step-4 bug candidate; the
  growth sits at chapter loads, flat inside chapters; a same-slot reload test separates a leak from caches).
- **Release a75c1a1060da, set `s4h_wg_rel1`: FAILED in chapter 3.** NEW GAME and tickets 1-2 done, 3 valid saves,
  the world drawn in every frame up to +520 s (sheets viewed); at +533 s, 48 s into chapter 3 (free flight in
  Metropolis), the host crashed: `[CRASH] Exception 0xC0000005`, heap corruption detected in `RtlFreeHeap` on the
  XMA decoder thread while it re-created an FFmpeg codec context (`FfmpegXmaFrameDecoder::Prepare`, a stream's
  sample rate or channel count changed). The corrupted heap header was written by something else; the minidump has
  no heap memory, so the writer is not known yet. One occurrence; the dev-default run above passed the same route.
  Commit until then: 7.0-7.3 GB (the dev-default build: 9.3-9.6 GB at the same points). Evidence:
  `_worktrees/s4-hygiene/archive/step4_claude_20261018/hygiene/rel1_crash/` (dump, log, commit CSV) and the set's
  `report/`. A re-run is queued for when the ROG queue is empty.

## Blockers, ranked by how early they bite

1. **One host crash, release exe, step 4** (2026-10-18 `s4h_wg_rel1` above: heap corruption detected on the XMA
   decoder thread in chapter 3, writer not known yet, one occurrence). Before it: none that crashes or hangs the
   game between NEW GAME and the ending (pf6, nat1; 0 crash markers in 9 rolls, ~4 h of gameplay; the dev-default
   step-4 run 0 too). The known audio crash `[ICALL_FAIL] 823399D4` (D-AUDIO-CMDBUF, ~1 in 5
   gameplay rolls before) occurred **0 times in 9 rolls**; it has no root fix, so keep counting it.
2. Plan-tool issues found and fixed on the way (not game defects): a repeatable minigame (race, Bizarro
   rampage) restarts while Superman stands in its start circle, and an idle rampage FAILS ("ME NOT SAVE
   METROPOLIS"), which aborted pf1; an `xp_to_cap` inside a short cutscene never ran (pf4); an idle
   60 s hold in the Twister fight let the city be destroyed - the game's own GAME OVER (pf5).
3. **Unverified for a player:** winning each boss by playing (Mongul, Metallo, Bizarro x2, the Twister):
   the plan passes them after a hold. Needs played rolls (the owner's drive sessions or input tapes).
4. **No in-game credits roll:** the story ends with CinematicLexIsland (the last story video award, 25)
   and free roam; the game's credits screen is the front end's `screens/misc/ScrCredits`. Whether our
   custom front end still reaches it was not checked.
5. The finale objectives LEXobj01-03 depend on objective bits 11/36 that no script writes
   (docs/OBJECTIVE_TABLE.md 3); the live game confirms the story ends at CinematicLexIsland.

What "dev-tool assisted" leaves unproven: that a player can WIN each fight (the plan passes the
objectives after a ~60 s hold). What it proves: every load, transition, cutscene, chapter advance,
boss arena and the ending run on our build without a crash or hang.

| step | what | ch | reached | loads | played (hold) | evidence (pf5 unless named) |
|---|---|---|---|---|---|---|
| MSTobj01-03 | meteor storm | 1 | yes | yes | (verified before) | P0, S01, S02 |
| WARobj01/02 | Warworld arena, Mongul | 1-2 | yes | yes | yes | S03, S04; OBJECTIVE COMPLETED +11500 XP (pf1 frame 032) |
| CinematicLuthorSteals | FMV, back in Metropolis | 2 | yes | yes | n/a | pf1 frames 036-045 |
| 3_4_Transition | NEW CHAPTER "DARK METAL" | 4 | yes | yes | n/a | pf1 frame 061 |
| CinematicBadNews | cutscene | 5 | yes | yes | n/a | pf3 |
| CinematicLexsFirst | Lex FMV | 6 | yes | yes | n/a | pf3 frames 062-065 |
| 6_7_Transition | NEW CHAPTER cards ("INFERNAL MACHINES", "ROBOT ROUNDUP", "SOME DISASSEMBLY REQUIRED") | 5-7 | yes | yes | n/a | pf3 frames 059-068 |
| CinematicMetallo2Intro | Metallo intro | 8 | yes | yes | n/a | pf4 frames 080-083 |
| MTOobj09 | Metallo ("METALLO'S FINAL CORRUPTION") | 8 | yes | yes | yes, 60 s | S10; OBJECTIVE COMPLETED +10000 XP (pf4 frame 090) |
| CinematicLexsPlot | cutscene (Daily Planet) | 9 | yes | yes | n/a | pf4 frames 091-094 |
| 10_11 / BIZ03_Transition | chapter transitions | 10-11 | yes | yes | n/a | steps.jsonl k=44-50 |
| BIZobj03 | Bizarro | 11 | yes | yes | yes, 60 s | S13 |
| BIZ15_Transition, BIZobj15 | Bizarro (second) | 13 | yes | yes | yes, 60 s | S15 |
| CinematicLoisLasso, CinematicPrison | cutscenes | 13-14 | yes | yes | n/a | k=60-64 |
| 11_12 / 15_16 / 16_17 transitions | chapters 15-17 (Twister skybox loads) | 15-17 | yes | yes | n/a | S16-S19 (TwisterParentWorldFlow loaded) |
| CinematicSupesDecides | Lex, yacht, the kryptonite island FMV; NEW CHAPTER "GRUDGE MATCH" | 18 | yes | yes | n/a | frames 149-159 |
| TWSobj04 | the Twister | 18 | yes | yes | yes, 60 s (city destroyed while idle) | S20; OBJECTIVE COMPLETED +50000 XP (frame 168) |
| CinematicLexIsland | the ending: the island FMV (video 25), then the sunny skybox | 18-19 | **yes (pf6)** | yes | n/a | pf6 S21_won_126; frames 198-201 |
| post-game | free roam, metro events (ME, `chapter19HP`) | 19 | yes (pf6) | yes | 180 s idle | pf6 frames 202-228 |
| LEX finale | LEXobj01-03 | - | dead in the static data (bits 11/36 have no writer) | | | OBJECTIVE_TABLE.md 3 |

Side events used as hero-point carriers (not story): races FastFlyer 95/96/97 (12 completions in
pf5), Bizarro rampage 67 (pf1). Ending: the story's last award is video 25 in CinematicLexIsland;
no Lua state in baseworl/objtiv/metropol.AST names the credits except the intro cinematic's
(`[CIN]: Credits START`, in CinematicSupermanReturns); the credits screen is `screens/misc/ScrCredits`, reached from the front end).

## Roll log (newest last; dev exe snapshot 260e3ef95b21-playthrough = edf1e1ac, render thread on)

All rolls: `archive/v1_playthrough/rolls/<tag>/` (frames `drive/attempt1/frames`, log
`drive/attempt1/run.log`, plan events `devtools/steps.jsonl`). Port features ON, saves empty.

- **pf1** (plan carrier = PlayableBIZ 67): NEW GAME -> MSTobj01-03 -> Warworld (WARobj01/02,
  OBJECTIVE COMPLETED, power-up) -> CinematicLuthorSteals -> Metropolis -> Bizarro rampage
  (Mxyzptlk intro FMV) -> NEW CHAPTER "DARK METAL" (chapter 4). Stopped at k=21: the repeatable
  Bizarro rampage re-engaged by itself (the player is left standing on its statue) and failed
  ("ME NOT SAVE METROPOLIS THIS TIME"), so `abort_when objective_failed` ended the plan before
  CinematicBadNews. 0 `[ICALL_FAIL]`, no crash. A plan-tool problem, not a game defect.
- **pf2** (carrier = FastFlyer 95/96/97 races): the same re-engage with the race start ("TOO
  SLOW! TRY AGAIN" loop); plus a transform bug in the guard (fixed).
- **pf3** (FastFlyer + fly away after each carrier pass, harness input): chapters 4-8 ("DARK
  METAL", "INFERNAL MACHINES", "ROBOT ROUNDUP", "SOME DISASSEMBLY REQUIRED"), CinematicBadNews
  and CinematicLexsFirst (Lex FMV) played; stopped before MTOobj09 (Metallo) when the 5th race
  re-engaged during the NEW CHAPTER card (no control). 0 `[ICALL_FAIL]`, no crash.
  Races (FastFlyer 92-101) and the Bizarro rampage (67-71) are side minigames, not story steps.
- **pf4** (+ the race carriers fly away DURING the race, pass held 20 s; harness script
  `archive/v1_playthrough/carrier_move2.txt`): all 9 carriers clean; Metallo2Intro, MTOobj09 held
  60 s and won, CinematicLexsPlot played. Stopped: the plan put `xp_to_cap` inside the short
  LexsPlot cutscene whose guard never held (planner placement bug; moved before its reach).
- **pf5**: chapters 1-18, TWSobj04 won; the game's own GAME OVER (metro destroyed during the idle
  Twister hold) stopped it before CinematicLexIsland. 0 `[ICALL_FAIL]`, 0 crash markers.

## Dev Tools skip hub: live proof (owner request, iter-465)

LEVEL SKIP -> SKIP: <active mission> / SKIP CHAPTER / SIDE MISSIONS / STORY TARGETS (docs/DEVTOOLS.md
0b), driven from the in-game pause menu > MORE OPTIONS > DEV TOOLS with the pad, port features on,
after the Metropolis-return plan. Exe `d976a3da9315-skipmenu2` (commit 1e17793c + c75a5ba6).
- **menu2** (`archive/v1_playthrough/rolls/menu2`): SIDE MISSIONS -> RACE #95: the race "INDUSTRIAL
  HASTE" started and completed (`act=levelskip.pass args=95 FastFlyer`); the game restarted it
  (Superman on its start), and **SKIP: RACE #95** completed it (`skip_mission ticket=1 active idx=95
  flow=FastFlyer -> pass`). Frames: `rolls/menu2/sheet_menu.png`.
- **menu3** (`rolls/menu3`, RESULT SUCCESS): SIDE MISSIONS -> BIZARRO RAMPAGE #67 started (the Mxyzptlk
  intro FMV, Bizarro mode) and completed; **SKIP: BIZARRO RAMPAGE #67** completed the restarted
  rampage (`active idx=67 flow=PlayableBIZ`); **SKIP CHAPTER** (the rampage was active again):
  `xp_to_cap` 7500 -> 19000 (the chapter-3 requirement) + pass -> the chapter's NEW POWER UP card and
  the save prompt (answered by autosave), then Superman back in free roam. Frames:
  `rolls/menu3/sheet_menu.png`, `rolls/menu3/chapter_pair.png`.
- Known limit: a race or rampage restarts by itself while Superman stands in its start circle (the
  game's own trigger); the harness fly-away (`tools/scenarios/devtools_skip_menu_play.txt`) did not
  always get out in time. A player flies off; the hub can be pressed again.
- **pf6**: the whole spine to CinematicLexIsland and the ending; RESULT SUCCESS. 0 `[ICALL_FAIL]`.
- **nat1**: the natural twin, NEW GAME to the ending; RESULT SUCCESS, 0 `[ICALL_FAIL]`.
- Owner report 2026-10-05: "the level skip ... just loops through those races". What he saw were these
  rolls' hero-point carriers (races 95/96/97 between story steps) and the first SKIP CHAPTER (race #95
  when nothing was active). SKIP CHAPTER was rewritten (commit 10c9ca33) to advance through the game's own
  objective state and never start a race; proof roll `chap1` (chapter-2 save, 3x SKIP CHAPTER, then a
  chosen race skipped with SKIP).

## SKIP CHAPTER, rewritten after the owner's report (iter-465, commits 10c9ca33, 9bfd6a64)

Roll **chap4** (`archive/v1_playthrough/rolls/chap4`, exe `d37208c13435-skipchapter2`, port features on,
the save lane's chapter-2 save seeded read-only; RESULT SUCCESS): LOAD GAME -> METROPOLIS, then F11 three
times. Each press reported done with the chapter one higher and **no race started**:
- ch 2 -> 3: hero points capped, the game spawned metro event ME01Type01 (idx 256), reached + passed;
- ch 3 -> 4: ME12Type01 (257) passed; the game's NEW CHAPTER "DARK METAL" card;
- ch 4 -> 5: ME04Type01 (258) passed, then the story trigger CinematicBadNews reached (Lex cutscene).
Then SIDE MISSIONS -> RACE #95 (the player's choice) started and completed; the game restarted it
(Superman on its start), and **F10** (SKIP) passed exactly that race (`skip_mission ticket=4 active idx=95
flow=FastFlyer`). Contact sheet: `rolls/chap4/proof_sheet.png`. Still true: a race or rampage restarts
while Superman stands in its start circle (the game's own trigger; the sheet's last frame shows it).

**Lift after a minigame skip (roll `lift3`, exe `dfc357650e30-lift3`, commits d73bd185 + 5ebdee11):**
SIDE MISSIONS -> RACE #95 started (Mxyzptlk intro, "INDUSTRIAL HASTE"); F10 passed it
(`skip_mission ticket=1 active idx=95`); `act=levelskip.lift` moved Superman [6056, 68.9, 52] ->
[6101, 88.9, 52]; F10 again 20 s later: `refused ... why=nothing_active` - the race did not restart.
Sheet `rolls/lift3/lift_sheet.png`. (lift1 had passed falsely - its last step also accepted a restarted
race; lift2 showed the lift's argument vector still gated on the cheats tool.)

## Boss saves for played runs (iter-465)

Roll **boss1** (exe `dfc357650e30-lift3`, port features on, autosave on): NEW GAME, then SKIP CHAPTER (F11)
17 times -> chapter 19 in 34 minutes, every chapter advanced through metro events and the story objectives,
no race; 0 `[ICALL_FAIL]`. A watcher copied the save after each chapter. Staged for the owner at
`archive/player/boss_saves/{metallo,bizarro1,bizarro2,twister}/content` (README there): the last autosave
before each fight (Metallo: entering ch 8; Bizarro: ch 10; Bizarro 2: right after Bizarro 1, ch 12; the
Twister: entering ch 17). All four smoke-loaded in the background (LOAD GAME -> METROPOLIS -> HUD, 40 s):
`rolls/smoke_*`, sheet `archive/player/boss_saves/smoke_sheet.png`. Played (won-by-play) runs of the four
fights are the owner's drive sessions from these saves (input.tape = the evidence).
