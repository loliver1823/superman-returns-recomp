#!/usr/bin/env python3
"""era_matrix.py - acceptance.zero's era matrix: the five whole-run rows R0-R4, one exe per arm, every attempt kept.

The ledger item acceptance.zero (docs/native_ledger.json): "the native meter reads 0 in every column over repeated
whole runs of every era". The rows come from Codex's final-acceptance template (dev archive/milestone2_20261016/
final_native_acceptance_20261016/final_native_acceptance_plan.json) and their tapes live in tools/perf/era/:

  R0 title    natural empty-save boot: intro movies, title, PRESS START, the attract cycle to the attract EOM (no input,
              port features OFF, sr_drive so the roll has its launch provenance)
  R1 newgame  empty-save NEW GAME, the opening FMV in full, the tutorial, a pause round trip (port features ON)
  R2 seed01   seed 01 -> CinematicLoisAsks -> the chapter-2 save -> Warworld (devtool-derived save)
  R3 ending   seed 17 -> two SKIP CHAPTER tickets (dev-tool assisted): CinematicLexIsland EOM, chapter >= 19, post-game
  R4 save     seed 12 fixed view + ordinary Up input: Metropolis with the HUD-glow ZPD queries, the fixed view only
              once the metro event's queries run (a bounded wait in the tape; tools/era-r4)

Arms (orchestrator 2026-10-17, dev archive/m2_claude_20261017/LANE_E_BRIEF_DRAFT.md):
  meter    the dev-default (oracle) exe: SR_NATIVE_METER=1 + SR_NATIVE_BRACKETS=1, TWO acquisitions per row, on the PC
           (one machine, so repeats compare). Graded per roll by era_census.py (--zpd zero, the exe hash), native_zero.py
           (all seven emulation columns 0 at the terminal snapshot, nrec > 0), run_health.py and terminal_snapshot.py.
  release  the release exe (the meter is compiled out): ONE acquisition per row, on the ROG; health, the terminal
           snapshot, the eras reached by their log markers (era_census's own marker and progression rules, no census
           frames) and a sparse present dump to VIEW (frame_sheet contact sheets). Never a meter claim.

    python tools/perf/era_matrix.py rows
    python tools/perf/era_matrix.py roll  --row R3 --arm meter --exe <snapshot>/SupermanReturns.exe --out <fresh dir>
           [--attempt-of 1] [--stage J:\\sr_stage --stage-source <folder holding the disc>] [--dump off|sparse]
    python tools/perf/era_matrix.py grade-roll --row R3 --arm meter --out <roll dir> --exe-sha256 <hex>
    python tools/perf/era_matrix.py grade --arm meter --exe-sha256 <hex> --json <out.json> (--set <set> | <roll dir> ...)
           [--repeats N (default: the arm's)] [--rows ...] [--pilot (a subset; never the acceptance verdict)]
    python tools/perf/era_matrix.py preflight --arm meter --exe <exe> [--rows ...] [--root <checkout>] [--stage <dir>]
    python tools/perf/era_matrix.py spec  --arm meter --exe <snapshot>/SupermanReturns.exe --out-spec <spec.json>
           [--repeats 2] [--rows R0,R1,R2,R3,R4] [--machine pc|ally] [--stage J:\\sr_stage --stage-source <dir>]
    python tools/set_runner.py run --spec <spec.json> --out <fresh set folder>      (the spec's last step = `grade`)

`roll` runs ONE acquisition from the checkout it lives in (cwd = that root: the disc junction, archive/saves): the
driver (perf_run.py for R1-R4, sr_drive.ps1 for R0) into <out>/roll, then grade-roll into <out>/grade. It strips every
inherited SR_* variable except the tool roots and SR_STUCK_HOLD (KEEP_ENV) so no operator shell gate leaks into an
acceptance roll, and it
exits 0 whenever the roll was acquired and graded - the VERDICT lives in <out>/grade/roll_verdict.json, never in the exit
code, so one failing row cannot stop the remaining rows of a set (all attempts are kept; no retry). It exits 2 when no
game log exists (the instrument failed: the set stops there). `grade` (the set's last step) exits 0 PASS, 1 FAIL,
3 INCOMPLETE (a row missing, short of its repeats, or a check without evidence).
A row with a PRECONDITION (R4: the tape's bounded wait for the query-producing event, ROWS[row]['event']) whose wait
missed is graded INCOMPLETE with `precondition_unmet` (never PASS); `roll` then re-acquires it (ROWS[row]['reacquire']
more launches, each in <out>/re<k>, all kept and graded) and `grade` lists such rolls but counts only the others toward
the row's repeats (a FAIL always counts; a row left short of its repeats is INCOMPLETE).

PER-ROLL CHECKS (roll_verdict.json; true / false / null = no evidence):
  driver     the driver's exit code is 0 (perf_run: 5 = the tape failed, 6 = unhealthy; sr_drive: the scenario failed)
  attempts   exactly one sr_drive attempt (no silent retry)
  play       R1-R4: perf_run's play_status.json complete (every tape command ran; an `expect` miss is in play.jsonl and
             is judged by the era checks, not here); R0: sr_drive's outcome.json SUCCESS
  health     run_health.py HEALTHY over every attempt log
  terminal   the graceful quit's terminal snapshot complete with a clean-exit receipt (complete, graceful, exit 0, no
             fallback), the stranded count present and equal in the record and the receipt, under the stranded policy
             (--stranded-policy; STRANDED_POLICIES): xdk-wait (default since tools/era-r4) - every stranded thread named
             by the quit and in GpuBlockOnFence with dropped_fences > 0 or GpuRingWaitForSpace with dropped_progress > 0
             on the settle line, anything else FAILS; forbid (accJ's rule) - stranded > 0 FAILS
  eras       meter: era_census.py PASS for the row's kind with --zpd zero and --exe-sha256 (every required era reached,
             pm4 0 in every era, gaps 0, no ring / kind-1 / BS item, no ZPD/EXT packet, the identity bound to the log);
             release: every required era's start marker / window / progression present (era_census rules)
  meter      meter arm only: native_zero.py PASS (terminal [NATIVE_METER_RUN] inside the snapshot: pm4 edram_res
             edram_xfer edram_load xma dispreg shm all 0, nrec > 0, frames = vdswaps - 1, the launch pid and hash)
  extra      the row's own clause: R1 the opening FMV's EOM after the HUD request and a closed pause window; R2 the
             CinematicLoisAsks progression; R3 the ending progression and both tickets done; R4 native ZPD records > 0
             (meter: era_census zpd_records; release: the [NATIVE_ZPD] terminal line issued > 0), after the tape's event
             wait hit (a miss: precondition_unmet, null)
  roll verdict: FAIL if any check is false, else INCOMPLETE if any is null, else PASS.
  Every roll also checks `identity` (launch_binding: provenance exeSha256 = the arm's exe, its pid = the log's single
  quit channel = the receipt's request / ack) and the stranded count is explicit in the terminal record AND receipt.

THE FINAL MATRIX (acceptance.zero; lane E 2026-10-17). Freeze the tools first (no merge into dev while a set runs; or
run from `git archive <commit>` of the tools beside a checkout with the disc and archive/saves):
  meter arm, PC (~2 h 20 min in one slot hold; QUIET_WINDOW cancels it):
    python tools/perf/era_matrix.py spec --arm meter --exe <dev-default snapshot>/SupermanReturns.exe \\
        --out-spec <S>.json --stage <fast-disk stage dir> --stage-source <main checkout>
    python tools/set_runner.py validate --spec <S>.json --out <set>   then   run --spec <S>.json --out <set>
    verdict: <set>/grade/attempt-0001/era_matrix.json (exit 0 PASS / 1 FAIL / 3 INCOMPLETE); per roll
    <set>/<row>_meter_a<n>/attempt-0001/r/grade/{roll_verdict,era_census,native_zero,run_health,terminal}.json
  release arm, ROG (one per row; the Ally's checkout at the same tools commit, the release snapshot copied whole):
    on the Ally (Invoke-AllyPs): python tools/perf/era_matrix.py spec --arm release --exe <ally snapshot>/SupermanReturns.exe
        --out-spec archive/<S>.json --machine ally
    Invoke-AllyRun -WorkDir <ally dev> -TimeoutSec 14400 -Command 'call ...\\sr-env.cmd && <python> tools\\set_runner.py
        run --spec archive\\<S>.json --out archive\\<set>'; copy the set back (sha256), VIEW each grade/frames_sheet.jpg.
  The equivalent per-roll CLI graders: era_census.py <kind>:<roll> --zpd zero --exe-sha256 <hex>; native_zero.py --log
  <roll>/drive/attempt1/run.log --receipt .../quit.json --provenance .../provenance.json --exe-sha256 <hex>;
  run_health.py --log ...; terminal_snapshot.py <log> --receipt <quit.json> --require --forbid-stranded.
"""
from __future__ import annotations

import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ERA = os.path.join(HERE, "era")
sys.path.insert(0, HERE)
import era_census  # noqa: E402
import native_zero  # noqa: E402
import run_health  # noqa: E402
import terminal_snapshot as tsnap  # noqa: E402

