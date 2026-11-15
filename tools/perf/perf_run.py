#!/usr/bin/env python3
"""perf_run.py - one perf roll: a scenario under a named configuration, with SR_PERF_STATS
(frame events + per-thread CPU) and optionally the host sampling profiler, then the reports.

  python tools/perf/perf_run.py --scenario title|tutorial|gameplay|arena --config default|player|port|portdev
         --exe <tools/snapshot_exe.ps1 copy> --out archive/v1_perf/<tag>
         [--hz 250] [--seconds N] [--gates "NAME=V,NAME=V"] [--dry-run]

Run it from the MAIN repo root (the content, the input tape and the save roots resolve
there) while holding the game slot (tools/game_slot.py acquire --owner perf).

Scenarios (eras: tools/perf/perf_report.py DEFAULT_ERAS):
  title     no input: boot, intro movies, title/attract           (run_recomp_x.ps1 -K 1)
  tutorial  sr_drive tools/scenarios/perf_tutorial.json + the tutorial input tape: boot,
            intro, title, front end (LOAD A GAME? / mode select), the tutorial flight
  gameplay  sr_drive tools/scenarios/perf_player_gameplay.json (the custom front end: title ->
            MAIN MENU -> NEW GAME -> tutorial) + tools/controls/harness_play.py running
            tools/player/gameplay_play.txt (walk / look / fly). Needs --config port (or SR_PORT_FEATURES=1 in
            --gates); the dev log is --config port --gates SR_LOG=dev.
            --from-save <content root> (iter-467, owner 2026-10-10: gates from a lit, repeatable scene):
            MAIN MENU -> LOAD GAME -> the newest save slot instead of NEW GAME. The newest saved-game
            file under the root (outside .backup) and its slot with the most play time are picked
            (pick_save); the profile folders are copied into <out>/save_seed and handed to sr_drive as
            the read-only seed (sr_drive copies it again into the attempt's fresh SR_CONTENT_DIR), so
            the root is only read. Its settings/ folder is NOT copied unless --save-settings: a
            player's display.ini / costume.ini (4K borderless, a mod suit) would make the roll a
            different configuration from every new-game roll. <out>/save_start.json records the pick.
            The tape (--play) must handle the hub after LoadComplete (the HUD, or the METROPOLIS /
            FORTRESS menu): tools/perf/flicker_gate.py --from-save writes one. A load lands on that carousel
            (the drive presses A = METROPOLIS) or straight in the HUD (2026-10-14: the chapter seeds of
            MSTobj03's prompt and the Warworld chapter; nothing is pressed); a load that lands on neither fails
            the drive (load_game_steps, sr_drive `onlyIfObserved`). The landing counts only after the slot's
            load press (sr_drive `expectSince`), and the capture window opens only after the gameplay context
            and a fresh window of advancing sim ticks (2026-10-15: a HUD request is not gameplay).
  arena     the dev-tool level skip (tools/devtools/skip_run.py --executor kernel) to the
            Warworld arena, then --seconds in the arena. DEV-TOOL ASSISTED (tagged).
Configurations (the gate sets live in CONFIGS below; docs/PERFORMANCE.md):
  default   the exe's defaults (what an automated roll gets today)
  player    the player log profile (SR_LOG=player), port features OFF (natural)
  port      what a player gets: SR_PORT_FEATURES=1 + SR_LOG=player (PORT FEATURE roll)
  portdev   port features with every log line (SR_LOG=dev): the r5 player-lane baseline
Every config pins SR_INSTANCE_ID=perf and turns on SR_HIGHFPS_PACE / SR_PM4_GPU_TIME (the
tools/player/era_fps.py lines; cheap).
Every config turns the timed frame dumps OFF (SR_FRAME_DUMP=0; step 4 item 7e, 2026-10-18): the exe writes three
framebuffer BMPs at 120 / 240 / 360 s into the working folder whenever SR_INSTANCE_ID is set
(kernel/guest_context.cpp startFrameDumpThread) - test infrastructure inside the measured window that players never
have, and litter in the repo root. --frame-dump (or --gates SR_FRAME_DUMP=1) opts in; the BMPs the roll's
own log names ([FRAME_DUMP] shot N written) and that are new or changed since the launch are then moved from the
working folder into <out>/frame_dump/. An SR_FRAME_DUMP inherited from the
caller's environment is ignored: only the command line opts in.
Outputs under --out: perf/ (frames.csv, threads.csv, meta.txt, hprof_*.txt), the game log,
report.txt (perf_report), report.json, profile.txt (host_profile, when --hz), frame_dump/ (--frame-dump only).

--stage <SSD folder> (2026-10-14, owner: "okay do them on j just be wary about space and resources"; tutorial /
gameplay): sr_drive -Stage copies what the game READS (the exe folder, the disc, the installed shader cache) to that
folder (tools/perf/stage_roll.py: content-addressed by sha256, reused, leased, refuses below 50 GiB free) and
launches the staged exe; the outputs above stay where they are (exceptions: the game's generated mods/.cache moves to
the stage with the disc; runtime shader scratch stays in %TEMP%). perf_run stages first (--verify quick, a lease held
until it exits) so a first 2.4 GB copy runs before the play tape starts. --stage-source <folder holding the disc>
stages the disc from there, so a lane worktree without it can roll its own tools. --dry-run with --stage prints
stage_roll's plan (nothing written). After the roll: python tools/perf/stage_roll.py check <--out> (full witnesses:
--config port --gates SR_LOG=dev). Clean up: stage_roll.py clean --stage <folder>.
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time
from run_health import UNHEALTHY_EXIT, scan_logs, write_receipt

HERE = os.path.dirname(os.path.abspath(__file__))

# Gate sets per configuration. A gate baked default-ON needs no entry; an A/B passes =0.
CONFIGS = {
    "default": {},
    "player": {"SR_LOG": "player"},
    "port": {"SR_PORT_FEATURES": "1", "SR_LOG": "player"},
    "portdev": {"SR_PORT_FEATURES": "1", "SR_LOG": "dev"},
}
COMMON = {"SR_INSTANCE_ID": "perf", "SR_HIGHFPS_PACE": "1", "SR_PM4_GPU_TIME": "1",
          # 7e: the timed 120 / 240 / 360 s BMP dumps are off unless the command line opts in (--frame-dump)
          "SR_FRAME_DUMP": "0"}
FRAME_DUMP_GLOB = "frame_dump_*.bmp"   # kernel/guest_context.cpp: frame_dump_<SR_INSTANCE_ID>_<secs>s.bmp in the cwd
# the exe's own record of each file it wrote ([FRAME_DUMP] is a milestone tag: kept by the player log profile too)
FRAME_DUMP_WRITTEN = re.compile(r"\[FRAME_DUMP\] shot \d+ written: (frame_dump_[^\s/\\]+\.bmp) ")


def frame_dump_identities(root):
    """{basename: (size, mtime_ns)} of the timed BMPs in the working folder - taken right before the launch."""
    ids = {}
    for path in glob.glob(os.path.join(root, FRAME_DUMP_GLOB)):
        try:
            st = os.stat(path)
        except OSError:
            continue
        ids[os.path.basename(path)] = (st.st_size, st.st_mtime_ns)
    return ids


def frame_dumps_written(logs):
    """The BMP names the roll's own game log(s) say this process wrote."""
    names = set()
    for log in logs:
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "[FRAME_DUMP]" in line:
                        m = FRAME_DUMP_WRITTEN.search(line)
                        if m:
                            names.add(m.group(1))
        except OSError:
            continue
    return names


