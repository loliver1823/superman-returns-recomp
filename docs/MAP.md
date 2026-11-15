# The pause map (iter-466 map lane): research and the useful-map port feature

Owner request (2026-10-05): "have the top-down view map actually be useful. It could mark
collectibles already found, mark where the side challenges are once unlocked and if you've already
completed them, maybe show your highest score or best time, mark current objective or the next
story mission you need to go to." Owner rulings the same day: it ships BUILT IN as a port feature
(like the costume viewer: no mods-library entry, no on/off setting; filters inside the map screen
are fine); story markers disappear once that beat is done, a replayable challenge keeps its marker;
the map is data-driven and mods can add markers or maps (section 4).

Everything below is observe-only: the host reads guest memory, the Apt patch draws. No guest
function is called that the game did not call, and no game state is written.

## 1. The retail pause map (what ships today)

- Screen: Apt movie `SCRMAP` (ui.AST TOC name), class `ScrMap` extends `BistroBaseScreen`, stage
  1280x720. Disassembly: `python tools/apt/aptasm.py dis SCRMAP`.
- What it draws: the city itself is the game's 3D renderer seen through the MAP CAMERA (a
  top-down perspective camera); Apt draws only the frame (top/bottom bars, mask quad `mcMaskQuad`
  at 72,54, 1136x612), the hint row (PAN MAP LStick, ZOOM MAP RStick, FORTRESS OF SOLITUDE Y, BACK
  B), the objective text box `mcObjText` (`objTitle`, `objDescr`, set to " " at InitGui), the
  Superman icon `mSupermanIcon` and an objective reticle `mcReticle` (an animated ring with four
  arrows).
- `mcReticle` is never shown in retail. It is made visible only by `ScrMap._TargetObjective`, and
  nothing calls it: the d-pad cycling (`_FindObjective`) is dead code (no AS caller; the strings
  `_FindObjective` / `_TargetObjective` do not exist in the xex). So the retail map marks exactly
  one thing: Superman. (Frame: section 5.)
- Native side (`MapAdapter`, registered by `MapAdapter_RegisterNatives_sub_822B8D20`):
  `GetObjectiveDetails` 822AC5E0, `UpdateObjectiveDetails` 822A6B78, `TargetObjective` 822A6CD8,
  `RegisterListener` 822A0568, `TransitionMapIn` 822A05A0 (camera interface state 6, 2.0 s blend),
  `TransitionMapOut` 822A0698, `SetPauseMapActive` 822A0738 (`byte_8296EC55`). Registered but
  unused by ScrMap: `GetMapMode`, `GetThreatDetails`, `UpdateThreatDetails`, `TargetSuperman`.
- `GetObjectiveDetails` (called by the game itself in `HandleMapTransitionIn` while no objective
  runs) lists, from the objective manager `*0x8296DF5C`, the `OBJ|TRIGGERS` triggers that pass
  `sub_82226790`: TYPE_OBJECTIVE with `objectiveAvailable != 0`, TYPE_MINIGAME whose bit `i&31` of
  flow attribute `objectiveFound<i>>5>` is set, TYPE_PERSISTENT_MINIGAME when `objMgr+3 != 0`.
  Title/description: trigger attributes `objectiveNameLocalizationKey` /
  `objectiveTextLocalizationKey`. The list is computed but never drawn.
- Per frame: `PauseFlowStateTick_sub_82151298` case 4 -> `PauseMap_TickAnalogInput` ->
  `sub_822AC538` calls `ScrMap.SetSupermanIcon(x, y)` through the AptExecutor (vt+0x20) every
  pause-flow tick while the map is active; `SetSupermanIcon` calls `ProcessIcons`.
- HUD minimap (not the pause map): native radar `HudRadarRenderBlips_sub_82304218`, 64 blip slots
  x 48 bytes, texture `minimap_icons`; not Apt, not reused here.

## 2. The world -> map transform (the game's own)

`sub_822A6A28` projects objectives and Superman alike through the LIVE camera frame (not a 2D
map of mapAreaMin/Max):

- `V = *(*0x8296D86C + 0x28)` (camera manager -> current frame object, vt 0x82087820):
  +0x04 fov (horizontal, rad), +0x08 aspect, +0x0C near, +0x10 far, +0x14 flag (set: +0x04 is
  tan(fov/2) already), +0x20 position (vec3), +0x30 Euler (x, y, z).