ZERO_COLS = ("pm4", "edram_res", "edram_xfer", "edram_load", "xma", "dispreg", "shm")
# The seeds' save payload sha256 (Codex's template seed_pins; the Ally's copies hashed equal 2026-10-17).
SEEDS = {
    "01_ch01_after_mstobj03": "7a88de164f9c783eb71a3b04020c1e5f3c2bf51e363e40d3a360ac621753178a",
    "12_ch12_after_bizarro1": "1cdc4c9bdfaf585fdc4417544e5a84519bc7fbce63fafe1c0e7a0e123ee00f7f",
    "17_ch17": "8a0ceeec2497658d2a6934a4b1e8bf729609d327d5d2d1ce8f7ed64a3446abbb",
}
SAVE_REL = os.path.join("0009000000000001", "454107ED", "00000001", "Superman Returns", "Superman Returns")
# Rows: kind = era_census's table; outer = the set_runner step timeout (the driver's own bound + grading).
ROWS = {
    "R0": {"kind": "title", "driver": "sr_drive", "scenario": "r0_title.json", "timeout": 720, "port": "0",
           "outer": 1200, "what": "natural empty-save boot: intro, title, attract (no input, port features off)"},
    "R1": {"kind": "newgame", "driver": "perf_run", "play": "r1_newgame_play.txt", "dwell": 180, "seconds": 900,
           "port": "1", "outer": 1300, "what": "NEW GAME, the opening FMV in full, the tutorial, pause/resume"},
    "R2": {"kind": "seed01", "driver": "perf_run", "play": "r2_seed01_play.txt", "dwell": 300, "seconds": 1100,
           "port": "1", "seed": "01_ch01_after_mstobj03", "outer": 1500,
           "what": "seed 01 -> CinematicLoisAsks -> the chapter-2 save -> Warworld"},
    "R3": {"kind": "ending", "driver": "perf_run", "play": "r3_ending_play.txt", "dwell": 900, "seconds": 1800,
           "port": "1", "seed": "17_ch17", "devtools": "r3_empty_plan.txt", "stage": False, "outer": 2300,
           "what": "seed 17 -> two SKIP CHAPTER tickets: the ending CinematicLexIsland, chapter 19, the post-game"},
    # R4 dwell: the tape is 245 s of fixed-view input + up to ~48 s of mode witness (template 260 s for 252 s) + up to
    # 300 s waiting for the query-producing event (tools/era-r4): 630 s; seconds 1000 (boot + load ~120 s on the ROG).
    # event: the tape's bounded wait (an `expect` step whose regex contains this text); its miss is the row's
    # precondition_unmet -> INCOMPLETE, never PASS, and `roll` re-acquires up to `reacquire` more times (all kept);
    # outer = (1 + reacquire) acquisitions of ~1150 s each.
    "R4": {"kind": "save", "driver": "perf_run", "play": "r4_fixed_view_play.txt", "dwell": 630, "seconds": 1000,
           "port": "1", "seed": "12_ch12_after_bizarro1", "outer": 3600, "event": r"\[NATIVE_ZPD\] census",
           "reacquire": 2,
           "what": "seed 12 fixed view + ordinary Up input: the HUD-glow ZPD queries (after the metro event starts)"},
}
# The graceful quit's stranded threads (orchestrator 2026-10-17, tools/era-r4; for the FRESH matrix after accJ, never a
# re-grade of accJ): "xdk-wait" - a stranded thread is acceptable only when the quit names it with its identity (one
# "[QUIT] stranded tid=..." record per counted thread, the count equal in the terminal record and the receipt), it sits
# in a known XDK GPU wait the parked CP can never end - GpuBlockOnFence on a fence of a publish the ring cut dropped
# (the settle line's dropped_fences > 0), or GpuRingWaitForSpace per native/quit-ringspace's rule (dropped_progress
# > 0) - and the quit is complete, graceful, exit 0, its terminal snapshot complete. Any other stranding, an incomplete
# snapshot or a non-zero exit FAILS. Why: the stranding is an artifact of the harness's ring cut (the game never quits
# this way on the console; kernel/quit.cpp strandedWait classifies exactly these) and it does not touch the whole-run
# counters (the measured window closes at the ring cut). "forbid" - stranded > 0 FAILS (accJ's pre-registered rule).
STRANDED_POLICIES = ("xdk-wait", "forbid")
STRANDED_WAITS = {"GpuBlockOnFence": "dropped_fences", "GpuRingWaitForSpace": "dropped_progress"}
SETTLE = re.compile(r"\[QUIT\] stage settle: .*? stranded=(\d+) .*?dropped_fences=(\d+)(?: dropped_progress=(\d+))?")
COMMON = (("SR_INSTANCE_ID", "finalnative"), ("SR_HIGHFPS", "uncapped"), ("SR_HIGHFPS_PACE", "1"),
          ("SR_FRAME_DUMP", "0"), ("SR_DRIVE_GRACEFUL_QUIT", "1"), ("SR_DRIVE_SKIP_CUTSCENES", "0"), ("SR_MODS", "0"),
          ("SR_ALLOW_FORCING", "0"), ("SR_LOG", "player"), ("SR_WINDOW_BACKGROUND", "1"), ("SR_HARNESS_INPUT", "1"),
          ("SR_HARNESS_REAL_INPUT", "0"), ("SDL_AUDIODRIVER", "disk"),
          ("SDL_AUDIO_DRIVER", "disk"),   # + the SDL3 spelling (tools/sdl_audio_env.py)
          ("SDL_DISKAUDIOFILE", "NUL"), ("SDL_AUDIO_DISK_OUTPUT_FILE", "NUL"))   # no sdlaudio.raw in the root
ARMS = {
    "meter": {"gates": (("SR_NATIVE_BRACKETS", "1"), ("SR_NATIVE_METER", "1")), "dump": "off", "repeats": 2,
              "machine": "pc"},
    "release": {"gates": (), "dump": "sparse", "repeats": 1, "machine": "ally"},
}
DUMPS = {"off": (("SR_PRESENT_DUMP", "0"),),
         "sparse": (("SR_PRESENT_DUMP", "1"), ("SR_PRESENT_DUMP_SPARSE_EVERY", "600"),
                    ("SR_PRESENT_DUMP_SPARSE_CAP", "200"))}
MACHINES = {"pc": {"min_free_gb": 8, "memory_limit_mb": 16384}, "ally": {"min_free_gb": 6, "memory_limit_mb": 12288}}
# Tool roots the drivers need; every other inherited SR_* is dropped (an operator shell's gate is not the row's).
# SR_STUCK_HOLD (Fable ruling 13): the close's stuck hold file. sr_drive reads it from its own process environment at
# startup (before any -Gates), so it must pass through here; it is a harness control the game never reads, and it is
# recorded in roll_plan.json kept_env.
KEEP_ENV = ("SR_MAIN_ROOT", "SR_PYTHON", "SR_SLOT_MIN_FREE_GB", "SR_FRAME_SCRATCH", "SR_SLOT_PRIORITY", "SR_STUCK_HOLD")
OPENING_FMV_MIN_FRAMES = 300  # the opening FMV (the new game's first movie after the HUD request) runs ~30 s
NATIVE_ZPD = re.compile(r"\[NATIVE_ZPD\] (\S+) mode=(\S+) issued=(\d+) ")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_extra_gates(text):
    """--extra-gates NAME=VALUE[,NAME=VALUE...] (fix/postgame-apt: a diagnostic or proof gate on top of the row's own;
    a name the row already sets is refused by gates_for's duplicate check, so an extra can never change a row gate)."""
    out = []
    for item in (text or "").split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError("--extra-gates item without '=': %r" % item)
        k, v = item.split("=", 1)
        out.append((k.strip(), v.strip()))
    return out


def gates_for(row, arm, exe, rolldir, dump=None, extra=()):
    """The row's complete gate list (ordered pairs) for one acquisition."""
    r = ROWS[row]
    g = list(COMMON) + list(ARMS[arm]["gates"]) + list(DUMPS[dump or ARMS[arm]["dump"]]) + list(extra)
    nb = os.path.join(os.path.dirname(os.path.abspath(exe)), "data", "shader_cache", "nb")
    if os.path.isdir(nb):
        g.append(("SR_NB_AOT_DIR", nb))
    g.append(("SR_PORT_FEATURES", r["port"]))
    if r.get("devtools"):
        g += [("SR_DEVTOOLS", "levelskip"), ("SR_DEVTOOLS_CMDS", os.path.join(ERA, r["devtools"])),
              ("SR_DEVTOOLS_OUT", os.path.join(rolldir, "devtools"))]
    else:
        g.append(("SR_DEVTOOLS", "0"))
    keys = [k.upper() for k, _ in g]
    dup = sorted({k for k in keys if keys.count(k) > 1})
    if dup:
        raise ValueError("duplicate gates for %s: %s" % (row, dup))
    return g


