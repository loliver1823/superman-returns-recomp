# Mods and port features (iter-463 overlay; iter-464 modcore: drop-in mods, the engine's loose-file overrides, port features as patches)

## Current UI direction (owner clarification, 2026-10-02)

**Built into the product:** the Mods menu entry and mod framework ship with the game,
alongside the custom frontend. Players do not install a separate menu/framework mod
or set development environment variables. Drop-in packs are optional content managed
by that built-in UI. Developer opt-outs remain only for unmodified-game diagnostics;
the current lane's opt-in gates are staging controls, not the shipping design.

Current lane checkpoint: the framework itself now defaults on, with
`SR_MOD_MANAGER=0` retained for developer A/B checks. Game builds copy the built-in
recipes next to the executable even after recipe-only edits, and snapshots pin
and carry those recipes and licence notices. The directory fix now defaults on
after the natural A/B in section 12; the polished browser and native costume-screen
integration remain open. These changes are not yet promoted to the main checkout.

**iter-465 (modlib lane): the mod UI is now the MOD LIBRARY** ("The mod library" below): a
full-screen in-game organiser drawn by our D3D12 backend, opened from the same MAIN MENU > MODS
and pause > MORE OPTIONS > MODS rows. The three-row Apt popup and its restart-to-wear costume
page are RETIRED (reachable only with the developer gate `SR_MOD_LIBRARY=0`).

The mod screen manages **which content is available**; the game's existing costume
screen manages **which suit is worn**. These are different choices:

1. **Installed library:** a full-size, controller-friendly screen with thumbnail cards,
   category filters and a selected-mod details panel. Categories include Skins & Characters,
   Missions, Worlds, Gameplay, Visuals, Audio, Interface and Other. Show name, author,
   version, description, enabled/disabled state, what it adds, dependencies and conflicts.
   Missing thumbnails use an intentional category placeholder, not a broken image.
2. **Pack details:** enable multiple independent packs. A pack containing several suits
   allows multiple suits to be included together; these are checkboxes, not a radio-button
   "wear this suit" choice. Persist inclusion by stable `mod-id/local-suit-id`, never a
   boot-local numeric slot. Disabling another pack must not change the saved identities.
   `mods/mods.json` stores unchecked suits in `disabled_costumes`, an array of stable
   keys. Missing/older files include every declared suit. Valid unchecked keys from
   temporarily absent packs are retained. Changes apply at the next asset mount;
   the current boot's registry and numeric slots remain immutable. The pack master
   switch is independent of its suit checkboxes.
3. **Apply content:** install/enable changes may require a restart to mount assets. Wearing
   an already installed suit must not be presented as a restart-only mod-menu operation.
4. **Native suit selection:** append the included mod suits to `screens/misc/ScrCostumes`.
   Retain the retail entries, their order, lock/award behavior, thumbnails and selection
   behavior. Preserve the fourth retail DAS/cheat costume too; it is not one of the three
   ordinary retail picker rows. A missing/disabled mod suit falls back safely without
   overwriting a retail model or silently selecting another mod's reassigned slot.
5. **Discovery:** provide an explicitly clicked Nexus Mods link and optional per-mod Nexus
   page links. Installed mods and local thumbnails work offline. Do not invent a Superman
   Returns Nexus game page, scrape a catalogue, embed account secrets, auto-open a browser
   on boot or offer a non-working download/install button. A later authenticated catalogue
   needs a registered application, compliant credentials and a separately verified download
   workflow. Linking does not authorize publishing assets converted from other games.

Visual reference: Bethesda's [Creations library/install workflow](https://help.bethesda.net/app/answers/detail/a_id/33413/~/how-do-i-install-and-play-a-fallout-4-creation%3F)
and its [thumbnail-card UI](https://help.bethesda.net/app/answers/detail/a_id/54457/~/how-do-i-download-the-included-creations-in-the-anniversary-edition).
Use the browsable library/details structure with Superman's own visual style, not copied
Bethesda artwork. Nexus integration requirements: [API acceptable-use policy](https://help.nexusmods.com/article/114-api-acceptable-use-policy).

**Verified integration constraint:** `RecordAdapter.GetCostumeData` (`822AF910`) constructs
exactly three AS objects; `SetActiveCostume` (`822B29E0`) accepts only indices below three.
Adding DAS attributes alone does not extend that UI. `ScrCostumes` already iterates the
returned array length. The additive extension must preserve/delegate the original API path
and provide mod-index selection through a naturally invoked game/AS API, never a host tick
that writes guest state or forces callbacks. Native-picker runtime acceptance is still open.

The browser metadata foundation accepts optional `category` in `mod.json`: `characters`,
`missions`, `worlds`, `gameplay`, `visuals`, `audio`, `interface`, `other` (case-insensitive).
Without it, costume packs infer `characters`; other/legacy mods use `other`. The existing
local `preview` image and category are carried into the menu's data model. This does **not**
mean the old popup renders thumbnail cards yet. Native suit thumbnails need Apt movie
packages; raw `preview.png` is not a verified `ThumbViewer.loadMovie` input.

### Implementation checkpoint (2026-10-02)

- Category/preview data-model foundation: native manager **95 passed, 0 failed**;
  Python/native content differential **30 passed, 0 failed**. Receipts in the mods/ship
  lane's `archive/test_20261002_library_{manager,python}.log`.
- The old popup picker was visually exercised (14-step smoke), but is superseded by the
  owner's library/native-picker requirement above. That smoke is not final UI acceptance.
- Private startup probe F reached base FSM index 3, UI run state 3, slot-switch mode 1,
  registered HUD and an active/unpaused actor FSM. Probe G's bounded read-only command
  observer saw the custom entity requested with actor ID **-1**; retail Lua cleared the
  request before an actor existed. A generated actor-readiness guard now preserves the
  request until a valid ID. Regression R28 was shown red before the fix; the fix and
  native/Python composition wiring are green, but its live asset/cloth acceptance is open.
- Loose Lua must **not** end with NUL: FSMA passes the file length to `lua_loadbuffer`.
  Loose XML/DAS uses a different reader and requires its terminator. Probe C caught this;
  D/F restored Lua loading after removing it. Do not conflate the formats.
- The complete forcing audit receipt `archive/audit_forcing_20261002_costume_lua.log`
  exits 1 for two inherited `gpu_ring_alloc_fix.cpp` sites (lines 816/835). The host-thread
  audit passes with no open launch forcing, but this is **not** a clean full forcing audit
  or a new natural-baseline acceptance. Browser/skin work remains isolated in mods/ship.

What the game reads is composed from up to three **layers** in the host file layer. The game
folder is never written and no big file is copied.

```
  disc            Superman Returns (USA)/                          (read-only)
  + port features built in; patches applied to the player's disc   (zero game bytes shipped)
  + mods          what the player dropped into mods/, in load order (top of the list wins)
```

**How content reaches the game: the engine's own override path** (design: the owner's private mod project
`research/modsystem/DESIGN.md`, approach F, scored 9/10; credits in CREDITS.md). On every boot
`MountLocalizedBigFiles_sub_8218D128` mounts a loose-file storage over `d:\DATA\_workinprogress`
at priority 0x7D0 (2000) - above `_workinp.AST` (1000), flow bundles and streams (60) and the
disc catalogs (50) - scanned recursively (`*.*`, then `__ASSET__*.*`, then subfolders) by
`FileStorageScanDirRecursive_sub_826EB970`. Each file is keyed by its extension's type (29
types, below) and by its **stem** (FNV-1a-64 of the upper-cased name before the last `.`; or
the 16 hex digits after `__ASSET__`), and the resolver (`sub_826F8838`) returns the FIRST
storage holding a name. So a loose `DATA\_workinprogress\<mod>\ScrPressStart.ast` overrides the
ScrPressStart movie inside `ui.AST`, one asset at a time, and two mods that override different
assets both apply.