def collect_frame_dumps(root, out, before, logs):
    """--frame-dump: move the timed BMPs THIS roll wrote from the working folder into <out>/frame_dump/.
    A file is this roll's only when (1) the roll's own log names it ([FRAME_DUMP] shot N written: <name>) and
    (2) it is new or changed since the pre-launch snapshot `before` (frame_dump_identities). Anything else stays
    where it is: another process's dump (a stale instance before the harness's sweep writes under the same names,
    but not into this roll's log), or a pre-existing file whatever its mtime (Astra on e1aa21b0: a future-dated
    BMP passed the old mtime >= t0 rule). -> (moved paths, {name: why skipped})."""
    moved, skipped = [], {}
    for name in sorted(frame_dumps_written(logs)):
        src = os.path.join(root, name)
        try:
            st = os.stat(src)
        except OSError:
            skipped[name] = "named in the log, not in the working folder"
            continue
        if before.get(name) == (st.st_size, st.st_mtime_ns):
            skipped[name] = "unchanged since before the launch"
            continue
        dst_dir = os.path.join(out, "frame_dump")
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, name)
        shutil.move(src, dst)
        moved.append(dst)
    return moved, skipped


# tools/harness-play-wait (2026-10-16): a gameplay roll whose --play tape failed (harness_play exited non-zero: its
# channel never stamped, an await timed out, a hold_ctx never confirmed) did not drive the roll it specifies. perf_run
# writes <out>/play_failed.txt, still lets the roll's game run to its end (sr_drive owns the game's lifecycle, the
# sweep and the stage lease; its log and frames are the failure's evidence) and then EXITS with this code, so no
# caller grades the roll as played (zpk2's keep_off: the tape died at 60 s, the roll ran untapped and exited 0).
PLAY_FAILED_EXIT = 5


def finish_play(play):
    """Reap the input driver; a tape cut short by the roll is incomplete evidence."""
    code = play.poll()
    interrupted = code is None
    try:
        if interrupted:
            try:
                play.kill()
            except ProcessLookupError:
                pass
            code = play.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"complete": False, "status": "cleanup_failed", "pid": play.pid,
                "exit_code": play.poll(), "error": str(error)}
    return {"complete": code == 0 and not interrupted, "pid": play.pid, "exit_code": code,
            "status": "interrupted" if interrupted else ("complete" if code == 0 else "failed")}


# --from-save (iter-467): the game's saved-game file under a content root (docs/SAVES.md, tools/save_records.py).
SAVE_PARTS = ("454107ED", "00000001", "Superman Returns", "Superman Returns")
SAVE_SLOTS = 3


def find_saves(root):
    """Every saved-game file under a content root, newest first: [(mtime, path)]. The layout is
    <16-hex profile>/454107ED/00000001/Superman Returns/Superman Returns; the port's .backup copy is skipped."""
    import re
    out = []
    if not os.path.isdir(root):
        return out
    for prof in os.listdir(root):
        if not re.fullmatch(r"[0-9A-Fa-f]{16}", prof):
            continue
        f = os.path.join(root, prof, *SAVE_PARTS)
        if os.path.isfile(f):
            out.append((os.path.getmtime(f), f))
    out.sort(reverse=True)
    return out