def driver_argv(row, arm, exe, rolldir, stage="", stage_source="", dump=None, root=ROOT, dwell=None, seconds=None,
                extra=()):
    """The driver command: the ROLL tools (perf_run, sr_drive and what they call) come from the root checkout the roll
    runs in (its disc, its archive/saves); the tapes come from this tool's tools/perf/era. dwell / seconds override the
    row's perf_run bounds (fix/postgame-apt: R3 with a >= 20 min post-game idle); extra = parse_extra_gates()."""
    r = dict(ROWS[row])
    if dwell:
        r["dwell"] = dwell
    if seconds:
        r["seconds"] = seconds
    gates = ",".join("%s=%s" % kv for kv in gates_for(row, arm, exe, rolldir, dump, extra))
    staged = bool(stage) and r.get("stage", True)
    if r["driver"] == "sr_drive":
        argv = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(root, "tools", "sr_drive.ps1"),
                "-Scenario", os.path.join(ERA, r["scenario"]), "-Out", os.path.join(rolldir, "drive"), "-ExePath", exe,
                "-GracefulQuit", "-TimeoutSeconds", str(r["timeout"]), "-Gates", gates]
        if staged:
            argv += ["-Stage", stage] + (["-StageSource", stage_source] if stage_source else [])
        return argv
    argv = [sys.executable, os.path.join(root, "tools", "perf", "perf_run.py"), "--scenario", "gameplay", "--config", "port", "--exe", exe,
            "--out", rolldir, "--hz", "0", "--gates", gates, "--play", os.path.join(ERA, r["play"]),
            "--dwell", str(r["dwell"]), "--seconds", str(r["seconds"])]
    if r.get("seed"):
        argv += ["--from-save", os.path.join(root, "archive", "saves", r["seed"])]
    if staged:
        argv += ["--stage", stage] + (["--stage-source", stage_source] if stage_source else [])
    return argv


def child_env():
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("SR_") or k.upper() in KEEP_ENV}
    env.setdefault("SR_PYTHON", sys.executable)
    return env


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=1, default=str)
        f.write("\n")


def check_seed(row, root=ROOT):
    seed = ROWS[row].get("seed")
    if not seed:
        return None
    f = os.path.join(root, "archive", "saves", seed, SAVE_REL)
    got = sha256(f) if os.path.isfile(f) else None
    if got != SEEDS[seed]:
        raise ValueError("seed %s payload %s != pinned %s" % (seed, got, SEEDS[seed]))
    return {"seed": seed, "sha256": got}


def cmd_roll(a):
    out = os.path.abspath(a.out)
    if os.path.exists(out) and os.listdir(out):
        print("era_matrix roll: --out exists and is not empty: " + out, file=sys.stderr)
        return 2
    exe = os.path.abspath(a.exe)
    root = os.path.abspath(a.root) if a.root else ROOT
    reacq = ROWS[a.row].get("reacquire", 0) if a.reacquire is None else a.reacquire
    # fix/postgame-apt: --dwell / --seconds / --extra-gates overrides (recorded in the plan; an altered roll is never
    # an acceptance acquisition - cmd_grade)
    extra = parse_extra_gates(getattr(a, "extra_gates", ""))
    dwell, seconds = getattr(a, "dwell", None), getattr(a, "seconds", None)
    # tools/era-r4: a roll whose only open check is the row's unmet precondition (R4: the query-producing event never
    # started within the tape's bound) is INCOMPLETE and is re-acquired here, up to `reacq` more launches, each into
    # its own folder (<out>/re<k>/{roll_plan.json, roll, grade}) - every acquisition kept and graded; the set grade lists
    # the unmet ones and counts only the others toward the row's repeats
    k = 0
    while True:
        sub = out if k == 0 else os.path.join(out, "re%d" % k)
        rolldir = os.path.join(sub, "roll")
        os.makedirs(sub, exist_ok=True)
        plan = {"schema": "era-matrix-roll/1", "row": a.row, "arm": a.arm, "what": ROWS[a.row]["what"], "exe": exe,
                "exe_sha256": sha256(exe), "seed": check_seed(a.row, root), "root": root, "tools": HERE,
                "attempt_of": a.attempt_of, "reacquisition": k, "stranded_policy": a.stranded_policy,
                "argv": driver_argv(a.row, a.arm, exe, rolldir, a.stage, a.stage_source, a.dump, root, dwell,
                                    seconds, extra),
                "gates": dict(gates_for(a.row, a.arm, exe, rolldir, a.dump, extra)),
                "overrides": {"dwell": dwell, "seconds": seconds, "extra_gates": dict(extra)},
                "kept_env": {kk: os.environ[kk] for kk in os.environ if kk.upper() in KEEP_ENV},
                "started": datetime.datetime.now().isoformat(timespec="seconds")}
        write_json(os.path.join(sub, "roll_plan.json"), plan)
        print("+ " + " ".join(plan["argv"]), flush=True)
        rc = subprocess.run(plan["argv"], cwd=root, env=child_env()).returncode
        plan.update(driver_exit=rc, ended=datetime.datetime.now().isoformat(timespec="seconds"))
        write_json(os.path.join(sub, "roll_plan.json"), plan)
        verdict = grade_roll(a.row, a.arm, sub, plan["exe_sha256"], stranded_policy=a.stranded_policy)
        print("era_matrix roll %s (%s)%s: driver exit %s -> %s %s" % (
            a.row, a.arm, (" re-acquisition %d" % k) if k else "", rc, verdict["verdict"], "; ".join(verdict["why"])))
        if rc in (5, 6):
            # Fable ruling 13: the driver's 5 (another copy of the exe runs; nothing killed) or 6 (SR_STUCK_HOLD: a
            # caught stuck game is held alive) stops the set - passed up so set_runner stops (exit 5 / 6), no
            # re-acquisition
            print("era_matrix roll %s: driver exit %d stops the set (Fable ruling 13)" % (a.row, rc), flush=True)
            return rc
        if verdict.get("instrument_failure"):
            return 2
        if not verdict.get("precondition_unmet") or k >= reacq:
            return 0
        k += 1
        print("era_matrix roll %s: precondition unmet (%s) -> re-acquisition %d of %d" % (
            a.row, verdict.get("precondition_unmet"), k, reacq), flush=True)


def attempt_logs(rolldir):
    return sorted(glob.glob(os.path.join(rolldir, "drive", "attempt*", "run.log")))