Registered extensions (table 0x82911D00): `dds wmv fxh fx rss rml spk sns sgp sbm bmp pus map loc
png lef igc bnk rcb txt ter tga xml das rx2 hkx lua ast vxd`. Not registered: `.apt`/`.const`
(ship the whole movie `.ast`), `.ini`, anything else (ignored by the engine; `srmods lint` warns).
Payloads are stored decompressed (no RefPack); text XML is accepted where the compiled-XML magic
is absent (the owner's private mod project `research/bxml_text_path.md`). Docs the game requests before the
`_workinprogress` mount (gameconfig) cannot be overridden this way.

**Prerequisite (platform fix, iter-464):** the host's `NtQueryDirectoryFile` must answer the
way Xenia does - one record per call, the mask honoured (`SR_NTQDIR_XENIA`, section 12). The
game's `FindFirstFileA`/`FindNextFileA` read only the FIRST record of each call, so the old
packing listing hid about three entries in four, loose files included.

Other routes, still served: **whole files** (a mod file at the same disc path replaces it, the
iter-463 overlay) and **new flows as a root `<Flow>.AST`** (`BigFileAssetLoad_826E83D0` opens
`D:\<name>.AST` when no storage holds a flow slot's name; inferred, one roll to confirm). The
**entry-level virtual container merge** (a reference implementation in
`tools/srmods/srvfs.py`: canonical re-layout of all 1,390 retail containers byte-exact) is the
FALLBACK, built only if a roll shows an asset class the loose route cannot reach (e.g. docs
pinned to their stream storage).

Automated runs (`SR_INSTANCE_ID`) mount neither mods nor port features unless asked, so
`docs/NATURAL_BASELINE.md` stays honest (section 10).

## 1. Players: installing a mod

1. Put the mod folder, or its `.zip`, into `<content root>/mods/` (the folder next to
   `Superman Returns (USA)/`).
2. Start the game. A new mod is **on** (unless its `mod.json` says `"enabled": false`).
   MAIN MENU > MODS (or pause > MORE OPTIONS > MODS) lists every mod with author, version, description and status; there you
   switch mods on/off, change the load order (top wins) and see conflicts. Changes apply at
   the next start ("restart required" and a RESTART NOW action).

The player's choices are saved in `<content root>/mods/mods.json` (section 4). A mod that fails
its checks is switched off with a reason shown in the menu and one `[MOD] error` log line; the
game never crashes for a bad mod.

## 2. The mod package

A mod is one folder `mods/<folder>/`, or one zip `mods/<file>.zip` whose root is the folder's
contents (or holds exactly one folder with them). Names starting with `.` or `@` are not mods
(`mods/.cache/` is the host's: extracted zips, generated assets).

```
mods/sample_mission/
  mod.json                                   the manifest (optional: a legacy mod has none)
  preview.png                                optional, named by the manifest
  DATA/_workinprogress/sample_mission/       PER-ASSET OVERRIDES (the normal way)
    LEX01TRIGGERDAS.das                        text XML DAS doc
    LEXobj01FSM_S_1234ABCD.lua                 Lua source
    X2_LEXLUTHOR.rx2  LexAnims.bnk  ScrCredits.ast
  LEXobj01.AST                               a NEW flow bundle (root <Flow>.AST)
  DATA/fmv1.AST                              a WHOLE FILE (replaces or adds that disc file)
  patches/scrcredits.srpatch                 an SRPATCH1 delta (see "patches")
```

- Every file under the mod folder mirrors the disc tree (whole files, the iter-463 overlay);
  not content: `mod.json`, the preview file, names starting with `.` or `@`.
- Put per-asset overrides in your OWN subfolder `DATA/_workinprogress/<mod id>/`: the engine
  scans subfolders, and your files never collide with another mod's by path.
- **Lua state scripts must not allocate on every tick.** The game never collects its Lua heap on its own (the GC threshold is a KB / bytes slip, docs/ENGINE_MAP.md 6c); it collects only when it loads a script. A `function main(mode)` state runs its 'process' branch 30 times a second, so a closure (`local function`, `function() ... end`), a table constructor (`{}`) or a string built from a changing value there grows the heap until the Gameplay::FSM pool refuses and every Lua call fails (LUA_ERRMEM). Declare helpers at chunk level, above `function main(mode)`, and keep that line the first `main(` in the file (the loader renames the first one). Our own costume switcher did this wrong until 2026-10-20: six closures per tick, out of Lua memory ~3.3 min into Metropolis (docs/LOGIC_AUDIT.md F4). `python tools/logic_audit/lua_heap.py live --pid <game> --side ours` shows what a running heap holds.
- **Stems are global.** Two mods with the same stem (`X2_LEXLUTHOR.rx2` in both) are a conflict
  (section 6), and a stem that names a retail asset of ANOTHER type still shadows it (the name
  lookup ignores the type).

### 2.1 mod.json

```json
{
  "format": 1,
  "id": "sample_mission",
  "name": "Sample Mission",
  "author": "Lachlan Oliver",
  "version": "0.2.0",
  "description": "A sample mission mod (an example manifest).",
  "category": "missions",
  "preview": "preview.png",
  "screenshots": ["screenshots/island.png", "screenshots/boss.jpg"],
  "nexus": "https://www.nexusmods.com/<game>/mods/<id>",
  "game": "SRX360-USA",
  "priority": 0,
  "enabled": true,
  "requires": [],
  "conflicts": ["other_finale"],
  "patches": [
    {"asset": "DATA/_workinprogress/sample_mission/ScrCredits.ast",
     "base": "DATA/ui.AST:SCRCREDITS", "patch": "patches/scrcredits.srpatch"}
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `format` | no (1) | 1; any other value is `bad-manifest` |
| `id` | no (default: folder / zip name without `.zip`) | the key in `mods.json` and the menu: letters, digits, `_` `-` `.`, not starting with `.`, at most 64 chars, unique case-insensitively |
| `name`, `author`, `version`, `description` | no | shown in the library (name defaults to the id): the card title, "by <author>", "v<version>", and the details view's scrolling description (line breaks kept, word-wrapped); UTF-8, at most 64 / 64 / 32 / 1024 bytes |
| `category` | no | the library's filter: `characters`, `missions`, `worlds`, `gameplay`, `visuals`, `audio`, `interface`, `other` (case-insensitive). Default: `characters` for a pack with `costumes`, else `other`. An unknown value is `bad-manifest` |
| `preview` | no | a PNG/JPG inside the mod (relative path): the card thumbnail and the first image of the details view. Shown 16:9, cropped to fill (centre); 1280x720 or 1920x1080 recommended. No preview: the card shows generated art (category colour + icon + the name) |
| `screenshots` | no | up to 12 PNG/JPG files inside the mod, in order: the details view's image strip after the preview (LB/RB, the arrows or a click browse them). A missing file is `bad-manifest` |
| `nexus` | no | this mod's Nexus Mods page, `https://www.nexusmods.com/...` (or `https://nexusmods.com/...`) only; the details view shows a NEXUS MODS PAGE button that opens it in the browser when the player clicks it. Anything else is `bad-manifest`. Never fetched by the game |
| `costumes` | no | a character pack's suits (section "Additive character packs"): each `{id, name, entity, description?}`; the library lists them with per-suit include checkboxes |
| `game` | no | the data the mod is made for: `"SRX360-USA"` = the USA retail disc (its data fingerprint, `srmods.py fingerprint` = `SRX360-USA:fe43807d8d15db16`), or an explicit `"SRX360-USA:<16 hex>"`. Anything else: status `wrong-game` |
| `priority` | no (0) | where a newly found mod lands in the default order (higher = nearer the top) |
| `enabled` | no (true) | the default for a newly found mod; the player's `mods.json` wins |
| `requires` | no | ids that must be enabled and ok too (else `missing-dependency`) |
| `conflicts` | no | ids that must not be enabled with this one (else the LOWER one gets `declared-conflict`) |
| `patches` | no | assets GENERATED from the player's own disc (ROM-hack style: ship our bytes, never the game's): `asset` = the disc path the result is served at, `base` = `"<container>:<top-level entry>"` (a name, or `#<16 hex>` id), `patch` = an SRPATCH1 file in the mod (section 7.2). Generated once into `mods/.cache/gen/` |

Unknown fields are ignored. A JSON syntax error (reported with line and column), a wrong type,
a path leaving the mod or a missing file: status `bad-manifest`.

## 3. Asset names (what a loose file overrides)

The stem before the last `.` is the asset NAME the game requests, case-insensitively: `ScrPressStart.ast`
= the Apt movie package `ScrPressStart` (`BigFileAssetLoad_826E83D0`, type ast). Documents are
requested one at a time by (type, name) through the resource manager (`RES_REQ` in the log:
Lua 46534D41, XML 6A1B8478, DAS 44415320, ...), so a loose doc with the same name wins over the
copy in its flow's stream container - unless that request goes to the stream storage directly
(the open question the fallback exists for). An id-only asset is named `__ASSET__<16 hex>.<ext>`.
`python tools/srmods/srmods.py simulate` shows what the game will register.

## 4. Load order and the player's state: mods/mods.json

```json
{
  "format": 1,
  "order": ["sample_mission", "texture_pack", "e1_quit_text"],
  "disabled": ["e1_quit_text"]
}
```

- `order`: every known mod id, **highest priority first** (the top of the menu wins a conflict;
  the iter-463 `enabled.txt` rule "earlier lines win"). Mods found but not listed are appended
  (mod.json `priority` first, then the id).
- `disabled`: ids the player switched off. **A newly dropped-in mod is on.**
- Written by the menu (atomically) and by `srmods.py order`. Never written by an automated run.
- **Migration**: with no `mods.json` and a `mods/enabled.txt`, the file is read once (one id per
  line, `#` comments): its ids go on top, on; every other mod found is off (it was off under
  enabled.txt). `mods.json` is written and `enabled.txt` renamed `enabled.txt.migrated`
  (`[MODS] migrated mods/enabled.txt -> mods/mods.json`).
- A mod folder named like a port feature (`mods/custom_frontend`, the iter-463 form-A build) is
  `reserved` and ignored: the port feature serves the front end now.

## 5. How a layer is served (kernel/mod_manager.cpp + kernel/mod_overlay.cpp)

At the first file open the host decides the layers (highest first: enabled mods in load order,
then port features) and mounts each as one or more MIRROR ROOTS (a mod's own folder; for a zip
its extracted copy; for generated assets `mods/.cache/gen/<layer>-<hash>/`). For a guest path
under the disc (`d:\...`, `game:\...`):

- `NtCreateFile`/`NtOpenFile` open the first root's file at the same relative path (read-only);
  reads, sizes and seeks follow it.
- `NtQueryDirectoryFile` lists the union: disc entries first (a shadowed one keeps the disc name
  and takes the mod's size), then names only layers provide (higher layer first, each sorted),
  minus the files a conflict hides (section 6). With `SR_NTQDIR_XENIA` one record per call.
- Nothing mounted: every call returns at once (the natural path, no probe per open).

## 6. Conflicts

Two enabled layers that provide **the same file path**, or under `DATA/_workinprogress/` **the
same asset stem** (with a registered extension; `__ASSET__` ids likewise) are a conflict. The
higher layer wins; for a stem conflict the overlay HIDES the loser's file from the listing, so
the engine registers only the winner (the engine's own pick between two equal stems in one
storage is undefined). Each conflict is logged once - `[MOD] conflict asset X2_LEXLUTHOR
(DATA/_workinprogress/beta/X2_LEXLUTHOR.rx2): alpha wins over beta` - and listed by the menu with
both names. A mod over a port feature's asset wins (mods sit above port features) and is
reported the same way. That includes the root movie: since step 5 E3-E6 r3 the custom front end
ships a generated `DATA/_workinprogress/custom_frontend/MAIN.ast` (class patches that only work in
MAIN, which registers its 40 `__Packages` classes first: `tools/apt/patch_movie.py`, THE
FIRST-REGISTRAR RULE). A mod that also overrides MAIN wins the stem, the port's MAIN is hidden,
and the boot logs `[MOD] conflict asset MAIN (...): <mod> wins over custom_frontend` and
`[ASSET] MAIN expect source=mod <id>`; the port's popup BACK hints and clickable hints are then off
with it, and the registration line says `source=unknown` (never silent). Shared registry docs that several mods must EXTEND (WORLDFLOW rows,
ONSCREENTEXT strings) are a conflict by this rule; the composer that merges such rows into one
generated doc is the the owner's private mod project side's `srmods sync` prototype (DESIGN.md step 4).

## 7. Port features

Port features are built-in parts of the product: `pause_map` (iter-466: the useful pause map,
docs/MAP.md and "Map markers and maps for mods" below; always on, no toggle), `custom_frontend` (MAIN MENU NEW / LOAD /
SETTINGS / EXIT, universal settings, the SR_Quit bridge); `autosave_without_asking` follows
when the fe lane merges (applied only while its own setting is on, default on). No player
toggle exists for the custom front end. ON for players, OFF in automated runs:

| Process | Port features | Mods |
|---|---|---|
| player launch (no `SR_INSTANCE_ID`, `SR_MODS` unset) | on | `mods/mods.json` |
| `SR_INSTANCE_ID` set (every roll harness) | off; `SR_PORT_FEATURES=1` (or `SR_MODS=custom_frontend`, the iter-463 roll gate) turns them on | only `SR_MODS=a;b` (`@player` = the player's state, read-only) |
| `SR_MODS=0` (the non-roll launchers; difftest) | off unless `SR_PORT_FEATURES=1` | none |
| `SR_PORT_FEATURES=0` | off (the verification switch; not a player option) | unchanged |

A run with port features on logs `[PORT] applied custom_frontend (5 generated asset(s) from the
player's disc)` and is tagged `port:<ids>`; like `mod:<name>` it never counts toward
`docs/NATURAL_BASELINE.md`.

### 7.1 Package (zero game bytes)

`data/port_features/<id>/` in the repo; the build copies `data/port_features/` next to the exe
(`<exe dir>/port_features/` wins over the repo copy):

```
data/port_features/custom_frontend/
  feature.json      {"format":1, "id":..., "name":..., "description":..., "option": null,
                     "base": {"DATA/ui.AST": {"size": 89595420, "toc_sha256": "<hex>"}},
                     "assets": [{"asset": "DATA/_workinprogress/custom_frontend/SCRPRESSSTART.ast",
                                 "base": "DATA/ui.AST:SCRPRESSSTART", "patch": "SCRPRESSSTART.srpatch"}, ...]}
  SCRPRESSSTART.srpatch  SCRSAVELOAD.srpatch  ...   one SRPATCH1 per edited movie
```

`base` pins the USA retail file (size + SHA-256 of its header and TOC); each patch also pins
its base entry (size + SHA-256). A mismatch skips the feature with `[PORT] error ...` and the
game runs without it. At boot the exe reads each base entry from the player's disc, applies the
patch, verifies the output hash and writes the asset into `mods/.cache/gen/port-<id>-<hash>/`
(once; reused while the patches are unchanged).

Built by `python tools/srmods/package_port.py` from the tracked recipes (tools/apt/mods/...):
it runs the builder, finds the changed top-level entries, encodes each with
`tools/srmods/srpatch.py`, refuses an unclean patch, and requires the generated assets to equal
the builder's entries byte for byte. `--check` redoes it without writing and fails if the
committed package is stale: run it after changing a recipe.

### 7.2 SRPATCH1 and the cleanliness rule

`tools/srmods/srpatch.py` (format in its docstring; the exe's applier `srpatchApply` in
kernel/mod_formats.cpp follows the same rules): header `SRPATCH1`, base size + SHA-256, output
size + SHA-256, then ops COPY (from the base entry), ADD (our bytes), FILL (one byte value),
COPYF (from anywhere in the base's disc FILE), END. **Cleanliness rule:** no ADD run contains a
32-byte window that also occurs anywhere in the base FILE; the encoder turns every such window
into a COPY/COPYF and the packager refuses a dirty patch. So a patch carries our bytes and copy
instructions, never the game's data.

### 7.3 Population and traffic density (iter-466, lane density; a PORT FEATURE)

**Players:** two PC options, `traffic_density` (driving and parked cars) and `population_density`
(pedestrians), integer percent 25..200 in steps of 25, default 100, stored in
`<save root>/settings/game_options.ini` (the options screen draws the rows; the API is
`kernel/mod_density.hpp`: `modDensityLoad` / `modDensitySave` / `modDensityApplied`). **A change
applies after restarting the game** (the document is chosen at boot); a UI shows "restart to
apply" while `modDensityLoad() != modDensityApplied()`.

**What the game is given (no hook, no guest writes):** the ambient traffic manager
(`MetropolisTraffic`, `dword_8296E110`, started by the Metropolis world flow's
`StartMetropolisTraffic`) sizes its car, parked-car and pedestrian pools from ONE document:
`WORLDDATA` `TrafficXML=MetropolisTraffic` = `metropol.AST` / `METROPOLISWORLDFLOW_OTHER` /
`METROPOLISTRAFFIC` (compiled XML; parser `MetropolisTraffic_XmlElement_sub_82293A78`). Retail:
`trafficSet` Car `count=60`, ParkedCar 40, Pedestrian 60 (spawn radius 250, pedestrians x0.8).
At boot the exe reads that document from the player's disc, writes the equivalent text document
with those three counts scaled (`modComposeTrafficDensity`; every other element and attribute
unchanged, the MEFireTruck/MERescueVehicles event sets stay 0; fails closed on any other shape)
to `mods/.cache/gen/density-<hash>/DATA/_workinprogress/_density/METROPOLISTRAFFIC.xml` and
mounts it as port layer `density`. The game then grows its own pools to those counts (one member
per group per tick), recycles them around Superman and places them on its own lanes; members it
cannot place stay parked off-world, so visible density saturates at the district's lane capacity.
100/100 mounts nothing: the retail document, byte-identical. Mission scripts that set their own
levels (`SetTrafficLevels` in MSTOBJ02/03, MTOOBJ09, TWSOBJ04, BIZOBJ03) are left as authored;
`ResetMetropolisTraffic` afterwards restores the (scaled) document counts.

**Runs:** off on natural rolls like every port feature; `SR_PORT_FEATURES=1` (or an `SR_MODS` list naming
`density`) plus `SR_TRAFFIC_DENSITY` / `SR_POPULATION_DENSITY` set it for a roll. Log:
`[PORT] applied density (traffic 200%, population 200%: Car 60->120, ParkedCar 40->80,
Pedestrian 60->120)`. Tests: `difftest --mod-manager-test` D0..D13.

**Limits:** the engine pools sized by `MEMORYDEFS` grow on overflow (`FixedPoolAddBlock_sub_821552D0`;
retail already grows `Power_Allocator` at 100%), so there is no code cap; the real limit is guest
heap headroom (HeapGameplay ~0.9 MB at default [I], render/physics heaps unmeasured).

**Evidence (iter-466, exe snapshot 9e7f641d724c, `archive/v1_density/`):**
- Frida on the live manager (`dword_8296E110`, groups at +52, 60 B each, +16 target, +20 document
  count, members at list +64/+68): 100% = Car 60/60 (members 60), ParkedCar 40/40, Pedestrian 60/60
  (= the Xenia golden `sound_gameplay`); 200% = 120/120 (members 120), 80/80 (80), 120/120 (120),
  both in the NEW GAME start area and in free roam from a chapter-5 save. The loose document wins.
- Cost (HIGHFPS_PACE `tick_cost_ms`, medians after the opening FMV; 4 perf rolls with input + 4
  idle rolls): sim tick 2.25 / 3.3 / 4.6-5.4 / 5.4-6.1 ms at 50 / 100 / 150 / 200% (of the 33.3 ms
  30 Hz step). Render tick and fps showed no measurable change (17-18 ms, 52-59 fps at every level:
  the frame is paced at 60 Hz and the host noise is larger than the difference).
- Stability gate (200%): two ~11.5-minute free-roam soaks from the chapter-5 save (`density_soak.json`):
  0 ICALL_FAIL, 0 GPU_HANG episodes, 0 crashes, 0 allocation-failure lines; the ME05 metro event's
  flow loaded and ran its scripts. Movement coverage was weak: the first soak flew for ~3 minutes and
  then stood at a wall, the second walked Midtown's streets among dense crowds (the take-off did not
  engage). So 200% stays offered (`pcsettings::kDensityMax = 200`).
- One unexplained stall: the 150% perf roll had a single 10.4 s present gap (GPU fence wait, watchdog
  H1, recovered) at the scenario point where the 100% roll had a 2.4 s gap; not seen in the 200% rolls.

## 8. Safety: validation and status

Checked at boot for every mod; a failing mod is switched off for this run (its saved enabled
flag is kept) with exactly one `[MOD] error <id>: <status> - <reason>` line:

| status (index) | when |
|---|---|
| `ok` (0) | mounted |
| `disabled` (1) | the player switched it off (no log line) |
| `bad-manifest` (2) | mod.json unreadable / invalid, an escaping path, a missing file |
| `bad-zip` (3) | the zip cannot be read (damaged, encrypted, zip64, a method other than stored/deflate, an unsafe path, a CRC mismatch) |
| `wrong-game` (4) | `game` names other data |
| `missing-dependency` (5) | a `requires` id is not enabled and ok |
| `declared-conflict` (6) | a `conflicts` id is enabled above it |
| `bad-patch` (7) | a `patches` file is corrupt or its base (size + SHA-256) is not the disc entry |
| `bad-content` (8) | reserved for the fallback merge |
| `duplicate` (9) | another folder/zip has the same id (the first, by name, keeps it) |
| `reserved` (10) | the id is a port feature's |
| `off` (11) | port features only: off in this run |

Data mods apply at boot: the menu's changes need a restart (section 9).

## 9. The menu API

C++ (`kernel/mod_manager.hpp`; pure apart from `mods.json` writes and the restart):

```cpp
std::vector<ModInfo> modMenuList();                 // load order, top = highest priority
std::vector<PortFeatureInfo> modMenuFeatures();
std::vector<ModConflictInfo> modMenuConflicts();    // {what, winnerId, loserId, winner, loser}
bool modMenuSetEnabled(const std::string& id, bool on);   // saves mods.json
bool modMenuMove(const std::string& id, int delta);       // -1 = up (more priority)
bool modMenuRestartRequired();                      // saved state != what this boot mounted
bool modMenuRestart(std::string* whyNot);           // relaunch the exe, then the SR_Quit path
// The FSCommand layer: one SR_Mod* command -> the FlowMgr.<method>(args) calls to deliver
// (at most 3 int/string args each). False when cmd is not an SR_Mod* command.
bool modMenuCommand(const std::string& cmd, const std::string& args, std::vector<ModMenuCall>* out);
```

Transport: the bridge's `fscommand("SR_Mod...", args)` -> `modBridgeDispatchEx` (the fe lane's
multi-call string channel, `kernel/mod_bridge.cpp` on `fe/settings-devtools`) calls
`modMenuCommand` for any command starting `SR_Mod` and delivers each returned call, in order,
exactly as it delivers `SR_DTItem` (convert `ModMenuArg` -> `ModArg` one to one).

| Command | Host action | Replies (in order) |
|---|---|---|
| `SR_ModList` | none | `SR_ModBegin(count, state)`; per mod i (load order): `SR_ModItem(i, "<id>", flags)`, `SR_ModText(i, "<key>", "<value>")` for `name`, `author`, `version`, `description`, `preview` (host path), `status`, `reason` (non-empty values only), `SR_ModConflict(i, wins 1/0, "<what> <-> <other name>")` per conflict; then `SR_ModEnd(count, conflicts)` |
| `SR_ModSet <id> <0\|1>` | saves the enabled flag | the `SR_ModList` replies |
| `SR_ModMove <id> <-1\|1>` | moves the mod up (-1) / down (1) one place, saves | the `SR_ModList` replies |
| `SR_ModRestart` | relaunches the game (same command line, environment and folder), then quits like SR_Quit; refused in automated runs | `SR_ModRestarting(1)` (then the process ends) or `SR_ModRestarting(0)` + `SR_ModText(-1, "restart", "<why>")` |
| `SR_ModFeatures` | none | per port feature j: `SR_ModFeature(j, "<id>", flags)` + `SR_ModText(-1-j, "name"/"description"/"reason", ...)`; then `SR_ModEnd(count, 0)` |
| `SR_ModSuitList <pack-id>` | none | `SR_ModSuitBegin(pack, count, restart)`; per suit `SR_ModSuit(index, stableId, flags)` and `SR_ModSuitText(index, "name", label)`; then `SR_ModSuitEnd(count)` |
| `SR_ModSuitSet <stable-id> <0\|1>` | saves that suit's inclusion checkbox | `SR_ModSuitSaved(id, saved 1/0, included 1/0)`, then that pack's suit-list replies; flags bit 0 = saved inclusion, bit 1 = active in the current boot |

`state` bits: 0 restart required, 1 mods mounted this boot, 2 port features applied, 3
automated run (restart refused). Mod `flags`: 0 enabled (saved), 1 mounted this boot, 2 has
conflicts, 3 has a preview, 4 is a zip, 5 legacy (no mod.json), bits 8-15 the status index
(section 8). Feature `flags`: 0 applied, 1 has an option, 2 the option is on, bits 8-15 status.

## 10. Environment overrides and automated runs

| Variable | Effect |
|---|---|
| `SR_MOD_MANAGER=0` | developer-only legacy-overlay comparison; the managed framework is built in and on by default |
| `SR_MODS=a;b` | exactly these mods, in this priority order (`;` or `,`; empty or `0` = none); a name that is a port feature's id turns that feature on |
| `SR_MODS=@player` (alias `@enabled.txt`) | the player's `mods/mods.json` (or a not-yet-migrated `enabled.txt`), READ-ONLY: how a roll tests the player's switch end to end |
| `SR_PORT_FEATURES=1` / `=0` | port features on / off whatever the process kind |
| `SR_CUSTOM_FRONTEND=1` / `=0` | iter-463 name, kept: the custom_frontend port feature on / off |
| `SR_NTQDIR_XENIA=1` / `=0` | the Xenia-faithful directory listing (section 12) |
| `SR_MODS=@<path>` | Legacy overlay only (`SR_MOD_MANAGER=0`): reads that list file. Managed runs use `@player` and a separate content root instead. |

**Automated runs never mount the player's mods or port features.** A process with
`SR_INSTANCE_ID` set ignores `mods/mods.json` / `enabled.txt` and never writes them. The roll
harnesses that set it: `run_recomp_x.ps1`, `sr_drive.ps1`, `replay_ab.ps1`, `hle_replay.ps1`.
Rolls get mods only through `-Gates "SR_MODS=<name>"` (one name per `-Gates`, since the runner
splits gates on commas) or `-Gates "SR_MODS=@player"`. Every other launcher sets `SR_MODS=0`
unless the caller already set it, which also keeps port features off: `run-30s.ps1`,
`run-test-capture-log.ps1`, `verify_present_window.ps1`, `sweep_driver.py`,
`qualify_draw_manifest.py`, `mutate_tex_census.py`, `mutate_cube_h1a.py`, `mutate_cube_h1b.py`,
`mutate_gateoff.py`, and `build/difftest.exe` itself (its `main` pins `SR_MODS=0`). A new
launcher of `SupermanReturns.exe` must do one or the other. A run that mounted anything says so
(`[MOD] enabled ...`, `[PORT] applied ...`); a natural run has no `[MOD]` and no `[PORT]` line.

## The mod library (iter-465, lane modlib; a PORT FEATURE)

USER 2026-10-03: "i asked him to create a proper frontend for mod selection taking inspiration of
the look from other in game mod organisers like bethesda, and all he did was make a list." The
library replaces the Apt list pages below. Its structure follows Bethesda's in-game Mods /
Creations menus (a library of thumbnail cards, a details page, a load order page); its look is
Superman Returns' own (sampled from the MAIN MENU and GAMEPLAY OPTIONS popups): deep-blue glass
panels with bright crystal chrome bands, white-over-silver extended italic type with a dark drop
shadow, the game's pale selection band, and the cape's red with the shield's gold for focus.

### Players

**Open it:** MAIN MENU > MODS, or in game pause > MORE OPTIONS > MODS. The game behind it is
dimmed and paused for input (it reads a neutral pad, as a 360 title did while the system Guide
was open). **B / Esc / right click** closes it back to the menu you came from.

**Header:** MODS, the tabs **LIBRARY | LOAD ORDER | GET MORE MODS** (LB / RB, Tab / Shift+Tab, or a
click), "N INSTALLED · N ENABLED", and, when a change is waiting, the red **CHANGES APPLY AFTER A
RESTART · (Y) RESTART NOW** banner. RESTART NOW asks first (in game: progress since the last save
is lost), then relaunches through the MODS menu's restart path.

**LIBRARY:**
- left rail: a **search box** (type to filter by name, author or description: click it, A on it,
  `/` or Ctrl+F) and the categories **All, Characters, Missions, Worlds, Gameplay, Visuals, Audio,
  Interface, Other, Built-in features**, each with its count; moving through the rail filters the
  grid live (LT / RT, PgUp / PgDn step through them from anywhere);
- the grid: 4 columns of cards. A card shows the mod's preview (or generated art: the
  category's emblem over a per-mod pattern; the name is printed once, under the art), its name,
  "by author · version", a category icon, an **enable checkbox** (click it, or X on the
  focused card), and badges: **BUILT-IN**, **CONFLICT** / **WINS CONFLICT**, **PROBLEM** (the manager
  refused it; the details say why), **NEEDS RESTART** (the saved choice differs from this boot).
  A switched-off mod's card is dimmed. The focused card has the gold-and-red frame and a light
  sheen; the mouse focuses by hovering, the wheel scrolls, and the **scroll bar** at the right
  edge drags (thumb) or pages (a click on the track) - the load order and the details column
  have the same bar. A mod folder without a mod.json shows its folder name as a title
  (`sample_mission` -> "Sample Mission"), no author, and generated art;
- **details** (A or a click on a card): the hero image with a strip of the preview + screenshots
  (LB / RB, LEFT / RIGHT, the arrows or a click), the title, "by author · Version · category ·
  size", badges (ENABLED / DISABLED / PROBLEM, LOADED NOW, NEEDS RESTART, CONFLICT, ZIP), the **ENABLE /
  DISABLE MOD** button, a **NEXUS MODS PAGE** button when the mod has one, and a scrolling column
  (right stick, wheel, PgUp / PgDn): the description, a PROBLEM box when it failed, **CONFLICTS**
  (each: WINS OVER / LOSES TO the other mod, and the asset), and for a character pack **SUITS IN
  THIS PACK**: one include checkbox per suit (saved as `disabled_costumes` in mods.json by its
  stable id) and the hint "Wear a suit from the Fortress of Solitude > Costumes." - the library
  never wears a suit; the game's own costume screen does.

**LOAD ORDER:** every mod, top wins: number, enable checkbox, preview, name and author,
category, version, conflict markers (gold "WINS n", red "LOSES n") and status (LOADED, OFF,
ON / OFF AFTER RESTART, PROBLEM). A = enable / disable; **X picks the mod up**, UP / DOWN move it
(each step saved), A / X / B drop it; with the mouse, **drag a row** by its handle or name and drop
it. The line under the list spells out the focused mod's first conflict ("SAMPLE MISSION WINS
<asset> OVER HD CITY TEXTURES"). The built-in features (custom front end, autosave without
asking) are listed under the mods, locked; only autosave has a toggle.

**GET MORE MODS:** how to install (download a folder or .zip, put it in the mods folder, restart),
**BROWSE NEXUS MODS** (opens https://www.nexusmods.com/ in the browser, only when clicked) and
**OPEN MODS FOLDER** (with its path). No catalogue, no download or install button, no account.

**Prompts** at the bottom follow the device used last: the Xbox 360 buttons (drawn), PlayStation
4 / 5 and Switch (positional) from the Julio Cacko set, or the keyboard keys drawn as key caps as wide as
their label, the arrow keys as one up / down cap (the mouse wheel stays a Julio Cacko picture). Every prompt is
clickable. Keyboard (lane E E3, docs/PC_CONTROLS.md 7.1: the keys and their caps follow the PC controls): arrows;
Enter = A and Esc = B always; then the menu bindings (defaults: Enter = A, Esc / Backspace = B, E = X, F = Y);
Tab / Shift+Tab = RB / LB, PgUp / PgDn = LT / RT; `/` or Ctrl+F = search. Pad: D-pad or left stick (with repeat),
A / B / X / Y, LB / RB, LT / RT, right stick to scroll.

Every change is saved at once in `mods/mods.json` (enable / disable, load order, suit
inclusion; autosave's option in `mod_options.ini`) and applies at the next start, because mods
mount when the game boots.

### How it is built

- `kernel/mod_library.{hpp,cpp}`: open / close, input routing, the data snapshot over the mod
  manager (`modMenuList`, `modMenuConflicts`, `modMenuFeatures`, `modCostumePackChoices`) and the
  actions (`modMenuSetEnabled`, `modMenuMove`, `modCostumeSetIncluded`, `modOptionSet`,
  `modMenuRequestRestart`, ShellExecute of a validated Nexus URL / the mods folder; never in an
  automated run).
- `gfx/mod_library_ui.{hpp,cpp}`: the overlay. Dear ImGui (third_party/imgui, MIT, v1.91.9b) is
  only the drawing layer (font atlas, draw lists, the DX12 renderer backend with its own SRV heap);
  every widget, the layout, focus, animation and input handling are ours. Laid out in 1280x720
  design units over the game's 16:9 image (the mouse cursor's fit rectangle), scale = image
  height / 720; the Saira fonts (OFL, `data/fonts/saira/`) are rasterised at that pixel size and
  rebuilt when the output height changes, so 720p to 4K text is crisp. Images (previews,
  screenshots, prompt glyphs) are decoded by a worker thread (WIC: PNG / JPG, downscaled to
  720 px for cards, 1920 for the hero), uploaded a few per frame on the render thread, and all
  released when the library closes.
- `gfx/d3d12_overlay.cpp drawHostOverlay`: the library draws on the present path after the frame
  dump and before Present, the mouse cursor on top of it. Present dumps (SR_PRESENT_DUMP) never
  contain it; `SR_MOD_LIBRARY_SHOTS=<dir>` + F12 writes the composited frame (overlays included)
  as `modlib_<n>_<w>x<h>.bmp`.
- Entries: `kernel/mod_menu.cpp modMenuPagesCommand` turns the Apt side's `SR_Menu "open mods
  FE|IG"` into `modlib::requestOpen` and answers `SR_DTEnd(0,0,0)`, so the Apt menu closes back
  to its caller (the MODS row stays focused) and the overlay draws over it. No Apt patch changed.
- Input: `app/platform_sdl.cpp` (keyboard + text events, while open and only where real input
  is allowed), `kernel/mouse_ui.cpp` (the real mouse or the harness feed, in stage coords; no
  Apt click or hover is queued while open; the cursor shows with the library's hover ring),
  the harness keyboard channel (polled), `hostinput::readActivePad` (the SDL pad).
  `kernel/stubs.cpp XamInputGetState` gives the game a neutral pad while open and after the
  close until everything is released; `kernel/pc_controls.cpp` keeps mouse-look off.
- Charter: a PORT FEATURE (player-facing host UI). No guest memory is read or written and no
  guest code is called; the only guest-visible effect is the neutral pad while it is open.
  `SR_MOD_LIBRARY=0` restores the retired Apt pages (developer A/B).
- Tests: `build/difftest.exe --mod-library-test` (categories, search, grid navigation, the
  input latch, the snapshot and every action over the real mod manager, the SR_Menu route).
- Rolls: `tools/modlib/make_test_mods.py` (ten sample mods: every category, previews and
  screenshots cropped from your own captures, a conflict pair, a four-suit pack, one without a
  preview, one off by default, one refused) into a test content root; `tools/scenarios/
  modlib_tour.json` (title > MAIN MENU > MODS, then every page by keyboard and mouse feed, F12
  captures, VALUE checks of the saves; gates `SR_PORT_FEATURES=1,SR_MODS=@player,
  SR_MODS_ALLOW_WRITE=1,SR_MOD_LIBRARY_SHOTS=<dir>,SR_WINDOW_SIZE=<w>x<h>`), and
  `tools/scenarios/modlib_look.json` (`SR_MOD_LIBRARY_AUTOOPEN=<s>`: opens by itself, for look
  iterations). `SR_MODS_ALLOW_WRITE=1` lets a roll on its own content root save mods.json.
  `modlib_pause.json` reached it from pause > MORE OPTIONS > MODS (context IG; retired with that route in lane E L-c - the
  9-row pause menu has no MODS: MODS is on the MAIN MENU only);
  `modlib_real.json` runs over a local root with REAL mods (junctions to their folders, never
  committed) beside the samples and drives the mouse scroll path (wheel, thumb drag).
  `SR_MOD_LIBRARY_PROMPTS=xbox|ps4|ps5|switch|keyboard` pins the prompt set for a look check.
- Look iterations without a game: `build/difftest.exe --mod-library-render <content root> <out>
  <WxH> <background image|-> "<script>"` draws the real overlay code on an offscreen D3D12 back
  buffer over the real mod manager (script: `frames N; key A; type text; move X,Y; click X,Y;
  down; up; wheel N; shot NAME`), and writes BMPs; the live roll confirms.

## The MODS menu (iter-464, lane modmenu; RETIRED by the mod library, `SR_MOD_LIBRARY=0` only)

USER 2026-10-02: "we're going to need a mod menu". The MODS pages are real game popups (the
game's ScrGenPop chrome, font and focus), pad-, keyboard- and mouse-navigable like every other
custom front-end page.

### Players: install, switch on, restart

1. Choose **OPEN MODS FOLDER**, then add the mod's folder or `.zip` (managed loader).
   The folder is `<content root>\mods\`, next to `Superman Returns (USA)\`. The legacy
   loader requires `.zip` files to be unzipped first; its help says so.
2. In the game: **MAIN MENU > MODS**, or in game **pause > MORE OPTIONS > MODS**.
3. The list shows every installed mod in **load order** (a mod higher in the list wins when two
   change the same thing), three per page:
   `SAMPLE MISSION: ON - CONFLICT`. **LEFT / RIGHT** switch the mod on or off; **A** opens its
   page; **MORE MODS** pages on; the last row is **HOW TO INSTALL MODS**, or **RESTART TO
   APPLY** once you changed something.
4. A mod's page: version, author, description, its status (`OK`, `CONFLICT - <what, with
   whom, who wins>`, `FAILED - <why>`) and whether it is loaded now; rows **ENABLED**,
   **LOAD ORDER n OF m** (LEFT = up, RIGHT = down, A = up), **WHAT IT CHANGES** (each file or
   entry: replaces / adds), **CONFLICTS** (the target, the other mod, which one is used), and
   **OPTIONS** when the mod has options.
5. Mods load when the game starts, so a change applies after a restart: **RESTART TO APPLY >
   RESTART NOW** restarts the game (in game it warns that progress since the last save is
   lost). **NOT NOW** keeps playing; the change applies at the next start.
6. No mods installed: a short explanation and **OPEN MODS FOLDER**, **RESTART TO APPLY**,
   **HOW TO INSTALL MODS**. Managed-loader discovery currently happens at boot, so it does
   not offer a non-working refresh action. Newly dropped mods default to ON. The legacy
   catalog still provides **CHECK AGAIN** and requires enabling newly found mods.

The release installation directory may be called **Superman Returns**, or anything else.
No source-checkout name or development path is baked into these messages. `file_io.cpp`
discovers the content root next to the executable, one level above it (`bin/` / `build/`),
then in the working directory. Executable-side assets take priority over an unrelated CWD.
Discovery uses Windows Unicode paths and returns UTF-8. The menu gets the exact mods folder
from the active manager, not a separate cached guess. The help page wraps its real path;
the main empty state avoids a long path paragraph. **OPEN MODS FOLDER** uses `ShellExecuteW`
on that absolute directory, never a command string or mod-provided URL; failures have a
visible help fallback, and automated runs never open Explorer.

Port features (the custom front end, autosave) are part of the game, not mods: they are never
listed, but a mod that collides with one says so (`... is also changed by AUTOSAVE WITHOUT
ASKING (PORT FEATURE OPTION) (that one wins)`).

### How it is built

- **Pages**: `kernel/mod_menu.cpp` builds every page (title, body, rows) on the host;
  `kernel/host_pages.{hpp,cpp}` is the engine shared with the Dev Tools pages; the Apt side
  (custom_frontend `devtools_ui.aptasm`, `_global.SRDevTools`) only renders them through
  `fscommand("SR_Menu", "<verb> <page> ...")` (SR_DevMenu = its older name). Page ids: `mods[:off]`,
  `mod:<id>`, `modfiles:<id>:<off>`, `modconf:<id>:<off>`, `modopts:<id>:<off>`, `modhow`,
  `modrestart`, `costumes[:off]` (contract: `kernel/mod_menu.hpp`).
- **Entries**: MAIN MENU row 3 (`srfrontend.aptasm` `OpenMods`; EXIT moved to row 4) and the
  pause menu's MORE OPTIONS panel row 1 (`srpause.aptasm`; DEV TOOLS moved to row 2). SETTINGS
  has no room (ScrGenPop shows at most 5 rows).
- **Data**: a `ModCatalog` (kernel/mod_menu.hpp). Until the modcore lane's mod manager merges,
  the built-in FALLBACK catalog serves today's overlay: the folders in `mods/`, the boot's list
  file (`mods/enabled.txt`; a line `# <id>` keeps a switched-off mod's place; the player's own
  comments are kept), an optional flat `mods/<id>/mod.json` (`name`, `author`, `version`,
  `description`, `port_feature`), and FILE-level conflicts (two layers shadowing the same
  disc path: before the entry-level merge the lower one loses the whole file). Unlisted
  installed mods are OFF under enabled.txt (modcore: a dropped-in mod is ON).
- **Restart**: RESTART NOW flags the relaunch and pushes SDL_QUIT (the SR_Quit path);
  `app/main.cpp` closes the log, then `modMenuSpawnRestartIfRequested` starts the same exe with
  the same command line, working folder and environment plus `SR_RESTART_GENERATION=<n+1>` and
  `SR_RESTART_PARENT_PID`; the child logs `[MODMENU] restarted by the MODS menu (generation n,
  parent pid p, pid c)`. In an automated roll the child logs to
  `superman_returns_<instance>_r<n>.log` and dumps frames to `<SR_PRESENT_DUMP_DIR>_r<n>`, so the
  parent's artifacts are never overwritten (`tools/modmenu/check_restart.ps1` checks the child
  and then ends it).
- **Rolls**: `tools/modmenu/make_croot.py` builds a dedicated content root (junctions only: the
  game folder, an exe snapshot as `bin/`, chosen mods, a seeded `mods/enabled.txt`), so a roll
  sees an exact set of installed mods. `SR_MODS=@enabled.txt` then reads and WRITES that root's
  own file. `SR_MODS=@<path>` reads another list file (a roll's own copy); `SR_MODS=a;b` keeps
  the menu's changes for the session only (the restart passes the list in `SR_MODS`).
- **Extension points**: `modMenuRegisterOption(modId, row)` adds a row to a mod's OPTIONS page
  (toggle / slider / action / readout, the Dev Tools row kinds); `modMenuRegisterCostume(def)`
  feeds the **COSTUME PICKER** page `costumes` (pending user request: extra Superman skins +
  Zod). A costume's `select()` must drive the game's OWN costume state (SetCostumeNum /
  AwardCostume) at a declared drain point on the game's tick, never from the Apt thread. The
  page has no Apt entry until a provider registers a costume.
- **The costume picker design (for the skins lane).** Retail already HAS a costume picker:
  `screens/misc/ScrCostumes` (`SCREEN.COSTUMES`, Fortress hub > COSTUMES; ui.AST movie
  SCRCOSTUMES, decompiled from the carve `0816_SCRCONTROLLEROPT` - carve n holds TOC record
  n+2, CUSTOM_FRONTEND 2.3). Its InitGui reads `_global.RecordAdapter.GetCostumeData()` (an
  array of `{Locked, ...}`), shows it in a ThumbViewer (1 row x 3 columns, the
  COSTUMETHUMB_1..3 / COSTUMEFULLSIZE_n clips), and X calls
  `_global.RecordAdapter.SetActiveCostume(i)` on an unlocked thumb (A = full-size view, B =
  `SwitchScreen(FORTRESS)`). So the natural route is: (1) a COSTUMES row (pause MORE OPTIONS,
  and MODS when a skin pack is installed) opens the game's OWN ScrCostumes as an overlay
  (`FlowMgr.SwitchLevel("NONE", SCREEN.COSTUMES, 60)`, the SETTINGS precedent, with the
  options screens' Back patch so B returns to the caller instead of the Fortress); (2) a skin
  pack adds entries to the native costume table behind `GetCostumeData` (to find in IDA: the
  RecordAdapter natives) plus its thumbnail clips and arenas; the player's choice then goes
  through the game's own `SetActiveCostume`. The host `costumes` page above is the fallback
  if the native table cannot grow (its `select()` must then be executed by the AS side calling
  `RecordAdapter.SetActiveCostume`, i.e. the game's own native, never a host write).
- **Log lines**: `[SR_FSCMD] #n SR_Menu '<args>' -> <k> call(s)`, `[SR_FSCMD] ... SR_Log 'menu
  show <page>'` (the renderer), `[MODMENU] saved <file>: a, -b, c` (the new order; `-` = off),
  `[MODMENU] <id> move up|down -> saved|refused`, `[MODMENU] RESTART NOW -> SDL_QUIT ...`,
  `[MODMENU] relaunching the exe ...`, `[MODMENU] restarted by the MODS menu (...)`.
- Tests: `build/difftest.exe --mod-overlay-test` M17a-M17ac (the pages through the real
  SR_Menu bridge command over a temp content root: empty state, badges, toggle, reorder, the
  file rewrite, conflicts naming the winner, the restart request, session mode, OPTIONS,
  COSTUMES, routing, `SR_MODS=@<file>`).

### Agreement with the modcore lane (branch mods/ship; its MODS.md sections 1-10)

The menu builds against modcore's contract. Changes this lane asks for (and what it does on its
side), to settle at whichever merge comes second:

1. **The menu uses the C++ API, not the SR_Mod\* FSCommands.** The pages are host-built (above),
   so `kernel/mod_manager.hpp`'s `modMenuList / modMenuSetEnabled / modMenuMove /
   modMenuRestartRequired / modMenuConflicts` plug into the menu through a small `ModCatalog`
   adapter (`modMenuSetCatalog`). The SR_ModList/SR_ModSet/... protocol is not needed by the menu.
2. **"What it changes"**: the menu shows each mod's targets. modcore's `ModInfo` has no change
   list; please add one (e.g. `std::vector<std::pair<std::string,char>> changes` = target +
   `r`eplace / `a`dd / `e`dit / `p`atch), or `modMenuChanges(id)`.
3. **Conflicts per mod**: `modMenuConflicts()`'s `{entry, winner, loser}` is enough; the adapter
   filters by mod and needs the display names of both sides (port features included).
4. **Restart in automated runs**: modcore's table refuses `SR_ModRestart` under
   `SR_INSTANCE_ID`. This lane's restart is safe in rolls (the child's log and frames go to their
   own `_r<n>` names; `check_restart.ps1` ends the child), and the restart path must be
   verified by a roll. Please let `modMenuRestart()` call `modMenuRequestRestart()`
   (kernel/mod_menu.hpp) instead of refusing.
5. **A writable roll state**: `SR_MODS=@player` is read-only by design. For menu rolls this
   lane uses `SR_MODS=@<path>` = a roll's OWN state file the menu may write (built for
   `enabled.txt`; the same for a `mods.json` copy), with `make_croot.py`.
6. **Where the menu lives**: MAIN MENU > MODS and pause > MORE OPTIONS > MODS (section 1 of
   modcore's doc says SETTINGS > MODS: the SETTINGS panel is full).
7. **Names**: this lane's types are `ModMenuEntry / ModMenuView / ModMenuConflict /
   ModMenuChange / ModMenuState` and `modMenuPagesCommand`, so they never clash with modcore's
   `ModInfo` / `modMenuCommand`.
8. **The adapter is already written**: `kernel/mod_menu_modcore.cpp` maps `modMenuList()` /
   `modMenuConflicts()` / `modMenuSetEnabled()` / `modMenuMove()` / `modMenuRestartRequired()` /
   `modMenuAutomated()` onto the pages. It compiles to a stub until `kernel/mod_manager.hpp` is in
   the tree (`__has_include`), then `modMenuCatalog()` uses it ahead of the fallback - no other
   change needed. "What it changes" walks `ModInfo::root` (the shared `modMenuWalkGameFiles`)
   until a change list exists. Syntax-checked against the 2026-10-02 05:15 draft of
   mod_manager.hpp, which has one compile error of its own: the member `requires` (a C++20
   keyword; clang: "trailing requires clause can only be used when declaring a function") -
   rename it (e.g. `requiresIds`). (Its 05:30 draft renamed it `deps`: resolved.)

### Lane status (modmenu, 2026-10-02 09:55, handoff block)

- **Works (unit-tested, not rolled):** MODS pages host-built (list/details/changes/conflicts/how-to/restart, OPTIONS + COSTUMES extension points), fallback catalog over `mods/enabled.txt`, modcore adapter (`__has_include`); difftest `--mod-overlay-test` 133/0, `--devtools-test` 33/0; branch `fe/settings-devtools` rebased on dev 55edc76.
- **Unproven:** NO live roll yet of any FE page (MODS, Autosave without asking, Dev Tools pages, pause MORE OPTIONS); RESTART NOW relaunch; the adapter against modcore's final header (modcore not merged: `mod_manager.hpp` uncommitted in sr-wt-modship); autosave disk round trip.
- **Next step:** `build_slot.py --owner modmenu -- cmake --build build/ -j 8` in sr-wt-fe, `tools/snapshot_exe.ps1 -Label modmenu`, take the game slot, then from the main root run `archive/v1_modmenu/batch_menu.ps1 -Snap <exe>` and `batch_fe.ps1 -Snap <exe>` (add `SR_INSTANCE_ID=modmenu` to every -Gates), view frames of every page, value-check the scenario logs.

## What the overlay covers

| Call | Behaviour with a mod enabled |
|---|---|
| `NtCreateFile` / `NtOpenFile` | a guest path that maps into the disc tree (`d:\...`, `game:\...`, bare `DATA\...`) opens the highest-priority mod's file instead, **read-only**. A folder that exists only in a mod opens as a directory |
| `NtReadFile`, `NtQueryInformationFile`, `NtSetInformationFile` (seek) | follow the opened file, so sizes and bytes are the mod's |
| `NtQueryDirectoryFile` | lists the union: disc entries first in disc order (a shadowed entry keeps the disc name and takes the mod's size), then names only mods provide (highest-priority mod first, each mod's names sorted) |
| Names the retail disc does not hold | the host's old fallback aliases (`sound1.AST`->`sound.AST`, `objtiv1.AST`->`objtiv.AST`, `fmv1..5.AST`->`fmv.AST`) were **deleted** 2026-10-20 (step4/audit batch 3, docs/LOGIC_AUDIT.md 2.0g). Those opens fail with OBJECT_NAME_NOT_FOUND as on Xenia and the console, so a mod's `sound.AST` answers `sound.AST` only. A mod that ships a file under one of those names (sample_mission's `objtiv1.AST`) is served under it like any added file (mod-overlay test M7f) |
| `NtQueryFullAttributesFile` | answers by real existence (step4/audit batch 3, `SR_IMPORT_FIX_ATTRS`, baked default ON 2026-10-20; `=0` = the old generic SUCCESS), as Xenia's disc device would. A disc file / folder: the ISO's attributes | READONLY, its length (a folder: its directory table length), the 1970 times. A file a mod adds or shadows, or a folder only a mod provides (`modOverlayResolveFile` / `modOverlayResolveDir`): the same attributes with the backing host file's / folder's size and times. A path nothing resolves (or a host drive path): `STATUS_NO_SUCH_FILE`. Its only guest caller is `GetFileAttributesA_sub_82469720` (the D3DX compiler's `ShaderDumpxe:\CompareBackEnds` probe and the effect preprocessor's file `#include`) |

- Matching is case-insensitive (the Windows host file system). `d:\DATA\UI.AST` finds
  `mods/x/DATA/ui.AST`.
- A path with a `.`/`..` component or a `:` stream never reaches a mod.
- Each path is resolved once per process. Changing mod files needs a restart.
- Save data (`rmcsave:\`, XAM content) is not part of the overlay; it lives in the host
  save folder (`docs/CUSTOM_FRONTEND.md` §8).
- `tools/apt/build_custom_frontend.py --mod/--enable` refuses a name the exe would skip.
- A mod name must be a plain folder name (letters, digits, `_`, `-`, `.`, not starting with
  `.`). A listed mod whose folder is missing gets one `[MOD] error` line and is skipped;
  the game runs without it.

### Option folders: `mods/<mod>/@<option>/` (iter-464)

A folder whose name starts with `@` at the top of a mod is an **option folder**. It is part of
the mod only while the host option of that name is 1 **at boot**, and then wins over the mod's
own files (it is mounted just above the mod). Options live in
`<save root>/settings/mod_options.ini` (next to `game_options.ini`; one `key=value` per line,
`#` comments), and the mod's own menu writes them with `fscommand("SR_SetModOption")`. Known
options and their defaults:

| Option | Default | Folder |
|---|---|---|
| `autosave_without_asking` | 1 | `custom_frontend/@autosave_without_asking/DATA/baseworl.AST` (docs/CUSTOM_FRONTEND.md 5.6) |
| `devtools_notice_ack` | 0 | none (the Dev Tools notice was acknowledged) |

An unknown option never mounts. A change takes effect at the next launch, like every overlay
file. The `@` folders are skipped when the mod's own tree is merged, so they are never served
as `DATA/...` paths by accident.

## Log lines

| Line | When |
|---|---|
| `[MOD] enabled (highest priority first): a, b  [source: ...; root "..."]` | once at boot, only when at least one mod mounted |
| `[PORT] applied <id> (N generated asset(s) from the player's disc)` | once per port feature applied |
| `[ASSET] MAIN expect source=<override\|retail\|mod ID> \| retail ... sha256=H \| override sha256=H` | once per boot: which MAIN this boot's mounts imply (step 5 E3-E6 r3, `kernel/asset_receipt.hpp`) |
| `[ASSET] MAIN #N source=<retail\|override> file="..." sha256=H const=match apt=match\|differs` | every registration of the root movie, matched byte for byte against the disc's MAIN and the port feature's generated one before the parser runs (only through fault-free guest reads, the whole length readable; in every build configuration, release included); `source=unknown` once when the first movie matches neither; graded by `tools/main_receipt.py` |
| `[PORT] error <id>: <status> - <reason>` | a port feature that cannot apply (another disc, a bad package) |
| `[MOD] error <id>: <status> - <reason>` | once per mod that fails validation (section 8) |
| `[MOD] error: mod '<x>' is enabled but not in mods/ - skipped` | an `SR_MODS` name with no folder |
| `[MOD] conflict <what>: <winner> wins over <loser>` | once per conflict (section 6) |
| `[MOD] overlay shadows/adds '<rel>' -> "<host path>" (mod|port feature <id>, N bytes)` | once per distinct path a layer answers |
| `[MODS] migrated mods/enabled.txt -> mods/mods.json ...` | the one-time migration |
| `[MOD] menu: set/move ...`, `[MOD] restart: ...` | menu actions |
| `[NTQDIR] h=.. dir=".." mask='..' restart=.. len=.. -> <status> entry='..'` | SR_NTQDIR_XENIA: the first 400 listing calls |
| `[MODS] automated roll (SR_INSTANCE_ID): mods/enabled.txt ignored ...` | a roll found an `enabled.txt` and ignored it |
| `[MOD] option folder <mod>/@<option> mounted|not mounted (option <option> = <v>)` | once per option folder at boot (`v` -1 = unknown option) |

A run with nothing mounted has **zero** `[MOD]` and `[PORT]` lines.

## Charter classification

A **content mod mechanism**, opt-in. It serves files the user placed; it calls no guest
function and writes no guest state, so it is neither forcing nor a platform fix, and the
overlay itself registers no override. Runs with a mod enabled are tagged `mod:<name>` and
never count toward `docs/NATURAL_BASELINE.md`.

**Port features** (section 7) are a declared class of their own (user decision 2026-10-02):
player-facing parts of the product, on for players, off in every automated run unless opted
in, tagged `port:<ids>`, never counted as natural progress. The merge itself (mod_vfs) serves
bytes only; it calls no guest function and writes no guest state.

## Host bridge: fscommand("SR_...") (iter-463, custom_frontend step 5)

Apt content talks to the host through the Flash `fscommand(cmd, args)` channel, which the
console's Apt runtime handed to a host-registered handler: `AptFsCommandDispatch_sub_826D39A0`
(ENGINE_MAP "Apt FSCommand -> host"). `kernel/mod_bridge.{hpp,cpp}` strong-defines that
function (docs/OVERRIDES.json: `platform-fix`, inert for retail content, which issues 0
FSCommands):

- A command starting `SR_` (case-sensitive) is handled on the host and never reaches the
  guest's registry, whose lookup returns 0 on a miss and is dereferenced with no null check
  (a fatal host AV under the VEH), so there is no natural behaviour to preserve for it.
- Every other command runs the game's own body unchanged.
- The bridge is NOT tied to a mod name (iter-463 review finding 2): any Apt mod, a renamed
  copy of custom_frontend, or a hand-placed ui.AST may use `SR_*`. Retail content issues 0
  FSCommands, so the natural path never reaches it.

| Command | Host action | Log |
|---|---|---|
| `SR_Quit` | `SDL_PushEvent(SDL_QUIT)`: the window-close path (app/main.cpp `hostExitNow`: flush the input tape, the log and every C stdio stream, `std::quick_exit(0)` (runs only `at_quick_exit` handlers, e.g. the guest profiler's final dump, then `_Exit`), as UnleashedRecomp `App::Exit` does with `std::_Exit`; `SR_FAST_EXIT=0` restores the old CRT teardown, which crashed 0xC0000005) | `[SR_FSCMD] #n SR_Quit ...`, then `[EXIT] event pump ended ...` |
| `SR_Log <text>` | none (mod diagnostics); the line also carries `saveFlow+16` (`*(*0x8296EE8C+16)`, the C++ popup id the save flow waits for), read fault-free | `[SR_FSCMD] #n SR_Log '<text>' saveFlow+16=...` |
| `SR_SetSettings [<tag> ]<packed>` | writes the universal game-options store `<save root>/settings/game_options.ini`, where the save root is the xam_content root that also holds the saves (docs/CUSTOM_FRONTEND.md section 8): `%LOCALAPPDATA%\SupermanRecomp\content` by default, `<exe dir>\content` when `<exe dir>\portable.txt` exists, `SR_CONTENT_DIR` when set. It is NOT under the content root that holds `mods/` (the last token is the packed value, kernel/mod_bridge.hpp; out-of-range or non-numeric values (a getter that returned undefined sends `NaN`) are rejected, the file kept and NO reply sent, so the mod's `_global.SRSettings` keeps its last good value; a failed file write (a locked or read-only ini, a full disk) still replies with the value the file would hold, so the session keeps the new options and the ini's `apply_to_loaded_saves`; `apply_to_loaded_saves` is host-owned and kept; volumes are taken only while the game's audio mixer exists (`dword_82976054`/`dword_82976070`, read-only), else the file's are kept or stay absent) | `[SR_FSCMD] #n SR_SetSettings '...' -> stored (music=.. sfx=.. ...)`, `-> NOT STORED (file write failed; kept for this session) (...)` or `-> REJECTED (...)` |
| `SR_GetSettings` | reads the store and replies (packed = -1: no or incomplete store). **Replies** (this and `SR_SetSettings`, which replies with the value as stored): the host writes a guest stack frame below r1 (back chain + two strings) and calls `AptAS2AllocIntObj` and the AS method `FlowMgr.SR_OnHostSettings(packed)` through AptExecutor vt+0x20 (target path `"FlowMgr"`) with one `AptAS2AllocIntObj` value - the call the game's natives make for `FlowMgr.CppShowPopup` (sub_822B15F8) - on the guest thread already inside the interpreter, on its own guest stack frame. The mod binds that method in the movie that sends the command, right before; it is the ONLY writer of `_global.SRSettings`. No executor: logged, nothing called | `[SR_FSCMD] #n SR_GetSettings -> <packed> (...)`, then `[SR_FSCMD] reply FlowMgr.SR_OnHostSettings(<packed>) delivered` |
| `SR_GetModOptions` | reads `settings/mod_options.ini` and replies `FlowMgr.SR_OnHostModOptions(packed)`: bit 0 `autosave_without_asking`, bit 1 `devtools_notice_ack`, bit 8 a dev tool is on, bit 9 the autosave option folder was mounted at boot, bit 10 a level skip is armed or running | `[SR_FSCMD] #n SR_GetModOptions -> <packed> (autosave=.. noticeAck=.. devtoolsOn=.. autosaveLuaMounted=.. levelskip=..)` |
| `SR_SetModOption <key> <0\|1>` | writes one known option (atomic tmp + rename) and replies as `SR_GetModOptions`; an unknown key or value is refused | `[SR_FSCMD] #n SR_SetModOption '<key> <v>' -> stored\|REJECTED (packed ..)` |
| `SR_SaveEvent new\|load\|save\|overwrite\|query` | the current-save-slot tracker (docs/CUSTOM_FRONTEND.md 5.6): reads `saveFlow(*0x8296EE8C)+0x54` fault-free (the slot SaveTask / LoadTask use); `new` clears, a valid `load`/`save` sets, `overwrite` latches (a confirmed overwrite leaves +0x54 = 3). Only `query` replies, `FlowMgr.SR_OnHostSlot(slot)` (-1 = none) | `[SR_FSCMD] #n SR_SaveEvent '<kind>' saveFlow+0x54=<v> -> slot <s> (was <s0>)` |
| `SR_DevMenu <verb> <page> ...` | the Dev Tools pages (docs/DEVTOOLS_DESIGN.md 13): the host's page model answers with several calls, `FlowMgr.SR_DTBegin(page, title, body)`, `SR_DTItem(row, label, kind)` per row, `SR_DTEnd(rows, focus, action)` | `[SR_FSCMD] #n SR_DevMenu '<args>' -> <n> call(s)` |
| `SR_<other>` | ignored | `[SR_FSCMD] #n unknown ...` |

Since iter-464 one command may answer with several calls, each with up to 3 int or string arguments (a string is copied into the same host-owned frame below r1 and made an Apt string by `AptObjectRecycleOrAlloc_sub_8269A210`, as `FlowMgr.CppShowPopup` does for its title). The mod binds each reply method around the fscommand only. Delivery logs one `[SR_FSCMD] reply ...` line per command.

`[SR_FSCMD]` lines are count-safe: one per command. A run with no `SR_*`-issuing mod has zero.
The game-options store format (one `key=value` per line, `#` comments; volumes 0..10 (absent = unknown, left alone), `control_scheme` 0..3, the rest 0/1; a missing non-volume game key = no store): `music_volume`, `sfx_volume`, `control_scheme`, `invert_x`, `invert_y`, `auto_center`, `vibration`, `apply_to_loaded_saves` (default 1; 0 = a loaded save keeps its own options, console behaviour). Where it is: `<save root>/settings/game_options.ini`, where the save root is the xam_content root that also holds the saves (docs/CUSTOM_FRONTEND.md section 8): `%LOCALAPPDATA%\SupermanRecomp\content` by default, `<exe dir>\content` when `<exe dir>\portable.txt` exists, `SR_CONTENT_DIR` when set. It is NOT under the content root that holds `mods/`. To change the host-only key `apply_to_loaded_saves`, edit that file (the menu has no row for it). It sits under the xam_content root, so an automated roll sees only its `SR_CONTENT_DIR` seed. PC settings (CUSTOM_FRONTEND 9.3) will extend the same channel.

## Building Apt (UI) mods

`tools/apt/build_custom_frontend.py` applies the tracked patch sources in
`tools/apt/mods/<mod>/*.patch` (+ `.aptasm` bodies; language: `tools/apt/patch_movie.py`)
to the user's own `DATA/ui.AST` and writes the result into `mods/<mod>/`. The generated
files are game-derived bytes and are gitignored (`/mods/`); only the sources are tracked.

```
python tools/apt/build_custom_frontend.py                      # custom_frontend, form A
python tools/apt/build_custom_frontend.py --mod e1_quit_text   # another tracked mod
python tools/apt/build_custom_frontend.py --enable             # ...and switch it on
```

Delivery forms (chosen by experiment E1, below):

- **Form A** `--form a` (default): the whole rebuilt `DATA/ui.AST` (89.6 MB). Two form-A mods
  do not compose: the higher-priority one wins the whole container.
- **Form B** `--form b`: one `DATA/_workinprogress/<MOVIE>.ast` per patched movie, the
  engine's own loose-file mount. **Rejected by E1 for the name `<TOC NAME>.ast`** (the file
  is listed, never opened). Other loose-file names are untested (see "Experiment E1"); the
  option stays for that follow-up.

## Verification

- `build/difftest.exe --mod-overlay-test`: 37 checks through the real guest `CreateFileA`
  / `ReadFile` (sub_824687F8 / sub_824685C8) and the `NtOpenFile` / `NtQueryDirectoryFile`
  / `NtQueryInformationFile` exports: off = disc bytes, priority order both ways,
  case-insensitive match, added files and folders, union listing, `..` and foreign paths
  refused, missing/bad names skipped, the `enabled.txt` grammar, the boot decision
  (`modComputeEnabledList`: enabled.txt read with no env, ignored under `SR_INSTANCE_ID`,
  `SR_MODS` / `SR_CUSTOM_FRONTEND` overrides), the names the disc does not hold (`sound1`, `fmv3`,
  `objtiv1`: no open, and a mod's real `objtiv1.AST` served, M7; until 2026-10-20 M7 checked the deleted
  alias), and
  difftest's own `SR_MODS` pin (M8), and the FSCommand bridge (M9a-f: `SR_*` handled with
  no mod enabled and under another mod name (my_menu); non-`SR_` commands left to the game; `SR_Log`, an unknown
  `SR_` command with null args and `SR_Quit` entered through the real strong def
  `sub_826D39A0`, exactly one quit request), and since step 8 M6h/M6i (`SR_MODS=@enabled.txt`). 62 passed, 0 failed (2026-09-30).
- `python tools/apt/rt_patch.py`: the build script's forms A and B, `--enable`, the mod
  name rule, `patch_movie --out` refusing the game folder, and (section Q) the runtime's
  unload contract with its mutations (32/32, 2026-09-30).
- An Apt content mod must survive a reload, not just a first load: the runtime renumbers
  every Push/ConstantPool slot on unload and a reload reuses those bytes, so the build
  stores `.const` items in the unload-walk order and refuses any output movie that fails
  `tools/apt/apt_unload.py` (all 820 movies are checked; docs/ENGINE_MAP.md "Apt writer
  invariants", docs/CUSTOM_FRONTEND.md 5.4 item 4).
- Alias roll (2026-09-30, `archive/v1_frontend/overlay_fix/alias_probe/attempt1/`, same
  scenario, `-Gates "SR_MODS=alias_probe"` with hard links of the disc's `sound.AST`,
  `objtiv.AST`, `fmv.AST`): all 10 alias opens (`sound1`, `objtiv1`, `fmv1..5`) opened the
  mod's file (handles 26, 31/32, 36-40), as did the direct opens; APT_REQ identical to
  e1_off (16 entries, same MD5); frame 040 PRESS START, 056 LOAD A GAME? popup. (Historical: the alias was
  deleted 2026-10-20, step4/audit batch 3.)
- Experiment E1 in the game: see below (form A identity, form A edit, form B).

## Experiment E1 (2026-09-30)

> **2026-10-02 correction: the form-B "REJECTED" verdict below is VOID.** In
> `archive/v1_frontend/overlay/e1_form_b/attempt1/run.log` 6274-6279 `SCRPRESSSTART.ast` came
> back in the SAME 244-byte NtQueryDirectoryFile call as `GAMECONFIG` and `__ASSET__GAMECONFIG`,
> then NO_MORE_FILES; the game converts only the first record per call, so it only ever saw
> GAMECONFIG (the owner's private mod project research/modsystem/DESIGN.md finding 2, three adversarial
> verdicts CONFIRMED). The name tried was right (Apt packages are looked up by the bare name
> `ScrPressStart`, type ast). The listing is fixed by SR_NTQDIR_XENIA (section 12); E2a re-runs
> form B on the fixed listing (PROGRESS.md iter-464 modcore).

Five sequential rolls, one process each, same scenario
(`tools/scenarios/mod_title_capture.json`: reach PRESS START, 20 s on the title, one START,
25 s on the boot-flow popup), exe built from this commit's tree. Artifacts:
`archive/v1_frontend/overlay/<roll>/attempt1/` (run.log, steps.log, frames/, apt_req.txt).
Tag: **mod:<name>** for every roll except e1_off (natural).

```
powershell -File tools/sr_drive.ps1 -Scenario tools/scenarios/mod_title_capture.json   -Out archive/v1_frontend/overlay/<roll> -TimeoutSeconds 360   -Gates "SR_MODS=<mod>,SR_PRESENT_DUMP_DIR=archive/v1_frontend/overlay/<roll>/frames"
```

| Roll | Mod (built by) | `[MOD]` log lines | APT_REQ list vs e1_off | Frame looked at | Result |
|---|---|---|---|---|---|
| e1_off | none (no `SR_MODS`) | 0 | reference (16 requests, ends ScrGenPop) | 038: title reads PRESS START; 046: LOAD A GAME? popup | natural baseline |
| e1_identity | `custom_frontend` = no patches, byte-identical ui.AST (`build_custom_frontend.py`) | 2: enabled, `shadows 'DATA/ui.AST' -> mods/custom_frontend/DATA/ui.AST (89595420 bytes)`; NtCreateFile handle 33 is the mod file | identical | 040: PRESS START; 049: LOAD A GAME? popup | **pass**: the overlay serves ui.AST, the natural FE is unchanged |
| e1_edit_a | `e1_quit_text` form A (`--mod e1_quit_text`, 89595436 B, 827/827 other entries byte-identical) | 2: enabled, `shadows 'DATA/ui.AST' ... (89595436 bytes)` | identical | 040: title reads **QUIT GAME**; 048: LOAD A GAME? popup unchanged | **pass**: an edited movie runs in the game |
| e1_edit_a2 | same, re-run on the final exe | 2 | identical | 039 (crop `title_text_crop.png`): **QUIT GAME** | pass (2/2 samples) |
| e1_form_b | `e1_quit_text_b` form B: `DATA/_workinprogress/SCRPRESSSTART.ast` (445704 B, the movie's nested container) | 1: enabled only | identical | 040: PRESS START | **fail**: the loose mount's boot enumeration listed `SCRPRESSSTART.ast` (NtQueryDirectoryFile entry[2], size 445704) but nothing ever opened it; ScrPressStart still loaded from ui.AST |

**Decision: form A.** The build tool emits form A by default. Form B is rejected **only for
the name tried**, `<TOC NAME>.ast` in `DATA/_workinprogress/`. Supporting observation: the two
loose files already in the disc's `_workinprogress/` (`GAMECONFIG`, `__ASSET__GAMECONFIG`)
are also listed at boot and never opened in any of the five rolls (0 NtCreateFile of a
`_workinprogress/<file>` path). What the loose storage would serve, if anything, is *not
checked* (IDA comment at `MountLocalizedBigFiles_sub_8218D128` 0x8218D1A0). `--form b` stays
in the tool for that follow-up.

Untested per-movie routes (IDA, `AptResNameParseToDescriptor_sub_826EA798`, reached from
`FileStorageScanDirRecursive_sub_826EB970`): a loose file is keyed by the extension's storage
type plus a 64-bit id. The id is either
- `ResNameHashFnv1a64NoCase_sub_82702FC0` of the name up to the `.`; whether the Apt request
  for ScrPressStart looks up the hash of `SCRPRESSSTART` or of a longer name (for example the
  request path `screens/bootflow/ScrPressStart`) is not checked; or
- an explicit id from a `__ASSET__<16 hex digits>` name (the form the disc's own
  `__ASSET__GAMECONFIG` uses), e.g. `__ASSET__<the movie's TOC id>.ast`. Never tried.

The game also probes loose files the disc does not have: `BaseWorldFlow_PHYSICS.AST` and
`BaseWorldFlow_TEMP.AST` in the disc root, and `DATA/parammap.AST`, `arenas.AST`, `xml.AST`,
`pus.AST` (all FILE_NOT_FOUND in e1_off). A mod can add those names through the overlay;
what the game does with them is not checked.

Consequence for composing mods: form-A mods that both patch `ui.AST` do not compose (the
higher-priority one wins the whole file). A composable per-movie route has **not** been ruled
out (above); merging several Apt mods into one ui.AST at build time is the fallback.

## 12. The directory listing (platform fix, SR_NTQDIR_XENIA)

`stub_NtQueryDirectoryFile` answered with as many FILE_DIRECTORY_INFORMATION records as fit the
caller's buffer and ignored the FileName mask (r10) and RestartScan (the 9th argument). The
game's `FindFirstFileA` (`sub_828A7D50` -> `sub_8246A640`: a 328-byte buffer, the mask after the
last `\`, `*.*` sent as an empty mask) and `FindNextFileA` (`sub_828A7C10` -> `sub_8246A7A8`: 328
bytes, no mask, RestartScan 0) convert only the FIRST record of each call (`sub_828A8640` never
reads NextEntryOffset), so the game saw about one entry in four. The Xenia port
(`xboxkrnl_io.cc NtQueryDirectoryFile_entry`, `xfile.cc XFile::QueryDirectory`): one record per
call, a new mask sets the rule and restarts, RestartScan restarts, `length < 72` ->
STATUS_INFO_LENGTH_MISMATCH, no match for a new rule -> STATUS_NO_SUCH_FILE, the end ->
STATUS_NO_MORE_FILES, IOSB `{status, length}` on success. `[NTQDIR]` log lines (first 400).
Classified `platform-fix` (it implements the console's listing; the guest makes every decision).

### Natural directory acceptance, 2026-10-02

`SR_NTQDIR_XENIA` now defaults **ON**; `=0` retains the broken packing path solely
for developer comparison. This is a platform fix, not an optional player mod.
On `mods/ship`, natural no-input boots with empty saves, no mods/port features,
no dev tools and forcing off gave the following **guest FileStorage contents**:

| Source | DATA-root AST entries | Missing disc AST IDs |
| --- | ---: | ---: |
| Xenia `xenia_canary_sound_menu.DMP` | 12 | 0 |
| Snapshot `49660f7bf193-native-costumes-dircheck`, gate 0 | 3 | 9 |
| Same snapshot, gate 1 | 12 | 0 |
| Snapshot `1ae84e34f9c3-directory-default`, variable absent | 12 | 0 |

The old claim of four ASTs included the `_workinprogress` directory: the live
table contains `baseworl`, `metropol`, and `text`, not four archives. All three live
scenarios completed the FE-tick readiness condition and 90-second observation
window. This verifies discovery, **not** gameplay, loose-asset decoding, skins,
or a visual milestone. Intro/logo/title-movie contact sheets were inspected;
their current `verify_run.py` time labels are present-index/60 estimates, not
measured wall time. No frame-rate claim is based on them.

Evidence: main checkout `archive/directory_natural_20261002/{off,on,default}/attempt1/`;
lane checkout `archive/directory_natural_20261002/{xenia_golden,live_off,live_on,live_default}.json`.
`tools/mod_storage_probe.py` reads the actual manager (`*829A2430`, vtable
`8208AE30`) and FileStorage (`8208A078`, priority 50) entry vectors, matching all
disc stems by FNV-1a64 ID. It does not count capped `[NTQDIR]` log lines, install
hooks, write game memory or call guest functions. Six decoder controls cover
missing entries, wrong priority, malformed/racing vectors and vacuous input.
Use the Python environment with Frida installed (on this machine Python313).

Reproduce: take the shared game slot, then run main `tools/sr_drive.ps1` with
`-Scenario <worktree>/tools/scenarios/mod_directory_natural.json`,
a fresh `-Out`, `-TimeoutSeconds 240`, the immutable lane `-ExePath`, and
`-Gates "SR_INSTANCE_ID=<private-id>,SR_NTQDIR_XENIA=0,SDL_AUDIODRIVER=disk"`.
During its observation window run:

```powershell
python tools/mod_storage_probe.py --pid <launched-pid> --disc '<repo>/Superman Returns (USA)' --out archive/<unique>.json
```

Release the slot in `finally`. Repeat sequentially with `=1`; for the default
check remove that environment variable entirely. The golden comparator uses
`--golden <absolute-dump-path>` instead of `--pid`. Output files are exclusive,
and incomplete/changed vectors fail rather than silently passing.
Rebuilt tests: `difftest --mod-overlay-test` 164 passed/0 failed;
`--mod-manager-test` 128 passed/0 failed. The forcing audit retains its two
inherited findings (`archive/directory_default_forcing.log`), not a clean audit.
This default is in the `mods/ship` lane, not yet promoted to dev.


## Additive character packs (owner decision, 2026-10-02)

Character packs must coexist and ADD selectable entries, never consume a retail skin slot.
Their `mod.json` may declare `costumes`: an array of `{ "id": "original", "name":
"EXAMPLE SUIT", "entity": "SRM_<mod FNV64>_ORIGINAL_ENT" }` objects.
`modAssetPrefix(modId)` / `srmods.asset_prefix(modId)` supplies the case-insensitive,
globally scoped prefix. Use it for entity, body, cape, texture and animation asset stems.
Local costume ids and entity stems must be unique within a pack; unscoped retail entities
such as `SupermanEnt` are rejected. Extra texture/animation mods can deliberately override
these names and declare their base-pack dependency; they do not declare the same costume again.

`modCostumeList()` aggregates mounted packs, ordered by stable `<mod>/<costume>` identity.
Boot-local DAS slots start at 5 (retail Costume1..4 are reserved); selection persistence
must store the stable identity, not this temporary slot number. Disabling/removing a pack
removes only its entries, with a retail fallback if the selected stable id is unavailable.
No changes are made to the source disc or original character rips.

**Current implementation boundary:** manifest validation, aggregated catalog and a shared
DAS composer exist. The composer preserves the four retail suits and appends enabled packs
in one generated loose `.das`. Native/Python differential checks use the required retail
attribute document and two character packs plus a dependent texture add-on; priority
changes, disabled/removed packs, retail replacement rejection and a natural no-mod launch
are covered. The picker/selection bridge, native model/material cooking and real converted
character/cape game checks are still pending. Registry tests are not proof that a costume
can be selected or rendered in game.

### Integration evidence (2026-10-02, WIP `mods/ship`)

- Native manager: 64 passed / 0 failed. Overlay, real guest file/listing wrappers and menu:
  164 passed / 0 failed, including release-name relocation, spaces/Unicode, CWD precedence,
  exact manager/menu path agreement, folder-open failure, and refusal of unresolved relative
  folders. Real guest file reads and directory enumeration, managed/legacy mod file reads,
  menu file details and the legacy list-file save also pass under a Unicode installation
  path. The file-read regression was first shown failing (158 / 1), then fixed with UTF-8
  host paths and Windows wide-character file opens. DevTools regression: 33 / 0.
  Python/native mod differential: 19 / 0. Apt patch regressions: 32 / 0.
- `package_port.py --check`: all seven frontend assets and the separate autosave script
  regenerate byte-exactly from the player's disc. No original disc data is edited.
- Combined loose frontend/menu smoke `modcore_menu_smoke_20261002_hold`: all 10 original
  steps passed, MODS rendered through the real mod-manager catalog. Snapshot
  `153727487247-modcore-registry`; artifacts under the main checkout's `archive/`.
  This is port-feature-assisted UI evidence, not a natural baseline or character acceptance.
- Relocated polished smoke `modcore_menu_smoke_20261002_release`: 13 / 13 steps passed;
  the exe under `Superman Returns/bin/` used that installation's mods folder rather than
  the unrelated working directory. Both MODS and install help were visually inspected from
  presented GPU frames; final copy removes the awkward final-word wrap seen in this pass.
- Final smoke `modcore_menu_smoke_20261002_final`, snapshot
  `b314968944a5-modmenu-release-final`: 13 / 13 passed after the Unicode file-open fix and
  final copy changes. Presented frames 062 (MODS) and 064 (help) were inspected: four-line
  empty state, three separate actions, wrapped detected path ending `Superman Returns\mods`,
  no overlapping text or orphaned final word. This validates the revised menu in game;
  Explorer opening is exercised via a path-preserving test seam, not automated desktop input.
- Override manifest regenerated: no needs-review entries; host-launch audit passes.
  The earlier full forcing audit ended without a recovered exit receipt and is not claimed
  as passing. These host-path/UI changes add no guest override or guest-state mutation.
- The first short-A-pulse attempt did not open MODS; an observed, longer pulse did. The
  polished relocated smoke additionally checks the help page. Loader and listing integration
  remain opt-in (`SR_MOD_MANAGER=1`, `SR_NTQDIR_XENIA=1`); nothing is merged into main yet.

## Built-in framework and inclusion checkpoint (2026-10-02 evening)

- Native manager: 113 checks pass; Python/native differential: 33 pass; overlay:
  164 pass. The new per-suit exclusion/fallback checks failed before implementation.
  A two-suit pack plus an independent pack survives individual exclusions, pack
  disable/re-enable and slot reassignment. Failed writes and automated runs cannot
  change saved inclusion. Running content remains immutable until restart.
- Actual `modBridgeDispatchEx` tests exposed missing routing for `SR_Mod*` metadata
  commands: direct manager-command tests had not exercised the game UI entrypoint.
  That route is now connected and tested, including explicit refusal replies.
- Relocated smoke `archive/modcore_menu_smoke_20261002_baked/attempt1` in the main
  checkout used snapshot `99b82d80743b-baked-mod-library`, with no manager gate,
  named frontend mod or source-checkout feature path. Bundled frontend generation
  and the custom main menu worked. **The Mods navigation step failed:** only one
  of three short DOWN pulses reached the guest, and the AS receipt selected LOAD
  GAME (button 1); the presented frame shows empty load slots. This is not a Mods
  acceptance result. The scenario now checks three separate delivered UI steps;
  the revised scenario still needs a rerun. Listing was explicitly enabled in
  this smoke, so it does not prove a no-flag player launch yet.
- Builds succeeded; vcpkg still warns that no dependency-inspection executable
  (`dumpbin`/`llvm-objdump`/`objdump`) is available. Snapshot dependencies were
  included and the snapshot ran. Credits check and host-launch audit pass. The
  forcing audit still reports the two inherited `gpu_ring_alloc_fix.cpp` sites;
  no clean full forcing-audit claim is made.

## Fortress costumes: mod suits in the game's own COSTUMES screen (iter-465, fortress lane)

This replaces the restart-to-wear picker and the "queued" draft of the screen extension above.
Retail ground truth and the switch contract: `docs/ENGINE_MAP.md` "Fortress COSTUMES and the
costume switch". The MODS menu chooses which packs are installed; it never chooses what
Superman wears.

**What the player sees.** Fortress of Solitude > COSTUMES lists the three retail suits and,
after them, every included mod suit (in display-name order, case-insensitive, the stable id
breaking ties; built-in Bizarro last), each with its own thumbnail, full-size picture (A), name
and description. Saves and the worn note keep the stable id, never a list position or appended
slot. More than three suits scroll in rows of three (the retail ThumbViewer does
this itself). With mod suits installed, the suit Superman starts in ("Superman Returns") is
always wearable, so a player can always go back. X (WEAR):

- a suit that moves on the animations this game session loaded (the retail suits and every mod
  suit without own banks, or the suits sharing the worn suit's own banks): "NEW COSTUME
  SELECTED!" and Superman wears it, exactly like a retail suit;
- a suit with other animations (an own-proportion suit from a retail session, or a retail suit
  while an own-proportion suit is worn): the description says "Superman wears this suit from the
  next start of the game: it moves with different animations." The choice is saved; the next
  start loads that suit's animations and Superman wears it.

**Why the next start.** The game reads Superman's animation banks (the six `SUPERMAN_*` banks
inside `baseworl.AST`) once, before the title screen, and never again; a second set does not fit
the animation memory. An own-proportion suit on the retail banks (or a retail suit on its banks)
is stretched or squashed, so the port never shows that.

**Persistence (a port feature; retail forgets the suit).** The worn suit's stable id is saved in
`<save root>/settings/costume.ini` (`worn=retail/2` or `worn=<mod>/<costume>`) and is worn again
after a load, a quit to the front end, the return from an arena and the next start. Retail resets
to the default suit at those points (BaseWorldFlow S_7E412353, LoadMetropolis); the port keeps
the player's choice instead. Story overrides (Warworld's Pod suit) still happen. A worn mod suit
that is no longer installed or included, or whose banks cannot be applied, falls back to the
retail default suit and the retail banks at the next start.

**Authoring a suit.** In the pack's `mod.json`:

```json
"costumes": [{
  "id": "original", "name": "EXAMPLE SUIT", "entity": "SRM_<prefix>ENT",
  "description": "Shown under the picture on the COSTUMES screen.",
  "thumbnail_image": "previews/original_thumb.dds",
  "fullsize_image": "previews/original_full.dds",
  "animation_banks": {
    "SUPERMAN_FLIGHT": "banks/SUPERMAN_FLIGHT.bin",
    "SUPERMAN_ONFOOT": "banks/SUPERMAN_ONFOOT.bin",
    "SUPERMAN_MELEEGROUND": "banks/SUPERMAN_MELEEGROUND.bin",
    "SUPERMAN_SUPER_POWERS": "banks/SUPERMAN_SUPER_POWERS.bin",
    "SUPERMAN_SUPERSPEED_ATTACKS": "banks/SUPERMAN_SUPERSPEED_ATTACKS.bin",
    "SUPERMAN_MELEEAIR": "banks/SUPERMAN_MELEEAIR.bin"
  }
}]
```

- The entity keeps `animsys project="SupermanAnims"` (the retail CCT, joint aimer and
  `clothanimdef="SupermanCape"`); only the bank bytes differ.
- A cape must fit the engine's own cloth limit (at most 200 vertices; the packs tested so far use 187).
  `SR_CLOTH_CAPACITY` (a resized cloth body for larger capes) is a **retired experiment**: default
  off for players and rolls, `=1` keeps it for A/B (`kernel/cloth_capacity.cpp`, docs/OVERRIDES.json).
- `animation_banks` is optional: without it the suit moves on the retail banks. Each file is a
  Superman-skeleton bank (kind 9, skeleton hash 0xE3D7527A) no larger than the retail bank it
  replaces, outside `DATA/`. The pack never ships a `baseworl.AST`: the exe writes the banks into
  the player's own file at boot (in place, offsets unchanged) when the suit is worn.
- Suits whose bank files are byte-identical (in one pack or several) share one family
  (`banks_<hash>`) and switch among themselves at once.
- `animation_project` (an own project) stays experimental behind `SR_COSTUME_PROJECT_PRELOADS=1`
  and is never switched to in game (a second project does not fit the animation memory).
- Pictures: `python tools/srmods/costume_preview.py --pack mods/<mod> --costume <id> --image
  render.png [--key-background | --hero-shot]` makes the 128x128 and 512x512 DXT5 .dds files
  (outside DATA/) and fills the two fields. `--hero-shot` takes an in-game screenshot (a pack's
  preview.png): it crops a square around the suit and brightens it like the retail thumbnails.
- The name line on the screen is one line: a long name makes the whole screen use a smaller
  name font (down to 16); over about 33 capitals it is cut with "..." and the full name opens the
  description. The description shows about 110 characters (cut at a word); keep it short there,
  the MODS library shows the whole text. A suit's bank and picture files never count as mod
  conflicts (the exe reads them; they are not game files). At boot the exe wraps them into the screen's Apt movies, using the
  player's own disc COSTUMETHUMB_1 / COSTUMEFULLSIZE_1 as the container (only the picture and the
  inner ids change; nothing from the disc is redistributed). A suit without pictures gets a
  generic card. `thumbnail` / `fullsize` (pack-built Apt movie stems) still win when present.

**How it is built.**

| Piece | Where |
|---|---|
| Screen: append records after `GetCostumeData`, WEAR on an appended record, retail-record route, record 0 wearable | `tools/apt/mods/custom_frontend/50_costumes.patch`, `srcostumes.aptasm`, `costumes_data.aptasm`, `costumes_input.aptasm` -> `data/port_features/custom_frontend/SCRCOSTUMES.srpatch` |
| `SR_ModCostumes` (records), `SR_CostumeWear <id>` (DAS setter + retail popup, or the next-start note), `SR_CostumeWorn <retail index>` (replies whether the retail call may switch now) | `kernel/mod_manager.cpp`, `kernel/mod_bridge.cpp` |
| Generated registry layer `_costume_registry`: Costume5..N, SRCostumeFamily/SRActiveFamily/SRWornCostume/SRAppliedTo, the switcher Lua, the two world-flow Lua edits, preview movies, the worn own-bank suit's `DATA/baseworl.AST` | `kernel/mod_costume_data.cpp`, `kernel/mod_costume_preview.cpp` (Python reference `tools/srmods/costume_data.py`) |
| `[COSTUME_WATCH]` log of VisRep/AnimSys/spawns around a switch (observe only) | `kernel/costume_watch.cpp` |

Gate: `SR_NATIVE_COSTUMES`; unset, it follows the PORT-FEATURE switch (on for players, off for
natural rolls; `kernel/host_automation.hpp portFeaturesFor`).

**Status (2026-10-04).** Live, end to end (docs/ENGINE_MAP.md "In-Fortress WEAR, live"): the
COSTUMES screen with retail + mod suits and previews, WEAR within the family (immediate, retail
popup, worn in Metropolis), back to retail, WEAR across families (the next-start note, then the
restart wears it), the worn suit after a reload, and the fallback when the worn pack is missing.
Keep suit names short: a long name wraps into the description field.

## Character packs: each character on its own animation project (approach C, iter-465)

Owner decision 2026-10-04: a suit or a whole character (later: other fighters with
rebuilt movesets) is its OWN actor entity with its OWN animation project, switched in the way
PlayableBizarro switches Superman (spawn + handover, then back). The costume lane makes the
content; the charswitch lane does the engine side: the animation pool, the handover at a
Fortress WEAR, and the switch back. Memory facts: `the owner's private mod project/research/costumes/ANIMATION_BINDING.md`
"Plan B data" (HeapAnimation 4 MiB, 0.91 MiB free in gameplay, about 2.44 MB per resident project).
The parked alternative (one bank set + a runtime pose retarget, `SR_RUNTIME_RETARGET`, default
OFF) is branch `feat/runtime-retarget` ce310aa4.

**Names.** `<P>` = the pack's asset prefix (`SRM_<FNV-1a64(mod id), 16 hex>_`), `<C>` = the character
id in capitals plus `_` (it may be empty for a one-character pack). Loose files under
`DATA/_workinprogress/<mod>/`:

| File | Content |
|---|---|
| `<P><C>ENT.xml` | the actor entity (below) |
| `<P><C>ANIMS.xml` | `<project name="<P><C>ANIMS" rcbfile="<P><C>CCT">` + banks `<P><C>FLIGHT`, `<P><C>ONFOOT`, `<P><C>MELEEGROUND`, `<P><C>SUPER_POWERS`, `<P><C>SUPERSPEED_ATTACKS`, `<P><C>MELEEAIR` in this (retail) order, retail clip names, events and timing; moveset banks are appended after them |
| `<P><C>CCT.rcb` | a byte-identical clone of SUPERMANCCTPROJECT (10,544 B, kind 22, skeleton 0xE3D7527A): its own resource/container, same 103 bones |
| `<P><C><SET>.bnk` | the banks (loose type 0xC9A0B102): header +08 = FNV-1(bank name), +0C = 0xE3D7527A, frame-cache block hashes unique against retail and every other pack; own rest offsets (lexbanks) + rest-pose rotation compensation (`--rest-align legs,arms`, restalign.py) |
| `<P><C>BODY.rx2` | the skinned model (129 joints, bound by name to the 103 animated bones) |
| `<P><C>CLOTH.xml`, `<P><C>CAPE_RSS.rss`, `<P><C>CAPE_COL.dds` | caped characters only: the retail-template cape (at most 200 cloth vertices) |
| `<P><C>MIM_BIZ.bnk`, `<P><C>MIM_MTO.bnk`, `<P><C>TWISTER_IGC.bnk`, `<P><C>WW_IGC.bnk` | own-offset copies of the mission banks that missions append to SupermanAnims (PLAYABLEBIZ/BIZOBJ15/ME34/ME36, MTOOBJ09, TWSOBJ04, WAROBJ01); the charswitch side appends them to `<P><C>ANIMS` when that mission's association loads (until then: back to Superman in those flows). `tools/rw4/add_mission_banks.py` |

**Planned cape compatibility (owner, 2026-10-11; not implemented):** mod capes need further visual
refinement and must work with the native cape improvements selected for the stock costume (RELEASE_PLAN
phase 6 item 11). Use the same native cape implementation with validated per-costume geometry, materials,
attachment / pin and body-collision data. Preserve costume appearance, support any required conversion and
recheck affected mod cape attachments after mod-skin arm retargeting; stock arm alignment remains unchanged.
The table above describes the current asset contract; extending
mesh capacity or changing the solver requires validation and documented authoring support before release.

**The entity** is retail `SupermanEnt` (SUPERMENACTORWORLDFLOW_OTHER/SUPERMANENT) with three
changes - `visrep asset="<P><C>BODY"`, `visrep clothdef="<P><C>CLOTH"` (caped; a capeless character uses
`visrep type="SkinnedModel"` and drops `clothdef` and `animsys clothanimdef`) and `animsys
project="<P><C>ANIMS"`. Inherited verbatim, so health, powers, combat and movement are Superman's:
`type="Actor" scale="1" filter="SUPERMAN|CAMERATARGET|TRAFFICOBSTACLE"`, the `audiorep` (ACTOR,
SND_CORE_COMBAT_BODY_HIT, SND_FLIGHT_ENGINE / SND_AMBIENCE_NORMAL / SND_MUSIC), `physrep CHARPROXY`
(vertexa 0 0.4 0, vertexb 0 -0.83 0, radius 0.3, mass 200, maxslope 60), `animsys statemgr=""
jointaimer="SupermanJointAimer" clothanimdef="SupermanCape"`, `attribsys SupermanAttributes`, `fsm
SupermanFSM_g_21A4B50D`, `linkagesys type="Superman"`, `movesys type="Superman"`, `powersys
SupermanPowers`, `targetsys SupermanTargetSys`. (PlayableBizarroEnt differs: filter adds
FEARSOURCE|BIZ, own powersys/targetsys, and it keeps `project="SupermanAnims"`.)

**mod.json.** One `costumes` entry per character: `id`, `name`, `entity: "<P><C>ENT"`,
`animation_project: "<P><C>ANIMS"`, pictures as today; no `animation_banks` (the two exclude each
other). The project is an ANIM preload (`<preload type="ANIM"><file name="<P><C>ANIMS"/>`, composed by
`modComposeCostumePreloads`, today behind `SR_COSTUME_PROJECT_PRELOADS`); when it loads (boot or at
WEAR) is the charswitch lane's call. Several entities may name one project: the nine
Superman suits of one pack series share one bank set (body bones agree within 2.4 mm), so one resident project
serves the family; Zod gets his own for his own moveset.

**Generator.** `the owner's private mod project/tools/rw4/write_animation_descriptor.py` writes the project XML,
scoped CCT, entity and cloth from a work folder's banks receipt
(`tools/rw4/build_source_banks.py --namespace ... [--rest-align legs,arms]`).

**Open (runtime checks for the first live switch):** the joint aimer, IK and attachments with own
rest offsets (the cloned CCT keeps retail inverse binds), SupermanCape cloth animation on the own
cape roots, and isolation of the scoped CCT's mutable maps (the descriptor records
`runtime_mutable_container_verified: false`).

## Character switch: wearing a whole character (charswitch lane, iter-465)

The engine side of "Character packs" above, plus the built-in Bizarro. A suit is a **character**
when it names an `animation_project` (a pack's own project) or is a built-in retail character. WEAR
on a character spawns that character's entity where Superman stands and hands the player to it,
the way the game's PLAYABLEBIZ mission swaps Superman for Bizarro. Every other suit still uses the
retail model swap (`ReplaceEntityVisRep`).

**Animation memory (kernel/anim_heap.cpp, PORT FEATURE, OVERRIDES.json `sub_82186500`).** All
projects' banks live in HeapAnimation (4 MiB, size dword `0x829118A0`, read once by
MemoryPoolSystemInit). Retail fills about 3.1 MiB, so a second ~2.5 MB project used to fail its loads
and hang the game. When characters are preloaded, the exe raises that dword by their bank and CCT
bytes plus 1 MiB before the game reads it: two test projects took it to 10 MiB, two plus Bizarro to
11 MiB (`[ANIM_HEAP]` boot line). Without a character, and on every natural roll, nothing is written.
`SR_ANIM_HEAP_MB=<n>` sets the size explicitly for A/B.

**Loading.** Each character's project is an ANIM entry in BASEWORLDFLOWPRELOADS
(`modComposeCostumePreloads`), so it loads at the start of play with Superman's own banks and stays
resident. Nothing loads at WEAR, so the switch never waits for a disc read.

**The switch (actor flow Lua, `kernel/mod_costume_data.cpp` kCostumeSwitchLua; the game's own
natives only).** DAS per suit: `SRCostumeChar<n>` (1 = character). Session state: `SRCharID` (the
character out now) and `SRSupesID` (the parked Superman).
- Superman -> character: Superman is parked like PLAYABLEBIZ parks him (`ClearFilter` SUPERMAN /
  CAMERATARGET / TRAFFICOBSTACLE, `DisableVisRep`, `PauseFSM`, moved to 0,0,0). Then
  `SpawnEntityActive(entity, his position, his orientation)`. The new actor gets `ApplyPowerups`,
  his `StaminaPoints`, `SelectedPower` and `CurMoveMode` (1 = flying: plus `RestartFSM`, so the
  FSM's DetermineMovement starts in flight), then `SetCameraHomeEntity` and
  `SetActorToDefaultController`. Because the character carries the SUPERMAN filter, every flow that
  looks Superman up (`GetIDFromBinIndex("SUPERMAN", 0)`, which is all of them except PLAYABLEBIZ
  itself) now finds the character.
- Character -> character: the old one loses the filter, the new one is spawned in its place and
  given the same handover, and the old one is despawned.
- Character -> a model suit: Superman is restored (filters, `EnableVisRep`, unpause, position and
  orientation, `ResetActor`, handover), the character is despawned, and the model swap runs.
- When the game respawns Superman (a load, the reopen) while a character is out, the pair is
  noticed (`EntityIDExists`, `GetBinSize("SUPERMAN") > 1`) and the worn character is applied again.
  This never happens in the front end's attract world (BaseWorldFlow `gameStarted`).
  LoadMetropolis never puts a character's model on Superman.
- The worn character is saved in `settings/costume.ini` like any suit, and is spawned again after a
  load or a restart.

**Built-in Bizarro (`SR_BUILTIN_CHARACTERS`, unset = native costumes + character switch).** At
boot the exe reads the player's own `objtiv.AST` PLAYABLEBIZ bundle. The entries Bizarro needs
outside his mission are written as loose files into the generated costume registry layer:
PlayableBizarroEnt, his cloth cape, powers and the Bizarro-only power includes, target system, the
BizarroExtraAnims / SingleJointAnims / BIZ01SupermanAnims projects and their banks, X2_BIZARRO and
the texture, shader and VFX arenas. Compiled XML is turned back into text (`modCompiledXmlToText`).
His projects and arenas become BASEWORLDFLOWPRELOADS entries, and his banks count toward the
animation heap. The COSTUMES screen gets a BIZARRO record. Nothing from the disc is shipped.

**Suit unlocks: the game's own rules, plus the game's own cheats (owner decision 2026-10-05).** Retail
suits unlock only by the game's challenges: the MSTobj03 / TWSOBJ04 / BIZOBJ15 reward states
(`AwardCostume` + `UnlockCostume`). Mod suits and characters, including built-in Bizarro, are never
locked: they have no retail challenge. Unlocking every suit is a cheat the player chooses: **pause >
MORE OPTIONS > CHEATS** (`kernel/cheats_menu.cpp`, a port feature on the shared host-page renderer).

The page lists the game's own cheats: the disc's preload `CheatDefs`, 13 button codes, parsed by
`ParseCheatConfigNode_sub_821F89A0` into the cheat manager `*0x8296DF30`.
- UNLOCK ALL SUITS = EXTRA_UNLOCK. It awards every costume, trophy and theater item to the save
  (`costumeUnlocked` / `trophiesUnlocked` bits), so it is kept once the player saves.
- The others are the game's ON/OFF switches: INFINITE STAMINA, INFINITE CITY HEALTH, ALL POWER
  UPGRADES, FAST CITY, LOW GRAVITY, BIZARRO UNLEASHED (the WB tie-in code: the Bizarro rampage) and
  DISGUISE (the five joke models SUPERMAN_SKIN1-5: phone booth, two vending machines, planter, ice
  chest).

How a cheat is entered (owner decision 2026-10-06, charter 2a: the game's functions are called
directly, with no virtual presses):
- Choosing a row closes the page. On the next UI tick, `cheatsMenuOnUiTick` runs on the game's own
  thread, after `PauseFlowStateTick_sub_82151298`, where the game's matcher
  (`CheatMgr_FeedPadButton_sub_82237360`) runs. It is called from that function's override, whose
  OVERRIDES.json entry is `port-feature`.
- It runs the matcher's own condition, in the matcher's order: the world flow's
  `objectiveActivated` (`sub_826CD320` + `sub_82194E48`) is 0, then objectiveStatus bit 6
  (WARobj02) through `WorldFlow_IsObjectiveStatusBitSet_sub_82224EE0(*0x8296DF5C, 6)`.
- If the condition passes, it makes the matcher's own effect call, `sub_82236840(cheatMgr, index,
  1)`, with the sound. If it fails, nothing is called, and the page says so the next time it opens.
- It is off unless port features are on, so natural rolls never call it.
- Parity with the pad path: every change of a cheat's ON flag logs a `[CHEATS] state` line (the
  ON flags and the save's costume / trophy / video / power-up unlock bits), however the cheat was
  entered. `archive/v1_charswitch/cheat_parity.py` compares a roll that types the codes on the pad
  (`cheat_pad.py`, the game's own matcher) with a roll that uses the page, from the same save.

Parity and live proof of the direct call (2026-10-06, snapshot 8a4d95671150, seed s9: a
chapter-2 save with no unlocks; `archive/v1_charswitch/rolls/p1`, `rolls/d1`, `view/d1_*`):
- Offline, `python tools/test_cheats_direct_call.py` checks the page against the matcher's own
  recompiled body. The calls, arguments and condition must match (18/18). Changing the bit 6 to 7
  in the page makes the test fail.
- Live parity: in roll p1, EXTRA_UNLOCK (3) and then INFINITE_STAMINA (1) were typed on the pad,
  where the game's matcher took them. In roll d1, the same two were chosen on the CHEATS page,
  which called `sub_82236840(mgr, 2, 1)` and then `(mgr, 1, 1)`.
  `cheat_parity.py p1 d1 --states 2` reports PARITY OK. Both rolls ended with ON flags `0A` and
  costume / trophy / video unlocks `FFFFFFFF`.
- After the cheat, COSTUMES lists POD SUIT and GOLDEN AGE SUPERMAN (`d1_costumes`), and the page
  shows UNLOCK ALL SUITS: DONE and INFINITE STAMINA: ON (`d1_cheats_after`).
- Refusal: in a NEW GAME, during MSTobj01, INFINITE CITY HEALTH was refused by the game's own
  check (`objectiveActivated 1`). Nothing was called, and the page shows the note above its text
  (`d1_refusal`).

Proof, roll c8 (2026-10-05, `archive/v1_charswitch/view/c8_*`; seed: a chapter-2 save with no
unlocks):
- Loaded, then UNLOCK ALL SUITS: `[CHEATS] code 3 entered: 0 -> 1`. COSTUMES lists POD SUIT and
  GOLDEN AGE SUPERMAN, which were `???` before.
- WEAR GOLDEN AGE switched at once; the suit shows from the front in gameplay.
- Saved to slot 2 and reloaded slot 2: the suit and the unlocks were kept.
- Reloaded slot 1, the untouched save: still `???`, as earned.
- NEW GAME, with an objective active, then INFINITE STAMINA: `code 1 entered: 0 -> 0`. CHEATS then
  shows the refusal note.

**A worn retail suit follows the loaded save.** `costume.ini` keeps the worn suit per install. A
retail Costume2 or Costume3 is put on only while the loaded save has it unlocked: BaseWorldFlow
`costumeUnlocked` bit n-1, the bit Fortress COSTUMES reads (`sub_82206F08`). Otherwise Superman
wears the default suit.
- How it reads the bit: through the game's `GetWorldFlowBitfieldValue("BaseWorldFlow",
  "costumeUnlocked", n-1)` (`sub_82207C00`). `GetWorldFlowDASValue` returns nothing for a Bitfield.
  The check only reads the bits.
- When it runs: on every tick of the actor flow's switcher (and in LoadMetropolis). The save's bits
  arrive after Superman spawns (roll c14: the bit read 0 during the load and 1 later). A load from
  the pause menu may still have the previous save's bits at LoadMetropolis.
- `SRDressedSuit` records the suit he has on. Only a change of the wanted suit, or a new Superman,
  replaces the model.
- `SRWornCostume` and `costume.ini` do not change, so loading a save with the suit unlocked dresses
  him in it again.
- Mod suits and characters, built-in Bizarro included, are never locked.

Proof, roll c14 (2026-10-05, `archive/v1_charswitch/view/c14_sheet.png`; seed: c8's two saves,
worn `retail/3`):
1. Main menu LOAD slot 2 (Golden Age unlocked): Golden Age. The watch log shows
   `GetWorldFlowBitfieldValue('BaseWorldFlow', 'costumeUnlocked', 2)` going 0 -> 1 when the save
   applies.
2. Pause LOAD slot 1 (locked): the default suit.
3. Pause LOAD slot 2: Golden Age again. `costume.ini` still says `worn=retail/3`.

**No popup, no restart (owner requests 2026-10-05).** WEAR switches at once, without the retail
"NEW COSTUME SELECTED!" popup, for every suit. Mod suits go through `requestCostume`, which no
longer opens the popup. For retail records, `SR_CostumeWorn` makes the same DAS request
(`requestCostumeSlot`) and replies queued 2, so `SRCostumes.Retail` skips the retail
`SetActiveCostume`. That function's only other effect is the popup, and the popup's close path takes
no callback. `SR_COSTUME_POPUP=1` brings the popup back.
Suits with their own banks (`animation_banks`, such as the costume lane's release candidates) no longer
wait for the next start. The boot turns each one into a character (`SR_BANK_SUITS_AS_CHARACTERS`,
unset = the character switch). Each bank family gets a generated `SRM_FAM_<family>_ANIMS` project:
the suit's banks, or retail ones where it has none, given new identities by `modScopedBankCopy`, plus
a copy of the retail CCT. Each suit gets an entity copy `<entity>_SRCHAR` on that project.
`baseworl.AST` is no longer patched, so Superman himself keeps the retail banks. The "next start"
note now appears only when that generation fails for a suit (logged).

**Objectives' extra Superman banks.** Some objectives add banks to SupermanAnims through an
association in their preloads: BIZ01SUPERMANANIMS (BIZOBJ15, ME34TYPE01, ME36TYPE01, PLAYABLEBIZ),
MTOOBJ09SUPERMANANIMS, SUPERMANTWISTERANIMS (TWSOBJ04) and WAROBJ01SUPERMANANIMS. When a character's
pack ships its own copy (`<P><C>MIM_BIZ`, `MIM_MTO`, `TWISTER_IGC`, `WW_IGC`), the boot adds a
sibling association `<P><C><tag>ANIMS` on the character's project to that objective's preloads
(`[COSTUME] <FLOW>: 1 character association(s) added`).

**Test content.** `tools/srmods/make_charswitch_test_pack.py` makes test characters: retail looks on
scoped copies of SupermanAnims with new bank identities only, built from the player's disc. Its
pack is `"test_only": true`, so its suits are listed only with `SR_TEST_CHARACTERS=1`.

**Live (archive/v1_charswitch, port features, chapter-2 save; frames under view/):**
- c1: WEAR CLASSIC -> a new actor whose controller carries project D66FC1AB (103 bones, cloth,
  aimer) while Superman's actor keeps SupermanAnims 0E762404. WEAR POD -> another new actor
  (CAAACB72) and the CLASSIC actor is freed. WEAR Superman Returns -> Superman back, the POD actor
  freed. 0 ICALL.
- c2: restarted with the character worn -> it spawns after LOAD, runs and flies (`c2_metro4`,
  `c2_fly2`). In-game SAVE + LOAD keeps it (`c2_reload_b`).
- c3: WEAR BIZARRO -> Bizarro free-roaming Metropolis with his model and purple cape (`c3_biz3`). He
  runs, flies (`c3_biz_runfly`, `c3_biz_fly2`) and uses his power from the power meter
  (`c3_biz_power`). Mid-air switches keep flying both ways (`c3_back`, `c3_air`). 0 ICALL.
- Unlock check from the game's own data: ThumbViewer.LoadThumbs loads a record's ThumbFile only when
  it is unlocked (a locked one gets `attachMovie("LockedThumb")`). c1 (same save, before the unlock
  code) loaded only CostumeThumb_1; c4 loaded CostumeThumb_2 and _3.
- m1 (dev-tool assisted level skip, NEW GAME with a character worn): MSTobj01-03, then into the
  WARobj01 arena (`m1_sheet`). Story overrides still apply: Warworld's `ReplaceEntityVisRep`
  (SupermanGrayEnt) dressed the character in the Pod suit. 0 ICALL.

**Open:** objectives that add banks to SupermanAnims (SUPERMAN_MIM_BIZ / MIM_MTO / TWISTER_IGC /
WW_IGC) do not yet reach a character's own project. The costume lane ships per-character copies, and
the sibling-association composition is still to do. Story `ReplaceEntityVisRep` calls dress a
character in a retail suit. No mission has yet been won by hand as a character (m1's passes are
dev-tool writes).

## Costume viewer: the COSTUMES page as a 3D viewer (viewer lane, iter-465)

OWNER 2026-10-05: "a new costume viewer ... a 3D model of the suit and cape ... doing an idle flight
pose", laid out like the Spider-Man (2000, PS1) COSTUME VIEWER and built from the game's own UI pieces.
Port feature (custom front end + `SR_COSTUME_VIEWER`, unset = the native costume screen's gate); natural
rolls keep the retail screen.

**The page (Apt, the game's own parts).** `tools/apt/mods/custom_frontend/55_viewer.patch` re-lays out
SCRCOSTUMES at the end of its InitGui: the centre well and the thumbnail strip are hidden (the Fortress
crystal art stays), and the ThumbViewer library's own symbols build the layout - "COSTUME VIEWER" title
(AdvancedText) top left; the suit list (AdvancedText rows, ThumbHighlight on the selection, "???" for a
locked suit) in a Scrim + ThumbBorder panel on the left; COSTUME / CHARACTER / COMMENTS (yellow
headings) on the right. One grid (owner 2026-10-06 "header alignment"): 40 px outer margins, equal
panels, 27 px side / 20 px top-bottom padding in both, the title / hint row on the panels' left edge
with 20 px between them and the panels, the highlight bar 4 px inside the border. The right panel
flows (heading, value, 22 px, next heading) and wraps values itself into single-line fields at
size + 2 px line pitch: Apt ignores TextFormat.leading, and a multiline field's _height does not
follow its text right after SetText (rolls lb720 / ld720). Then one horizontal ButtonHints row along the bottom (device glyphs), real button
actions only: WEAR COSTUME [A], BACK [B] (owner 2026-10-05: no ROTATE / ZOOM hints; on the keyboard
Enter / Esc, the menus' select / back). Selection, WEAR and BACK stay on the page: up/down (d-pad /
left stick) select, A (Enter) wears and so does X (the same `SRCostumes.Wear` /
`RecordAdapter.SetActiveCostume` calls as costumes_input.aptasm; a retail suit the host switched
itself - charswitch part 2, no popup - keeps its description), B is the retail back. Left/right and
the right stick's codes are swallowed so the hidden thumbnail grid never moves; the bumpers and
triggers are free (not repurposed). The page source is AS-like text (`viewer/srviewer.as`) compiled by
`tools/apt/as2asm.py` into `srviewer.aptasm` (rebuild: `python tools/apt/as2asm.py
tools/apt/mods/custom_frontend/viewer/srviewer.as tools/apt/mods/custom_frontend/srviewer.aptasm srv`,
then `tools/srmods/package_port.py --feature custom_frontend`).

**Turn and zoom (host, never the page).** Right stick X turns the suit, right stick Y zooms (the pad
state the game reads, `costumeViewerNotePad` in stubs.cpp; it idles into a slow turntable). Mouse: drag
left/right turns, drag up/down zooms, the wheel zooms (SDL events, players only). Zoom 1..2.6 frames the
upper body. Harness rolls drive the right stick with I/K/J/L (`mapKeysToPad`, test keymap).

**The model (host overlay).** The page sends `SR_CostumeViewer show <id> <locked>` (kernel/costume_viewer.cpp)
and the overlay (gfx/costume_viewer_draw.cpp) draws the suit in the page's open centre, over a procedural
shield outline and glowing ring: the suit's own skinned body and textures, the cape's RSS surface
skinned to its chains with a slow sway, posed in the Hover clip of its flight bank, turning slowly.
Sources, all read at runtime from the player's own files: the entity's `visrep` asset / `clothdef` and
`animsys project` (loose pack files first, else objtiv.AST SUPERMENACTORWORLDFLOW / PLAYABLEBIZ, the cape
RSS from preload.AST PRELOAD_OTHER); the bank from the suit's own `animation_banks` / own project, else
baseworl.AST BASEWORLDFLOW_ANIM. Decode = kernel/costume_viewer_model.cpp (a C++ port of the costume
lane's rw4 / sranim readers; offline check `sr_costume_viewer_test`). A locked suit draws as a
silhouette. While a popup is up (gPopupMgr) the model is hidden, so WEAR's "NEW COSTUME SELECTED!"
shows. mod.json costumes may name their CHARACTER line (`"character": "GENERAL ZOD"`); default
SUPERMAN, BIZARRO for the built-in.

## Map markers and maps for mods (iter-466, map lane)

The pause map is a built-in PORT FEATURE (`pause_map`, docs/MAP.md): it marks the current objective
and the next story mission, the races and Bizarro statues (unlocked, completed, best time / score)
and the kittens (found or not), all read from the game's own state. Its built-in markers are
written in the same MAP DEFINITION schema a mod uses (`tools/apt/mods/pause_map/map.json`, shipped
as `data/port_features/pause_map/map.json`), so a mod can add its own markers - a custom event, a
new challenge - or a whole map.

A mod gives its map data as a `"map"` object in `mod.json`, or as a `map.json` file beside it (not
both). The schema (docs/MAP.md 4.1) in short:

```json
{
  "format": 1,
  "maps": [{
    "id": "metropolis",
    "types": [{"id": "example_event", "label": "EXAMPLE EVENTS", "icon": "ring", "color": "40FF80"}],
    "markers": [{
      "id": "example.rooftop", "type": "example_event", "pos": [-2158, 1614, -284],
      "name": "ROOFTOP RESCUE", "detail": "A custom event at the Daily Planet.",
      "show": {"bit": "objectiveStatus", "index": 6},
      "completed": {"lua": "ExampleRooftopDone"},
      "record": {"lua": "ExampleRooftopBest", "format": "time_ms", "label": "BEST TIME", "when": "completed"},
      "after_complete": "keep"
    }]
  }]
}
```

That is the whole example pack `tests/fixtures/mods/map_example/mod.json` (its `"map"` object): one
green ring on the Daily Planet once WARobj02 is won; selecting it on the map shows ROOFTOP RESCUE,
then COMPLETED and BEST TIME once the mod's own script has set the two Lua globals.

- **Adding to a map:** a `maps[]` entry whose `id` names an existing map (`metropolis` = the game's
  Metropolis) and has no `view` adds its `types` and `markers`; a marker or type id it repeats
  replaces the earlier one (the higher-priority mod wins; the boot log notes it).
- **Replacing or adding a map:** an entry with a `view` replaces the map with that id, or adds a new
  one: `{"kind": "game"}` keeps the game's 3D map camera, `{"kind": "art", "movie": "<Apt movie in
  the pack>", "rect": [x0, z0, x1, z1], "y": h}` is the mod's own map art placed by projecting its
  world rectangle through the same camera (`"size": [w, h]` = the movie's own pixel size; the drawing
  path is built and unit-tested on the host side but NOT yet proven in the game: no art test movie
  exists yet). `world` names the loaded world flow it belongs to (empty
  = any). The highest-priority match for the loaded world is shown. Where the game itself refuses
  the map ("THE MAP IS NOT AVAILABLE IN THIS AREA") nothing is shown.
- **Positions:** `pos` [x, y, z] in world units (Y up; the map is north-up: +Z is up, +X is left),
  or `trigger` = a live objective trigger's position by its objectiveIndex, or `each_trigger` = one
  marker per live trigger matching `trigger_type` / `flow` / `filter` / `not_filter` / `available`.
- **Status sources** (all READ-ONLY; the map never writes game state or calls game functions):
  `{"das": "name"}` an integer DAS attribute of the base world flow (`"flow": "F"` for another loaded
  world flow), `{"bit": "prefix", "index": n}` bit n of the words `prefix0, prefix1, ...` (the
  game's objectiveStatus / kittens convention), `{"stat": "Name"}` a game stat, `{"lua": "Global"}` a
  number / boolean global of the mod's own Lua scripts (a plain `MyEventDone = 1` in a state script
  lands in the shared `_G`), `{"trigger_available": n}`, `{"trigger_engaged": n}`, `{"const": v}`,
  and `all` / `any` / `not` / `ge`.
- **Marker rules:** `show` (when it is on the map), `completed`, `record` / `target` (a value with
  `format` time_ms | score | count, `label`, `when` always | completed | pending, `else_label`),
  `completed_label`, `pending_label`. `after_complete`: `hide` (a story beat leaves the map once
  done) or `keep` (a replayable challenge stays, shown completed). `becomes: "<marker id>"`: a beat
  that introduces a replayable challenge hands its place to that challenge's marker once completed.
- **Icons:** the map screen's own art - `ring`, `reticle` (the objective reticle), `pulse` (its pulse
  ring), `dot` (a small ring), `superman` - in the type's `color` (matched to the nearest of the
  screen's 10 marker colours, docs/MAP.md 4.1), with an optional `glyph` of up to 3 characters
  (`>>`, `B`, `!`). A completed marker is drawn dimmed.
- **Counts:** a type's legend count (`KITTENS 37/100`) counts only the markers of the layer that
  defined the type. A mod marker of a built-in type is drawn but not counted; for a counted set of
  your own, define your own type.
- **Validation:** like the rest of mod.json. A bad entry refuses the mod with a located reason in
  the MODS library details, e.g. `mod.json: map: maps[0] (metropolis).markers[0] (x): a marker needs
  exactly one of pos, trigger, each_trigger`; a marker naming an unknown type is dropped with a boot
  log note. Never a crash. Tests: `build/difftest.exe --map-test` (D/V/G/E/P/L/R/B/M groups).
  Check a definition before shipping it: `build/difftest.exe --map-validate <map.json>` (a map.json,
  or a mod.json's `map` object saved on its own) prints the located error or `OK: N map(s), N marker(s)`.

## Native costume-screen extension checkpoint (2026-10-02 evening, WIP)

*Superseded by "Fortress costumes" above (iter-465); kept as history.*

### Interactive test and cape isolation (2026-10-02 22:18)

The first user-controlled run used snapshot `9622ef929e3b-costume-preloads`,
real keyboard/controller/audio, private saves, built-in frontend/autosave/mods,
and opt-in native costumes. It is an experimental costume run, not natural
baseline acceptance. It stopped with `ICALL_FAIL target=0 call_site=821673FC`
immediately after loading the legacy Original body/cloth/RSS. No forcing was
enabled to bypass the failure. Receipt: `archive/ut1/launch.json` and `exit.json`.

Sequential background comparisons used the **same executable**, input-free
`archive/costume_boot_control.ps1`, `SR_PORT_FEATURES=1`, `SR_NATIVE_COSTUMES=1`,
private instances/saves and `SDL_AUDIODRIVER=disk`:

- Installed but not selected: survived the 140-second observation after FE256;
  its final captured frame was still a loading card, not gameplay acceptance.
- `-Equip`: repeated the same fatal call after actual BODY/CLOTH/RSS opens.
- `-BodyOnly`: loaded byte-identical BODY SHA256
  `727fb4a5bb33be4321861d4f1319ef8acac78d33b2feb32148c859f8d7f369f1`
  at 22:17:40, then survived until normal harness cleanup at 22:18:43. Presented
  frame `p002100` shows the title over the city. This isolates a cloth-path
  difference; it does **not** accept appearance, physics or suit switching.

Main-checkout receipts are `archive/costume_user_{control,equipped,bodyonly}_20261002/`.
The body-only diagnostic changes VisRep to `SkinnedModel`; existing retail
AnimSys is retained by ReplaceEntityVisRep, so it does not remove every
animation-side cloth association. Its generated manifest name was shortened
to fit the native 64-byte limit; no mesh bytes changed. The own-rig packaging
guard remains intact. Do not call the full cape pack test-ready yet.

### Screen implementation and remaining acceptance

The built-in frontend recipe now includes `50_costumes.patch` / SCRCOSTUMES.
It appends records after the retail `RecordAdapter.GetCostumeData` call and
before the original unlock/hint loop and `ThumbViewer.Create`. It never wraps
or replaces the global RecordAdapter method. Retail record objects, order,
Locked values, navigation and wear path remain intact; only X/WEAR on an
appended record's stable `SRId` enters the new bridge. Movie-owned callbacks
are bound only around synchronous requests, and the helper object is deleted
by the screen's ExitScreen. Partial catalog replies leave the retail list alone.

The new behavior is gated by **`SR_NATIVE_COSTUMES=1`**, default OFF pending
live execution. This is a development rollout gate, not a proposed player
toggle. Port-feature packaging remains baked in.

- `SR_ModCostumes` replies with `SR_CostumeBegin(count)`, then
  `SR_CostumeItem(index, stableId, name)` and three
  `SR_CostumeText(index, field, value)` calls (`Description`, `ThumbFile`,
  `FullSizeFile`) per boot-active suit, then `SR_CostumeEnd(count)`.
  Pending inclusion changes do not mutate this boot's catalog. No generated
  registry, or gate OFF, returns zero additions.
- `SR_CostumeWear <stableId>` requires the current interpreter context and an
  active registered suit. It calls the game's FindFlowByName (821F6850) and
  DAS integer setter (821E6F90) on that same guest thread, using the same actor
  flow / SetCostumeNum contract as retail SetActiveCostume (822B29E0).
  There is no host timer, entity replacement, numeric-slot API, save-file write
  or reboot. `SR_CostumeResult(id, queued, message)` reports a queued request,
  **not proof of a changed model**. Gate OFF, absent context, inactive identity,
  absent flow or missing DAS node all refuse the request.
- Costume manifest entries now accept optional `description` (512 UTF-8
  bytes), `thumbnail` and `fullsize` (scoped Apt movie stems, no extension,
  maximum 27 ASCII characters so `.swf` fits the retail 31-character buffer).
  URLs, filesystem paths, unscoped retail names and invalid types are rejected.
  These fields name native thumbnail assets; they do not load PNGs directly.

Evidence: `difftest --mod-manager-test` **128 passed, 0 failed**. It exercises the real FSCommand entry, controlled
dispatch leaves for argument/context checks, then the real recompiled flow
lookup and DAS setter against a synthetic guest descriptor. This is
wiring/function evidence, not live-game acceptance. Protected-memory controls
also check unreadable roots and readable-but-unwritable targets.
`--mod-overlay-test`: **164 passed, 0 failed**.
`tools/srmods/rt_srmods.py`: **34 passed, 0 failed** including metadata parity.
`tools/apt/test_costume_patch.py`: **4 passed**; its three original splice tests
were first observed failing before `after-call=` support was implemented
(`archive/native_costume_splice_red.log`). The full Apt patch suite passes
**32/32**, including unload/reload and mutation checks. Generated SCRCOSTUMES
is a clean disc-applied patch, not redistributed retail movie data.
The build and credits check pass; the forcing audit remains at its two inherited
`gpu_ring_alloc_fix.cpp` findings (no new override classifications or host threads).

Still required: native thumbnail/fullsize asset generation and fallback artwork,
live screen/controller/reload verification, actual costume/cape changes and
return-to-retail checks, and binding each source-proportion rig to its own
animation offsets/default pose. The gate must not be baked on from these tests.
Targeted decompilation confirms why the latter is separate: the costume Lua's
ReplaceEntityVisRep path (821CCED0 -> 821C7510 -> visitor 821D9638) only parses
`visrep`, then replaces actor+0x6C through 821C6C00. It leaves AnimSys at
actor+0x64 intact; a new ENT's `animsys` project alone does not switch banks.

## Own-animation project preload checkpoint (2026-10-02 evening, experimental)

*iter-465: own animations ship as `animation_banks` (see "Fortress costumes"); a second resident
project overflows the animation memory (costume lane, 2026-10-03). This path stays experimental.*

Costume records may additionally declare `animation_project`: an exact-case
resource/project name in that pack's `modAssetPrefix(id)` namespace, without an
extension or path (at most 63 ASCII characters). For example, a pack's prefix
looks like `SRM_845c2c9fa40695d1_`. The project XML name,
entity `animsys project`, and manifest name must agree exactly: the runtime
project registry uses case-sensitive FNV-1, unlike file-resource lookup.

With **`SR_COSTUME_PROJECT_PRELOADS=1`** (exact value, default OFF), the shared
composer preserves BASEWORLDFLOWPRELOADS and appends the included suits' own
ANIM projects. Shared projects appear once; excluding one suit keeps a shared
project while another included suit uses it. Retail SupermanAnims and every
other preload group/attribute remain present. The composed document is part of
the same content hash and atomic generated registry as the costume attributes.
No original asset, guest state, registry pointer or guest call is changed here.

The winning loose compiled preload override is used, then hidden only when the
complete generated registry succeeds. Missing overrides read the player's disc.
Empty, malformed, case-ambiguous, oversized or unsupported winning documents
refuse the entire generated costume registry: they do not silently fall back to
disc. **Text preload bases are currently unsupported**, including otherwise
valid text supplied by an unrelated mod; this compatibility gap remains open.
Gate OFF with an own-project suit also refuses the generated registry while
retaining the ordinary mod layers. Preloading alone does not switch a live actor's
animation project, and these suits are not yet accepted as playable.

Verification in this lane:

- `build/difftest.exe --mod-manager-test`: 134 passed, 0 failed. Required retail
  documents, deduplication, exact case, unsupported bases and malformed compiled
  bounds are covered; output remains untouched on composition failure.
- `python tools/srmods/rt_srmods.py --exe build/difftest.exe`: 45 passed, 0 failed
  in six consecutive final runs. Two independent multi-suit packs are compared
  byte-for-byte against the Python reference, including per-suit exclusion and
  winning-override refusal. Earlier new tests failed against the old executable
  (`archive/costume_preload_red.log`); empty-override and malformed-gate controls
  failed before their fixes (`archive/costume_preload_edges_red.log`).
  Review also exposed native discovery entering ignored `.draft`/`@backup`
  directories; a failing control (`archive/costume_preload_private_red.log`)
  now verifies that only mounted, visible files can supply the winning base.
- `build/difftest.exe --costume-animation-test`: 39 passed, 0 failed. Real guest
  functions reproduce stale-tree, shared-head-list and double-free hazards, then
  exercise a limited same-controller/player migration fixture. It has empty bank
  iterators, synthetic compatible skeletons, no cloth/aimer/active powers, and a
  stand-in action-boundary condition. It proves neither clip playback nor live
  character/FSM continuity; the transaction exists only in this test.
- `build/difftest.exe --mod-overlay-test`: 164 passed, 0 failed. Both executable
  targets rebuilt. Receipts are `archive/costume_projects_final_*.log` and,
  after the ignored-directory fix, `archive/costume_projects_reviewed_*.log`.

Test-instrument caveat: an earlier native dump intermittently returned exit 0
and empty stdout despite normal boot stderr (`archive/costume_projects_repeat_4.log`).
The runner now rejects missing catalogue output, and the native dump checks its
write and explicit flush. Twelve pre-edge-fix and six final full-suite runs passed
with that change. The original output-loss mechanism is **not established**;
there are no retry-to-green semantics. No game roll or default-on promotion is
claimed for this checkpoint.


## Windows long-install-path fix (2026-10-02)

Both `SupermanReturns.exe` and `difftest.exe` now embed `app/windows.manifest`
with `longPathAware=true`, retaining `asInvoker` / `uiAccess=false`. **Windows
must also have `LongPathsEnabled=1`**; the application does not change that
machine policy. The existing native Unicode file APIs remain unchanged, as do
full SHA-256 generated-directory identities and hidden-directory rules. There
is no shortened temporary filename or blanket path-prefix workaround.

The original failing install at
`archive/interactive_costume_20261002_attempt2/Superman Returns` now publishes
its costume registry: all six generated files hash-identically to the short
`archive/ut1/Superman Returns` installation under the same full SHA identity.
The actor script's final path was 259 characters and its temporary path 263.
The new regression additionally exercises final paths above 260 characters,
Unicode and spaces, byte-exact readback of all six assets, temporary-file
cleanup and repeated boot. With the old manifest it failed three checks
(138 passed / 3 failed); with the fix it passes (141 passed / 0 failed).

Both embedded executable manifest resources were inspected. Receipts:
`archive/longpath_manifest_{red,green,resources}.log` and
`archive/longpath_manifest_original_{dump,stderr,compare}.log`.
This verifies native file generation/readback, **not live costume or game
acceptance**; no game was launched for this fix.
## Port features (iter-464): PC additions in the exe, not mods

A **port feature** is a player-facing PC addition built into the exe (not an overlay folder):
it changes what the game does on purpose, behind its own `SR_*` gate, and is inert in
automated/natural rolls by construction (they have no real input and never set the gate). Runs
with a gameplay-changing port feature on are not natural and never count toward
`docs/NATURAL_BASELINE.md`. Overrides that implement one are classed `port-feature` in
`docs/OVERRIDES.json`.

| Feature | Gate | Default | Changes gameplay? | Doc |
|---|---|---|---|---|
| PC keyboard + mouse layout, Dodge key | `SR_PC_CONTROLS` | ON | no (pad input only; the Dodge key sends the game's own evade input) | docs/PC_CONTROLS.md 1, 5 |
| Mouse-look on foot (camera angles, 1:1) | `SR_PC_MOUSELOOK` | ON (writes only while a real mouse moves) | camera only | docs/PC_CONTROLS.md 4.2 |
| HOVER DODGE / AIR STRAFE DASH | `SR_PC_MOVES` | OFF | yes: two new air moves | docs/PC_CONTROLS.md 6 |