- `R = Rz(e2) * Ry(e1) * Rx(e0)`; `view_i = dot(p - pos, R_row_i)`;
  `x' = view.x / (view.z * t)`, `y' = view.y / (view.z * t / aspect)` with `t = tan(fov/2)`;
  `stageX = W/2 * (1 - x')`, `stageY = H/2 * (1 - y')`, `W, H` = u32 at `*0x829A9384 + 0x3C/+0x40`
  (1280x720 in the golden dump = the SCRMAP stage). Nothing behind the camera is clipped.
- World positions: entity/actor +0x20 = vec3 f32, Y up (ground plane X/Z). Superman = the
  `SUPERMAN` entity's ACTR +0x20 (the map clamps its y to `maxH - 1000`, `sub_822AAA00`).
- The map camera (`camMgr+0x1060`, vt 0x82005944, tick `MapCam_UpdateTick_sub_8216D540`): yaw 0
  (north-fixed: screen up = world +Z, screen right = world -X), pitch
  `pi/2 - lerp(minPitch, maxPitch, t)` by height; pan/zoom by the sticks, clamped to the
  `mapCamLibDefinitions` "default" def (`MapCamDefsXML`): mapAreaMin (-8000, 0, -7000), mapAreaMax
  (5000, 4000, 3000), fov 1.7, height 2169.8..2855 (golden dump values).
- Because the projection uses the live frame V, a host observer that reads V each frame
  reproduces the game exactly, through pan, zoom and the 2 s transition blend. The feature checks
  itself: every tick it projects Superman and compares with the (x, y) the game just passed to
  `SetSupermanIcon` (`[MAP] check` log line, section 5).

## 3. Data inventory (what the game stores; V = verified, I = inferred)

Reader paths: the base world-flow DAS (`World::dasVal`, kernel/devtools_skip_core), the stat list
`*0x8296DF70` (name at +0x1C, f64 value at +0x08, flags +0x22 &4 = saved), the live trigger
records (`World::triggers()`: objectiveIndex, flow, available = state flags bit 0 clear, engaged,
actor +0x20 = position). Research notes: `archive/v1_map/research/`.

### 3a. Collectibles
- **Kittens: 100, the game's only collectible placed in the world** (V). Found state: DAS bitfields
  `kittens0..3`, bit `kittenID % 32` of word `kittenID / 32`; count `kittensFound`, total
  `numKittens = 100`; all saved. Unlocked with story objective 6 (WARobj02). Positions: entities
  `KIT00..KIT99TriggerEnt` (objectiveIndex 156..255) placed in `lunits.AST` load units; a kitten
  exists live only while its 600 m load unit is resident (and despawns if found), so the live game
  cannot give all 100 positions. The map uses a static table of the 100 placed positions (facts
  derived from the disc, like `data/devtools/objective_table_retail.tsv`) plus the live found bits.
- Trophies (7), concept art, videos, costumes: awards (bitfields `trophiesUnlocked`,
  `artUnlocked0..8`, `videosUnlocked`, `costumeUnlocked`), **not placed in the world**: nothing to
  mark on a map.
- Bizarro statues are not separate collectibles: they are the 5 rampage start points.

### 3b. Side challenges (all TYPE_MINIGAME, all `triggerIsRepeatable = 1`, V)
Race triggers and rampage statues are placed in `BASEWORLDFLOWTRIGGERSLAYER`: they exist with a
position whenever Metropolis is loaded, locked or not; locked = trigger state flags bit 0 (V).

| idx | race (flow FastFlyer, name key `FastFlyer_Route_n`) | best time (stat, ms, saved) | unlocked by |
|---|---|---|---|
| 96 | Beginner's Boost | `BeginnersBoost` | WARobj02 (6) |
| 95 | Industrial Haste | `IndustrialHaste` | 6 |
| 97 | Coast-to-Coast | `CoastToCoast` | 6 |
| 99 | Suicide Slum Sprint | `SkidRowRun` | 95 + 96 + 97 |
| 92 | Hyper Sector Scramble | `HyperSectorScramble` | 99 |
| 93 | Escape Velocity | `EscapeVelocity` | 92 |
| 101 | Rush Hour | `RushHour` | 93 |
| 100 | Inner City Speed | `InnerCitySpeed` | 101 |
| 94 | Midtown Momentum | `MidtownMomentum` | 100 |