def read_json(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


QUIT_CHANNEL = re.compile(r"\[QUIT\] channel (Local\\SR_QUIT_\S+?_(\d+)) ready")


def launch_binding(log, receipt_path, rec, exe_sha):
    """Astra round 1 (P1): the LAUNCHED exe, for both arms - sr_drive's provenance.json beside the log (written before
    the launch: exeSha256 of the exe it started, pid) must name the expected hash, its pid must be the one process of
    the log's quit channel, and the quit receipt's request / ack must be that channel. {ok: True | False | None, why}."""
    prov, why = era_census.read_provenance(log)
    if prov is None:
        return {"ok": None, "why": why}
    sha = str(prov.get("exeSha256") or "").lower()
    pid = prov.get("pid")
    chans = set()
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[QUIT] channel " in line:
                m = QUIT_CHANNEL.search(line)
                if m:
                    chans.add((m.group(1), int(m.group(2))))
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        return {"ok": None, "why": "provenance.json has no exeSha256"}
    if sha != exe_sha.lower():
        return {"ok": False, "why": "launched %s, expected %s" % (sha, exe_sha.lower())}
    if len(chans) != 1:
        return {"ok": None, "why": "the log has %d quit channels (want exactly one)" % len(chans)}
    chan, cpid = next(iter(chans))
    if type(pid) is not int or pid != cpid:
        return {"ok": False, "why": "the provenance pid %s is not the log's process %d" % (pid, cpid)}
    if not isinstance(rec, dict):
        return {"ok": None, "why": "no quit receipt %s" % receipt_path}
    if rec.get("request") != chan or rec.get("ack") != chan.replace("Local\\SR_QUIT_", "Local\\SR_QUIT_ACK_", 1):
        return {"ok": False, "why": "the quit receipt's request/ack %s/%s is not the log's channel %s" % (
            rec.get("request"), rec.get("ack"), chan)}
    return {"ok": True, "why": "%s pid %d (provenance = quit channel = receipt)" % (sha[:12], cpid),
            "sha256": sha, "pid": cpid, "started": prov.get("startedUtc")}


def marker_eras(kind, log, rdir):
    """The eras reached by their markers (era_census's SEQ / windows / progression, which identify movies by their
    [MOVIE_PACE] EOM: a dev-default exe's log): the meter arm's R1 / R2 / R3 clauses read it."""
    sc = era_census.scan(log)
    prov, _ = era_census.read_provenance(log)
    eras, windows, prog, _ = era_census.eras_and_windows(kind, sc, rdir, prov)
    reached = {e[0] for e in eras}
    title = next((e for e in eras if e[0] == "title"), None)
    if title and any(m["kind"] == "EOM" and m["ln"] < title[2] for m in sc["movies"]):
        reached |= {"boot", "intro"}  # the intro movies played before the title request
    for w, (t0, t1) in windows.items():
        if t0 is not None and (t1 is not None or not w.startswith("pause_")):
            reached.add(w)
    need = era_census.REQUIRED[kind]
    return {"reached": sorted(reached), "missing": [e for e in need if e not in reached],
            "progression": {k: (str(v) if isinstance(v, datetime.datetime) else v) for k, v in prog.items()} or None,
            "windows": {w: [str(t0), str(t1)] for w, (t0, t1) in windows.items()}}, sc, windows, prog


# RELEASE WITNESSES (the release exe compiles the development oracles out, SR_ORACLE=0: no [PM4_COV] census, no
# [MOVIE_PACE] - kernel/cs_timeout_underflow_fix.cpp's movie observer sits inside `#if SR_ORACLE` - so no movie is
# identified by its EOM there; lane E checked the stage6c release log: 0 MOVIE_PACE lines). The release arm proves each
# row's ROUTE by the markers the release build does print - the screens, the front-end commands, the save events, the
# dev tool's tickets, the gameplay context with a mode, the roll's own saved game, the native ZPD census - and the
# movies by the presented frames (a sparse dump, contact-sheeted for viewing). The movie identities themselves are the
# meter arm's (era_census on the dev-default exe over the same tapes).
# The attract on release: kernel/fe_flow_watch.cpp's [FMVSM_POLL] logs the FMV state machine's state on every change
# (sampled every 2 s, not oracle-gated): state=2 while a movie plays, 0 when idle. The attract movie = the first
# state=2 at >= 30 s after the title request (era_census's attract rule) followed by state=0, at least
# ATTRACT_MIN_S long (lane E r0p2, dev-default 545a3b98 on the Ally: FMVSM state=2 07:38:30 -> 0 07:41:40, the same
# movie as MOVIE_PACE movie#7 EOM wall=190.94 s media=181.47 s, 78 s after ScrPressStart).
FMVSM = re.compile(r"\[FMVSM_POLL\] t=\d+s sm=[0-9A-F]+ state=(\d+) ")
ATTRACT_MIN_S = 60


def release_witnesses(row, log, rdir):
    """The release arm's era evidence for one roll: ({name: {"ok": bool, "at": line or why}}, missing names, scan)."""
    sc = era_census.scan(log)
    m = sc["marks"]
    w = {}

    def mark(name, hit, why_missing):
        w[name] = {"ok": hit is not None, "at": (hit[1] if hit else why_missing)}
        return hit

    def first(key, after=-1):
        return era_census.first_after(m[key], after)

    fe = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f):
            if "[NAT_FE] #1 natural FE master tick" in line:
                fe = (ln, line[1:24])
                break
    mark("fe_alive", fe, "no [NAT_FE] #1 natural FE master tick")
    title = mark("title", first("title"), "no ScrPressStart request")
    kind = ROWS[row]["kind"]
    if kind == "title":
        start = end = None
        t_title = era_census.ts_of(title[1]) if title else None
        with open(log, encoding="utf-8", errors="replace") as f:
            for ln, line in enumerate(f):
                if not title or ln <= title[0] or "[FMVSM_POLL]" not in line:
                    continue
                mv = FMVSM.search(line)
                t = era_census.ts_of(line[1:24])
                if not mv or t is None:
                    continue
                if start is None and mv.group(1) == "2" and t >= t_title + 30 * era_census.SEC:
                    start = (ln, line[1:24], t)
                elif start is not None and mv.group(1) == "0":
                    end = (ln, line[1:24], t)
                    break
        dur = (end[2] - start[2]).total_seconds() if start and end else None
        ok = dur is not None and dur >= ATTRACT_MIN_S
        w["attract"] = {"ok": ok, "at": {"start": start[1] if start else None, "end": end[1] if end else None,
                                         "seconds": dur} if start else "no FMV state 2 at >= 30 s after the title"}
    else:
        fem = mark("fe_menu", first("fe_menu", title[0] if title else -1), "no custom front end claim")
        after = fem[0] if fem else -1
        if kind == "newgame":
            new = mark("new_game", first("loading_new", after), "no NEW GAME answer")
            # APT_(REQ|SCREEN): [APT_REQ] lines stop after request 24 (kernel/fe_flow_watch.cpp n <= 24; a new game's
            # HUD is request 17, a carousel load's 23), [APT_SCREEN] logs a path's first 16 requests
            hud = mark("opening_fmv", era_census.first_after(hud_lines(log), new[0] if new else after),
                       "no HUD request after NEW GAME")
            ing = mark("ingame", first("context_mode", hud[0] if hud else after),
                       "no gameplay context with a mode after the HUD (the opening FMV's end)")
            pause = mark("pause", era_census.first_after(m["pause"], ing[0] if ing else after), "no ScrPauseMenu")
            mark("resume", first("context", pause[0]) if pause else None, "no gameplay context after the pause menu")
        else:
            lm = mark("load_menu", first("load_menu", after), "no LOAD GAME answer")
            ld = mark("loading", first("loading_save", lm[0] if lm else after), "no SR_SaveEvent 'load'")
            land = mark("landing", first("landing", ld[0] if ld else after), "no landing (HUD / carousel)")
            la = land[0] if land else after
            rs = era_census.roll_save(rdir, era_census.read_provenance(log)[0])
            if row == "R2":
                sv = mark("chapter_save", era_census.first_after(m["save_event"], la),
                          "no SR_SaveEvent 'save' after the landing (CinematicLoisAsks' chapter boundary)")
                ok = "error" not in rs and rs.get("chapter") == 2 and rs.get("play", 0) > rs.get("seed_play", 1e9)
                w["chapter_2"] = {"ok": ok, "at": rs}
                mark("warworld", first("context_mode", sv[0]) if sv else None,
                     "no gameplay context with a mode after the chapter save")
            elif row == "R3":
                ing = mark("ingame", first("context_mode", la), "no gameplay context with a mode after the landing")
                st = next((x for x in sc["skip_start"] if x["ln"] > la), None)
                w["ticket1_start_17"] = {"ok": bool(st and st["chapter"] == 17), "at": st or "no ticket 1 start"}
                ends = [e for e in sc["skip_end"] if st and e["ln"] > st["ln"]]
                d18 = next((e for e in ends if e["how"] == "done" and e["from"] == 17 and e["to"] == 18), None)
                d19 = next((e for e in ends if e["how"] == "done" and e["to"] >= 19 and (not d18 or e["ln"] > d18["ln"])),
                           None)
                w["done_17_18"] = {"ok": d18 is not None, "at": d18 or ["%d %s %d->%d" % (
                    e["ticket"], e["how"], e["from"], e["to"]) for e in ends]}
                w["done_to_19"] = {"ok": d19 is not None, "at": d19 or "no done -> 19 after ticket 1"}
                ok = "error" not in rs and rs.get("chapter", 0) >= 19 and rs.get("play", 0) > rs.get("seed_play", 1e9)
                w["chapter_19"] = {"ok": ok, "at": rs}
                mark("post_game", first("context_mode", d18["ln"]) if d18 else None,
                     "no gameplay context with a mode after the ending's ticket")
                if ing and st and ing[0] > st["ln"]:
                    w["ingame"] = {"ok": False, "at": "the first moded context came after the dev tool's start"}
            else:
                mark("ingame", first("context_mode", la), "no gameplay context with a mode after the landing")
    missing = [k for k, v in w.items() if not v["ok"]]
    return w, missing, sc


HUD_ANY = re.compile(r"\[APT_(?:REQ|SCREEN)\].*path='screens/hud/main_hud'")


def hud_lines(log):
    """Every HUD request line [(ln, ts)]: [APT_REQ] (logged for requests 1-24 only) or [APT_SCREEN] (a path's first 16)."""
    out = []
    with open(log, encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f):
            if "main_hud" in line and HUD_ANY.search(line):
                out.append((ln, line[1:24]))
    return out


def frames_sheet(rolldir, gdir):
    """The release arm's frames to VIEW: the sparse present dump of the roll -> grade/frames_sheet.jpg (24 evenly
    spaced tiles, tools/frame_sheet.py). Returns {count, sheet} (count 0: nothing to view)."""
    frames = sorted(glob.glob(os.path.join(rolldir, "drive", "attempt*", "frames", "*.bmp")) +
                    glob.glob(os.path.join(rolldir, "drive", "attempt*", "frames", "*.png")))
    res = {"count": len(frames), "sheet": None}
    if frames:
        sheet = os.path.join(gdir, "frames_sheet.jpg")
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "frame_sheet.py"), os.path.dirname(frames[0]),
                            "--out", sheet, "--count", "24"], capture_output=True, text=True)
        res.update(sheet=sheet if p.returncode == 0 and os.path.isfile(sheet) else None, rc=p.returncode,
                   tail=(p.stdout + p.stderr)[-400:])
    return res


def native_zpd_issued(log, snap):
    """The [NATIVE_ZPD] census: the terminal line inside the snapshot, else the last line (issued count) or None."""
    last = term = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if "[NATIVE_ZPD] " in line:
                m = NATIVE_ZPD.search(line)
                if m:
                    last = int(m.group(3))
                    if m.group(1) == "terminal" and snap.get("request_line") and \
                            snap["request_line"] < n < (snap.get("complete_line") or 0):
                        term = int(m.group(3))
    return term if term is not None else last