def save_slots(path):
    """The three slot descriptors of a saved-game file (save_records.py PART1): [{slot, empty, progress,
    TotalPlayTime, chapter}]. Raises ValueError on a file that is not the game's container."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 28 + 88:
        raise ValueError("%s: shorter than the container header + part 1" % path)
    magic, total, l1 = struct.unpack_from(">3I", data, 0)
    if magic != 0x4D433032 or l1 != 88 or total != len(data):
        raise ValueError("%s: not the game's save container (magic %08X, len1 %d, size %d/%d)"
                         % (path, magic, l1, total, len(data)))
    slots = []
    for i in range(SAVE_SLOTS):
        prog, _u, play, chapter, _u2 = struct.unpack_from(">fIdII", data, 28 + 16 + 24 * i)
        slots.append({"slot": i, "empty": prog <= -1.0, "progress": round(prog, 4),
                      "TotalPlayTime": round(play, 1), "chapter": chapter})
    return slots


def devtool_marker(path):
    """The port's dev-tool marker beside the save folder (<...>/00000001/Superman Returns.devtool, JSON
    {"run_class": "dev-tool-assisted", "tools": ...}) -> its dict, {} when the file is absent (a played save)."""
    import json as _json
    m = os.path.join(os.path.dirname(os.path.dirname(path)), "Superman Returns.devtool")
    if not os.path.isfile(m):
        return {}
    try:
        with open(m, encoding="utf-8") as f:
            v = _json.load(f)
        return v if isinstance(v, dict) else {"raw": str(v)}
    except (OSError, ValueError):
        return {"raw": open(m, "rb").read()[:200].decode("utf-8", "replace")}


def pick_save(root):
    """--from-save: the newest saved-game file under root and its newest slot (the most play time; play time
    only grows within a playthrough) -> dict for <out>/save_start.json. downs = DOWN presses on the LOAD
    slot picker, which focuses the first in-use slot (save_load_portfeat.json): the in-use slots before the
    picked one. Raises ValueError when there is no usable save."""
    saves = find_saves(root)
    if not saves:
        raise ValueError("no saved game under %s (<profile>/%s)" % (root, "/".join(SAVE_PARTS)))
    mtime, path = saves[0]
    slots = save_slots(path)
    used = [x for x in slots if not x["empty"]]
    if not used:
        raise ValueError("%s: every slot is empty" % path)
    pick = max(used, key=lambda x: (x["TotalPlayTime"], x["slot"]))
    prof = os.path.relpath(path, root).split(os.sep)[0]
    return {"content_root": os.path.abspath(root), "save_file": path, "profile": prof, "mtime": mtime,
            "slots": slots, "slot": pick["slot"], "chapter": pick["chapter"], "progress": pick["progress"],
            "TotalPlayTime": pick["TotalPlayTime"], "downs": sum(1 for x in used if x["slot"] < pick["slot"]),
            "devtool_marker": devtool_marker(path),
            "other_files": [p2 for _m, p2 in saves[1:]]}


def seed_from_save(save, seed, with_settings=False):
    """Copy the picked profile folder (and settings/ only when asked) into seed; the root is only read."""
    if os.path.exists(seed):
        shutil.rmtree(seed)   # perf_run's own output folder (no junctions inside)
    os.makedirs(seed)
    shutil.copytree(os.path.join(save["content_root"], save["profile"]), os.path.join(seed, save["profile"]))
    if with_settings and os.path.isdir(os.path.join(save["content_root"], "settings")):
        shutil.copytree(os.path.join(save["content_root"], "settings"), os.path.join(seed, "settings"))
    return seed


# Where a load lands (the first screen LoadComplete requests): the METROPOLIS / FORTRESS carousel, or the HUD itself.
LANDS_CAROUSEL = "\\[APT_SCREEN\\] path='screens/frontend/ScrMainMenu'"
LANDS_HUD = "\\[APT_SCREEN\\] path='screens/hud/main_hud'"
# 2026-10-15 (Astra round 1 on tools/chapter-saves, P1): a HUD request is not gameplay - [APT_SCREEN] is logged BEFORE
# the screen request runs, and a frozen game with a surviving window passes a dwell. After either landing the drive
# needs (1) the game's own gameplay input context AFTER the load action (kernel/pc_controls.cpp, logged on every
# context change, capped at 400 lines - a from-save roll reaches the load after a handful) and (2) a FRESH
# [HIGHFPS_PACE] window (every ~2 s, on the guest's swaps; perf_run pins SR_HIGHFPS_PACE=1) whose guest SIM ticks
# advanced (sim_ticks/s >= 1.0, the sim manager's own tick counter): the game is playing, not frozen behind a window.
LOAD_BOUNDARY = "load-slot"   # sr_drive `expectSince`: the landing and the gameplay context count only after this step
IN_GAMEPLAY = "\\[PC_CONTROLS\\] context gameplay "
# 2026-10-20 (lane A, the heat tapes): an SR_INPUT_REPLAY tape owns every pad poll from the main_hud request (a v2 .inpr
# is anchored there), and kernel/stubs.cpp runs the keyboard merge - where pccontrols::mergePoll logs the context - only
# on a poll the tape does not own (`if (!replayed)`): on a tape roll IN_GAMEPLAY cannot come after the load (all four
# heat-tape rolls of 2026-10-20 failed 'in-game' while the game played the tape, a false perf_run rc 1). A tape roll's
# in-game step takes the HUD request after the load instead; sim-runs still proves the game plays (a frozen game fails
# there).
TAPE_IN_GAME = "\\[APT_REQ\\] #\\d+ path='screens/hud/main_hud'|\\[APT_SCREEN\\] path='screens/hud/main_hud'"
SIM_ADVANCES = "\\[HIGHFPS_PACE\\] w=\\d+ .* sim_ticks/s=[1-9][0-9]*\\.[0-9] "


def load_game_steps(downs, tape=False):
    """The sr_drive steps that replace NEW GAME: LOAD GAME on the custom MAIN MENU, the slot picker (downs DOWN
    presses from the first in-use slot), the slot's load (the host slot tracker's line), where it lands - LoadComplete's
    carousel, then A = METROPOLIS until a fresh HUD request (save_smoke_load.json / natural_load_lane.json's proved
    sequence), or straight in the HUD, where nothing is pressed (sr_drive `onlyIfObserved`) - counted only after the
    slot's load press (sr_drive `expectSince`), then the evidence that the game PLAYS: the gameplay input context after
    the load and a fresh window of advancing sim ticks, before the capture window opens. tape=True (an SR_INPUT_REPLAY
    gate): the HUD request after the load stands in for the gameplay context, which a tape roll cannot log (TAPE_IN_GAME)."""
    steps = [
        {"name": "down-to-load", "key": "DOWN", "holdMs": 250, "maxPulses": 1, "dwellMs": 1500},
        {"name": "load-game", "purpose": "LOAD GAME (MAIN MENU row 1)", "key": "A", "holdMs": 1000,
         "holdUntilConfirmed": True, "maxPulses": 1,
         "expect": "\\[SR_FSCMD\\].*answer cppId=\\S+ button=1", "expectMs": 15000},
        {"name": "slots-filled", "purpose": "LOADING CONTENT, then the slot list (5-8 s in roll save_load_natural_try2)",
         "dwellMs": 12000},
    ]
    if downs:
        steps.append({"name": "down-to-slot", "key": "DOWN", "holdMs": 250, "maxPulses": downs, "gapMs": 1200,
                      "dwellMs": 1500})
    steps += [
        {"name": "load-slot", "key": "A", "holdMs": 1000, "holdUntilConfirmed": True, "maxPulses": 1,
         "expect": "\\[SR_FSCMD\\].*SR_SaveEvent 'load' saveFlow\\+0x54=\\d -> slot \\d", "expectMs": 30000},
        {"name": "load-lands", "purpose": "LoadComplete's first screen: the METROPOLIS / FORTRESS carousel (ScrMainMenu: the "
         "Metropolis saves - the owner's post-Warworld save, the four boss saves, the chapter seeds from chapter 3 on) OR "
         "straight in the gameplay HUD (main_hud: the chapter seeds of MSTobj03's prompt and the Warworld chapter, 2026-10-14). "
         "The first screen a load requests is one of the two (rolls 01-04 of archive/saves/_verify); a load that lands on "
         "neither still fails here, loudly. Only lines after the slot's load press count (expectSince: a HUD or carousel "
         "line from before the load is not a landing; expectFresh would miss the HUD landing, logged 11 ms after the "
         "load event)",
         "expect": LANDS_CAROUSEL + "|" + LANDS_HUD, "expectSince": LOAD_BOUNDARY, "expectMs": 240000},
        {"name": "metropolis", "purpose": "A = METROPOLIS until the HUD, only after a carousel landing (onlyIfObserved: never A in "
         "play after a HUD landing): the capture window ('gameplay') opens only after a FRESH main_hud (orchestrator 2026-10-10: "
         "the void gate final_save_void2 captured the carousel)", "onlyIfObserved": LANDS_CAROUSEL, "key": "A", "holdMs": 1000,
         "holdUntilConfirmed": True, "maxPulses": 3, "gapMs": 15000,
         "expect": "\\[APT_REQ\\] #\\d+ path='screens/hud/main_hud'|\\[APT_SCREEN\\] path='screens/hud/main_hud'", "expectFresh": True, "expectMs": 240000},
        {"name": "in-game", "purpose": "the game's own gameplay input context after the load (both landings; the title's attract "
         "gameplay contexts are before the boundary): the HUD request alone is not gameplay",
         "expect": IN_GAMEPLAY, "expectSince": LOAD_BOUNDARY, "expectMs": 240000} if not tape else
        {"name": "in-game", "purpose": "an SR_INPUT_REPLAY roll: the tape owns the pad polls from the main_hud request, so the "
         "gameplay context line cannot come (TAPE_IN_GAME); the HUD request after the load stands in, and sim-runs below "
         "still proves the game plays",
         "expect": TAPE_IN_GAME, "expectSince": LOAD_BOUNDARY, "expectMs": 240000},
        {"name": "sim-runs", "purpose": "a FRESH [HIGHFPS_PACE] window with the guest sim advancing (>= 1 tick/s) after the HUD "
         "and the gameplay context: a frozen game behind a surviving window fails here instead of passing the dwell",
         "expect": SIM_ADVANCES, "expectFresh": True, "expectMs": 120000},
    ]
    return steps


def from_save_scenario(d, downs, tape=False):
    """perf_player_gameplay.json with NEW GAME -> the opening FMV replaced by the LOAD GAME steps (tape: an
    SR_INPUT_REPLAY roll, load_game_steps)."""
    names = [x["name"] for x in d["steps"]]
    i, j = names.index("new-game"), names.index("game-starts")
    d = dict(d)
    # iter-467: a loaded game has no opening FMV, so the new-game scenario's opening-movie skip goes too (its A
    # pulses would land in gameplay, and the capture window must still follow A on METROPOLIS directly)
    rest = [x for x in d["steps"][j + 1:] if not (x.get("cutsceneSkip") and x["name"] == "skip-opening-movie")]
    d["steps"] = d["steps"][:i] + load_game_steps(downs, tape) + rest
    d["purpose"] = ("perf_run --from-save: " + d.get("purpose", "") + " NEW GAME -> the opening FMV is replaced by "
                    "LOAD GAME -> the save slot (save_start.json).")
    return d


def frame_scratch_root(env):
    r"""tools/sr_artifacts.ps1 Get-SrFrameScratchRoot: SR_FRAME_SCRATCH (0 / off = none), else J:\sr_dumps, used only
    when that folder exists."""
    e = env.get("SR_FRAME_SCRATCH", "")
    if e and (e == "0" or e.lower() == "off"):
        return None
    root = e or r"J:\sr_dumps"
    return os.path.abspath(root).rstrip("\\/") if os.path.isdir(root) else None


def present_dump_scratch(env, out):
    r"""Step 4 lane D (2026-10-20, 4(c) closed diag-only): a title roll's present dumps (SR_PRESENT_DUMP=1 with no
    SR_PRESENT_DUMP_DIR) went to <root>\archive on the D: HDD - the synchronous BMP writes there stalled the
    presenting thread (the H1s of every-present captures). Like an sr_drive attempt (sr_artifacts.ps1
    New-SrAttemptRoot): <out>\frames is a junction to a fresh SSD scratch folder <root>\<stamp>_<8 hex>_a1 (+ its
    .source.txt, which the scratch GC reads), so every reader keeps the same path. Returns the dump dir or None (an
    explicit SR_PRESENT_DUMP_DIR, dumps off, or no scratch root: unchanged)."""
    v = env.get("SR_PRESENT_DUMP", "")
    if not v or v.startswith("0") or env.get("SR_PRESENT_DUMP_DIR"):
        return None
    root = frame_scratch_root(env)
    if not root:
        return None
    import uuid
    leaf = "%s_%s_a1" % (time.strftime("%Y%m%d-%H%M%S"), uuid.uuid4().hex[:8])
    target = os.path.join(root, leaf)
    link = os.path.join(out, "frames")
    os.makedirs(target)
    with open(os.path.join(root, leaf + ".source.txt"), "w", encoding="ascii") as f:
        f.write(link + "\n")
    try:
        import _winapi
        _winapi.CreateJunction(target, link)
    except (ImportError, AttributeError, OSError):
        if subprocess.run(["cmd", "/c", "mklink", "/J", link, target], capture_output=True).returncode != 0:
            return None
    return link


def run(cmd, env, cwd):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, env=env, cwd=cwd).returncode


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=("title", "tutorial", "gameplay", "arena"), required=True)
    ap.add_argument("--config", choices=sorted(CONFIGS), default="default")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hz", type=int, default=0, help="host profiler rate (0 = off)")
    ap.add_argument("--seconds", type=int, default=0, help="title: roll length; tutorial: unused "
                    "(scenario dwell); arena: linger in the arena")
    ap.add_argument("--gates", default="", help="extra NAME=VALUE pairs, comma-separated")
    ap.add_argument("--dwell", type=int, default=0, help="gameplay: seconds in the game after the HUD "
                    "(default: the scenario's 150)")
    ap.add_argument("--play", default="", help="gameplay: the harness_play script (default "
                    "tools/player/gameplay_play.txt; tools/perf/stutter_play.txt = a streaming-heavy flight)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--from-save", default="", help="gameplay: start from a saved game (LOAD GAME) under this content root instead of NEW GAME; the root is only read (see the docstring)")
    ap.add_argument("--save-settings", action="store_true", help="--from-save: also copy the root's settings/ (display / costume / input / mod options) into the seed")
    ap.add_argument("--stage", default="", help="tutorial / gameplay: stage the game's read set in this SSD folder "
                    "(e.g. J:\\sr_stage) and run the staged exe (sr_drive -Stage; tools/perf/stage_roll.py)")
    ap.add_argument("--stage-source", default="", help="--stage: the folder holding 'Superman Returns (USA)' to "
                    "stage from (default: beside the exe, its parent, or the repo root)")
    ap.add_argument("--frame-dump", action="store_true", help="opt in to the exe's timed frame dumps "
                    "(SR_FRAME_DUMP=1: BMPs at 120 / 240 / 360 s, or e.g. SR_FRAME_DUMP_SECS=20;35 in --gates); "
                    "off by default; the roll's BMPs are moved into <out>/frame_dump/")
    a = ap.parse_args(argv)
    if a.seconds < 0:
        print("--seconds must be nonnegative", file=sys.stderr)
        return 2

    root = os.getcwd()
    if a.stage_source and not a.stage:
        print("--stage-source needs --stage", file=sys.stderr)
        return 2
    if a.stage and a.scenario not in ("tutorial", "gameplay"):
        print("--stage needs a scenario sr_drive runs (tutorial, gameplay); %s launches through another harness"
              % a.scenario, file=sys.stderr)
        return 2
    disc_root = os.path.abspath(a.stage_source) if a.stage_source else root
    if not os.path.isdir(os.path.join(disc_root, "Superman Returns (USA)")):
        print("run from the main repo root (no 'Superman Returns (USA)' here)" if not a.stage_source else
              "--stage-source %s has no 'Superman Returns (USA)'" % disc_root, file=sys.stderr)
        return 2
    out = os.path.abspath(a.out)
    if os.path.exists(out) and os.listdir(out):
        print("--out exists and is not empty: " + out, file=sys.stderr)
        return 2
    env = dict(os.environ)
    gates = dict(COMMON)
    gates.update(CONFIGS[a.config])
    if a.frame_dump:
        gates["SR_FRAME_DUMP"] = "1"
    for pair in [p for p in a.gates.split(",") if p.strip()]:
        k, _, v = pair.partition("=")
        gates[k.strip().upper()] = v.strip()  # Windows environment names are case-insensitive
    # the exe's own rule: a nonempty SR_FRAME_DUMP decides by its first character (0 = off)
    frame_dump = not gates["SR_FRAME_DUMP"].startswith("0") if gates["SR_FRAME_DUMP"] else True
    env.update(gates)
    # Mirror host_automation::portFeaturesFor: explicit nonempty values use
    # first-character 0=off, otherwise on; unset follows the instance policy.
    # Check the actual child environment before save/stage/output side effects.
    port = env.get("SR_PORT_FEATURES", "")
    port_on = not port.startswith("0") if port else not bool(env.get("SR_INSTANCE_ID"))
    if a.scenario == "gameplay" and not port_on:
        print("--scenario gameplay needs port features enabled: effective SR_PORT_FEATURES=%r; "
              "--gates overrides --config port and the scenario requires the custom LoadCreate flow" % port,
              file=sys.stderr)
        return 2
    save = None
    if a.from_save:
        if a.scenario != "gameplay":
            print("--from-save needs --scenario gameplay (the custom front end's LOAD GAME)", file=sys.stderr)
            return 2
        try:
            save = pick_save(a.from_save)
        except ValueError as e:
            print("--from-save: %s" % e, file=sys.stderr)
            return 2
    os.makedirs(out, exist_ok=True)
    perf = os.path.join(out, "perf")
    if save and gates.get("SR_HIGHFPS_PACE") != "1":
        # 2026-10-15: the from-save drive's sim-runs step waits for a [HIGHFPS_PACE] window (load_game_steps)
        print("--from-save needs SR_HIGHFPS_PACE=1: the drive waits for a [HIGHFPS_PACE] window with the sim advancing",
              file=sys.stderr)
        return 2
    gates["SR_PERF_STATS"] = "1"
    gates["SR_PERF_DIR"] = perf
    if a.hz:
        gates["SR_HOST_PROFILE_HZ"] = str(a.hz)
    gates.setdefault("SDL_AUDIODRIVER", "disk")
    gates.setdefault("SDL_AUDIO_DRIVER", gates["SDL_AUDIODRIVER"])   # SDL3 spelling (tools/sdl_audio_env.py)
    if gates["SDL_AUDIODRIVER"] == "disk":   # never sdlaudio.raw in the working directory (both spellings)
        gates.setdefault("SDL_DISKAUDIOFILE", gates.get("SDL_AUDIO_DISK_OUTPUT_FILE", "NUL"))
        gates.setdefault("SDL_AUDIO_DISK_OUTPUT_FILE", gates["SDL_DISKAUDIOFILE"])
    env.update(gates)
    if a.scenario == "title":   # the drive scenarios get the same from sr_drive's attempt root
        scratch = present_dump_scratch(env, out)
        if scratch:
            gates["SR_PRESENT_DUMP_DIR"] = env["SR_PRESENT_DUMP_DIR"] = scratch
            print("perf_run: present dumps -> %s (a junction to the SSD frame scratch; SR_FRAME_SCRATCH=0 keeps the "
                  "old <root>/archive)" % scratch)
    with open(os.path.join(out, "perf_run.txt"), "w") as f:
        f.write("scenario=%s config=%s exe=%s hz=%d seconds=%d\n" % (a.scenario, a.config, a.exe, a.hz, a.seconds))
        f.write("timed_frame_dumps=%s\n" % ("on (opted in; moved into frame_dump/)" if frame_dump else "off"))
        for k, v in sorted(gates.items()):
            f.write("gate %s=%s\n" % (k, v))
        if save:
            f.write("from_save=%s slot=%d chapter=%d downs=%d settings=%s\n" % (
                save["save_file"], save["slot"], save["chapter"], save["downs"], "copied" if a.save_settings else "not copied"))
        if a.stage:
            f.write("stage=%s stage_source=%s\n" % (a.stage, a.stage_source or "(beside the exe / the repo root)"))
    if save:
        import json as _json
        save["settings_copied"] = bool(a.save_settings)
        with open(os.path.join(out, "save_start.json"), "w", encoding="utf-8") as f:
            _json.dump(save, f, indent=1)
    stage_args = []
    if a.stage:
        # sr_drive runs stage_roll.py with this interpreter; the plan below sees the same gates sr_drive will
        env["SR_PYTHON"] = sys.executable
        stage_args = ["-Stage", os.path.abspath(a.stage)] + (
            ["-StageSource", os.path.abspath(a.stage_source)] if a.stage_source else [])
    if a.dry_run:
        print("dry run: gates", gates)
        if a.stage:
            plan = [sys.executable, os.path.join(HERE, "stage_roll.py"), "plan", "--stage", os.path.abspath(a.stage),
                    "--exe", os.path.abspath(a.exe), "--root", root] + (
                ["--source", os.path.abspath(a.stage_source)] if a.stage_source else [])
            print("dry run: sr_drive would add " + " ".join(stage_args))
            return run(plan, env, root)
        return 0
    if a.stage:
        # Staged BEFORE the play tape starts: harness_play gives the game 60 s to open its input channel, and a first
        # stage copies 2.4 GB from D:. Leased to this process until it exits; sr_drive -Stage then re-verifies every
        # staged byte after the scenario's gates (copying nothing) and holds its own lease through the roll.
        import atexit
        lease = "perf_run-%d-%d" % (os.getpid(), int(time.time() * 1000))
        stage_py = os.path.join(HERE, "stage_roll.py")
        rc = run([sys.executable, stage_py, "stage", "--stage", os.path.abspath(a.stage),
                  "--exe", os.path.abspath(a.exe), "--root", root, "--json", os.path.join(out, "stage_pre.json"),
                  "--verify", "quick", "--lease-id", lease, "--lease-pid", str(os.getpid()),
                  "--lease-note", "perf_run " + out] + (
            ["--source", os.path.abspath(a.stage_source)] if a.stage_source else []), env, root)
        if rc != 0:
            print("--stage: stage_roll.py exit %d - no roll (stage_roll.py clean --stage %s frees space)"
                  % (rc, a.stage), file=sys.stderr)
            return rc
        atexit.register(subprocess.call, [sys.executable, stage_py, "release", "--stage", os.path.abspath(a.stage),
                                          "--lease-id", lease], env=env, cwd=root)
    ps = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
    log = None
    logs = []
    play_failed = None   # gameplay: why the --play tape failed (play_failed.txt), the roll's exit is then PLAY_FAILED_EXIT
    t0 = time.time()
    dumps_before = frame_dump_identities(root) if frame_dump else {}   # --frame-dump: what was there before the launch
    if a.scenario == "title":
        secs = a.seconds or 240
        rc = run(ps + [os.path.join(root, "tools", "run_recomp_x.ps1"), "-K", "1", "-TimeoutSeconds",
                       str(secs), "-ExePath", a.exe], env, root)
        # run_recomp_x sets SR_INSTANCE_ID=run<i> itself, so the log is superman_returns_run0.log, not
        # superman_returns_<our SR_INSTANCE_ID>.log (that name is a stale file from another harness, if any:
        # iter-467 p2's title A/B first graded one). It deletes run0's log before launching; the mtime check
        # still refuses a log this roll did not write.
        src = os.path.join(root, "superman_returns_run0.log")
        if os.path.exists(src) and os.path.getmtime(src) >= t0:
            log = os.path.join(out, os.path.basename(src))
            shutil.copy2(src, log)
    elif a.scenario == "tutorial":
        drive = os.path.join(out, "drive")
        rc = run(ps + [os.path.join(root, "tools", "sr_drive.ps1"), "-Scenario",
                       os.path.join(HERE, "..", "scenarios", "perf_tutorial.json"), "-Out", drive,
                       "-TimeoutSeconds", str(a.seconds or 900), "-ExePath", a.exe,
                       "-Gates", "SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr"] + stage_args, env, root)
        logs = glob.glob(os.path.join(drive, "attempt*", "superman_returns_run0.log"))
        log = logs[0] if logs else None
    elif a.scenario == "gameplay":
        drive = os.path.join(out, "drive")
        inst = gates["SR_INSTANCE_ID"]
        game_log = os.path.join(root, "superman_returns_%s.log" % inst)
        if os.path.exists(game_log):
            os.remove(game_log)  # a stale log must not feed harness_play's await
        # the tape starts before sr_drive (its stage pass, the previous game's sweep, the launch): it waits for the
        # game's input channel as long as the roll itself may take, not harness_play's default 60 s
        play_cmd = [sys.executable, os.path.join(HERE, "..", "controls", "harness_play.py"),
                                 "--instance", inst, "--log", game_log, "--script",
                                 os.path.abspath(a.play) if a.play else
                                 os.path.join(HERE, "..", "player", "gameplay_play.txt"),
                                 "--out", os.path.join(out, "play.jsonl"),
                                 "--channel-timeout", str(a.seconds or 1200)]
        scen = os.path.join(HERE, "..", "scenarios", "perf_player_gameplay.json")
        import json
        with open(scen, encoding="utf-8") as f:
            d = json.load(f)
        # sr_drive applies the scenario's own gates AFTER the environment, so a --gates value
        # for a gate the scenario sets (e.g. SR_PRESENT_DUMP=0 for a stutter census: every
        # sparse dump is a synchronous readback + file write on the render thread) has to be
        # written into the scenario copy (iter-466 stutter).
        sg = [p for p in str(d.get("gates", "")).split(",") if p.strip()]
        over = {k: gates[k] for k in (p.split("=", 1)[0].strip() for p in sg) if k in gates and
                any(q.split("=", 1)[0].strip() == k for q in a.gates.split(","))}
        if save:
            # the read-only seed: sr_drive copies it into the attempt's fresh SR_CONTENT_DIR (hermetic saves)
            env["SR_CONTENT_DIR"] = seed_from_save(save, os.path.join(out, "save_seed"), a.save_settings)
            d = from_save_scenario(d, save["downs"], tape=bool(gates.get("SR_INPUT_REPLAY")))
        if a.dwell or over or save:  # a copy of the scenario with the gameplay dwell / gates / start changed
            for s in d["steps"]:
                if a.dwell and s["name"] == "gameplay":
                    s["dwellMs"] = a.dwell * 1000
            d["gates"] = ",".join("%s=%s" % (p.split("=", 1)[0].strip(), over[p.split("=", 1)[0].strip()])
                                  if p.split("=", 1)[0].strip() in over else p for p in sg)
            scen = os.path.join(out, "scenario.json")
            json.dump(d, open(scen, "w"), indent=1)
        # Prepare the scenario/save before creating a child, and close/reap it on
        # every driver exit, including exceptions. Keep failed output for grading.
        with open(os.path.join(out, "play_stdout.txt"), "w", encoding="utf-8") as play_output:
            play = subprocess.Popen(play_cmd, stdout=play_output, stderr=subprocess.STDOUT, cwd=root,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            try:
                rc = run(ps + [os.path.join(root, "tools", "sr_drive.ps1"), "-Scenario", scen, "-Out", drive,
                               "-TimeoutSeconds", str(a.seconds or 1200), "-ExePath", a.exe] + stage_args, env, root)
            finally:
                playback = finish_play(play)
                with open(os.path.join(out, "play_status.json"), "w", encoding="utf-8") as f:
                    json.dump(playback, f, indent=2)
                if not playback["complete"]:
                    play_failed = "harness_play %s (exit %s); the tape did not complete" % (
                        playback["status"], playback["exit_code"])
                    with open(os.path.join(out, "play_failed.txt"), "w", encoding="utf-8") as f:
                        f.write(play_failed + "\n")
                    print("HARNESS_PLAY FAILED - " + play_failed)
        logs = glob.glob(os.path.join(drive, "attempt*", "run.log"))
        log = logs[-1] if logs else None
    else:  # arena
        rc = run([sys.executable, os.path.join(root, "tools", "devtools", "skip_run.py"), "run", "--side",
                  "ours", "--executor", "kernel", "--plan",
                  os.path.join(root, "data", "devtools", "plans", "warworld_from_new_game.txt"),
                  "--exe", a.exe, "--linger", str(a.seconds or 120), "--max-minutes", "20"], env, root)
        runs = sorted(glob.glob(os.path.join(root, "archive", "v1_devtools", "skip", "devtool_*_ours")),
                      key=os.path.getmtime)
        if runs and os.path.getmtime(runs[-1]) >= t0:
            with open(os.path.join(out, "skip_run_dir.txt"), "w") as f:
                f.write(runs[-1] + "\n")
            logs = glob.glob(os.path.join(runs[-1], "**", "superman_returns_*.log"), recursive=True)
            log = logs[0] if logs else None
    print("scenario exit code", rc)
    if frame_dump:
        dumps, skipped = collect_frame_dumps(root, out, dumps_before, sorted(set(logs + ([log] if log else []))))
        print("frame dumps (--frame-dump): %d moved into %s" % (len(dumps), os.path.join(out, "frame_dump")))
        for name, why in sorted(skipped.items()):
            print("frame dumps: %s not collected (%s)" % (name, why))
    health = scan_logs(sorted(set(logs + ([log] if log else []))))
    write_receipt(health, os.path.join(out, "run_health.json"))
    if not health["healthy"]:
        print("RUN HEALTH %s: %s (line %s); diagnostic artifacts retained" %
              (health["verdict"], health["reason"], health["line_number"]), file=sys.stderr)
    rep = [sys.executable, os.path.join(HERE, "perf_report.py"), perf, "--json", os.path.join(out, "report.json")]
    if log:
        rep += ["--log", log]
    with open(os.path.join(out, "report.txt"), "w") as f:
        report_rc = subprocess.run(rep, stdout=f, stderr=subprocess.STDOUT).returncode
    print(open(os.path.join(out, "report.txt")).read())
    if a.hz and glob.glob(os.path.join(perf, "hprof_*.txt")):
        with open(os.path.join(out, "profile.txt"), "w") as f:
            subprocess.run([sys.executable, os.path.join(HERE, "host_profile.py"), perf, "--eras-json",
                            os.path.join(out, "report.json"), "--exe", a.exe, "--stacks", "6"],
                           stdout=f, stderr=subprocess.STDOUT)
        print("profile:", os.path.join(out, "profile.txt"))
    if play_failed:
        print("perf_run: exit %d - the --play tape failed, the roll was not driven as specified: %s"
              % (PLAY_FAILED_EXIT, play_failed), file=sys.stderr)
        return PLAY_FAILED_EXIT
    return rc or report_rc or (0 if health["healthy"] else UNHEALTHY_EXIT)


if __name__ == "__main__":
    sys.exit(main())