- Completed: `objectiveStatus` bit idx (+ `RoutesComplete` bit route) (V). Route 7 (idx 98) does
  not exist.
- **Best time: stored**, one saved stat per route in ms. It starts at the route's par time (STATDEFS
  default) and the race FSM keeps min(old, finish) (I: BESTTIME semantics not checked in IDA). So
  the map shows it as BEST only for a completed race and as PAR otherwise; it is read only while no
  objective runs (it may be live during a race).
- Bizarro rampages (flow PlayableBIZ, name key `MGM_PlayableBizarro_Name` "You am Bizarro"): idx
  67..71 = statues 0..4, first-run target scores 1,000,000 / 1,250,000 / 1,750,000 / 2,000,000 /
  1,500,000. Completed: `bizarroStatuesFound` bit statue (+ objectiveStatus bit idx).
  **Best score: one global saved stat `BestBizarroScore`; no per-statue record exists** (V). The map
  shows the statue's target and, for a completed statue, the global best labelled as such.
  Unlock: use the live available flag (the file dependencies read empty at runtime,
  DEVTOOLS_DESIGN open item 6).
- idx 118 CheatBizarroRampage: only the BIZARRO UNLEASHED cheat makes it available; shown only
  while available.
- No other repeatable minigame has a world trigger: ButtonMashingMIM / FinesseMIM / MomentumMIM run
  inside BIZobj15 / MTOobj09 (not replayable); KittenFinding is the kitten hunt.

### 3c. Story: the current objective and the next mission
- Current: base DAS `objectiveActivated`, `objectiveBitIndex`, `objectiveWorldFlowName`; its
  position = the trigger record with `engaged == true` (actor +0x20).
- Next: the AVAILABLE trigger records whose actor DAS `triggerType` is TYPE_OBJECTIVE or
  TYPE_TRANSITION, excluding filter METROEVENT (idx 120) and the cut rows
  (`data/devtools/objective_table_retail.tsv`, reachable = False). Names from their
  `objectiveNameLocalizationKey`. Triggers placed at (0,0,0) have no world location (WAR01/WAR02,
  the MST/BIZ transitions, some cinematics): the flow engages them, so they get no pin.
