# Saves: importing from Xenia and the save library (iter-463)

Tool: `tools/import_xenia_saves.py` (tests: `tools/test_import_xenia_saves.py`).
Host storage: `kernel/xam_content.cpp` (commit 5780fbb), ENGINE_MAP section 6i, and
CUSTOM_FRONTEND.md section 8.

This is debugging infrastructure. A save lets a roll start at a chosen chapter,
which makes it the forcing-free way to skip to a level. The game's own code loads
the save through its own LOAD path. The host only provides the files that a
console hard drive would hold. Nothing writes guest memory, and nothing calls guest
code on the game's behalf.

## Your steps (the user)

1. **Sign in to a profile in Xenia.** Your Xenia already has two profiles
   (`E03000007EB97E5D` and `E030000007AFB800`). Superman Returns has run under
   `E03000007EB97E5D`, but no save has been written yet.
2. **Play, and save at each chapter you want to be able to jump to.** The game keeps
   one save package, `Superman Returns`. To keep several chapters, snapshot after
   each save (step 3) before you play on.
3. **Snapshot it into the library** (the port does not need to be running):

   ```powershell
   python tools\import_xenia_saves.py library add ch03-warworld --note "saved at the Warworld gate"
   ```

   Or import it straight into your own port saves:

   ```powershell
   python tools\import_xenia_saves.py import            # to %LOCALAPPDATA%\SupermanRecomp\content
   python tools\import_xenia_saves.py import --dry-run  # preview only
   ```

   The Xenia folder defaults to your install
   (`R:\...\Xenia Canary\content`). Override it with `--src` or `SR_XENIA_CONTENT`.
   The profile is picked automatically when only one profile has Superman saves;
   otherwise pass `--src-xuid E03000007EB97E5D`.

## What the tool does

- It copies every package at `<xuid>/454107ED/00000001/<name>/` and its
  `<xuid>/454107ED/Headers/00000001/<name>.header` into the port's layout under
  profile `0009000000000001`, the port's signed-in user 0 (`kXamUser0Xuid`).
  Profile packages (`FFFE07D1/...`) are ignored.
- **It never overwrites.** A package or header that already exists stops the whole
  run (exit 2) unless you pass `--force`. With `--force`, the old copy is moved to
  `<dest>/.import_backup/<timestamp>/...` first. The port never enumerates that
  folder, because it is not a 16-hex-digit profile name.
- It skips, with a reason, any package the port cannot load:
  - an STFS container file, since the port mounts folders only;
  - the wrong content type or title;
  - a folder name that cannot round-trip through `XCONTENT_DATA`;
  - a header whose file name is not its folder's name;
  - an unrecognised header size.
- It warns when a display name is not `Superman Returns: The Videogame`, and when a
  package has no header at all. The game matches saves by that display name, so
  either one means the game will not see the save.
- `verify [ROOT]` checks any content root against the port's own header rules (size,
  NUL-terminated display name, file name equal to the folder name, type) and against
  the game's display-name match. With no ROOT it checks your player saves.

## Format compatibility: does a Xenia save load unchanged apart from the XUID folder?

Mostly. The package contents load unchanged, but a save written by current Xenia needs
its **`.header` rewritten**. Both sides were checked in source:
`kernel/xam_content.cpp` against `reference/xenia-canary-oracle` (Xenia Canary
0bd090d, 2026-09-20). Your Xenia build is from 2026-09-21, and its headers are
40960 bytes.

