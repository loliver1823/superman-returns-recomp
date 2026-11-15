# Dev tools: user guide (iter-463 phase 2)

Debugging aids you switch on yourself. They are **never on by default**, they are **not mods**
and they are **not forcing**: a run that uses one is tagged `dev-tool-assisted` everywhere it can
be seen, and it never counts toward `docs/NATURAL_BASELINE.md` or a milestone.

Design and evidence: [`DEVTOOLS_DESIGN.md`](DEVTOOLS_DESIGN.md). Tool registry:
[`DEVTOOLS.json`](DEVTOOLS.json). Toolkit section: [`TOOLKIT.md`](TOOLKIT.md) 1.13.

## 0. Quick start: skip from a new game to the Warworld arena

What it does: on a NEW GAME, the game's own objective chain runs to the Warworld arena by itself.
The exe passes each objective with the same two stores the game makes when you win one, waits for
the game to start the next one, and answers the save prompt ("Continue Without Saving") with a
virtual pad press. The game's own code does everything else: the transitions, the Lois cinematic,
the XP award (2000) and the arena load. About 5 to 6 minutes after you pick START NEW GAME.

1. In the folder that holds `Superman Returns (USA)\` (the game folder; on the dev machine the
   repo root, `<repo>`), create **`dev.txt`** containing one line:
   ```
   levelskip
   ```
2. Copy the plan [`data/devtools/plans/warworld_from_new_game.txt`](../data/devtools/plans/warworld_from_new_game.txt)
   into the same folder as **`levelskip.txt`**.
3. Start the game as usual (`build\SupermanReturns.exe` from that folder). At the title press
   START, then at "LOAD A GAME?" pick **START NEW GAME** (A). Then let go of the controller: the
   skip starts by itself when the first objective (MSTobj01) is running, and you are in the
   arena when the "FAVORED GLADIATOR" HUD shows.
4. To switch it off, delete `dev.txt` (or the `levelskip` line). Nothing else changes.

What it tags (so a skipped run can never be mistaken for a natural one):
- the window title is `Superman Returns [dev-tool-assisted: levelskip]` from the first second
  (fixed in the iter-463 review: before it, the default present path never set the tag), and the log has
  `[RUN_CLASS] dev-tool-assisted tools=levelskip source=dev.txt` and one `[DEVTOOL] act=...` line
  per write or press;
- every save written in that session gets a `.devtool` marker beside it, and loading such a save
  later tags that run too (`devtool-derived-save`);
- `devtools_out\` (next to `dev.txt`) holds `steps.jsonl` (every step, with its exact writes)
  and a `.snap` + `.report.json` of the story state at each check point.

If something goes wrong the skip stops by itself and the game just carries on: an objective that
fails (the plan's `abort_when objective_failed`), a step that does not happen in time (each step
has a tick limit and a wall-clock `deadline`), or a state it does not recognise. The reason is
the last line of `devtools_out\steps.jsonl` and the `[DEVTOOL] refused` line in the log.

Other targets: the planner writes a plan for any objective it can reach from a new game, e.g.
`py -3.13 tools/devtools/skipplan.py plan --target <objective index> --image xenia:sound_gameplay --out levelskip.txt`
(section 3). It refuses what it cannot plan. Only the Warworld plan has been run end to end
(on Xenia and on our build, iter-463, 2026-10-01). This exact quick start (`dev.txt` +
`levelskip.txt` in the repo root, `build\SupermanReturns.exe` started from there with no `SR_*`
variables, START, START NEW GAME, hands off) was run once in the iter-463 review: the arena
with the WARWORLD banner about 4 minutes after START NEW GAME, story state EQUAL to the Xenia
oracle at all 4 check points (`DEVTOOLS_DESIGN.md` 6.9).

Two things that are NOT crashes: a window that vanishes at the end of an automated test is the
harness closing it; and a boot that crawls at the title usually means the PC is busy (six
`audit_forcing.py` runs at once made the front end ~8x slower in the review; the same exe booted
normally once they finished).

## 0b. From the menus: SETTINGS -> DEV TOOLS (iter-464)

With the `custom_frontend` mod on (`mods/enabled.txt`, docs/MODS.md) the same tools are in the
game's menus, as game pages:

- **Main menu:** SETTINGS -> **DEV TOOLS**.
- **In game:** pause -> **MORE OPTIONS** (the row above QUIT GAME) -> **DEV TOOLS**.

Controls: UP/DOWN move, **A** selects, **B** goes back, LEFT/RIGHT flip a toggle or move a
slider. START works as A and BACK as B.

1. **DEV TOOLS: OFF** -> A. The first time, a notice says what switching them on means: "Dev
   tools change the game. This session and every save made in it will be marked dev-tool
   assisted." **ENABLE DEV TOOLS** switches them on. The notice is shown once; the tools
   themselves start OFF at every launch. CANCEL leaves them off.
2. **LEVEL SKIP** (iter-465) is the skip hub. Its rows:
   - **SKIP: <active mission>** names the objective the game has ACTIVE right now (a story objective,
     a race, a Bizarro rampage, a metro event, a cutscene; read on the game's tick from the base
     flow's `objectiveActivated` / `objectiveBitIndex` / `objectiveWorldFlowName`). A completes exactly
     that one with the level skip's own `pass` (the two stores a win makes), so the game runs its
     own end-of-objective path. Nothing active: the row says **SKIP: NOTHING ACTIVE** and does
     nothing. The same command is `skip_mission` (command channel, tool `levelskip`) and **F10**.
   - **SKIP CHAPTER** (also **F11**; command `skip_chapter`) plays on to the next chapter through the
     game's OWN objective state, read every active tick: an active cutscene or chapter transition is
     left to end; any other active objective (the story's, or a metro event) gets its hero points capped
     (`xp_to_cap`) once and is then passed; with nothing active it reaches the first story trigger the
     game has made available (story-spine order from `objective_table_retail.tsv`); with none, it caps
     the hero points and reaches the metro event the game spawns next (a player would fly to it). It
     stops when `currentHeroPointChapter` rises (20-minute limit). It never starts a race or a rampage.
     Live: roll `chap4` (docs/PLAYTHROUGH.md) - chapter 2 -> 3 -> 4 -> 5 from the chapter-2 save.
   - **SIDE MISSIONS** lists the races (FastFlyer #92-#101) and the Bizarro rampages (#67-#71) with
     when they open; A starts one and completes it (a generated reach + pass plan, written to
     `<content root>/devtools_out/menu_side_<n>.txt`).
   - **STORY TARGETS** lists the plans in story order: METEOR STORM PART 2, METEOR STORM PART 3,
     WARWORLD ARENA, WARWORLD ROUND 2, BACK IN METROPOLIS (one plan per file in
     `data/devtools/plans/`; a `# menu:` line names it). Pick one, back out to the main menu, start a
     **NEW GAME** and let go of the controller: the skip starts when the first objective runs,
     exactly as in section 0. From the pause menu a story plan only starts from a new game.
   While a plan is armed or running the page offers **STOP LEVEL SKIP**. A race or rampage restarts
   by itself while Superman stands in its start circle (the game's own trigger), so after any skip
   ends one (SKIP, F10, SKIP CHAPTER, a plan's `pass`) the tool **lifts Superman clear**: once pad 0's
   actor is Superman again it moves him +45 X / +20 Y (up) through the game's own
   `SupermanActor_SetPosition_sub_821AA218` (the warp path; tool levelskip), logged
   `[DEVTOOL] act=levelskip.lift args=<flow> before=[..] after=[..]`. Live: roll `lift3`
   (docs/PLAYTHROUGH.md). SIDE MISSIONS only STARTS the chosen one; play it, or SKIP / F10 completes it. Live proof: `tools/scenarios/devtools_skip_menu.json` (+ `_play.txt`), docs/PLAYTHROUGH.md.
3. **STATUS** shows the session (natural or dev-tool assisted, the tools on), how many actions
   the tools made on the game, the level skip's state and last event, the save slot in use
   and the autosave setting. REFRESH re-reads it.
4. **CHEATS** is where the DT-2 tools appear (pause timers, mini-event control, free camera,
   sliders such as flight speed). None are built yet.
5. **DEV TOOLS: ON** -> A switches every tool off and stops a level skip. The session stays
   marked dev-tool assisted until you quit (window title ` [dev-tool-assisted: session]`).

The tagging is the same as for `dev.txt` (section 2): `[RUN_CLASS] dev-tool-assisted
tools=devmenu source=menu (devmenu on at runtime)` in the log, the window-title suffix, and a
`.devtool` marker beside every save of the session, including saves made before you switched the
tools on. Design: `DEVTOOLS_DESIGN.md` 13.

**Autosave and the level skip.** With "Autosave without asking" ON (docs/CUSTOM_FRONTEND.md 5.6)
the mod answers the game's save prompt **Continue Without Saving** while a level skip is armed
or running, so a skip never writes a save slot by itself.

## 1. Switching a tool on

| how | effect |
|---|---|
| `<content root>/dev.txt` | one tool name per line, `#` starts a comment (the grammar of `mods/enabled.txt`: BOM, CRLF, blanks and duplicates are fine). The content root is the folder that holds `Superman Returns (USA)/`. |
| `SR_DEVTOOLS=a;b` (or `a,b`, or `a+b` inside a comma-separated gate list) | replaces the file's list. `SR_DEVTOOLS=0` or empty = none. `SR_DEVTOOLS=@dev.txt` reads the file on purpose. |
| automated rolls | a process with `SR_INSTANCE_ID` set ignores `dev.txt`; every launcher that pins `SR_MODS=0` also pins `SR_DEVTOOLS=0` unless you set it (`run_recomp_x.ps1`, `replay_ab.ps1`, `sr_drive.ps1`, `run-30s.ps1`, the mutate/sweep drivers, difftest). |

Unknown names are logged (`[DEVTOOL] unknown tool <x>`) and ignored. There is no `all`.

| tool | class | built | what |
|---|---|---|---|
| `objstate` | observe | yes | `[OBJSTATE]` story-state dumps from the game's own tick (reads only) |
| `levelskip` | devtool | yes | runs a level-skip plan (`SR_DEVTOOLS_CMDS`) |
| `devmenu` | devtool | yes | the in-game Dev Tools pages (section 0b); normally switched on from the menu (after the one-time notice); listing it in `dev.txt` / `SR_DEVTOOLS` opens the pages ON. Never persisted by the menu |
| `cheats` | devtool | yes (iter-464) | the commands `pause_timers`, `minievents`, `metro_lock` (section 6) |
| `freecam` | devtool | yes (iter-464) | the retail debug camera, `freecam on/off` or F6 (section 6.4) |
| `hud`, `streaming` | observe | no | display-only overlays (design 10 steps 13-14) |
| `menuinput` | devtool | no | (design 10 step 13; the in-game Dev Tools menu is Apt pages instead) |

## 2. How a tagged run looks

- Log header: `[RUN_CLASS] dev-tool-assisted tools=levelskip source=env` (a devtool-class tool),
  or `[RUN_CLASS] natural+hud tools=objstate` (observe tools only). A natural run logs no
  `[RUN_CLASS]` line.
- Every `[GAUGE]` line ends `devtools=<mask> devtoolActs=<n>` (`0x0 0` on a natural run).
- The window title gains ` [dev-tool-assisted: levelskip]` (set at startup on the default
  event-pump path; a run with no dev tool keeps its normal title).
- Every `SR_PRESENT_DUMP` directory of a dev-tool-assisted run gets `RUN_CLASS.json`;
  `tools/verify_run.py` refuses it (exit 3), and refuses a run whose log carries a
  `[RUN_CLASS]` tag other than `natural+hud` or `devtoolActs > 0` (`--log`).
- `tools/run_recomp_x.ps1` prints a `runClass` column (`natural` only when untagged, no seed).
- **Saves follow the taint.** Every save package a dev-tool-assisted run writes gets a marker
  `<TYPE dir>/<package>.devtool` beside its folder. Loading a marked save taints the run
  (`[RUN_CLASS] devtool-derived-save`), and its later saves (the autosave) inherit the marker.
  `tools/import_xenia_saves.py` refuses a marked source unless `--allow-devtool`.
- A save-seeded run logs `[RUN_CLASS] save-seeded label=<SNAPSHOT.json label>`.

## 3. Level skip (`levelskip`)

The plan is the command language of `DEVTOOLS_DESIGN.md` 5.4, generated by the planner:

```
py -3.13 tools/devtools/skipplan.py plan --target 5 --image xenia:sound_gameplay --out plan.txt --json plan.json
```

Run it in our build (the exe executes it on the game's own tick, at the entry of
`sub_8223EB08`, and writes only the stores the game itself makes):

```
python tools/devtools/skip_run.py run --side ours --executor kernel --plan plan.txt --max-minutes 30
```

or by hand: `SR_DEVTOOLS=levelskip`, `SR_DEVTOOLS_CMDS=<plan>`, `SR_DEVTOOLS_OUT=<dir>`.
`<dir>` then holds `steps.jsonl` (every event, with each write list), `<label>.snap` +
`<label>.report.json` per `snapshot` step, `k<k>_<prim>_<event>.snap/.event.json` per write,
and `RUN_CLASS.json`. Check a run against the reference on its own bytes:

```
python tools/devtools/kernel_skip_check.py <dir> --plan <plan>
```

**The save prompt.** Our build is always signed in, so MSTobj03 ends with the "save your game?"
popup, and the popup stops every game tick. The exe answers it the way Xenia's executor does:
every 10 s the `[GAUGE]` thread checks whether a `pad` step is current and no tick has run since
the last check; if the step's guard holds (the prompt is up) it logs the `pad` event (variant
`idle`) and the exe's **virtual pad** presses the buttons (`DOWN A` = Continue Without Saving)
through `XamInputGetState`, as a player would. The log shows `[DEVTOOL] act=levelskip.pad`.

Limits today: `play_objective` refuses `unsupported`; a `deadline` is checked by the 10 s
`[GAUGE]` thread too, so it can fire up to 10 s late while no tick runs.

## 4. Objective-state dumps (`objstate`)

`SR_DEVTOOLS=objstate` logs `[OBJSTATE] {...}` (the reference report: base flow, DAS values,
won / found bits, loaded flows, triggers, base FSM state, XP and cap, run state, story key,
condition values) every `SR_DEVTOOLS_OBJSTATE_EVERY` active ticks (default 300) when the story
key changed, and with `SR_DEVTOOLS_OUT` also `objstate_<n>.snap` / `.report.json`
(`kernel_skip_check.py` checks them). Reads only; the run is `natural+hud`.

## 5. Gates read only by `kernel/devtools.cpp`

`SR_DEVTOOLS`, `SR_DEVTOOLS_CMDS`, `SR_DEVTOOLS_OUT`, `SR_DEVTOOLS_OBJSTATE_EVERY`,
`SR_DEVTOOLS_PLANS` (the folder the menu lists plans from; default `<content root>/data/devtools/plans`), and the
player's `dev.txt`. `scripts/audit_forcing.py` fails if any other file reads them.
`SR_DEVTOOLS`, `SR_DEVTOOLS_CMDS`, `SR_DEVTOOLS_OUT`, `SR_DEVTOOLS_OBJSTATE_EVERY`, and the
player's `dev.txt`. `scripts/audit_forcing.py` fails if any other file reads them. The DT-2 commands add
no gate: they are the `cheats` and `freecam` tools of the same closed set.

## 6. Dev-tool commands: pause timers, mini events, metro lock, free camera, warp, god mode (DT-2, iter-464)

Six commands, built on the retail game's own functions. The first four are lines of the plan language, `<command> on|off`; `warp` and
`infinite_stamina` are command-channel lines only (section 6.3). All run on the game's own tick thread at the drain
point (entry of `sub_8223EB08`), on the next active tick.

### 6.1 Switching them on

Add the tool names to `dev.txt` (or `SR_DEVTOOLS`): `cheats` for the first three, `freecam` for the
camera. Both are devtool class: the run is tagged dev-tool-assisted (section 2), every action logs
`[DEVTOOL] act=cheats.<command>` / `act=freecam.<command>` with its write list, and `devtoolActs`
counts it. With neither tool on, the hooks are the exact pass-through they always were.

### 6.2 Command reference

| command | tool | what it writes (every store is one the game's own code makes) | `off` |
|---|---|---|---|
| `pause_timers on` | cheats | byte +37 ("running") := 0 of every RUNNING script timer owned by a world flow (the objective scripts' timers: the store of retail `sub_82209270`, and of `ScriptCmd_PauseTimer`) | resumes exactly the timers `on` paused, if they still exist and are still paused |
| `minievents off` / `on` | cheats | objective manager `*0x8296DF5C` +4 := 0 / 1 (the BaseWorldFlow availability script asks `AreMiniEventsEnabled()` before it spawns a metro event) | `on` writes 1 (its value on every golden) |
| `metro_lock on` | cheats | Metropolis health `*0x8296DF50`: health := max, fraction := 1.0, damage-enabled byte +0x2C := 0 (the retail INFINITE_METROPOLIS code cheat makes the same stores plus a lock byte) | restores the damage byte `on` cleared, unless the game's lock byte is set |
| `freecam on` / `off` | freecam | the retail debug camera (section 6.4) | restores the game camera |
| `infinite_stamina on` / `off` | cheats | Superman's half of GodMode, by the retail INFINITE_STAMINA cheat's own steps: the upgrade registry `*0x8296DF6C` entry "StaminaInfinite" (`FindEntryByName_sub_821F0CB8`) +0x23 owned := 1, then `ApplyPowerups_sub_8221F808(registry)`; only while the Superman actor is up (+0x68), as the cheat checks. NOT the cheat's own bytes: its active flag and the cheat-used byte the achievements read | owned := 0, then ApplyPowerups |
| `warp X Y Z` / `warp camera` | cheats (+ freecam for `camera`) | none of its own. The pad-0 actor (`GameInput_GetPadActor_sub_8229E210`), checked to be the Superman actor class (vt 0x82009798), gets its vt+0xC `sub_821AA218(actor, &(X, Y, Z, 0))`, which stores actor +0x20 and moves the physics proxy's body. `camera` = the free camera's position (F7) | - |

Notes, stated plainly:
- `pause_timers` is one-shot: a timer the script starts or restarts afterwards runs
  (`ScriptCmd_StartTimer` sets the running byte again). Timers owned by entities (AI scripts; 64 of the
  67 on the gameplay golden) are not touched.
- `metro_lock` stops metro damage that goes through `SubtractMetropolisHealth` (the scripts' path).
  Superman's half is `infinite_stamina` (F8 toggles it).
- A command whose structure check fails is refused, never guessed: `no_timer_mgr`, `timer_map_shape`
  (the timer-tree walk did not end on its anchor after exactly the map's node count), `no_objective_mgr`,
  `no_metro`, `unreadable`; freecam adds `no_camera_mgr`, `camera_vt`, `camera_owned`, `already_on`,
  `not_on`, `no_input_slots`, `not_selected`, `tool_off`; warp adds `no_input_ctl`, `no_superman`,
  `bad_position`, `not_on` (`warp camera` without the free camera); infinite_stamina adds `no_upgrades`,
  `no_entry`, `no_superman`.
- `warp` logs `[DEVTOOL] act=cheats.warp ... before=[x,y,z] target=[...] after=[...]`; its ticket event
  carries the same. Superman keeps his velocity and state; the game decides what happens next (he may
  fall).- The Frida executor (Xenia side) and the reference `skipexec.py` run the first three exactly as the exe
  does (same write lists on the same image; DEVTOOLS_DESIGN 6.10). They refuse `freecam` (`unsupported`): it is a
  guest call, which Frida cannot make inside Xenia.

### 6.3 Three ways to send a command

1. **In a plan** (`SR_DEVTOOLS_CMDS` / `levelskip.txt`; under the `levelskip` tool). For example, an
   idle-safe objective: once MSTobj02 runs, stop metro damage, metro events and its timers, idle, then
   give everything back:
   ```
   abort_when objective_failed
   when das objectiveActivated == 1 and das objectiveBitIndex == 1 and base_state 107 do metro_lock on
   minievents off
   pause_timers on
   when das objectiveBitIndex == 1 and stable 9000 do snapshot idle_5min
   pause_timers off
   minievents on
   metro_lock off
   ```
   A plan's `off` undoes the same run's `on` from either channel (one shared state). A plan with
   these commands needs both tools: `levelskip` runs the plan, `cheats` (or `freecam`) allows the
   command, e.g. `SR_DEVTOOLS=levelskip+cheats`. `skip_run.py --executor kernel` adds them from the
   plan by itself. In the plan, the act is tagged with its own tool (`act=cheats.metro_lock`).
2. **From the in-game Dev Tools menu** (the FE lane's Apt pages, through an FSCommand bridge) or any
   host code: `kernel/devtools.hpp`
   ```cpp
   uint64_t sr::kernel::devToolsSubmit(const std::string& line, std::string* err); // ticket, or 0 + *err
   std::string sr::kernel::devToolsReadoutJson();                                  // the readout below
   bool sr::kernel::devToolsFreecamRouting();                                      // the camera is on
   ```
   `devToolsSubmit` takes ONE command line (a `when ... do` guard is allowed), parses it on the caller's
   thread, refuses it when its tool is off, and queues it (16 at most); it runs on the next active tick.
   The readout (`devtools_readout/1`) is cheap and safe from any thread; it never reads guest memory
   (the tick thread refreshes its copy every 30 active ticks and after every command):
   ```
   {"schema": "devtools_readout/1", "tools": ["cheats", "freecam"], "run_class": "dev-tool-assisted",
    "acts": 3, "freecam": "off",
    "commands": [{"id": 1, "line": "pause_timers on", "status": "done", "event": {... "writes": [...]}}],
    "cheats": {"timers": {"count": 67, "nodes": [{"node", "owner", "hash", "flow", "run"}, ...]},
               "minievents": {"enabled": 1}, "metro": {"health": 25000, "max": 25000, "damage": 1, "lock": 0, ...}}}
   ```
   Ticket status: `queued`, `running`, `done`, `refused`, `stopped`, `timeout`. A menu toggle shows a
   timer as paused when its `flow` is 1 and `run` is 0, mini events as on when `enabled` is 1, and the
   metro lock as on when `damage` is 0.
3. **F6** toggles the free camera (with the `freecam` tool on); **F7**, while the free camera is on and
   the `cheats` tool too, warps Superman to it (`warp camera`); **F8** (`cheats`) toggles
   `infinite_stamina`; **F2** (`cheats`, iter-467 native-a) submits the PRESET warp `warp X Y Z` from
   `SR_DEVTOOLS_WARP` (`"X Y Z"`, or `"X Y Z;X Y Z"`: each press takes the next point, the last repeats;
   unset = nothing), logged `[DEVTOOL] F2 preset warp #n: "warp X Y Z" -> ticket t` - the automated way to
   put every arm of a diagnostic roll at the same point F2 because nothing else binds it: F12 is the front end's screenshot key (settings / mod library UI), F9 the
   capture hotkey. The harness keyboard has no F2, so an automated roll sets `SR_DEVTOOLS_WARP_ON_MARK=1`: the
   same preset fires once at the harness channel's mark (the tape's `mark dump`), logged `[DEVTOOL] harness mark
   preset warp #1: ...`. None of them is in the keyboard-as-pad merge. The keys (and the free
   camera's IJKL / R / F) are read from the same sources as that merge (`kernel/host_automation.hpp`):
   a player's focused window, or, on an automated run (`SR_INSTANCE_ID` set), the harness keyboard
   channel only - the real keyboard and pad are ignored there, so a background roll never reacts to
   the user's typing.
   `tools/scenarios/devtools_freecam.json` drives all three on an automated roll (sr_drive keys `F6`, `F7`, `F8`,
   `KEY_W` / `KEY_A` / `KEY_S` / `KEY_D`, `KEY_I` / `KEY_J` / `KEY_K` / `KEY_L`, `KEY_R` / `KEY_F`).

### 6.4 The free camera

The game's debug camera is a free camera whose code ships in retail: the camera manager
`*0x8296D86C` builds it at +0x1150 and `CameraInputKeyHandler_sub_821691A8` flies it; retail has no
switch for it. `freecam on` claims the camera manager's owner mode, starts the debug camera at the
game camera's frame (`EulerAnglesToRotationMatrix_sub_82169048`), selects it
(`SetInterfaceState_sub_821745D0(mgr, 7, 1)`) and puts controller 2's input context on
`DebugContext`. The game keeps running; only the view moves.

While it is on, **your pad and keyboard drive guest controller
2** and controller 1 reads neutral (Superman stands still):

| control | pad | keyboard |
|---|---|---|
| move | left stick | WASD |
| look | right stick | IJKL |
| up / down | RT / LT | R / F |
| move speed up / down | RB / LB | E / Q |
| look rate up / down | A / B | Z (or Space) / X |
| pause the game / resume | START / LB+START | Enter / Q+Enter |
| single step while paused | R3 | - |
| lock onto an entity (cycle off / look at / approach) | X | C |
| entity readout on the debug console | L3 | - |

`freecam off` (or F6 again) puts back the camera state it replaced, applies any camera change the game
requested meanwhile, sets the game mode back to running, restores controller 2's context and gives the pad back to Superman. `freecam on` is refused
(`camera_owned`) while the camera manager's owner mode is taken (non-zero); while the camera is on, the game's own camera changes with owner mode 0 pend until `off`.

### 6.5 What is verified

Offline: the three implementations (reference
`skipexec.py`, `frida_skip.js` for Xenia, the exe's `devtools_skip_core.cpp`) give the same readout and
the same write list for every command on the three goldens, and the same events in 10 hand-derived
simulations (pause / resume with a restarted timer, mini events, the metro lock cycle and lock byte,
`off` without `on`, a timer map shorter than its count, the freecam refusal, a guarded command timing
out, observe ticks); planted defects in each are killed. `kernel_skip_check.py` checks a plan's cheat
commands on the run's own bytes, each `off` against the state its run's `on` left. `freecam` and
`warp` are guest calls: the reference cannot replay them, so their checks are the live ones. Live
results: DEVTOOLS_DESIGN 6.10.