- **Story -> challenge links (owner rule "a story beat that introduces a challenge keeps its
  marker as that challenge"):** none in this game's data. No story objective is a race or rampage;
  story idx 6 (WARobj02) UNLOCKS races 95/96/97, rampages 67..71 and all kittens, and each minigame
  introduces itself (the first race / kitten plays Mxyzptlk's intro FMV 1000). No story, base-flow
  or Metropolis Lua references FastFlyer, PlayableBIZ or KittenFinding (V). All 15 minigame
  triggers stay available after a win (`sub_822737D0` blocks only won AND not repeatable) - the
  game lets you replay every race and rampage. So: built-in story markers vanish when done;
  challenge markers persist with their completed state. The schema still carries the rule
  (`after_complete`, `becomes`) for mods.

### 3d. Metro events
Spawned dynamically (BaseWorldFlow state S_3876280C spawns `MiniEventTriggerEnt` at a spawn-point
bin, at most `maxMiniEventTriggers = 4` alive), filter `OBJ|TRIGGERS|METROEVENT`: they appear in the
live trigger records with a position while they exist. Nothing persistent marks one done. The map
shows live, available ones.

## 4. Design: the `pause_map` port feature (data-driven, mod-extensible)

- Ships as port feature `pause_map` (`data/port_features/pause_map/`: `feature.json`, the
  `SCRMAP.srpatch` clean patch applied to the player's own disc at boot, and `map.json`, the
  built-in MAP DEFINITION). Always on for players, no toggle, not a mods-library entry; off in
  automated rolls unless `SR_PORT_FEATURES=1` (docs/MODS.md section 7).
- Apt side (recipe `tools/apt/mods/pause_map/`, AS source compiled by `tools/apt/as2asm.py`): a
  generic renderer `_global.SRMap` appended to ScrMap's class InitAction. It knows nothing about
  races or kittens: it draws what the host sends (types: label, icon, colour; markers: type, state,
  name, detail lines), re-uses the screen's own pieces (the objective reticle `mcReticle` and the
  objective text box `mcObjText` for the selected marker; marker icons are copies of the screen's
  own ring / reticle / Superman art, tinted per type), adds a legend with per-type filters, and
  owns the d-pad (retail never used it, section 1).
- Host side (`kernel/map_markers.{hpp,cpp}`): loads the map definitions (the port feature's
  `map.json`, then every enabled mod's `map` section, in mod priority order), evaluates every
  marker's sources against guest memory (read-only: the devtools `World` reader), projects
  positions with the game's own camera transform (section 2) and answers the screen through the
  FSCommand bridge (`SR_Map`, kernel/mod_bridge.cpp).

### 4.1 The map definition schema (format 1)

```json
{
  "format": 1,
  "maps": [{
    "id": "metropolis",
    "world": "Metropolis",
    "view": {"kind": "game"},
    "types": [
      {"id": "story",   "label": "STORY",   "icon": "reticle", "color": "FFC830"},
      {"id": "race",    "label": "RACES",   "icon": "ring",    "color": "38B8FF"}
    ],
    "markers": [
      {"id": "story", "type": "story",
       "each_trigger": {"trigger_type": ["TYPE_OBJECTIVE", "TYPE_TRANSITION"], "available": true,
                        "not_filter": "METROEVENT"},
       "name": {"trigger_text": "objectiveNameLocalizationKey"},
       "detail": {"trigger_text": "objectiveTextLocalizationKey"},
       "after_complete": "hide"},
      {"id": "race.96", "type": "race", "trigger": 96,
       "name": {"text_key": "FastFlyer_Route_5"},
       "show": {"trigger_available": 96},
       "completed": {"bit": "objectiveStatus", "index": 96},
       "record": {"stat": "BeginnersBoost", "format": "time_ms", "label": "BEST TIME",
                  "when": "completed", "else_label": "PAR TIME"},
       "after_complete": "keep"},
      {"id": "kitten.0", "type": "kitten", "pos": [-3393.56, 1812.7, 832.18],
       "name": {"text": "KITTEN 1"},
       "show": {"bit": "objectiveStatus", "index": 6},
       "completed": {"bit": "kittens", "index": 0}, "completed_label": "RESCUED",
       "after_complete": "keep"}
    ]
  }]
}
```

- `maps[].world`: the map is offered while a loaded world flow has this name; the highest-priority
  matching map is shown (mods above the port feature, mods in `mods.json` order).
- `view`: `{"kind": "game"}` = the game's own 3D map camera (markers projected through it);
  `{"kind": "art", "movie": "<Apt movie path in the pack>", "rect": [x0, z0, x1, z1], "y": h}` = a
  mod's own map art loaded under the markers and placed by projecting its world rectangle through
  the same live camera (a redrawn Metropolis). Where the game refuses the map
  ("THE MAP IS NOT AVAILABLE IN THIS AREA") nothing is shown: that is the game's rule.
- `types[]`: `id`, `label` (legend text; a type whose markers have a `completed` source also shows
  done / total, counting only the markers from the layer that defined the type, so a mod's extra
  marker of a built-in type is drawn but never turns KITTENS 0/100 into 0/101), `icon` (the screen's own art: `ring`, `reticle` = the game's
  objective reticle, `pulse` = its pulse ring, `dot` = a small ring, `superman`), `color` (RRGGBB, matched to
  the nearest of the screen's 10 marker colours: FFFFFF FFC830 38B8FF C070FF FF5040 40FF80 FF9030 30E0E0
  FF60C0 A0A0A0; each is a template painted by its own placement colour transform, because this Apt's
  `Color.setTransform` only scales and the ring art is black), `glyph` (up to 3 characters on the icon).
- `markers[]`: `id` (unique), `type`, a position (`pos` [x, y, z] in world units, Y up; or
  `trigger` = a live objective trigger's position by objectiveIndex), `name` / `detail` (text:
  `{"text": "..."}` literal, `{"text_key": "..."}` the game's localized text,
  `{"trigger_text": "<trigger DAS string attribute>"}`), and status sources:
  `show` (default: always), `completed`, `record` / `target` (a value + `format` time_ms | score |
  count, `label`, `when` completed | always, `else_label`), `completed_label`.
- `route` (built-in races only; owner 2026-10-06: "rampages and mods don't need a trail line"): [[x, y, z],
  ...] (2..256 points, start .. finish). While a race is selected its TRAIL is drawn through the same
  camera projection: a continuous line through every waypoint in order (a dark edge under the race
  colour; each leg is the panel art stretched and rotated), direction chevrons every 110 px, START
  (green ring) and FINISH (gold reticle), each leg clipped to the map window so the off-screen parts
  run to its edge. A mod's `route` parses but is not drawn (the host sends routes for the
  `pause_map` layer only). The races carry the game's own
  routes: the `FASTFLYER<route>WAYPOINTLAYER` waypoint chains (FastFlyerWaypointEnt, edges id -> id + 1, the
  first waypoint is the start line, 420..500 world units from the race trigger) from the disc's objtiv flow data. No rampage area is defined in the data.
- `after_complete`: `hide` (a story beat: the marker disappears once done) or `keep` (a replayable
  challenge: the marker stays, shown completed). `becomes: "<marker id>"`: once this marker is
  completed it is replaced by that marker (a story beat that introduces a replayable challenge
  keeps a marker, as that challenge's). The built-in story markers carry no `becomes`: no story
  beat in this game is itself a minigame (section 3c).
- `each_trigger`: one marker per live objective trigger matching `trigger_type`, `flow`,
  `available`, `engaged`, `not_filter`; the engaged one is the CURRENT objective (state active).
- VALUES (every status source; all read-only):
  `{"das": "name"[, "flow": "F"]}` an integer DAS attribute of the base world flow (or of the
  loaded world flow F); `{"bit": "prefix", "index": n[, "flow": "F"]}` bit n of the bitfield words
  `prefix0, prefix1, ...` (the game's own objectiveStatus / kittens convention); `{"stat": "Name"}`
  a game stat; `{"trigger_available": n}`, `{"trigger_engaged": n}`; `{"lua": "Global"}` a Lua
  global of the mod's own scripts (see 4.3); `{"const": v}`; `{"all": [...]}`, `{"any": [...]}`,
  `{"not": v}`, `{"ge": [v, n]}`.

### 4.2 Mods
- A mod adds markers or maps with a `map` object in its `mod.json` (or a `map.json` in its pack) in
  the same schema. A `maps[]` entry whose `id` names an existing map and has no `view` ADDS its types
  and markers to it; one with a `view` REPLACES that map (or adds a new map for its `world`).
  Two mods replacing the same map: the higher priority wins and the library lists the conflict
  (the existing `mods.json` rules, kernel/mod_manager.cpp).
- Validated at discovery like the rest of `mod.json`: a bad entry refuses the mod's map data with a
  clear `mod.json: map: ...` reason in the MODS library details; never a crash.

### 4.3 Lua globals (the `{"lua": ...}` source)
The game's script VM is **Lua 5.0.2** (string "Lua 5.0.2" at 0x8294343C; `lua_rawget(L, -10001)` =
5.0's LUA_GLOBALSINDEX), numbers are 32-bit floats, one shared state: script manager
`*0x8296D8CC` (68 bytes, created by `LuaScriptContextCreate_sub_82186ED0`), `L = *(mgr + 0x0C)`,
`_G` = TObject at `L + 0x40` (tag 5) / `L + 0x44`. Table: `+0x07` lsizenode (u8), `+0x10` node
array; Node 20 bytes (key TObject +0, value TObject +8, next +0x10); TString `+0x08` hash, `+0x0C`
len, `+0x10` chars; stock `luaS_hash` (h = len; step = (len >> 5) + 1; h ^= (h << 5) + (h >> 2) +
byte). FSM state scripts run on that state with no environments of their own, so a mod script's
plain global lands in `_G`. The reader follows the key's hash chain, accepts only number (tag 3) and
boolean (tag 1) values, bounds every pointer to the node array, never touches a key whose value is
nil (a dead key's string may be freed), and reads twice: only an agreeing answer counts (the guest
may rehash `_G`). Verified against all three golden dumps (2048 nodes, 1414 string keys all
hash-consistent). Unit test: `--map-test` L1..L3.

### 4.4 The wire (fscommand `SR_Map`, kernel/mod_bridge.cpp -> kernel/map_markers.cpp)
| AS -> host | host -> AS (FlowMgr methods, at most 3 int / string arguments each) |
|---|---|
| `SR_Map open` (end of `ScrMap.InitGui`) | `SR_MapBegin(nTypes, nMarkers, 1 = a map matched)`; per type `SR_MapType(i + 65536 * hidden, icon * 256 + palette index, label)`, `SR_MapGlyph(i, glyph)` when it has one, `SR_MapCount(i, done, total)` when it is counted; per marker `SR_MapMk(i, type + 256 * state, title)`, `SR_MapInfo(i, detail, status)`, `SR_MapRec(i, readout)` when it has a record line; `SR_MapEnd(n)`; for a mod map with art `SR_MapView(kind, movie, size)`; then every position |
| `SR_Map tick <x> <y>` (end of `ScrMap.SetSupermanIcon`, every pause-map tick; x, y = what the game passed) | `SR_MapXY(i, x, y)` per marker that moved, `SR_MapRtXY(k, x, y)` per point of the selected route that moved, `SR_MapArt(0, x, y)` + `SR_MapArt(1, x, y)` for the art's corners; whole stage pixels, `-30000` = no position (behind the camera, no pin, or far off the stage) |
| `SR_Map filter <typeIndex> 0\|1` (X on a legend row) | none; the host keeps it for the process (the next open sends it back) |
| `SR_Map select <i>` (d-pad / A, or the current objective at tick 75) | `SR_MapRoute(i, nPoints)` then the route's `SR_MapRtXY` pairs (nPoints 0 = no route) |
| `SR_Map key <code>` / `SR_Map dbg <text>` / `SR_Map close` | none (log lines: the pad code ScrMap.HandleInput received; the screen's own diagnostics, 200 per open) |

Icons: 0 ring, 1 reticle, 2 superman, 3 dot, 4 pulse. State bits: 1 completed, 2 current objective.
**Every number on this wire stays below 2^24**: Apt numbers are float32 here (ENGINE_MAP "The pause
map"); an earlier packed (x, y) put every marker up to 32 px off in y and dropped the colour of
icons >= 1 (rolls race96_L / ch2_L, fixed in exe t15).
A title or line starting with `@` is a text.AST key the screen localizes
(`LocalizationAdapter.GetLocalizedText`, a lookup). Per-tick position replies are not logged
(`modBridgeDeliver` skips batches made only of `SR_MapXY` / `SR_MapRtXY` / `SR_MapArt`).

Log lines: `[MAP] open #n map=... markers=... (with position N, completed N)`, `[MAP]   type`,
one `[MAP]   marker` line per non-kitten marker (id, type, state, world position, title, lines),
`[MAP]   state:` (the DAS words the built-ins read), `[MAP]   triggers (type=seen/available)`,
`[MAP]   unreadable: ...` for a source the game did not have, `[MAP] check tick=N superman
game=(x,y) ours=(x,y) d=... px` (the self-check, ticks 1..3 then every 60th; a log line, never
fatal), `[MAP] stage <id> at (x,y)` + `stage superman world=(...)` at tick 90, `[MAP] select <i> <id>
route=N`, `[MAP] route <id>: start (x,y) mid (x,y) finish (x,y) in-window n/N`, `[MAP] filter`,
`[MAP] key`, `[MAP] dbg`, `[MAP] close after N ticks (worst self-check ...)`.

### 4.5 The SCRMAP size budget
A movie's own container stores each entry size in a field whose width its header sets. SCRMAP's
is 2 bytes (65,535 max). Retail SCRMAP's .apt is 19,021 bytes; with the markers it reached 64,792
(exe t15) and with the race trail 69,932. `tools/apt/bgfa.py` now **widens the field to 3 bytes**
when an entry outgrows it, and moves the entries down if the TOC grows. 308 retail movies already
use 3-byte sizes (MAIN's .apt is 105,010) and the game's reader takes the widths from the header;
SCRMAP at 3 bytes is proven in game by rolls *_P (exe t18). `tools/apt/patch_movie.py` still
refuses, by movie name, anything a 4-byte field cannot hold. **So the "64 KB limit" was a field
width, not a hard wall.** Keep SCRMAP's patch to the hooks and
the renderer; a large new screen feature is better as its own movie the screen loads
(`createEmptyMovieClip` + `loadMovie`, as a mod's map art is).

## 5. Proof (iter-466, 2026-10-06; exe snapshots archive/v1_map/exe/tN, rolls archive/v1_map/rolls)

**Repro.** Build the worktree, then run `archive/v1_map/batch.ps1 -Exe <exe> -Suffix <s>` from the repo
root. It holds the game slot and runs five rolls with tools/sr_roll.ps1:
- ch2, biz and twister on map_open.json (saves archive/v1_map/seeds/{ch2, bizarro1, twister}, port features on);
- race96 on map_race.json, with the level skip `reach 96` and the proof-only pack archive/v1_map/proofmod/map_proof
  (one marker of each type, pending and completed). The pack is copied into mods/ for that roll only;
- natural on map_natural.json (port features off).
Read the `[MAP]` lines in each roll's run.log and the frames in drive/attempt1/frames.

| What | Evidence | Result |
|---|---|---|
| Markers match the save | `[MAP]   state:` vs the legend (ch2_N / biz_N / twister_N) | ch2 objectiveStatus0=0x67, biz objectiveStatus3=0x1E000000, twister objectiveStatus0=0x1067; no minigame won, no kitten found: RACES 0/3, BIZARRO 0/5, KITTENS 0/100 on all three; no TYPE_OBJECTIVE available (31/0) and no live metro event, so these saves have no story or metro marker |
| Our projection = the game's | `[MAP] check` (the game's own SetSupermanIcon x, y vs ours), every roll | worst 0.14 px, including Superman off-centre (1130,669) and during the 2 s camera blend |
| The screen draws what the host computed | race96_N: `[MAP] stage proof.story at (445,366)`, `race_done (842,366)`, `rampage (643,179)`, `metro (643,573)` | race96_N_0.png: every icon sits on its logged point (before exe t15 every marker was up to 32 px low: float32, section 4.4) |
| Current objective + race data | race96: `marker race.96 state=2 desc='CURRENT OBJECTIVE - PAR TIME 1:15.00'`, auto-selected | box "BEGINNER'S BOOST / CURRENT OBJECTIVE / PAR TIME 1:15.00"; race.96's stage point (643,366) next to Superman (640,360) standing at its trigger |
| Race trail | archive/v1_map/proof/final_race96_trail.png (race96_R, t20), ch2_Q_race97_a.png (ch2_Q, t19), final_race97_trail.png (ch2_R) | race 96: one continuous line through all 40 waypoints from START to FINISH, clipped at the map window, chevrons pointing the way, under the markers and the (near-opaque) panels; race 97 Coast-to-Coast: the trail edge to edge across the map |
| Rampage readouts | ch2_N `select 3 rampage.67` | "YOU AM BIZARRO / STATUE 1 OF 5 / TARGET SCORE 1,000,000" |
| Legend, filters, d-pad | ch2_N / race96_N steps + `[MAP] filter` / `select` lines | UP/DOWN pick the type; LEFT/RIGHT/A step through its markers, including off-screen ones (details box); X hides/shows; the counts ignore the proof pack's markers (RACES 0/1, KITTENS 0/100 in race96_N) |
| Icons | race96_N_0.png | story = gold reticle, race = blue ring ">>", rampage = purple ring "B", kitten = small white ring, metro = red pulsing ring "!", completed = dimmed (proof race at 40 %) |
| Natural roll unchanged | natural_N / natural_P: `[APT_SCREEN] path='screens/misc/ScrMap'`, 0 `[MAP]` lines | the retail map (port features off) |
| No forcing | scripts/audit_forcing.py | UNGATED 2 (the dev baseline); the feature makes no guest call and no guest write; 0 ICALL_FAIL in every roll |

**Found and fixed on the way** (each has its evidence in its commit):
- Apt numbers are float32, so the wire values now stay below 2^24 (4.4).
- Property names are case-insensitive: `endS` was the method `Ends`, and as2asm now refuses such names.
- LB/RB never reach ScrMap.HandleInput, so the legend is driven by the d-pad.
- The movie container's 2-byte size field is now widened (4.5).

**Not in the data, or not done:**
- No per-statue best: the one stat is BestBizarroScore, shown as BEST (ALL STATUES).
- No rampage area.
- Trophies and art are not world-placed.
- The Fortress is not a map location.
- Found kittens (dimmed) were shown only with the proof pack; none of the three saves has a kitten found.
- A mod's map art (view kind `art`) is still unproven in game.
- No auto-framing of a highlighted race (coordinator decision, 2026-10-06). The map camera has no zoom-to function, and its only pan-to is TargetObjective, which retail never calls. The player pans and zooms with the sticks.
- The game's Superman icon draws over the legend when Superman sits under the legend corner (cosmetic).