| Item | Xenia (current) | Port | Action |
|---|---|---|---|
| Folder layout | `<xuid>/<title>/<type>/<name>/` + `Headers/<type>/<name>.header` | the same (ResolvePackagePath / ComputeHeaderPath) | rename the xuid folder only |
| Package files | the game's own bytes, in a host folder (ContentPackageDirectory; CreatePackage always makes a directory) | the same | copied byte for byte |
| `.header` | the whole `XContentContainerHeader`: an STFS `CON ` header (0x344), `XContentMetadata` (0x93D6), and a Xenia-only `file_name[42]` at 0x971A, zero-padded to **0xA000**. The 0x971A form is also accepted. | **exactly 0x148 or 0x14C** (`readHeader`), `XCONTENT_AGGREGATE_DATA`: device @0, type @4, display name @8, file name @0x108, xuid @0x138, title @0x140 | **converted**. As-is, the port rejects it and lists the package under its folder name, `Superman Returns`, which the game never matches. |
| Display name | `metadata.display_name[English]` @0x411, else the first filled language (GetContentMetadata) | @0x08, must NUL-terminate within 128 wchars | taken the way Xenia reads it |
| File name | `file_name_raw` @0x971A, else the folder name | @0x108, must equal the folder name | the same rule, checked |
| Device | GetContentMetadata sets 1 | enumeration hands header byte 0 to the guest, and the game refuses device 0 (sub_828A8170 returns 87) | written as 1 (HDD, which the port's device selector also returns) |
| Title | `execution_info.title_id` @0x360 | @0x140 | must be 454107ED (checked), written as 454107ED |
| xuid | `profile_id` @0x371 (Xenia's profile) | @0x138, inert: the port writes 0 for the game's own saves (readXcd), enumeration copies only the first 0x134 bytes to the guest, and path resolution uses the signed-in profile | written as **0** |
| Licence | OR of `license_bits` where `license_flags` != 0 | optional licence word at 0x148 (0x14C file) | kept when nonzero (saves normally have none) |
| Thumbnail | inside the header (@0x171A, and by default the SPA title icon) | `__thumbnail.png` in the package, written only by XamContentSetThumbnail | not extracted by default (the game never reads it back); use `--thumbnail` to extract it |

A header already in the old 0x148 / 0x14C form (older Xenia, or a port-made
snapshot) is copied byte for byte.

**Inside the save data, nothing XUID-bound was found.**
- XamUserGetXUID has two callers: `sub_82467EF0`, a profile-object getter reached
  through the vtable at 0x820C82B8 and from RefreshSignedInUser, and the logon path
  `sub_8246E248`. Neither is on the save path in ENGINE_MAP 6i.
- All five record writers were checked (iter-463, IDA): options
  (`SaveRecords_WriteOptions_sub_82165088`: music, SFX, and scheme / X / Y / vibration per
  **input slot**), flag&4 stats (`SaveRecords_WriteStats_sub_82165198`, keyed by stat
  name), entity flag byte +35 (`SaveRecords_WriteEntityFlags_sub_82165250`: the 29
  power-up upgrades, keyed by name), Metropolis health
  (`SaveRecords_WriteMetroHealth_sub_82186F70`) and the BaseWorldFlow persistent
  attributes (`SaveRecords_WriteBwfDas_sub_82181E28`: objective bits, chapter, weights,
  unlocks, keyed by attribute name). None reads a profile id or XUID, and none is keyed
  by one. The rest of the file (MCM2 header, slot descriptors) carries sizes, CRCs,
  progress, play time and chapter only. So **no record is XUID-bound**: a save can move
  between profiles. See "Save file format" below.
- XamContentGetCreator reports the signed-in user as the creator of an existing
  package, as Xenia does for saved games.

**The port's saves load in Xenia (observed, 2026-10-17/18).** Lane H loaded the chapter-18
seed and lane K a chapter-19 save in stock Xenia Canary (`_worktrees/postgame/archive/
m2_claude_20261017/K/XENIA_NOTES.md` "Setup": a private `--content_root` holding lane H's
profile tree `E03000007EB97E5D` plus the save, `--logged_profile_slot_0_xuid=E03000007EB97E5D`);
step-4 lane A then cold-loaded all 18 seeds of the library there (17 chapter seeds +
post_warworld; LOAD A GAME? -> LOAD GAME -> slot 0): 18 of 18 reached the guest's main_hud
request (docs/WHOLE_GAME_FIDELITY.md "The Xenia half"). So a save written by the port, moved to
another profile folder, loads unchanged in retail code, as the static reading above says.
**Still not confirmed by a roll:** the other direction, a save written by Xenia loading in the
port (the owner's Xenia has no Superman save; none has been made there yet).

## Save file format (the `Superman Returns` file inside the package)

Decoder: `tools/save_records.py` (tests: `tools/test_save_records.py`). Every fact below is
from IDA on the retail XEX. Real saves decode since 2026-10-14: every capture of the chapter
seed library (below) is decoded by this tool (`chapter_seeds.py` `describe()`: the newest
slot's chapter, objectives won, play time, the container CRCs and the slot checksum all
checked). (Before that no real save existed: the content rolls were empty, the user's Xenia
has no Superman save, and in both sound goldens the save manager had never saved.)

```
python tools/save_records.py decode  <file or content root>    # named fields, JSON
python tools/save_records.py verify  <file or content root>    # CRCs, slot checksums, keys
python tools/save_records.py diff A B                          # semantic diff, stale bytes ignored
python tools/save_records.py roundtrip <file>                  # decode -> encode -> identical?
python tools/save_records.py from-image xenia:sound_gameplay   # what the game would write now
python tools/save_records.py schema --image xenia:sound_gameplay   # regenerate data/saves/...
```

- **Container** (writer `SaveMcm2_BeginWrite_sub_8263EEB0`, reader
  `SaveMcm2_ReadEvents_sub_8263F8D8`): 28-byte `MCM2` header {magic `'MC02'` (0x4D433032;
  iter-465, read off the first real save - the iter-463 text said 'MCM2'), total, len1 = 88,
  len2 = 96 x count, CRC(part1), CRC(part2), CRC(header bytes 0-23)}, then part1, then
  part2. The CRC (`SaveMcm2_Crc32Msb_sub_826424F8`) is MSB-first CRC-32, poly 0x04C11DB7,
  seeded with the complement of the first 4 data bytes. Retail size: 28 + 88 + 96 x 207 =
  **19988 bytes**.
- **Part1** (88 bytes) = save manager `mgr+8..mgr+96` (`mgr = *0x8296D8D4`): 16 zero bytes,
  then three 24-byte slot descriptors {f32 progress (-1.0 = empty), 4 unwritten, f64
  TotalPlayTime, u32 currentHeroPointChapter, 4 unwritten}.
- **Part2** = six regions of `count` 16-byte records: regions 0-2 are the three slots as
  saved, regions 3-5 are each slot's **chapter-start checkpoint** (a copy of the
  AutoSaveGame capture), which Retry restores through `LoadAutoSaveGame`.
  `count = 4 x pads + 3 + DAS + entities + stats + 1` (`SaveMgr_RecordCount_sub_821770E0`);
  retail: 4 pads, 41 stats, 29 entities, 117 DAS attributes = **207** (names and order:
  `data/saves/save_schema_retail.json`, read from both sound goldens, identical; the
  capture buffer in the golden dump ends exactly at record 207).
- **Record** {u32 key, 4 bytes, 8-byte value}: header (+8 slot checksum), music, SFX, per
  pad scheme / X / Y / vibration, stats (key = Java string hash of the name, f64), entities
  (Java hash, u8), Metropolis health (u32, floored at `MetroHealthSaveThreshold` x max), DAS
  (key = FNV-1 of the attribute name, u32 or f32). `objectiveStatus4` bits 13/18/20 (objectives
  141/146/148) are overwritten with the status of 34/12/44 on save
  (`PackObjectiveStatusBits_sub_82165638`).
- **Stale bytes.** Only dword 0 of each record is zeroed when a buffer is allocated. Every
  byte a writer does not write (record +4, the tail of u8/u32 values, the stack-garbage low
  word of DAS values and of the header record) is **leftover heap or stack memory**, and it
  is saved to disk. Two saves of the same state differ there. `diff` and `semantic_bytes`
  ignore these bytes; any byte-level acceptance test (critique B4) must too.
- **Slot checksum** (`SaveMgr_SlotChecksum_sub_821773E8`): the sum of each record's u32 at
  +8 shifted left by 8 x ((index & 3) + 1), over the live and checkpoint regions. The shift
  of 32 yields 0 (PowerPC `slw`), so **records whose index is 3 mod 4 are not covered**, and
  the shifts drop the top 1-3 bytes of the others. The file CRCs cover every byte.

**First real save (iter-465, natural roll `archive/v1_save/rolls/n1`).** Retail FE, port
features off, MSTobj03's SAVE GAME BEFORE CONTINUING? -> slot 0 -> SAVE SUCCESSFUL: 19988 bytes,
all CRCs and the slot checksum right; `verify` OK after two tool fixes (magic `MC02`; a slot
saved before the first chapter advance has an all-zero-key checkpoint region - no AutoSaveGame
capture yet - which is the game's state). Decode: chapter 1, objectives won {0,1,2,144,145,147}.
The chapter autosave (port feature, `rolls/p4`) decodes as chapter 2 with 5 and 6 won.

**Verification status.**
- **Tests:** `python tools/test_save_records.py` runs 25 tests.
  - The save bytes are produced by an independent model of the five writers.
  - The CRC and the slot checksum are re-implemented from the PowerPC disassembly.
  - The CRC table is compared with the retail image.
  - Four mutants are killed: CRC seed, checkpoint omitted from the checksum, checksum shift
    off by one, wrong key hash.
- **Round trip** (decode, then encode, is byte-identical): proven on synthetic data only.
- **Schema:** read the same way from both sound goldens; the two are identical.
- **Layout (protocol step 3), corrected by the independent verifier (iter-463):** score 1.0
  in `match.tsv` does not mean identical code; "propagated" pairs only need body similarity
  >= 0.55. An address-normalized comparison (the matcher's own n1 tokens: struct offsets and
  immediates kept) gives:
  - identical: the record count, the MCM2 writer, the path/DAS lookups, the pack function;
  - the five record writers, the checksum, the capture and `ChapterProgressEntryFill` differ
    from dev ONLY in save-manager offsets: record cursor dev +0x50 / retail +0x68, slot
    buffer dev +0x4C / retail +0x60, capture buffer dev +0x48 / retail +0x0, slot
    descriptors dev +0x0 / retail +0x18. The object fields they read (stat +0x1C/+0x22,
    entity +8/+0x23, metro +4/+8, DAS node +0/+4/+8) are identical;
  - **the dev container is NOT the retail format**: the dev part-1 accessor returns mgr+0
    (retail mgr+8) and its length function returns 0x48 = 72 (retail 88), so a dev save is
    28 + 72 + 96 x count(dev) bytes with no 16-byte reserved block. `save_records.py`
    rejects it (`len1_ok` false) rather than misreading it.
- **Semantic compare fixed (iter-463 verifier):** `semantic_bytes` used to keep the CRC words
  (+0x10..+0x17). The CRCs cover the stale bytes, so two saves of the same state always
  differed there once the container was rebuilt the way the game rebuilds it. It now keeps
  only magic and sizes; `test_stale_byte_invisible_after_the_game_rebuilds_the_container`
  fails on the old code.
- **Open:**
  - decode a real save (the port's own, once a roll saves; or a Xenia one);
  - a byte-exact round trip on that save;
  - a cross-check of each field against the golden objstate reader.

## Save library (test rolls from a chosen save)

A snapshot is a complete content root:
`archive/saves_library/<label>/0009000000000001/454107ED/...`, plus `SNAPSHOT.json`,
which records the source, time, note, and per-file sha256. The library lives in
`archive/`, which is gitignored local evidence. Set `SR_SAVES_LIBRARY` or pass
`--library` to use another folder.

```powershell
python tools\import_xenia_saves.py library list
python tools\import_xenia_saves.py library path ch03-warworld      # prints the absolute path
.\tools\run_recomp_x.ps1 -K 1 -TimeoutSeconds 200 -Gates "SR_CONTENT_DIR=<that path>"
.\tools\sr_drive.ps1 ... -Gates "SR_CONTENT_DIR=<that path>"
```

`run_recomp_x.ps1` and `sr_drive.ps1` already treat `SR_CONTENT_DIR` as a **seed**.
They copy it into a fresh per-roll root (`archive\content_rolls\<stamp>\run<i>`, or the
attempt's `content\`) and never write the snapshot. The roll's save state afterwards
stays next to its log. To snapshot a roll's end state, point `--src` at that per-roll
root. Its 0x148 headers copy byte for byte:
`library add after-ch3 --src archive\content_rolls\<stamp>\run0`.

**Reporting rule.** A save-seeded roll runs zero forcing, but it does not start from
boot. Tag its result **natural, save-seeded (`<label>`)**. It never moves the
from-boot number in NATURAL_BASELINE.md, and a NATURAL_BASELINE / PROGRESS entry
built on one names the snapshot, so the repro command includes it.

## Chapter seed library (2026-10-14, `tools/devtools/chapter_seeds.py`)

Save seeds for every chapter, so a live proof (audio, GPU queries, perf, the flicker gate) can start anywhere in
the story with `perf_run.py --from-save` / `flicker_gate.py --from-save`. Seeds live in the dev checkout's
`archive/saves/<nn>_<name>/` (local, untracked): each is a content root exactly as the game wrote it
(`0009000000000001/...` + `settings/`), plus `README.txt` (objective, how made, exe hash, date, the `.devtool`
tag, what is on screen when loaded, load checks), `seed.json` (the same, machine-readable) and `screenshot.png`
(the last gameplay frame of the load check).

**How the game is made to save.** The level skip's plans answer the save prompt Continue Without Saving (the
plan's `pad DOWN A`, and the autosave port feature declines while a plan is armed or running: `SRModOptions`
bit 10). SKIP CHAPTER (F11, `skip_chapter`) is not a plan but a tick-side controller, so with port features ON
the "Autosave without asking" feature (CUSTOM_FRONTEND.md 5.6) saves at every chapter advance through the game's
own code: BaseWorldFlow 105's `PromptUserSaveGame` -> Save Game -> the current slot -> SAVE SUCCESSFUL -> OK. It
also answers MSTobj03's own "SAVE GAME BEFORE CONTINUING?" with Save Game, which gives a chapter-1 seed before
Warworld. No code change was needed.

```powershell
# 1. one roll from NEW GAME: 17 x SKIP CHAPTER = chapter 1 -> 19 (~40 min); every save write is captured
python tools\devtools\chapter_seeds.py make --exe <exe> --out <dev>\archive\saves\_rolls\<tag> --chapters 17 `
    --root <dev> --slot-owner <owner>
python tools\devtools\chapter_seeds.py captures <dev>\archive\saves\_rolls\<tag>
# 2. a capture -> a seed folder
python tools\devtools\chapter_seeds.py seed --capture <roll>\captures\03_ch02 --dest <dev>\archive\saves\02_ch02_metropolis `
    --title "..." --objective "..." --exe <exe>
# 3. each seed loads (perf_run --from-save, one short roll per seed, the slot taken per roll)
python tools\devtools\chapter_seeds.py verify --seed "<dev>\archive\saves\[0-9]*" --exe <exe> --root <dev> `
    --stage J:\sr_stage --slot-owner <owner>
python tools\devtools\chapter_seeds.py note --seed <seed> --on-screen "..."   # after looking at screenshot.png
```

`make` cannot be staged on J: (`stage_roll.py` refuses dev-tool rolls: the dev tools read and write under the
content root), so it runs from a checkout holding `Superman Returns (USA)` (`--root`); `verify` can be staged.
Every seed carries the `.devtool` marker and loading it tags the run `devtool-derived-save`: fine for renderer /
audio / query proofs, never a natural-play claim.

**What passes (2026-10-15, Astra round 1 on tools/chapter-saves).**
- *Containment.* Every path the tool writes or deletes (`make --out`, `seed --dest` and its `--force`
  replacement, `verify --rolls` (default `<SR_MAIN_ROOT>/archive/saves/_verify`), the seed's `seed.json` /
  `README.txt` / `screenshot.png` / `check_roll_*.txt`, `judge --update`, `note`) must resolve, junctions and
  symlinks followed, to a folder strictly inside `<SR_MAIN_ROOT>/archive/saves`. The library root itself and
  anything outside are refused before anything is written or deleted. `--force` replaces only a folder holding
  `seed.json` and no link or junction.
- *Captures.* A capture is valid only when the copied save decodes with its container CRCs and slot checksum
  intact and the `.devtool` marker is beside it (`capture.json` `valid` / `problems`). `seed` refuses an invalid
  capture, and removes a copy that fails the same checks.
- *make* keeps every capture but exits 1 (`make.json` `complete: false`, `problems`) when sr_drive fails, a crash
  or abort marker is in the log, a requested SKIP CHAPTER ticket did not report `done`, or any capture is invalid.
- *Load checks.* `verify` and `judge` pass a roll only when the drive attempt whose log they read (the highest
  `attempt<N>`) ended SUCCESS and no crash or abort marker is in that log. The markers are
  `scripts/crash_markers.py` FATAL_MARKERS, tools/perf's abort markers (`boot_pairs.ABORT_MARKERS`) and
  `[PHYS_CAP]`. The roll must also carry the `[RUN_CLASS] devtool-derived-save` tag. After the load event it needs
  a landing (carousel or HUD), the HUD, then the gameplay context. After that context, `[HIGHFPS_PACE]` windows
  must show the guest sim advancing for at least 20 s, and it must still be advancing within 20 s of the gameplay
  dwell's end. A dead verify tape (`play_failed.txt`) or a non-zero perf_run exit fails the check, and `verify`
  exits 1 when any seed fails. Re-graded with these rules: seeds 01 (HUD landing) and 03 (carousel) pass. The
  other 15 load checks below were graded by the earlier rules; `chapter_seeds.py judge <roll>` re-grades them.
- *The drive* (`perf_run --from-save`, every from-save roll): the landing counts only after the slot's load press
  (sr_drive `expectSince`), and the capture window opens only after the gameplay context (`in-game`) and a fresh
  window of advancing sim ticks (`sim-runs`). A frozen game behind a surviving window now fails the drive.

**The library (2026-10-14, exe 2087ca4acdb4 = dev d71946e2; roll `archive/saves/_rolls/make2_ch1-19`).** One roll
from NEW GAME, 17 x SKIP CHAPTER, 36 min to the post-game, 17 saves, 0 crash markers. The game saves once per
chapter advance plus MSTobj03's prompt. Chapters 11 and 13 have no save of their own: BIZobj03 advances 10 -> 11
-> 12 in one go, and BIZobj15's chapter 13 runs straight into the CinematicLoisLasso / CinematicPrison advance to
14. Chapter 1's start is NEW GAME itself (no save before MSTobj01).

| seed | chapter | the save | lands | next in the story |
|---|---|---|---|---|
| `01_ch01_after_mstobj03` | 1 | MSTobj03's own prompt | HUD | 143 CinematicLoisAsks (2 min FMV), then Warworld |
| `02_ch02_warworld_start` | 2 | the chapter-2 advance | HUD (the arena) | 5 WARobj01, 142, 6 WARobj02 |
| `03_ch03_metropolis_after_warworld` | 3 | after Warworld, 121, ME 256 | carousel | metro events, 135 3_4_Transition |
| `04_ch04_dark_metal` | 4 | NEW CHAPTER "DARK METAL" | carousel | 122 CinematicBadNews |
| `05_ch05_after_bad_news` | 5 | after 122 | carousel | 123 CinematicLexsFirst |
| `06_ch06_after_lexs_first` | 6 | after 123 | carousel | 136 6_7_Transition |
| `07_ch07_robot_chapters` | 7 | after 136 | carousel | 141 CinematicMetallo2Intro |
| `08_ch08_before_metallo` | 8 | after ME 261 | carousel | 141, 34 MTOobj09 (Metallo) |
| `09_ch09_after_metallo` | 9 | after 34, 124 | carousel | 137 10_11_Transition |
| `10_ch10_before_bizarro` | 10 | after 137 | carousel | 146, 12 BIZobj03 (Bizarro) |
| `12_ch12_after_bizarro1` | 12 | after 12 BIZobj03 | carousel | 148, 44 BIZobj15 (Bizarro 2) |
| `14_ch14_after_prison` | 14 | after 44, 128, 125 | carousel | 138 11_12_Transition |
| `15_ch15` / `16_ch16` / `17_ch17` | 15-17 | after 138 / 139 / 140 | carousel | 139 / 140 / 129 CinematicSupesDecides |
| `18_ch18_before_twister` | 18 | after 129 | carousel | 22 TWSobj04 (the Twister), 126 CinematicLexIsland |
| `19_ch19_post_game` | 19 | after 22, 126 (the ending) | carousel | post-game free roam, sunny skybox |

Each seed's README has the load check (perf_run --from-save on the exe, a fly-around tape, the GPU query and
audio-native counters after the load) and `check_roll_<seed>.txt` (`check_roll.py --grade` of that roll).
**Load checks (2026-10-14/15, exe 2087ca4acdb4 staged on J:, rolls `archive/saves/_verify/`): 17 of 17 load and
reach gameplay, perf_run's drive SUCCESS for all** (01 and 02 since the HUD landing), 0 crash markers. Audio N6
after the load, every seed: `[AUDIO_NATIVE]` guest_mmio_fns 0 and xma_regfile_writes 0, `[NATIVE_METER]` xma max 0
over 59-119 two-second windows; check_roll's "audio native census" and "D-AUDIO-CMDBUF witness" PASS in all 17.
GPU queries after the load (the tape flies ~45 s): zpd +30 (12, 16), +24 (06), +10 (03, 14), +2 (07, 10, 17, 18),
0 elsewhere; ext 0 everywhere. Seed 08 plays the Metallo intro cinematic right after METROPOLIS; chapters 14-18
load under the darker overcast sky.

**GPU occlusion queries (p2.zpd).** The make rolls are the first rolls that counted ZPD queries
(`[GPU_CP] ... zpd=`; ext stayed 0). In make2 the counter rose during the metro events of chapters 3-7, Metallo
(+12), Bizarro 1 (+12) and the Twister and post-game (+30), to 92 in all; MSTobj03's mega-meteor fired +2 in make1
and 0 in make2. The only retail query sites are the sun flare (`OpticalFX_DrawOcclusionFadedFlare_sub_82721118`)
and the HUD glow of a threat marker on screen beyond its type's distance (`HudGlowOcclusionQuery_sub_82304058`;
200 units for bosses, robots, meteors / the Twister, fires, blimps, objective starts and metro events, 5 for
citizens and ambulances: the HUD object `[0x82970174]+0xC44+8*type`). **The sun flare cannot fire in retail:**
`VfxDeferredList_DrawAndClear_sub_828DA650` calls 82721118 only for an effect definition whose first word is 2
(an "optical" geoEffect); the only geoEffect list (baseworl `GEOFX_DEFINITIONS`) defines 7, all contrails, and no
other AST holds one. Live, the VFX manager (`*0x829AAE48`, 192 20-byte slots at +0x3C, `VfxMgrRegisterEffectDef_sub_82798D38`)
held 105 definitions in chapter 10 of make2: 98 particle (slot type 0) and 7 geo (slot type 2), every geo
definition's first word 0 (contrail), none optical; 109 (7 geo, all contrails) on seed 19 in the post-game daylight,
where Frida counted 0 calls of 82721118 against 4812 of its caller 828DA650 in 81 s of play
(`archive/saves/19_ch19_post_game/flare_check.txt`). So every query a proof can provoke is the HUD glow: fly toward
a threat (a metro event's robots, a boss, the Twister) and keep its marker on screen from more than 200 units.

## Tests

`python tools/test_import_xenia_saves.py`: 24 tests, 0 failed.
- The Xenia structures are rebuilt independently with ctypes, field by field from
  `xcontent.h`. Every offset the tool hard-codes is checked against them, with Free60
  STFS offsets as a third observer.
- The synthetic Xenia trees are built through those structures, not through the tool.
- A converted header is checked to be byte-identical to what the port's
  `writeHeaderLocked` stores for the game's own save.
- The tests cover:
  - overwrite refusal and backup, and dry runs;
  - skipped packages;
  - licence flags;
  - XUID auto-detection;
  - the user's current tree, which has profiles but no Superman saves;
  - a library round trip.
- **Mutation-validated.** Each of these tool mutations fails the suite:
  - a display-name, title, or file-size offset;
  - xuid written nonzero;
  - the overwrite guard removed;
  - the port-size rule removed;
  - licence bits counted without flags;
  - a legacy header rebuilt instead of copied.
- One mutant survives, and it is equivalent: Xenia's `display_name(kInvalid)`
  slot-1 quirk. The fallback runs only when slot 0 is empty, so the quirk is never
  observable.

Tool output against the real Xenia tree (read-only dry run):
`archive/v1_devtools/import_xenia_saves_check.txt`.