def event_state(row, rolldir):
    """tools/era-r4: the row's precondition from its tape's bounded wait (ROWS[row]['event'], an `expect` step of the
    tape) in <rolldir>/play.jsonl: ("hit" | "miss" | "absent" | None, detail). None: the row has no precondition."""
    ev = ROWS[row].get("event")
    if not ev:
        return None, "no precondition"
    path = os.path.join(rolldir, "play.jsonl")
    if not os.path.isfile(path):
        return "absent", "no play.jsonl (the tape's event wait never ran)"
    found = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                st = json.loads(line)
            except ValueError:
                continue
            step = str(st.get("step") or "")
            if step.startswith("expect ") and ev in step:
                found = st  # the last such step
    if found is None:
        return "absent", "the tape has no event wait (%s)" % ev
    hit = found.get("hit")
    return ("hit", "event at t=%.1f s: %s" % (found.get("t") or 0, str(hit)[:120])) if hit else \
        ("miss", "the query-producing event never started within the tape's bound (t=%.1f s)" % (found.get("t") or 0))


def terminal_check(snap, rec, log, policy):
    """The graceful quit under the stranded policy (STRANDED_POLICIES): (True | False | None, why). None only when the
    evidence is absent (no complete / failed snapshot, no stranded count)."""
    if policy not in STRANDED_POLICIES:
        raise ValueError("stranded policy must be one of %s" % (STRANDED_POLICIES,))
    st_snap = snap.get("stranded")
    st_rec = rec.get("stranded") if isinstance(rec, dict) else None
    if snap.get("status") == "complete" and snap.get("stranded_mismatch"):
        return False, "a stranded thread the quit did not name (count %s, %d records)" % (
            st_snap, len(snap.get("stranded_threads") or []))
    if not tsnap.is_complete(snap):
        # failed / rejected / incomplete are observed quit failures; absent / unconfirmed carry no exit evidence
        return (False if snap.get("status") in ("rejected", "failed", "incomplete") else None), \
            "terminal snapshot %s" % snap.get("status")
    # Astra round 1 (P1): a missing stranded count is unmeasured, never zero - the record AND the receipt must say it
    if st_snap is None or st_rec is None:
        return None, "stranded count missing (terminal %s, receipt %s)" % (st_snap, st_rec)
    if st_snap != st_rec:
        return False, "stranded terminal=%s != receipt=%s" % (st_snap, st_rec)
    if not tsnap.accept(snap, "allow"):
        return False, "terminal snapshot not complete"
    if st_snap == 0:
        return True, "stranded 0"
    if policy == "forbid":
        return False, "stranded %d (policy forbid)" % st_snap
    thr = snap.get("stranded_threads") or []
    tids = [d.get("tid") for d in thr]
    if len(thr) != st_snap or len(set(tids)) != len(tids) or any(type(t) is not int for t in tids):
        return False, "stranded %d but %d distinct named records" % (st_snap, len(set(tids)))
    bad = [d for d in thr if d.get("wait") not in STRANDED_WAITS or not d.get("dev") or not d.get("target")]
    if bad:
        return False, "stranded outside a known XDK GPU wait: %s" % "; ".join(
            "tid %s in %s" % (d.get("tid"), d.get("wait")) for d in bad)
    settle = None
    lo, hi = snap.get("request_line") or 0, snap.get("complete_line") or 0
    with open(log, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if lo < n < hi and "[QUIT] stage settle:" in line:
                m = SETTLE.search(line)
                if m:
                    settle = m
    if settle is None:
        return False, "no settle line with the dropped counts inside the snapshot"
    if int(settle.group(1)) != st_snap:
        return False, "the settle line's stranded=%s != %d" % (settle.group(1), st_snap)
    dropped = {"dropped_fences": int(settle.group(2)), "dropped_progress": int(settle.group(3) or 0)}
    need = sorted({STRANDED_WAITS[d["wait"]] for d in thr})
    if any(dropped[k] <= 0 for k in need):
        return False, "stranded in %s but %s" % (", ".join(sorted({d["wait"] for d in thr})),
                                                ", ".join("%s=%d" % (k, dropped[k]) for k in need))
    return True, "stranded %d acknowledged (policy xdk-wait): %s; %s" % (st_snap, "; ".join(
        "tid %d in %s target %s completed %s" % (d["tid"], d["wait"], d["target"], d.get("completed")) for d in thr),
        ", ".join("%s=%d" % (k, dropped[k]) for k in need))


def extra_checks(row, arm, era, census, windows, prog, sc, log, snap):
    """The row's own clause (module doc, extra): (bool | None, why)."""
    kind = ROWS[row]["kind"]
    # A clause whose evidence is ABSENT (the route did not get there) is None, as era_census reads a missing era
    # (INCOMPLETE); False only where the log contradicts it (R4: the queries ran as emulated packets).
    if row == "R0":
        return (True if era and "attract" in era["reached"] else None), "the attract EOM window %s" % (
            "reached" if era and "attract" in era["reached"] else "MISSING")
    if row == "R1":
        hud = next((ln for ln, _ in hud_lines(log)), None)
        fmv = None if hud is None else next((m for m in sc["movies"] if m["kind"] == "EOM" and m["ln"] > hud and
                                             m["frames"] >= OPENING_FMV_MIN_FRAMES), None)
        pause = [w for w, (t0, t1) in windows.items() if w.startswith("pause_") and t1 is not None]
        ok = True if fmv is not None and pause else None
        return ok, "opening FMV EOM %s, closed pause windows %s" % (
            "frames=%d media=%.2fs" % (fmv["frames"], fmv["media"]) if fmv else "MISSING", pause or "NONE")
    if row in ("R2", "R3"):
        if not prog:
            return None, "no %s progression (no landing)" % kind
        ok = True if prog.get("ok") else None
        if row == "R3" and ok:
            dones = [e for e in (prog.get("evidence") or {}).get("skip_ends", []) if e["how"] == "done"]
            ok = True if len(dones) >= 2 else None
            return ok, "ending progression ok; tickets done: %s" % ["%d->%d" % (e["from"], e["to"]) for e in dones]
        return ok, "progression -> %s: %s" % (prog.get("era"), "reached" if ok else prog.get("why"))
    if row == "R4":
        if arm == "meter":
            w = (census or {}).get("whole_run") or {}
            rec = w.get("zpd_records")
            if rec is None:
                return (False if w else None), "native ZPD records: none (the native ZPD path is %s; F 5B=%s)" % (
                    "on" if w.get("zpd_native") else "OFF", w.get("f_zpd"))
            return rec > 0 and w.get("f_zpd", 1) == 0 and w.get("f_ext", 1) == 0, \
                "native ZPD records %s, F 5B %s 5A %s" % (rec, w.get("f_zpd"), w.get("f_ext"))
        issued = native_zpd_issued(log, snap)
        return (None if issued is None else issued > 0), "[NATIVE_ZPD] issued=%s" % issued
    return None, "no clause"


def grade_roll(row, arm, out, exe_sha, rolldir=None, plan=None, stranded_policy="xdk-wait"):
    """One acquisition -> <out>/grade/*.json and roll_verdict.json (module doc, PER-ROLL CHECKS). rolldir: the
    driver's folder (default <out>/roll; grade-roll --roll grades a roll made elsewhere, read-only)."""
    rolldir = rolldir or os.path.join(out, "roll")
    gdir = os.path.join(out, "grade")
    os.makedirs(gdir, exist_ok=True)
    plan = plan if plan is not None else (read_json(os.path.join(out, "roll_plan.json")) or {})
    logs = attempt_logs(rolldir)
    checks, why, res = {}, [], {"schema": "era-matrix-verdict/1", "row": row, "arm": arm, "kind": ROWS[row]["kind"],
                                "exe_sha256": exe_sha, "logs": logs}
    if not logs:
        res.update(verdict="INCOMPLETE", instrument_failure=True, checks={}, why=["no game log under %s" % rolldir])
        write_json(os.path.join(gdir, "roll_verdict.json"), res)
        return res
    log = logs[0]
    att = os.path.dirname(log)
    receipt, prov = os.path.join(att, "quit.json"), os.path.join(att, "provenance.json")
    rc = plan.get("driver_exit")
    checks["driver"] = None if rc is None else rc == 0
    checks["attempts"] = len(logs) == 1
    if ROWS[row]["driver"] == "perf_run":
        ps = read_json(os.path.join(rolldir, "play_status.json"))
        checks["play"] = None if ps is None else bool(ps.get("complete"))
        why.append("play %s" % (ps or {}).get("status"))
    else:
        oc = read_json(os.path.join(att, "outcome.json"))
        checks["play"] = None if oc is None else oc.get("result") == "SUCCESS"
        why.append("sr_drive outcome %s" % (oc or {}).get("result"))
    health = run_health.scan_logs(logs)
    write_json(os.path.join(gdir, "run_health.json"), health)
    checks["health"] = bool(health["healthy"]) if health["verdict"] != "UNKNOWN" else None
    if not health["healthy"]:
        why.append("health %s: %s (line %s)" % (health["verdict"], health["reason"], health["line_number"]))
    snap = tsnap.read(log, receipt if os.path.isfile(receipt) else tsnap._AUTO)
    write_json(os.path.join(gdir, "terminal.json"), snap)
    rec = read_json(receipt) if os.path.isfile(receipt) else None
    # the stranded policy (STRANDED_POLICIES; tools/era-r4): a missing stranded count is unmeasured, never zero (Astra
    # round 1, P1) - the terminal record AND the receipt must carry it and agree; an exe before native/quit-drain
    # prints no field: that roll is INCOMPLETE here
    res["stranded_policy"] = stranded_policy
    checks["terminal"], tdet = terminal_check(snap, rec, log, stranded_policy)
    why.append("%s; stranded terminal=%s receipt=%s; quit: %s" % (tsnap.summary(snap), snap.get("stranded"),
                                                                 (rec or {}).get("stranded"), tdet))
    ident = launch_binding(log, receipt, rec, exe_sha)
    res["launch"] = ident
    checks["identity"] = ident["ok"]
    why.append("launch identity: %s" % ident["why"])
    census = None
    if arm == "meter":
        census = era_census.grade(ROWS[row]["kind"], rolldir, zpd_mode="zero", exe_sha256=exe_sha)
        write_json(os.path.join(gdir, "era_census.json"), census)
        checks["eras"] = {"PASS": True, "FAIL": False}.get(census["verdict"])
        why.append("era_census %s%s%s" % (census["verdict"],
                                          (" failed " + ",".join(census.get("failed", []))) if census.get("failed") else "",
                                          (" missing " + ",".join(census.get("missing_eras", [])))
                                          if census.get("missing_eras") else ""))
        if os.path.isfile(receipt) and os.path.isfile(prov):
            # the counters' grader takes the snapshot with its stranded threads ("allow": the measured window closes
            # at the ring cut) under xdk-wait - the `terminal` check above decides whether the stranding is acceptable
            nz = native_zero.grade(log, receipt, prov, exe_sha,
                                   stranded_policy="allow" if stranded_policy == "xdk-wait" else "forbid")
        else:
            nz = {"verdict": "UNKNOWN", "reasons": ["no quit.json / provenance.json beside the log"], "counters": None}
        write_json(os.path.join(gdir, "native_zero.json"), nz)
        checks["meter"] = {"PASS": True, "FAIL": False}.get(nz["verdict"])
        res["meter"] = nz.get("counters")
        why.append("native_zero %s%s" % (nz["verdict"], (" (" + "; ".join(nz["reasons"][:4]) + ")") if nz["reasons"] else ""))
        # the era rows for the matrix table (also needed by the R1 / R2 / R3 clauses)
        era, sc, windows, prog = marker_eras(ROWS[row]["kind"], log, rolldir)
    else:
        wit, missing, sc = release_witnesses(row, log, rolldir)
        era, windows, prog = {"reached": sorted(k for k in wit if wit[k]["ok"]), "missing": missing}, {}, {}
        write_json(os.path.join(gdir, "release_witnesses.json"), wit)
        checks["eras"] = True if not missing else None
        why.append("release witnesses: missing %s" % (missing or "none"))
        fr = frames_sheet(rolldir, gdir)
        res["frames"] = fr
        checks["frames"] = True if fr["count"] and fr["sheet"] else None
        why.append("frames to view: %d dumped, sheet %s" % (fr["count"], fr["sheet"]))
    res["eras_by_marker"] = era
    if arm == "release" and row != "R4":
        ok, text = (True if not era["missing"] else None), "the route's witnesses (release_witnesses.json)"
    else:
        ok, text = extra_checks(row, arm, era, census, windows, prog, sc, log, snap)
    # tools/era-r4: the row's precondition (R4: the tape's bounded wait for the query-producing event). A miss makes
    # the clause unmeasured (None: INCOMPLETE, never PASS) unless the log contradicts it (False stands); a precondition
    # row whose tape never ran the wait cannot PASS either
    est, edet = event_state(row, rolldir)
    res["event"] = {"state": est, "detail": edet}
    unmet = None
    if est == "miss" and ok is not False:
        ok, unmet = None, edet
    elif est == "absent" and ok is True:
        ok = None
    if est is not None:
        text += " | precondition: %s" % edet
    checks["extra"] = ok
    why.append("extra: " + text)
    if arm == "meter" and census:
        res["census_eras"] = [{k: e[k] for k in ("name", "frames", "pk", "zpd", "ext")} for e in census.get("eras", []) +
                              census.get("windows", [])]
    fail = [k for k, v in checks.items() if v is False]
    unknown = [k for k, v in checks.items() if v is None]
    res.update(checks=checks, failed=fail, unknown=unknown, why=why,
               verdict="FAIL" if fail else "INCOMPLETE" if unknown else "PASS",
               # recorded, re-acquired, not counted toward the row's repeats (cmd_grade) - only when nothing else is open
               precondition_unmet=unmet if (unmet and not fail and unknown == ["extra"]) else None)
    write_json(os.path.join(gdir, "roll_verdict.json"), res)
    return res


def cmd_grade_roll(a):
    v = grade_roll(a.row, a.arm, os.path.abspath(a.out), a.exe_sha256.lower(),
                   os.path.abspath(a.roll) if a.roll else None, stranded_policy=a.stranded_policy)
    print(json.dumps({k: v[k] for k in ("row", "arm", "verdict", "checks", "why") if k in v}, indent=1))
    return {"PASS": 0, "FAIL": 1}.get(v["verdict"], 3)


def cmd_grade(a):
    """The set: every roll's verdict (re-graded from its files), the one-exe rule, the repeat count per row."""
    a.repeats = a.repeats or ARMS[a.arm]["repeats"]
    if a.repeats < 1:
        print("era_matrix grade: --repeats must be >= 1", file=sys.stderr)
        return 2
    found = list(a.rolls)
    if a.set:
        found += sorted(os.path.dirname(p) for p in glob.glob(os.path.join(a.set, "*", "attempt-*", "r", "roll_plan.json")))
        # tools/era-r4: the re-acquisitions `roll` made of a row whose precondition was unmet (<step>/r/re<k>)
        found += sorted(os.path.dirname(p) for p in glob.glob(os.path.join(a.set, "*", "attempt-*", "r", "re*",
                                                                           "roll_plan.json")))
    rolls, seen = [], set()
    for d in found:
        d = os.path.abspath(d)
        key = os.path.normcase(os.path.realpath(d))
        if key in seen:  # Astra round 1 (P1): the same folder twice is one acquisition, never two
            continue
        seen.add(key)
        plan = read_json(os.path.join(d, "roll_plan.json"))
        if plan is None:
            rolls.append({"dir": d, "verdict": "INCOMPLETE", "why": ["no roll_plan.json"]})
            continue
        # re-graded into the grade step's own folder, never over the roll's (a set_runner resume checks the roll
        # step's saved outputs - r/grade/* - byte for byte)
        gout = os.path.join(os.path.dirname(os.path.abspath(a.json)), "regrade", "%03d" % len(rolls))
        v = grade_roll(plan["row"], plan["arm"], gout, a.exe_sha256.lower(), os.path.join(d, "roll"), plan,
                       stranded_policy=a.stranded_policy)
        v.update(dir=d, planned_exe_sha256=plan.get("exe_sha256"), planned_stranded_policy=plan.get("stranded_policy"),
                 overrides=plan.get("overrides"))
        rolls.append(v)
    problems = []
    for v in rolls:
        if v.get("arm") and v["arm"] != a.arm:
            problems.append("%s is a %s roll, not %s" % (v["dir"], v["arm"], a.arm))
        if v.get("planned_exe_sha256") and v["planned_exe_sha256"] != a.exe_sha256.lower():
            problems.append("%s planned exe %s" % (v["dir"], v["planned_exe_sha256"]))
        if v.get("planned_stranded_policy") and v["planned_stranded_policy"] != a.stranded_policy:  # one policy per set
            problems.append("%s rolled under stranded policy %s, graded under %s" % (
                v["dir"], v["planned_stranded_policy"], a.stranded_policy))
    rows = a.rows.split(",")
    # Astra round 1 (P1): one acquisition = one launch; two roll folders of one launch (a copy) count once and fail
    launches = {}
    for v in rolls:
        L = v.get("launch") or {}
        if L.get("sha256") and L.get("pid") is not None:
            launches.setdefault((L["pid"], L.get("started")), []).append(v["dir"])
    for k, dirs in launches.items():
        if len(dirs) > 1:
            problems.append("one launch (pid %s, started %s) graded as %d acquisitions: %s" % (k[0], k[1], len(dirs), dirs))
    # tools/era-r4: a roll whose only open check is its row's unmet precondition is RECORDED (listed, kept) but not
    # counted: the row needs `repeats` counted rolls (PASS or FAIL - a real failure always counts)
    unmet = [v for v in rolls if v.get("precondition_unmet")]
    counted = [v for v in rolls if not v.get("precondition_unmet")]
    count = {r: sum(1 for v in counted if v.get("row") == r) for r in rows}
    short = {r: n for r, n in count.items() if n < a.repeats}
    # Astra round 1 (P1): an ACCEPTANCE verdict needs every row of the matrix at the arm's repeat count; a subset or a
    # lower count is a pilot (--pilot), reported as such and never as the matrix's PASS
    full = set(rows) == set(ROWS) and a.repeats >= ARMS[a.arm]["repeats"]
    # fix/postgame-apt (Astra review 1, P2): a roll run with --dwell / --seconds / --extra-gates is a diagnostic or a
    # proof of something else, never an acquisition of the matrix as registered: it can grade, never be acceptance
    altered = [v["dir"] for v in rolls if any((v.get("overrides") or {}).values())]
    if altered:
        full = False
        problems_incomplete_note = "altered rolls (overrides: never acceptance): %s" % altered
    else:
        problems_incomplete_note = None
    if not full and not a.pilot:
        problems_incomplete = ["not the full matrix: rows %s x %d (acceptance needs %s x %d; --pilot for a subset)" % (
            ",".join(rows), a.repeats, ",".join(ROWS), ARMS[a.arm]["repeats"])]
    else:
        problems_incomplete = []
    if problems_incomplete_note:
        problems_incomplete.append(problems_incomplete_note)
    verdicts = [v["verdict"] for v in counted]
    if problems or "FAIL" in verdicts:
        verdict, rc = "FAIL", 1
    elif short or problems_incomplete or "INCOMPLETE" in verdicts or not rolls:
        verdict, rc = "INCOMPLETE", 3
    else:
        verdict, rc = "PASS", 0
    if a.pilot and not full:
        verdict = "PILOT " + verdict
    for v in rolls:
        m = v.get("meter") or {}
        print("%-3s %-8s %-10s %s%s" % (v.get("row", "?"), v.get("arm", "?"),
                                       v["verdict"] + (" (precondition unmet: recorded, not counted)"
                                                       if v.get("precondition_unmet") else ""),
                                       " ".join("%s=%s" % (c, m.get(c)) for c in ZERO_COLS + ("nrec", "frames")) if m else "",
                                       ("  failed " + ",".join(v.get("failed", []))) if v.get("failed") else ""))
        for w in v.get("why", []):
            print("      " + w)
    print("era_matrix %s: %s (rolls %d; counted per row %s; precondition unmet %d; repeats wanted %d; stranded "
          "policy %s%s)" % (a.arm, verdict, len(rolls), count, len(unmet), a.repeats, a.stranded_policy,
                            ("; " + "; ".join(problems + problems_incomplete)) if problems or problems_incomplete else ""))
    if a.json:
        write_json(a.json, {"schema": "era-matrix/1", "arm": a.arm, "exe_sha256": a.exe_sha256.lower(),
                            "repeats": a.repeats, "rows": rows, "count": count, "short": short, "problems": problems,
                            "not_full": problems_incomplete, "acceptance": full and not a.pilot,
                            "stranded_policy": a.stranded_policy,
                            "precondition_unmet": [{"dir": v["dir"], "row": v.get("row"), "why": v["precondition_unmet"]}
                                                   for v in unmet],
                            "verdict": verdict, "rolls": rolls, "zero_columns": ZERO_COLS})
    return rc


STAGE_FLOOR_GIB = 50   # tools/perf/stage_roll.py refuses to stage below this much free space


def preflight(rows, arm, exe, root, stage="", stage_source=""):
    """The checks a set makes before its first game (the spec's first step): [(name, ok, detail)]. Each is a reason a
    roll would fail for an INSTRUMENT reason rather than a route / meter one."""
    out = []
    exe_dir = os.path.dirname(exe)
    out.append(("exe", os.path.isfile(exe), exe))
    nb = os.path.join(exe_dir, "data", "shader_cache", "nb")
    nbn = len(glob.glob(os.path.join(nb, "*"))) if os.path.isdir(nb) else 0
    out.append(("package nb blobs", nbn > 0, "%d files in %s" % (nbn, nb)))
    # The SDL runtime is SDL3.dll since RELEASE_PLAN step 3; a pre-SDL3 exe (a baseline) carries SDL2.dll.
    sdl_ok = any(os.path.isfile(os.path.join(exe_dir, d)) for d in ("SDL3.dll", "SDL2.dll"))
    out.append(("package DLLs", sdl_ok and os.path.isfile(os.path.join(exe_dir, "dxcompiler.dll")), exe_dir))
    # The rolls read the installed shader cache from the root (cwd data/shader_cache/dxil + point_sprite.gs.dxil, as
    # every perf_run roll does; runtime translations persist to the set's %TEMP%). A package that ships its own cache
    # must ship the same bytes, else the roll would draw with shaders the exe was not built with.
    pkg, mine = os.path.join(exe_dir, "data", "shader_cache", "dxil"), os.path.join(root, "data", "shader_cache", "dxil")
    if os.path.isdir(pkg):
        def tree(d):
            return {os.path.relpath(os.path.join(a, f), d): sha256(os.path.join(a, f)) for a, _, fs in os.walk(d) for f in fs}
        tp, tm = tree(pkg), (tree(mine) if os.path.isdir(mine) else {})
        diff = sorted(set(tp) ^ set(tm)) + sorted(k for k in set(tp) & set(tm) if tp[k] != tm[k])
        out.append(("package dxil cache = the root's installed cache", not diff,
                    "%d files, %d differ%s" % (len(tp), len(diff), (": " + ", ".join(diff[:5])) if diff else "")))
    # Astra round 1 (P2): an unstaged row reads the disc at the root (perf_run / sr_drive cwd); a staged row stages it
    # from --stage-source (default: beside the exe, its parent, or the root - perf_run's own search)
    unstaged = [r for r in rows if not (stage and ROWS[r].get("stage", True))]
    if unstaged:
        disc = os.path.join(root, "Superman Returns (USA)")
        out.append(("disc at the root for the unstaged rows %s" % ",".join(unstaged), os.path.isdir(disc), disc))
    if len(unstaged) < len(rows):
        cands = [stage_source] if stage_source else [os.path.dirname(os.path.dirname(exe)), exe_dir, root]
        hit = next((c for c in cands if c and os.path.isdir(os.path.join(c, "Superman Returns (USA)"))), None)
        out.append(("disc for the staged rows", hit is not None, hit or "none of %s" % cands))
    for r in rows:
        tape = os.path.join(ERA, ROWS[r].get("play") or ROWS[r].get("scenario"))
        out.append(("%s tape" % r, os.path.isfile(tape), tape))
        if ROWS[r].get("devtools"):
            out.append(("%s dev-tool plan" % r, os.path.isfile(os.path.join(ERA, ROWS[r]["devtools"])), ROWS[r]["devtools"]))
        seed = ROWS[r].get("seed")
        if seed:
            try:
                out.append(("%s seed %s" % (r, seed), True, check_seed(r, root)["sha256"]))
            except (OSError, ValueError) as e:
                out.append(("%s seed %s" % (r, seed), False, str(e)))
    for tool in (os.path.join(root, "tools", "sr_drive.ps1"), os.path.join(root, "tools", "perf", "perf_run.py"),
                 os.path.join(root, "tools", "controls", "harness_play.py")):
        out.append(("roll tool", os.path.isfile(tool), tool))
    if stage and any(ROWS[r].get("stage", True) for r in rows):
        try:
            import shutil as _sh
            free = _sh.disk_usage(os.path.splitdrive(os.path.abspath(stage))[0] + os.sep).free / 2 ** 30
            size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(exe_dir) for f in fs) / 2 ** 30
            # the floor, the exe folder (a new exe is staged once) and 3 GiB of headroom (the disc is already staged)
            need = STAGE_FLOOR_GIB + size + 3
            out.append(("stage free space", free >= need, "%.1f GiB free on %s, need >= %.1f (floor %d + package "
                                                          "%.1f + 3)" % (free, stage, need, STAGE_FLOOR_GIB, size)))
        except OSError as e:
            out.append(("stage free space", False, str(e)))
    quiet = os.path.join(os.path.dirname(root), "native", "archive", "v1_native", "QUIET_WINDOW")
    out.append(("no QUIET_WINDOW", not os.path.exists(quiet), quiet))
    return out


def cmd_preflight(a):
    root = os.path.abspath(a.root) if a.root else ROOT
    res = preflight(a.rows.split(","), a.arm, os.path.abspath(a.exe), root, a.stage, a.stage_source)
    for name, ok, detail in res:
        print("%-5s %s - %s" % ("ok" if ok else "FAIL", name, detail))
    bad = [n for n, ok, _ in res if not ok]
    if a.json:
        write_json(a.json, {"schema": "era-matrix-preflight/1", "ok": not bad, "checks": [
            {"name": n, "ok": ok, "detail": d} for n, ok, d in res]})
    print("era_matrix preflight: %s" % ("OK" if not bad else "FAILED (%s)" % ", ".join(bad)))
    return 0 if not bad else 2


def cmd_spec(a):
    """A set_runner spec (docs/INVESTIGATION_WORKFLOW.md): one step per acquisition (rows interleaved by repeat, so the
    repeats are separated in time), then `grade --set {run}` (every attempt directory of the set, none skipped).
    Inputs pinned: the whole exe folder, tools/perf/era, the seeds, the root's roll tools and this tool's graders; the
    set holds the game slot (perf_run / sr_drive run inside it, as native_proof --slot-held does)."""
    exe = os.path.abspath(a.exe)
    root = os.path.abspath(a.root) if a.root else ROOT
    arm = ARMS[a.arm]
    reps = a.repeats or arm["repeats"]
    machine = MACHINES[a.machine or arm["machine"]]
    rows = a.rows.split(",")
    me = os.path.join(HERE, "era_matrix.py")
    steps = [{"id": "preflight", "argv": ["{python}", me, "preflight", "--arm", a.arm, "--exe", exe, "--root", root,
                                          "--rows", ",".join(rows), "--json", "{step}/preflight.json"] +
              (["--stage", a.stage] if a.stage else []) + (["--stage-source", a.stage_source] if a.stage_source else []),
              "timeout_seconds": 600, "reuse": "never", "outputs": ["preflight.json"]}]
    for rep in range(1, reps + 1):
        for row in rows:
            argv = ["{python}", me, "roll", "--row", row, "--arm", a.arm, "--exe", exe, "--out", "{step}/r",
                    "--attempt-of", str(rep), "--root", root, "--stranded-policy", a.stranded_policy]
            if a.dump:
                argv += ["--dump", a.dump]
            if a.stage:
                argv += ["--stage", a.stage] + (["--stage-source", a.stage_source] if a.stage_source else [])
            outer = ROWS[row]["outer"]
            if a.dwell:
                argv += ["--dwell", str(a.dwell)]
            if a.seconds:
                argv += ["--seconds", str(a.seconds)]
                outer = max(outer, a.seconds + 500)
            if a.extra_gates:
                parse_extra_gates(a.extra_gates)   # refuse a malformed list now, not mid-set
                argv += ["--extra-gates", a.extra_gates]
            steps.append({"id": "%s_%s_a%d" % (row, a.arm, rep), "argv": argv, "timeout_seconds": outer,
                          "reuse": "never", "outputs": ["r/roll_plan.json", "r/grade/roll_verdict.json"]})
    # an altered set (--dwell / --seconds / --extra-gates) is always a pilot (Astra review 1, P2)
    pilot = set(rows) != set(ROWS) or reps < arm["repeats"] or bool(a.dwell or a.seconds or a.extra_gates)
    steps.append({"id": "grade", "argv": ["{python}", me, "grade", "--arm", a.arm, "--exe-sha256", sha256(exe),
                                          "--repeats", str(reps), "--rows", ",".join(rows), "--set", "{run}",
                                          "--stranded-policy", a.stranded_policy,
                                          "--json", "{step}/era_matrix.json"] + (["--pilot"] if pilot else []),
                  "timeout_seconds": 3600, "reuse": "never", "outputs": ["era_matrix.json"]})
    roll_tools = [os.path.join(root, "tools", p) for p in (
        "sr_drive.ps1", "sr_drive_core.ps1", "sr_quit.ps1", "bg_window.ps1", "sr_artifacts.ps1", "game_slot.py",
        "set_runner.py", "owned_process.py", os.path.join("controls", "harness_play.py"),
        os.path.join("scenarios", "perf_player_gameplay.json"), os.path.join("perf", "perf_run.py"),
        os.path.join("perf", "perf_report.py"), os.path.join("perf", "run_health.py"), os.path.join("perf", "stage_roll.py"))]
    graders = [os.path.join(HERE, p) for p in (
        "era_census.py", "transport_census.py", "terminal_snapshot.py", "native_zero.py", "native_meter.py", "run_health.py",
        "perf_report.py", "perf_run.py")]
    seeds = sorted({ROWS[r]["seed"] for r in rows if ROWS[r].get("seed")})
    inputs = [os.path.dirname(exe), ERA] + sorted(set(roll_tools + graders)) + [
        os.path.join(root, "archive", "saves", s) for s in seeds]
    # the canonical rig sentinel beside the ROOT checkout (set_runner's own default is beside ITS checkout: wrong when
    # the set runs from a frozen `git archive` export of the tools)
    quiet = os.path.join(os.path.dirname(root), "native", "archive", "v1_native", "QUIET_WINDOW")
    spec = {"version": 1, "cwd": root, "inputs": inputs, "steps": steps, "quiet_window": quiet,
            "game_slot": {"owner": a.owner or ("eramatrix-" + a.arm), "wait_seconds": a.slot_wait,
                          "min_free_gb": machine["min_free_gb"]},
            "memory_limit_mb": machine["memory_limit_mb"], "process_limit": 64}
    write_json(os.path.abspath(a.out_spec), spec)
    print("era_matrix spec: %s - preflight + %d acquisitions (%s x %d) + grade, exe %s, root %s" % (
        a.out_spec, len(steps) - 2, ",".join(rows), reps, sha256(exe), root))
    return 0


def cmd_rows(_a):
    for row, r in ROWS.items():
        print("%s %-8s %-8s %s" % (row, r["kind"], r["driver"], r["what"]))
        print("    %s" % os.path.join(ERA, r.get("play") or r.get("scenario")))
    print("zero columns: %s (nrec > 0)" % ", ".join(ZERO_COLS))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("rows")
    p = sub.add_parser("roll")
    p.add_argument("--row", choices=sorted(ROWS), required=True)
    p.add_argument("--arm", choices=sorted(ARMS), required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--attempt-of", type=int, default=1)
    p.add_argument("--dump", choices=sorted(DUMPS))
    p.add_argument("--stage", default="")
    p.add_argument("--stage-source", default="")
    p.add_argument("--root", help="the checkout the roll runs in (cwd: its disc, archive/saves, roll tools); default this one")
    p.add_argument("--reacquire", type=int, default=None, help="re-acquisitions when the row's precondition is unmet "
                   "(default: the row's; R4 2)")
    p.add_argument("--dwell", type=int, help="override the row's perf_run --dwell (seconds in the game after the HUD)")
    p.add_argument("--seconds", type=int, help="override the row's perf_run --seconds bound")
    p.add_argument("--extra-gates", default="", help="NAME=VALUE[,...] on top of the row's gates (a row gate is refused)")
    p = sub.add_parser("grade-roll")
    p.add_argument("--row", choices=sorted(ROWS), required=True)
    p.add_argument("--arm", choices=sorted(ARMS), required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--exe-sha256", required=True)
    p.add_argument("--roll", help="grade this driver folder instead of <out>/roll (read-only; the grades go to <out>)")
    p = sub.add_parser("grade")
    p.add_argument("--arm", choices=sorted(ARMS), required=True)
    p.add_argument("--exe-sha256", required=True)
    p.add_argument("--repeats", type=int, default=0, help="default: the arm's (meter 2, release 1)")
    p.add_argument("--rows", default=",".join(ROWS))
    p.add_argument("--pilot", action="store_true", help="a subset / fewer repeats: graded, reported as PILOT, never "
                   "the acceptance verdict")
    p.add_argument("--json", required=True, help="the matrix verdict; each roll is re-graded into <its folder>/regrade/")
    p.add_argument("--set", help="a set_runner folder: every <step>/attempt-*/r roll in it (all attempts, none skipped)")
    p.add_argument("rolls", nargs="*")
    p = sub.add_parser("preflight")
    p.add_argument("--arm", choices=sorted(ARMS), required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--rows", default=",".join(ROWS))
    p.add_argument("--root")
    p.add_argument("--stage", default="")
    p.add_argument("--stage-source", default="")
    p.add_argument("--json")
    p = sub.add_parser("spec")
    p.add_argument("--arm", choices=sorted(ARMS), required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--out-spec", required=True)
    p.add_argument("--repeats", type=int, default=0, help="default: the arm's (meter 2, release 1)")
    p.add_argument("--rows", default=",".join(ROWS))
    p.add_argument("--machine", choices=sorted(MACHINES))
    p.add_argument("--dump", choices=sorted(DUMPS))
    p.add_argument("--stage", default="")
    p.add_argument("--stage-source", default="")
    p.add_argument("--slot-wait", type=int, default=43200)
    p.add_argument("--dwell", type=int, help="every roll: override the row's perf_run --dwell")
    p.add_argument("--seconds", type=int, help="every roll: override the row's perf_run --seconds (the step timeout follows)")
    p.add_argument("--extra-gates", default="", help="every roll: NAME=VALUE[,...] on top of the row's gates")
    p.add_argument("--owner", default="", help="the set's game-slot owner (default eramatrix-<arm>)")
    p.add_argument("--root", help="the checkout the set runs in (default: this one)")
    for name in ("roll", "grade-roll", "grade", "spec"):  # tools/era-r4: the graceful quit's stranded policy
        sub.choices[name].add_argument(
            "--stranded-policy", choices=STRANDED_POLICIES, default="xdk-wait",
            help="xdk-wait (default; the fresh matrix after accJ): a stranded thread named by the quit in "
                 "GpuBlockOnFence on a dropped fence / GpuRingWaitForSpace with a dropped progress word, quit complete "
                 "exit 0; forbid: stranded > 0 FAILS (accJ's rule)")
    a = ap.parse_args(argv)
    if getattr(a, "rows", None) and any(r not in ROWS for r in a.rows.split(",")):
        ap.error("--rows: one of %s" % ",".join(ROWS))
    return {"rows": cmd_rows, "roll": cmd_roll, "grade-roll": cmd_grade_roll, "grade": cmd_grade,
            "preflight": cmd_preflight, "spec": cmd_spec}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
