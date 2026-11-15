#!/usr/bin/env python3
"""flicker_gate.py - the FLICKER GATE (iter-467). One-present rendering defects (an object vanishing,
a draw executed with another draw's state for one present) are invisible to sparse sheets and to
CHECKs at generation time. This gate measures them directly, two ways, on perf_run's gameplay scenario:

  span    every present of a 360-present window (uncapped; from p20000 by default), scored by flicker_diff.py
          (A-B-A outliers, whole-frame motion presents excluded). The COARSE screen: PASS at <= --span-max
          counted presents (default 5). Clean exes score 0-1 in calm scenes and up to 5 where a car passes close
          or a speed-blur pulses (trunk_span_off); the S1 regression scored 5-21 unguarded / 3 guarded. It cannot
          separate a loud defect from a busy scene on its own - the freeze decides.
  freeze  THE ORACLE (exact, deterministic, scene-independent). SR_DIAG_SIM_FREEZE holds the sim 2 s
          every 8 s; every present inside a freeze is dumped (up to 40) and consecutive presents must be
          pixel-identical (freeze_diff.py --exact
          --allow-maxdiff 1 = the measured noise floor: S1-off fz4 had maxdiff 1 on <= 23 px, the
          regression maxdiff up to 245 on thousands). Freezes that begin before the ingame era
          (movies run on the media clock) are excluded through perf_report's era markers, and so is an in-game
          freeze that overlaps a movie the game plays later (a mid-level FMV, its [MOVIE_PACE] EOM record's wall
          window: zpd4's freeze roll held the sim through the 65 s Mxyzptlk cutscene, freezes 5-11 differed in
          every pair while the 19 others were exact).
          A freeze whose WORST differing pair stays within --allow-pixels (64) changed pixels AND
          --allow-spike (8) per-channel levels is reported as "speck" and does not fail: player12 had
          one pixel at maxdiff 2 in one of 11 frozen ingame spans, while the S1 regression changed
          thousands of pixels at up to 245 levels and a vanished streetlight still exceeds the spike.
          THE SHADER TIME IS HELD TOO (iter-467 native-a, f2b83734; HIGH_FRAMERATE.md 6): the sea surface is drawn
          from shader semantic 0x29 (ShaderParam_WriteTimeSeconds 8260DDC8), wall-clock seconds, not the sim. Under
          SR_DIAG_SIM_FREEZE the exe holds 0x29 with the sim (still during a hold, the held time removed after it;
          [SIM_FREEZE] shader time 0x29 HELD at boot, [SHTIME_HOLD] per hold); SR_DIAG_SIM_FREEZE_SHTIME=0 lets it run
          (the A/B). An exe BEFORE f2b83734 moves the sea inside every hold - the progressive water class: every pair
          of a sea freeze differs (39/39), k0 -> k39 growing to thousands of pixels, maxdiff up to ~100 - so a gate
          with the sea in view FAILS there for the instrument, not the render (sgb1's save gate: save_gate_drift;
          archive/v1_native/sgd6b on_run, the same exe with SHTIME=0: bad [1, 2, 9, 10] on the water while the held
          ON and OFF arms PASS 15/15 and 16/16 on the same roof).
          THE EYE ADAPTATION IS HELD TOO (2026-10-13; HIGH_FRAMERATE.md 6): the exposure history steps once per
          rendered frame, so it keeps converging inside a hold - whole frame, one direction, decaying, maxdiff 2-4,
          below one level only at ~100 fps. Under SR_DIAG_SIM_FREEZE the exe sets the adaptation's c0.x = 0 during a
          hold ([SIM_FREEZE] eye adaptation HELD at boot); SR_DIAG_SIM_FREEZE_ADAPT=0 lets it run (the A/B). An exe
          before this FAILS slow rolls for the instrument (74ef828f at SR_HIGHFPS=40: archive/claude_baseline40).

  THE WORLD MUST BE DRAWN (iter-467 ledger; the rule lives in world_floor.py, shared with tools/native_ledger.py).
  A black 3D scene under a working HUD is perfectly stable, so both halves pass it - and so does the pause menu over
  a black world (its panel alone clears a luma floor). Eight archived rolls cited as passes graded black frames.
  - freeze: at least --min-world (8) GRADED freezes must show the world: their middle dumped present clears the
    scene floor (central luma >= 8, near-black <= 0.6) and either the player context there is `gameplay` or - on
    the pause menu - the side bands the panel leaves uncovered show the world. Otherwise WORLD NOT DRAWN = FAIL.
  - span: at least --span-in-play (0.8) of its presents must be IN PLAY (dumped at or after the run's own in-game
    era start - the freeze era's start - in the gameplay context), and the median central region over sampled span
    presents must clear the scene floor (healthy gates: luma 21-32, dark 0.04-0.20; fg55: 2.0 / 0.97). The span is
    PLACED IN THE ROLL (iter-467 hooks): the span roll plays tools/perf/flicker_play.txt, whose `mark dump` (once
    Superman is on foot after the opening movie) opens the window (SR_PRESENT_DUMP_FROM_MARK=1; the log says
    "[PRESENT_DUMP] window opened by the harness dump mark at present N"). A present number is not a place in the
    game: over the 64 archived gates the HUD came at p9,767-p21,251, so postmerge_sj's span, re-placed from its
    first roll's count (p21,391 = 40 s into play there), opened on the next roll's opening movie. A mark-placed
    span that misses play is a finding: it is not re-rolled.
    An exe without the mark (before iter-467 hooks) dumps from SR_PRESENT_DUMP_FROM as before; when that span misses
    play the gate RE-PLACES it once (replace_from: --replace-settle s after that roll's own in-game start, moved to
    the next stretch the roll's log shows in the gameplay context) and rolls it again - still a guess across boots,
    said so in the attempt record; the first attempt is kept as span_try1/. A STRICT gate is never re-placed (C2
    2026-10-23): its span MEASURED out of play is NO DATA (a mark-placed one, or one whose placement could not be
    measured, stays a FAIL), and the runner may declare ONE fresh strict gate when close/strict_fg.py exits 2.
  The in-game start is the END OF MEDIA of the opening movie (perf_report.INGAME_MOVIE): a stall that splits the FMV
  into a `GAP` record and an `EOM` record no longer opens it on the movie (postmerge_hooks gate2's span PASSED on 360
  presents of the opening FMV, luma 108). A SKIPPED opening movie (the roll presses the game's skip input) logs no
  EOM: its end is the player's first gameplay MODE after the HUD request (`[PC_CONTROLS] context gameplay ... mode
  on-foot camera`; or an `[EV187] ... ACCEPTED-KILL` edge) (perf_report.MOVIE_SKIP); either way the start then moves
  to the first gameplay context after it.
  BOTH ROLLS PLAY THE GATE TAPE, which never pauses (the player tape's pause round trip lost its resume tap inside a
  2 s freeze and left the menu up: "menu over world 10"); a tape that dies (harness_play exits non-zero: an await
  timed out, a hold_ctx never confirmed) FAILS the gate (perf_run writes <roll>/play_failed.txt).
  The floor is not an oracle: the frozen-pass A/B against the guest path is still what proves a draw-path change.

    python tools/perf/flicker_gate.py --exe <exe> --out <dir> [--gates "SR_X=1"] [--only span|freeze]
    python tools/perf/flicker_gate.py --exe <exe> --out <dir> --from-save <content root> [--save-label <step>]
                                                                # SAVE-STARTED (iter-467, owner 2026-10-10): both
                                                                # rolls LOAD the newest slot of a saved game (a lit,
                                                                # repeatable scene, no intro) instead of NEW GAME
    python tools/perf/flicker_gate.py --grade <dir> [--reuse]   # re-grade the rolls already in <dir>
                                                                # (--reuse: from the stored diff outputs)
                                                                # A STRICT gate (<dir>/strict_rule.json, written
                                                                # by its runner) ignores --reuse: both halves are
                                                                # recomputed and the span records fresh_run
    ... --stage J:\\sr_stage [--stage-source <disc folder>]      # 2026-10-14: both rolls read the game from an
                                                                # SSD (perf_run --stage; tools/perf/stage_roll.py);
                                                                # logs / frames / perf / saves stay where they were

SAVE-STARTED GATES (--from-save): perf_run.py --from-save picks the newest saved-game file under the content root
and its slot with the most play time, copies the profile folder (not settings/: a player's display / costume
settings would change the configuration) into the roll's own seed, and drives MAIN MENU -> LOAD GAME -> that
slot. The tape is flicker_play.txt behind a prelude that answers the hub LoadComplete shows (the HUD, or the
METROPOLIS / FORTRESS menu: Return until the HUD). The in-game start of a save-started roll is the first
gameplay context after the HUD request (there is no opening movie). flicker_gate.json's `start` records it:
"save-started (dev-tool save, <step>)" with the save's dev-tool marker (run_class), or "new game" - the default.

STRICT GATES AND STORED OUTPUTS (C2 2026-10-22, after Astra e1cca492 r4): a stored <half>_flicker.txt / _freeze.txt
is graded only by --grade --reuse and by readonly callers (the ledger), for diagnostics and regrading; such a result
records reused / readonly = true, and strict_fg FAILS it. A strict gate - its runner writes strict_rule.json before the
gate runs - always recomputes both halves with this code in this run, and the span carries fresh_run (the id passed to
this run's flicker_diff, read back from its output, with the graded code's sha256). Every parse boundary (the stored
output's records, the prior flicker_gate.json, save_start.json, the clock record) catches any failure, records
<name>_error and fails closed; a half whose grading raises is a FAIL with <half>_error, and the JSON is always written.
ROUND 6 (Astra e1cca492 r5, the same classes on the inherited paths): a fresh half is graded only when its detector
COMPLETED - flicker_diff's exit agrees with its FLICKER record, freeze_diff's table ends with its FREEZE_EXACT record,
its rows add up to that record's counts and its exit agrees (a run that died part way never grades as the rows it
printed); a stored table that carries the record must be complete too. Required table fields are parsed strictly
(table_int: a malformed count is a FAIL, never 0). grade_span / grade_freeze guard themselves (direct callers too), and
main's orchestration measurements are guarded (a roll that cannot be measured is not re-placed and FAILS). Evidence
writes never stop a verdict: flicker_diff's worst image, freeze_diff's sheets, the pre-world copy, the floor cache.

Writes <dir>/flicker_gate.json and prints FLICKER_GATE PASS/FAIL (exit 1 on FAIL). Hold the game
slot first (this tool never acquires or releases it) and run it from the checkout with the disc
beside it. ~4 min per roll.
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import flicker_rule  # noqa: E402  (the span detector's one rule-parameter table)
import log_records  # noqa: E402  (the inventory tags' record forms, shared with the standalone detectors - r21)
import perf_report  # noqa: E402
import world_floor as wf  # noqa: E402
from gpu_hang_check import review_gpu_hangs  # noqa: E402

BASE = "SR_INSTANCE_ID={inst},SR_HIGHFPS=uncapped,SR_HIGHFPS_PACE=1"
SPAN = ("SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_EVERY=1,SR_PRESENT_DUMP_FROM={frm},SR_PRESENT_DUMP_CAP={cap},"
        "SR_PRESENT_DUMP_FROM_MARK=1")   # an exe without the mark ignores it and dumps from SR_PRESENT_DUMP_FROM
FREEZE = "SR_DIAG_SIM_FREEZE=8:2000:80,SR_PRESENT_DUMP_FREEZE=2,SR_PRESENT_DUMP_FREEZE_CAP=40"
FZ_BEGIN = re.compile(r"\[SIM_FREEZE\] #(\d+) begin at ([\d.]+) s")
FZ_END = re.compile(r"\[SIM_FREEZE\] #(\d+) end: ")
FZ_HOLD_US = 3_000_000  # a freeze with no end line: its window (SR_DIAG_SIM_FREEZE holds 2 s, 40 dumps at most)
# A movie's [MOVIE_PACE] EOM record is logged at its end of media; its wall time reaches back to its first frame-get
# (kernel/cs_timeout_underflow_fix.cpp moviePaceSummary). A GAP record is not a window: it is logged when the NEXT
# movie starts, its movie having ended an unknown time (>= 2 s) earlier.
MOVIE_EOM = re.compile(r"\[MOVIE_PACE\] movie#(\d+) EOM wall=([\d.]+)s frames=(\d+)")
# A frozen present's dump line (its log time places a freeze_diff pair row against a movie window).
FZ_DUMP = re.compile(r"\[SIM_FREEZE_DUMP\] freeze=(\d+) k=\d+ present=(\d+)")
# freeze_diff.py --exact's per-pair row of a differing pair: pair | freeze | present a | present b | pixels | maxdiff
PAIR_ROW = re.compile(r"^pair \| (\d+) \| (\d+) \| (\d+) \| (-?\d+) \| (\d+)\s*$")
# ... and its row of EVERY compared pair: compared | freeze | present a | present b | static|unsettled|plain (lane C r11)
COMPARED_ROW = re.compile(r"^compared \| (\d{1,12}) \| (\d{1,12}) \| (\d{1,12}) \| (static|unsettled|plain)\s*$")
# ... and its row of a freeze whose shown stamp moved: fid | frames | pairs | (shown stamp moved: 1.000,2.000 - not a frozen
# window) - whole (lane C r17)
MOVED_ROW = re.compile(r"^\d{1,12} \| \d{1,12} \| \d{1,12} \| \(shown stamp moved: \d+(?:\.\d+)?(?:,\d+(?:\.\d+)?)+ - not a "
                       r"frozen window\)\s*$")
SPAN_LINE = re.compile(r"^FLICKER (PASS|FAIL): (\d+) of (\d+) presents", re.M)   # flicker_diff's verdict record
UNREADABLE_LINE = re.compile(r"^FLICKER UNREADABLE:.*$", re.M)                    # its fail-closed record (Fable 28 (1))
# round 6: the COMPLETE records (a line cut anywhere matches neither, and the span FAILS on it)
SPAN_RECORD = re.compile(r"^FLICKER (PASS|FAIL): (\d+) of (\d+) presents carry an A-B-A outlier \(fail at \d+\)\s*$")
UNREADABLE_RECORD = re.compile(r"^FLICKER UNREADABLE: \S.*$")
# flicker_diff's inventory summary (lane C r8): the window it scored, its file count and its numbering gaps
INVENTORY_LINE = re.compile(r"^flicker_diff .*: presents p(\d{1,12})\.\.p(\d{1,12}) \((\d{1,12}) files, (\d{1,12}) gaps "
                            r"in numbering\), ", re.M)
# Fable 28: the span detector's identity (flicker_diff.DETECTOR - test_flicker_diff checks the two agree). A stored
# <d>_flicker.txt without it was written by another detector (pre-Fable-28 outputs carry no identity line at all).
DETECTOR = "motion-check-2 (Fable 26(3) + 28; complete-support predictions)"
DETECTOR_LINE = re.compile(r"^FLICKER DETECTOR: (.+?)\s*$", re.M)
PARAMS_LINE = re.compile(r"^FLICKER PARAMS: (\{.*\})\s*$", re.M)   # the effective rule parameters (flicker_rule)
NO_DETECTOR = "none (pre-Fable-28, or no output)"
RUN_LINE = re.compile(r"^FLICKER RUN: ([0-9a-f]{32})\s*$", re.M)   # flicker_diff's echo of this gate's fresh-run id
# C2 2026-10-22 (after Astra e1cca492 r4 P1): a STRICT gate - one whose runner wrote <out>/strict_rule.json before the
# gate ran - never grades a stored output. Both halves are recomputed from the dumps by THIS code in THIS run, and the
# span records the fresh-run stamp (fresh_run.run_id: the id this gate passed to its own flicker_diff invocation and
# read back from that invocation's output). --reuse / readonly stay for diagnostics and regrading only: their results
# record reused / readonly = true, which strict_fg FAILS. That closes every stale-output path at once (an output an
# older detector wrote under today's id, a hand-edited record, an output of another run).
STRICT_RULE = "strict_rule.json"
# C2 2026-10-22, the parse-boundary rule: every parse boundary (the stored output's diag31 / PARAMS / verdict records,
# the prior flicker_gate.json, save_start.json, the clock record) catches Exception, records <name>_error and fails
# closed. An evidence value is kept only when it is SHALLOW (nesting depth and node count bounded), so the gate's own
# JSON can always be written.
EVIDENCE_DEPTH = 16
EVIDENCE_NODES = 20000
EVIDENCE_CHARS = 200000   # ... and at most this many characters of keys and strings (Astra r5: depth alone is no size bound)
DIAG_MAX = 1000          # diag31 records kept per span (the rest counted in diag31_truncated)
TEXT_MAX = 300   # a recorded line / id taken from an output is cut here
# freeze_diff --exact's completion record: a fresh table is graded only when it ends with it and its rows add up to it
# (Astra e1cca492 r5 P1: a run that died after eight clean freezes graded as a PASS on those eight). Lane C r8 (Astra
# e7b0cca r7 P1-4): the record is recognised by its COMPLETE grammar - freeze_diff's plain record, its A/B
# (--ab-control) record, or its UNREADABLE record - to the end of the line; a cut, partial or bare record (`FREEZE_EXACT
# PASS`) is no record, and there is no older-format fallback (a stored table carrying any FREEZE_EXACT line must be
# complete under this grammar; one with none is graded as stored, which is never a strict verdict: reused / readonly).
_IDS = r"(?:-|\d{1,12}(?:,\d{1,12})*)"   # freeze_diff's freeze list: '-' or 3,7,9
FREEZE_PLAIN = re.compile(
    r"^FREEZE_EXACT (?P<verdict>PASS|FAIL|NO DATA): (?P<nbad>\d{1,12}) freezes with a differing present \((?P<ids>"
    + _IDS + r")\) \| "
    r"(?P<frames>\d{1,12}) frozen presents, (?P<pairs>\d{1,12}) consecutive pairs, allow-maxdiff \d{1,12}\s*$")
FREEZE_AB = re.compile(
    r"^FREEZE_EXACT (?P<verdict>PASS|FAIL|NO DATA): (?P<nbad>\d{1,12}) freezes with a differing STATIC pair \((?P<ids>"
    + _IDS + r")\) \| "
    r"(?P<frames>\d{1,12}) frozen presents, (?P<pairs>\d{1,12}) consecutive pairs, allow-maxdiff \d{1,12} \| "
    r"A/B control: \d{1,12} static pairs, \d{1,12} unsettled \(excluded\), \d{1,12} freezes NO DATA \(below \d{1,12} "
    r"static pairs or moved: a control above maxdiff \d{1,12}\) \(" + _IDS + r"\), worst control "
    r"(?:-|freeze \d{1,12} p\d{1,12}->p\d{1,12} \d{1,12} px at maxdiff \d{1,12})\s*$")
FREEZE_UNREADABLE = re.compile(r"^FREEZE_EXACT UNREADABLE: \S.*$")


def freeze_record(ln):
    """The match of ONE complete FREEZE_EXACT record (FREEZE_PLAIN / FREEZE_AB / FREEZE_UNREADABLE), or None."""
    for rx in (FREEZE_PLAIN, FREEZE_AB, FREEZE_UNREADABLE):
        m = rx.match(ln)
        if m:
            return m
    return None
DIAG_LINE = re.compile(r"^diag31\s+p(\d+)\s+r(\d+)c(\d+)\s+(\{.*\})\s*$")   # Fable 31's readonly diagnostic
MARK_LINE = re.compile(r"\[PRESENT_DUMP\] window opened by the harness dump mark at present (\d+)")
PLAY_TAPE = os.path.join(HERE, "flicker_play.txt")   # the gate's tape (both rolls): no pause, the span's dump mark
frame_scene = wf.frame_scene   # kept importable from here (older callers)


# --from-save: perf_run's DRIVE loads the save and enters METROPOLIS (A on LoadComplete's carousel until a fresh HUD
# request); the tape presses nothing until then. Anchored on the MAIN MENU's LOAD GAME answer (button 1), so nothing
# before the load can match; then the HUD, fatal when absent (a capture window must never open on the carousel: the
# void gate final_save_void2, whose Return taps never entered METROPOLIS).
SAVE_PRELUDE = [
    "# flicker_gate --from-save prelude (tools/perf/flicker_gate.py SAVE_PRELUDE): perf_run's drive loads the save and",
    "# enters METROPOLIS; the gate actions start only after the HUD request.",
    r"await \[SR_FSCMD\].*answer cppId=\S+ button=1 1500",
    r"await \[APT_(REQ|SCREEN)\].*path='screens/hud/main_hud' 900",   # no '#' in a tape command: harness_play strips it
]
HUD_AWAIT = re.compile(r"^await .*main_hud")
PLAY_AWAIT = re.compile(r"^await \\\[PC_CONTROLS\\\] context gameplay")
# A save can put Superman in the air: PC_CONTROLS then logs `context gameplay ... mode -`, never `mode on-foot camera`
# (gate/final_save: the tape waited for it until the roll ended - no dump mark, no span). The save tape waits for any
# gameplay context, then SAVE_SETTLE_MS more (the HUD request precedes the drawn world by ~8 s after METROPOLIS: the
# first in-game freeze of that gate, 8 s after the HUD, showed the world).
SAVE_PLAY_AWAIT = r"await \[PC_CONTROLS\] context gameplay 240"
SAVE_SETTLE_MS = 7000


def save_tape(out_dir, base=None):
    """The gate tape of a save-started gate: SAVE_PRELUDE, then flicker_play.txt without its first `await ...main_hud`
    (the prelude has seen the HUD; a second await would wait for a request that never repeats) and with its on-foot
    gameplay await relaxed to any gameplay context + SAVE_SETTLE_MS (a save can spawn Superman in the air). -> its path."""
    base = base or PLAY_TAPE
    lines = open(base, encoding="utf-8").read().splitlines()
    i = next((k for k, ln in enumerate(lines) if HUD_AWAIT.match(ln.strip())), None)
    if i is None:
        raise ValueError("%s has no `await ...main_hud` line to replace" % base)
    rest = lines[:i] + lines[i + 1:]
    k = next((n for n, ln in enumerate(rest) if PLAY_AWAIT.match(ln.strip())), None)
    if k is None:
        raise ValueError("%s has no `await [PC_CONTROLS] context gameplay` line to relax" % base)
    rest[k:k + 1] = [SAVE_PLAY_AWAIT, "wait %d" % SAVE_SETTLE_MS]
    path = os.path.join(out_dir, "flicker_play_from_save.txt")
    os.makedirs(out_dir, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(SAVE_PRELUDE + ["# --- %s (its HUD await dropped; any gameplay mode, +%d ms settle) ---"
                                          % (os.path.basename(base), SAVE_SETTLE_MS)] + rest) + "\n")
    return path


def start_record(out, label=None):
    """flicker_gate.json's `start`: how the gate's rolls began (perf_run's save_start.json in span/ or freeze/)."""
    for k in ("span", "freeze"):
        p = os.path.join(out, k, "save_start.json")
        if os.path.exists(p):
            # a parse boundary (C2 2026-10-22): an unreadable record is named as unknown, never an exception
            sv, err = load_json_record(p, "save_start.json")
            try:
                if err:
                    raise ValueError(err)
                mk = sv.get("devtool_marker") or {}
                kind = "dev-tool save" if mk.get("run_class") == "dev-tool-assisted" else (
                    "%s save" % mk["run_class"] if mk.get("run_class") else "played save")
                step = label or "chapter %d, slot %d" % (sv.get("chapter", -1), sv.get("slot", -1))
                return {"kind": "save", "label": "save-started (%s, %s)" % (kind, step),
                        "run_class": mk.get("run_class"), "devtool_marker": mk, "save_file": sv.get("save_file"),
                        "slot": sv.get("slot"), "chapter": sv.get("chapter"), "TotalPlayTime": sv.get("TotalPlayTime"),
                        "settings_copied": sv.get("settings_copied")}
            except Exception as e:
                return {"kind": "unknown", "label": "unknown (%s/save_start.json unreadable)" % k,
                        "start_error": err or err_text(e)}
    return {"kind": "new game", "label": "new game"}


def save_started(d):
    """Was this roll started from a save (perf_run --from-save wrote save_start.json)?"""
    return os.path.exists(os.path.join(d, "save_start.json"))


def stage_cli(stage=None, stage_source=None):
    """perf_run's --stage / --stage-source (SSD staging, tools/perf/stage_roll.py), or nothing."""
    if not stage:
        return []
    return ["--stage", stage] + (["--stage-source", stage_source] if stage_source else [])


def roll(exe, out, gates, repo, hz=0, from_save=None, tape=None, save_settings=False, stage=None, stage_source=None):
    cmd = [sys.executable, os.path.join(repo, "tools", "perf", "perf_run.py"), "--scenario", "gameplay",
           "--config", "port", "--exe", exe, "--out", out, "--hz", str(hz), "--gates", gates, "--play", tape or PLAY_TAPE]
    if from_save:
        cmd += ["--from-save", from_save] + (["--save-settings"] if save_settings else [])
    cmd += stage_cli(stage, stage_source)
    # the roll's console is evidence (no grade reads it): one that cannot be opened - an earlier try's copy still locked
    # (lane C r10, Astra 2453fdf r9 P2: the re-roll never launched) - goes to a fresh name, or is discarded; the roll runs
    f = None
    for console in (out + ".txt", out + "_console_%d.txt" % os.getpid()):
        try:
            f = open(console, "wb")
            break
        except Exception as e:
            print("roll   : the console %s cannot be written (%s)" % (os.path.basename(console), err_text(e)))
    if f is None:
        return run_child(cmd, repo, None)
    try:
        return run_child(cmd, repo, f)
    finally:
        try:
            f.close()
        except Exception as e:   # evidence: a close that fails is noted, never the roll's outcome
            print("roll   : the console could not be closed (%s)" % err_text(e))


def run_child(cmd, cwd, sink):
    """Run the roll (perf_run) to its end and return ITS exit code. Its console is evidence: drained through a pipe into
    sink (an open binary file, or None - discarded), every write guarded - lane C r25 (Astra 947fad8 conv24 P2: the
    child wrote straight into the file, so a console that failed after it opened - a full disk, a dropped share - could
    end the child and its exit failed the gate): a sink that stops taking writes is noted once and the rest discarded;
    the child never writes to it."""
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    failed = sink is None
    with p.stdout:
        for chunk in iter(lambda: p.stdout.read1(65536), b""):
            if failed:
                continue
            try:
                sink.write(chunk)
                sink.flush()
            except Exception as e:
                failed = True
                print("roll   : the console stopped taking writes (%s) - the rest of it is discarded" % err_text(e))
    return p.wait()


def run_log(d):
    for p in (os.path.join(d, "drive", "attempt1", "run.log"),):
        if os.path.exists(p):
            return p
    return None


KERNEL_COMPILE = re.compile(r"\[NB_AOT\] compiled (\S+) - no shipped blob (\S+)")
LOG_TS = re.compile(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")


def _log_ts(text):
    m = LOG_TS.search(text or "")
    try:
        return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None
    except ValueError:
        return None


def review_kernel_compiles(log):
    """The exe's shipped resolve kernels (data/shader_cache/nb, SR_NB_AOT default ON): a `[NB_AOT] compiled <kernel> -
    no shipped blob <file>` line means the staged shader cache does not hold THIS exe's key for that kernel and the
    exe compiled it at boot - in the release configuration inline, on the CP's first command (s3m_gate2 2026-10-16
    on the Ally: two compiles, then [GPU_HANG] H3 at 15.79 s with the CP 5.1 s into its first IB, then two more; the
    Ally's checkout 054a577a predated the exe's blobs, so its data/shader_cache/nb held the old keys). A roll on such
    a package is not this exe's timing. -> {"n", "kernels": [blob file names], "first", "last" (log timestamps)}."""
    names, first, last = [], None, None
    try:
        with open(log, encoding="utf-8", errors="replace") as source:
            for line in source:
                m = KERNEL_COMPILE.search(line)
                if not m:
                    continue
                names.append(os.path.basename(m.group(2).replace("\\", "/")))
                t = LOG_TS.search(line)
                if t:
                    first = first or t.group(1)
                    last = t.group(1)
    except OSError:
        pass
    return {"n": len(names), "kernels": names, "first": first, "last": last}


def grade_roll_health(log, result):
    """Stable pixels cannot excuse a GPU stall; a dump-stall VOID needs a new roll. A resolve kernel compiled at boot
    (review_kernel_compiles) fails the roll as a PACKAGE finding: the staged package is not this exe's and the roll's
    timing is not the exe's; a GPU_HANG whose trigger falls inside the compile window (+- 2 s) is named as such."""
    health = review_gpu_hangs(log)
    kc = review_kernel_compiles(log)
    if kc["n"]:
        why = ("PACKAGE: %d resolve kernel(s) compiled at boot - no shipped blob (%s): the staged data/shader_cache/nb"
               " does not hold this exe's keys" % (kc["n"], ", ".join(kc["kernels"][:3]) + (" ..." if kc["n"] > 3 else "")))
        t0, t1 = _log_ts("[%s]" % kc["first"]), _log_ts("[%s]" % kc["last"])
        if t0 and t1:
            pad = datetime.timedelta(seconds=2)
            for ev in health.get("evidence", []):
                t = _log_ts(ev)
                if "GPU_HANG" in ev and t and t0 - pad <= t <= t1 + pad:
                    why += " - the GPU_HANG at %s falls inside the compile window (%s .. %s)" % (
                        t.strftime("%H:%M:%S.%f")[:-3], kc["first"][11:], kc["last"][11:])
                    break
        health = dict(health, status="FAIL", evidence=[why] + list(health.get("evidence", [])))
    health["kernels_compiled"] = kc
    result["roll_health"] = health
    if health["status"] != "PASS":
        result["pixel_verdict"] = result["verdict"]
        result["verdict"] = "FAIL"
        result["line"] = result.get("line", "") + " | ROLL " + health["status"] + ": " + health["evidence"][0]
    return result


def scene_stats(d, samples=40):
    """iter-467: is the WORLD drawn at all? A black 3D scene under a working HUD is perfectly stable, so
    the A-B-A screen and the freeze both pass it (the row-9 record bake fg55: the owner saw it first).
    Per sampled span present: the central region (HUD margins cut) - mean luma and the fraction of
    near-black pixels (< 12). Calibration: four healthy gates median luma 23-27, dark fraction
    0.11-0.19; fg55 (black world) luma 2.0, dark 0.97.
    The sample is drawn from the span's EXPECTED inventory, ONE ENTRY PER PRESENT (world_floor.present_frames: the
    applicable sidecar's fixed inventory, else the BMPs on disk and every frame its run.log / outcome.json say were
    dumped), so it is the same 40 presents whatever is readable now. The floor is UNMEASURED - {"n", "unmeasured",
    "why"}, a FAIL in grade_span, never a median of the rest - when a sampled present has no valid measurement (its
    BMP and sidecar entry gone; an archived roll whose sidecar is rejected or missing, whatever BMPs are on disk; an
    ambiguous present; a BMP that cannot be decoded because Pillow is missing: Astra 2026-10-14), and when the span
    has no frame inventory at all (a scored span had frames). Pillow is needed only to decode a BMP; stored
    measurements are read without it."""
    fdir = os.path.join(d, "drive", "attempt1", "frames")
    # one entry per present (world_floor.present_frames, Astra round 5), in frame-name order: an ambiguous present
    # (two frames claim it, or its frame was logged twice) is in the sample as unmeasured
    frames = [(p, n) for p, n in wf.present_frames(fdir) if n is None or n.startswith("present_")]
    if not frames:
        return {"n": 0, "unmeasured": 0, "why": wf.inventory_problem(fdir) or
                "no frame of this span is on disk, in %s, or named by its run.log / outcome.json" % wf.SIDECAR}
    lumas, darks, missing, nopil, on_disk, bad = [], [], 0, 0, 0, 0
    for p, f in frames[::max(1, len(frames) // samples)]:
        if not f:   # ambiguous: no frame is provably this present's
            missing += 1
            continue
        on_disk += os.path.exists(os.path.join(fdir, f))
        try:
            m = wf.measure(os.path.join(fdir, f), "center")
        except ImportError:   # a BMP to decode and no Pillow: unmeasured, never skipped
            m, nopil = None, nopil + 1
        except Exception:     # a damaged frame (truncated / undecodable - Astra f5e06465 r1 P1): unmeasured, never a crash
            m, bad = None, bad + 1
        if not wf.valid_measure(m):
            missing += 1
            continue
        lu, dk = m
        lumas.append(lu)
        darks.append(dk)
    if missing:
        why = "%d of %d sampled presents have no measurement" % (missing, len(lumas) + missing)
        if nopil:
            why += " (%d BMP(s) cannot be decoded: Pillow is not installed)" % nopil
        if bad:
            why += " (%d frame(s) unreadable: damaged or truncated)" % bad
        ctx = wf.archived_problem(fdir) or (wf.inventory_problem(fdir) if not on_disk else None)
        if ctx:
            why += " (%s)" % ctx
        return {"n": len(lumas) + missing, "unmeasured": missing, "why": why}
    lumas.sort()
    darks.sort()
    return {"n": len(lumas), "luma_median": round(lumas[len(lumas) // 2], 1),
            "dark_median": round(darks[len(darks) // 2], 3)}


PLAY = re.compile(r"\[PC_CONTROLS\] context gameplay ")


def ingame_us(d, log):
    """The freeze era's start (us from the perf t0), or None. A save-started roll (perf_run --from-save): the first
    `[PC_CONTROLS] context gameplay` line after the HUD request. A new-game roll: perf_report's ingame marker (the end of
    the first movie after the HUD request), moved to the first `[PC_CONTROLS] context gameplay` line
    after it when one follows within 30 s. The post-cutscene LOADING screen sits between the two:
    its spinner animates on the render clock while the sim is held, so a freeze that lands on it
    differs in every pair (iter-467 native-a roll p1b_gate, freeze 4: 1,902 px at maxdiff 45 in the
    strip 531,641-749,661 = the loading dots; crop archive/v1_native/p1b_gate_freeze4_crop.png)."""
    meta = perf_report.read_meta(os.path.join(d, "perf"))
    t0 = float(meta.get("epoch_ms_t0", "0") or 0)
    if not t0:
        return None, t0
    marks = perf_report.log_markers(log, perf_report.DEFAULT_ERAS, t0, None)
    saved = save_started(d)
    ig = None
    for era, t in marks:
        if era == ("gameplay" if saved else "ingame"):
            ig = t
    if ig is None:
        return None, t0
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace")
            if not PLAY.search(line):
                continue
            ts = perf_report.LOG_TS.match(line)
            if not ts:   # lane C r12 (Astra 190f7a5 r11 P1: a skipped one moved the era LATER and excused an earlier
                # defective freeze): a gameplay-context record whose log time cannot be read places nothing - it raises
                raise ValueError("a [PC_CONTROLS] context gameplay record without a readable log time: %r"
                                 % line.strip()[:160])
            dt = datetime.datetime.strptime(ts.group(1), "%Y-%m-%d %H:%M:%S.%f")
            t_us = int((dt.timestamp() * 1000 - t0) * 1000)
            if t_us >= ig:
                # a save-started roll (iter-467): no opening movie; play begins at the first gameplay context after
                # the HUD request, however long the load took
                return (t_us if (saved or t_us - ig <= 30_000_000) else ig), t0
    return ig, t0


def era_start_ms(d, log):
    """The in-game era start as epoch ms (ingame_us + the perf t0), or None."""
    ig, t0 = ingame_us(d, log)
    return (t0 + ig / 1000.0) if (ig is not None and t0) else None


def span_in_play(d):
    """(in play, dumped) presents of a span roll: dumped at or after its own in-game era start, in the
    gameplay context. fg67's span: 25 of 360 (the title and main menu). The denominator is the span's frame
    inventory, one row per present (world_floor.inventory_rows): a frame no surviving log line names, or an
    ambiguous one, has unknown timing and counts as not in play - a truncated log cannot shrink it, a repeated line
    cannot weight it."""
    log = run_log(d)
    if not log:
        return 0, 0
    rows = wf.inventory_rows(os.path.join(d, "drive", "attempt1", "frames"), log, era_start_ms(d, log), classify=False)
    return sum(1 for r in rows if r[2]), len(rows)


def span_placement(d):
    """span_in_play's counts and how they were measured (C2 2026-10-23, after conv27 P1-3 / conv28 P1-1): (in play,
    dumped, presents whose timing is unknown, presents at or after the in-game start whose context is unknown - no
    `[PC_CONTROLS] context` record before them, so whether they are in play is unknown -, the in-game era start in
    epoch ms or None, why the era is unknown or None)."""
    log = run_log(d)
    if not log:
        return 0, 0, 0, 0, None, "the span has no run log"
    ig, t0 = ingame_us(d, log)
    era = (t0 + ig / 1000.0) if (ig is not None and t0) else None
    why = None
    if not t0:
        why = "the roll's timing metadata is missing (perf/meta.txt epoch_ms_t0)"
    elif ig is None:
        why = "the roll's in-game start is not in its log"
    rows = wf.inventory_rows(os.path.join(d, "drive", "attempt1", "frames"), log, era, classify=False)
    ctx_unknown = 0
    if era is not None:
        # world_floor.inventory_rows reads the same timeline: a present with no context record before it is not
        # gameplay there - here it is UNMEASURED, not a measured miss
        timeline = wf.contexts(wf.read_text(log)) if wf.marker_exists(log) else []
        for r in rows:
            if r[1] is not None and wf.ts_ms(r[1]) >= era and wf.context_at(timeline, r[1]) is None:
                ctx_unknown += 1
    return (sum(1 for r in rows if r[2]), len(rows), sum(1 for r in rows if r[1] is None), ctx_unknown, era, why)


def detector_of(out):
    """The FLICKER DETECTOR id an output names (NO_DETECTOR when it names none; cut at TEXT_MAX characters)."""
    m = DETECTOR_LINE.search(out or "")
    return m.group(1)[:TEXT_MAX] if m else NO_DETECTOR


def err_text(e):
    """An exception as ONE bounded line (the <name>_error records; lane C r8, convergence P3: whitespace runs, newlines
    included, collapse to one space, so a record or a printed line never splits)."""
    try:
        return "%s: %s" % (type(e).__name__, " ".join(str(e).split())[:200])
    except Exception:
        return type(e).__name__


def shallow(v, depth=EVIDENCE_DEPTH, nodes=EVIDENCE_NODES, chars=EVIDENCE_CHARS):
    """Is a parsed JSON value small enough to record (nesting <= depth, <= nodes values, <= chars characters of keys and
    strings)? Iterative: no recursion."""
    stack, n, c = [(v, 0)], 0, 0
    while stack:
        x, k = stack.pop()
        n += 1
        if k > depth or n > nodes:
            return False
        if isinstance(x, dict):
            c += sum(len(key) for key in x if isinstance(key, str))
            stack.extend((y, k + 1) for y in x.values())
        elif isinstance(x, list):
            stack.extend((y, k + 1) for y in x)
        elif isinstance(x, str):
            c += len(x)
        if c > chars:
            return False
    return True


DOC_DEPTH, DOC_NODES, DOC_CHARS = 32, 2000000, 64000000   # a whole JSON document this gate reads back (its own JSON)


def load_json_record(path, what):
    """A JSON object read at a parse boundary -> (dict or None, error or None). Any failure - unreadable, not UTF-8,
    truncated, too deep, not an object, too large to record - is an error, never an exception."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
        v = json.loads(raw.decode("utf-8"))
        if not isinstance(v, dict):
            raise TypeError("%s is a %s, not an object" % (what, type(v).__name__))
        if not shallow(v, DOC_DEPTH, DOC_NODES, DOC_CHARS):
            raise ValueError("%s is too deep or too large to record (depth %d, %d values, %d characters at most)"
                             % (what, DOC_DEPTH, DOC_NODES, DOC_CHARS))
        return v, None
    except Exception as e:
        return None, err_text(e)


# the grading options a strict gate never takes (main: any of them given to a strict gate, even at its default value,
# is a strict_params_error FAIL; grade() records every limit in world_rule, which strict_fg requires to equal its
# canonical table)
STRICT_LIMITS = ("only", "span_max", "allow_maxdiff", "allow_pixels", "allow_spike", "min_world", "span_in_play")


def publish_receipt(out, text):
    """flicker_gate.json, written atomically (a temp file, then a rename) - THE receipt writer of every main() path. On
    ANY failure no earlier receipt may stand for this run (Astra 6fb621c r6): it is removed. -> None, or the error text
    (the removal's own failure named in it). A strict folder never holds an earlier receipt here: it is single-use
    (main refuses a --grade of it, or a run into it once it holds a receipt - C2 2026-10-23, r33)."""
    jp = os.path.join(out, "flicker_gate.json")
    try:
        with open(jp + ".tmp", "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(jp + ".tmp", jp)
        return None
    except Exception as e:
        err = err_text(e)
        try:
            if os.path.lexists(jp):
                os.remove(jp)
        except Exception as e2:
            err += "; the earlier flicker_gate.json could NOT be removed (%s) - it is not this run's" % err_text(e2)
        return err


def strict_gate(out):
    """A strict gate: its runner wrote <out>/strict_rule.json (any entry by that name - a damaged marker still means
    strict; strict_fg reads the marker itself)."""
    return os.path.lexists(os.path.join(out, STRICT_RULE))


def code_stamp():
    """sha256 of the code that graded a fresh span (evidence beside fresh_run.run_id)."""
    rec = {}
    for name in ("flicker_diff.py", "flicker_rule.py", "flicker_gate.py"):
        try:
            with open(os.path.join(HERE, name), "rb") as f:
                rec[name] = hashlib.sha256(f.read()).hexdigest()
        except Exception as e:
            rec[name] = "unreadable (%s)" % err_text(e)
    return rec


def flicker_diff_cmd(d, run_id=None):
    """The fresh span run: flicker_diff.py with EXACTLY the rule's one parameter table (flicker_rule.canonical_args()) -
    the gate never relies on flicker_diff's defaults, and test_flicker_gate checks this command against the table -
    plus this run's fresh-run id (provenance, not a rule parameter)."""
    return ([sys.executable, os.path.join(HERE, "flicker_diff.py"), d, "--worst", d + "_flicker_worst.jpg"]
            + flicker_rule.canonical_args() + (["--run-id", run_id] if run_id else []))


def guarded_half(name, d, readonly, fn, *args, **kw):
    """C2's parse-boundary rule at EVERY entry path (Astra e1cca492 r5 P1: direct callers - tools/native_ledger.py's
    readonly grades - bypassed grade()'s guard): a half whose grading raises is a FAIL with <half>_error, never an
    exception."""
    try:
        return fn(*args, **kw)
    except Exception as e:
        return {"dir": d, "verdict": "FAIL", "%s_error" % name: err_text(e), "reused": None, "readonly": bool(readonly),
                "line": "the %s could not be graded (%s)" % (name, err_text(e))}


def grade_span(d, span_max, dark_max=wf.SCENE_DARK_MAX, luma_min=wf.SCENE_LUMA_MIN, in_play_min=wf.SPAN_IN_PLAY_MIN,
               reuse=False, readonly=False, strict=False):
    """The span half. reuse: grade the stored <d>_flicker.txt when it exists (else run flicker_diff.py);
    readonly: never run anything or write (the ledger) - a missing stored output is NO DATA.
    strict (C2 2026-10-22; main() sets it for a gate with a strict_rule.json marker): never grade a stored output -
    flicker_diff runs fresh, whatever reuse says. The result records reused / readonly, and a fresh run records its
    stamp (fresh_run: the id passed to that flicker_diff invocation and read back from its output). Guarded: any
    exception is a FAIL with span_error."""
    return guarded_half("span", d, readonly, _grade_span, d, span_max, dark_max, luma_min, in_play_min, reuse,
                        readonly, strict)


def _grade_span(d, span_max, dark_max, luma_min, in_play_min, reuse, readonly, strict):
    r = {"dir": d, "verdict": "NO DATA", "reused": False, "readonly": bool(readonly)}
    if strict:
        reuse = False
    log = run_log(d)
    if not log:
        if strict:   # lane C r29c (Astra 76d38ae r30 P2): no run log - the placement is unmeasured, a named FAIL
            r.update(verdict="FAIL", placement_error="the span has no run log",
                     line="PLACEMENT UNMEASURED: the span has no run log")
        return r
    if strict:   # r13: STRICT LOG INTEGRITY - a run log with a cut / corrupt record grades nothing (raises: span_error)
        log_integrity(log)
    stored = d + "_flicker.txt"
    out = None
    fresh_exit = None
    if (reuse or readonly) and os.path.exists(stored):
        try:   # the stored output: a parse boundary (unreadable / not UTF-8 -> recorded; never an exception)
            with open(stored, "rb") as f:
                raw = f.read()
            try:
                out = raw.decode("utf-8")
            except UnicodeDecodeError as e:
                r["stored_error"] = err_text(e)
                out = raw.decode("utf-8", "replace")
        except Exception as e:
            r["stored_error"] = err_text(e)
            out = None
            if readonly:
                return r
        if out is not None and not readonly and detector_of(out) != DETECTOR:
            out = None   # Fable 28 lifetime: --reuse never re-grades another detector's stored output - run this one
        r["reused"] = out is not None
    elif readonly:
        return r
    if out is None:
        run_id = uuid.uuid4().hex
        p = subprocess.run(flicker_diff_cmd(d, run_id), cwd=REPO, capture_output=True, text=True, errors="replace")
        out = p.stdout or ""
        try:
            with open(stored, "w", encoding="utf-8", errors="replace") as f:
                f.write(out + (p.stderr or ""))
        except Exception as e:   # the stored copy is evidence: the verdict below is graded from this run's own stdout
            r["stored_write_error"] = err_text(e)
        if run_id in RUN_LINE.findall(out):
            r["fresh_run"] = {"run_id": run_id, "exit": p.returncode,
                              "at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                              "code_sha256": code_stamp()}
        else:
            r["fresh_run_error"] = "flicker_diff (exit %s) did not echo this run's id" % p.returncode
        fresh_exit = p.returncode
    # read-only (the ledger) grades a stored output as written - an older gate's evidence stays what it was - but a
    # verdict from any detector other than this one is NAMED in the line and in r["detector"], never silent
    r["detector"] = detector_of(out)
    try:   # the effective rule parameters beside the identity (C2 (b)); evidence only - a parse boundary
        pm = PARAMS_LINE.search(out)
        if pm:
            pv = json.loads(pm.group(1))
            if not isinstance(pv, dict) or not shallow(pv):
                raise ValueError("FLICKER PARAMS is not a small JSON object")
            r["params"] = pv
        elif re.search(r"^FLICKER PARAMS:", out, re.M):   # the record is there but not readable (Astra r5 P3)
            raise ValueError("a FLICKER PARAMS line that is not a record")
    except Exception as e:
        r.pop("params", None)
        r["params_error"] = err_text(e)
    # Fable 31 (2)(i)-(ii): flicker_diff's READONLY admission diagnostic - recorded for the filing, read by NO pass / fail
    # code below or anywhere (test_flicker_gate proves the verdict identical with it on and off)
    try:   # isolated end to end (Astra r3 P2; C2's parse-boundary rule): ANY failure drops the whole record -> diag31_error
        diag = [DIAG_LINE.match(ln.strip()) for ln in out.splitlines() if ln.lstrip().startswith("diag31 ")]
        if diag:
            if any(dm is None for dm in diag):   # a diag31 line that is not a record (truncated, say): named (r5 P3)
                raise ValueError("a diag31 line that is not a record")
            rec = []
            if len(diag) > DIAG_MAX:   # bounded: the first DIAG_MAX records are kept, the rest counted
                r["diag31_truncated"] = len(diag)
            for dm in diag[:DIAG_MAX]:
                e = json.loads(dm.group(4))
                if not isinstance(e, dict) or not shallow(e):
                    raise ValueError("a diag31 record is not a small JSON object")
                e.update(present=int(dm.group(1)), row=int(dm.group(2)), col=int(dm.group(3)))
                rec.append(e)
            r["diag31"] = rec
    except Exception as e:
        r.pop("diag31", None)
        r.pop("diag31_truncated", None)
        r["diag31_error"] = err_text(e)
    # the verdict is parsed from flicker_diff's own records ONLY: the diagnostic's lines are dropped first and the verdict
    # lines are anchored at a line start, the LAST one taken (Astra f5e06465 r2 P2: diagnostic text must not be a verdict)
    try:   # a parse boundary too: a record that cannot be read (say a count too long for int) FAILS the span, named
        verdict_text = "\n".join(ln for ln in out.splitlines() if not ln.lstrip().startswith("diag31 "))
        # round 6 (Astra 6fb621c r6): every line-start FLICKER record is recognised COMPLETELY or the span FAILS, and the
        # LAST verdict-type record decides - a FLICKER UNREADABLE after a verdict overrides it, a cut verdict is no verdict
        # (an output another detector wrote - an older gate's stored evidence, graded readonly by the ledger - keeps the
        # older reading: the last anchored record; it is never a strict verdict: reused / readonly)
        m = u = None
        if r["detector"] != DETECTOR:
            ms = list(SPAN_LINE.finditer(verdict_text))
            us = list(UNREADABLE_LINE.finditer(verdict_text))
            m = ms[-1] if ms else None
            u = us[-1] if us and not m else None
        else:
            for ln in verdict_text.splitlines():
                if not ln.startswith("FLICKER "):
                    continue
                if ln.startswith(("FLICKER DETECTOR:", "FLICKER PARAMS:", "FLICKER RUN:")):
                    continue   # the identity / evidence records: read by their own parsers above
                full = SPAN_RECORD.match(ln)
                if full:
                    m, u = full, None
                elif UNREADABLE_RECORD.match(ln):
                    m, u = None, UNREADABLE_RECORD.match(ln)
                else:
                    raise ValueError("a FLICKER record that cannot be read: %r" % ln[:120])
        if m:
            r["bad"], r["scored"] = int(m.group(2)), int(m.group(3))
            r["verdict"] = "PASS" if r["bad"] <= span_max and r["scored"] >= 100 else "FAIL"
            r["line"] = ("FLICKER %s: %s of %s presents" % (m.group(1), m.group(2), m.group(3)))[:TEXT_MAX]
            # a fresh run COMPLETED only when its exit agrees with its own record (flicker_diff: 0 = PASS, 1 = FAIL)
            if fresh_exit is not None and fresh_exit != (0 if m.group(1) == "PASS" else 1):
                raise ValueError("flicker_diff exited %s after a FLICKER %s record: not a completed run"
                                 % (fresh_exit, m.group(1)))
            if fresh_exit is not None and r["detector"] == DETECTOR:
                # lane C r8 (Astra e7b0cca r7 P1-3): a fresh verdict covers the whole inventory flicker_diff listed - its
                # one summary line: consecutive presents, no gap, and every file in a scored triple (files = scored + 2;
                # flicker_diff itself refuses an inventory the run log or the numbering says is incomplete)
                inv = list(INVENTORY_LINE.finditer(out))
                if len(inv) != 1:
                    raise ValueError("a fresh FLICKER %s with %d inventory lines (flicker_diff prints one)"
                                     % (m.group(1), len(inv)))
                first, last, files, gaps = (int(x) for x in inv[0].groups())
                if gaps or files != r["scored"] + 2 or last - first + 1 != files:
                    raise ValueError("flicker_diff's inventory p%d..p%d (%d files, %d gaps) is not the %d scored presents "
                                     "+ 2 of one consecutive window" % (first, last, files, gaps, r["scored"]))
        elif u:
            # Fable 28 (1): flicker_diff fails closed on a damaged frame - the span is a FAIL, named, never a silent drop
            r["verdict"] = "FAIL"
            r["line"] = u.group(0)[:TEXT_MAX]
    except Exception as e:
        for k in ("bad", "scored"):
            r.pop(k, None)
        r["verdict"] = "FAIL"
        r["verdict_error"] = err_text(e)
        r["line"] = "FLICKER record unreadable (%s)" % r["verdict_error"]
    if r["detector"] != DETECTOR:
        r["line"] = r.get("line", "") + " | detector: %s, not %s" % (r["detector"], DETECTOR.split(" ")[0])
    try:
        sc = scene_stats(d)
    except Exception as e:   # fail closed: a scene floor that cannot be measured is UNMEASURED (a FAIL), never a crash
        sc = {"n": 0, "unmeasured": 1, "why": "the scene floor could not be measured (%s)" % err_text(e)}
    if not sc or sc.get("unmeasured") or sc.get("why"):   # no scene floor measured -> never a PASS
        r["scene"] = sc
        r["line"] = r.get("line", "") + " | scene UNMEASURED: %s" % (
            (sc or {}).get("why") or "the scene floor could not be measured")
        r["verdict"] = "FAIL"
    else:
        r["scene"] = sc
        blank = sc["dark_median"] > dark_max or sc["luma_median"] < luma_min
        r["line"] = r.get("line", "") + " | scene luma %.1f dark %.2f%s" % (
            sc["luma_median"], sc["dark_median"], " -> WORLD NOT DRAWN" if blank else "")
        if blank:
            r["verdict"] = "FAIL"
    mk = mark_present(d)
    if mk is not None:
        r["mark"] = mk
    n, tot, unknown, ctx_unknown, era, era_why = span_placement(d)
    r["in_play"] = [n, tot]
    passing_but_for_play = r["verdict"] == "PASS"
    if not tot or n < in_play_min * tot:
        r["verdict"] = "FAIL"
        r["not_in_play"] = True
        r["line"] = r.get("line", "") + " | in play %d of %d -> SPAN NOT IN PLAY" % (n, tot)
    else:
        r["line"] = r.get("line", "") + " | in play %d of %d" % (n, tot)
    if mk is not None:
        r["line"] += " (window from the tape's dump mark, p%d)" % mk
    r = grade_roll_health(log, r)
    if strict:
        # ONE placement rule for EVERY strict span, whatever its play share (C2 2026-10-23, after conv27 P1-3 and Astra /
        # conv r29 P1-1): the placement as measured is recorded for strict_fg's schema (era known, no present of unknown
        # timing or context) - else a named measurement FAIL (placement_error), in play or not
        r["era_start_ms"] = era
        r["timing_unknown"] = unknown
        r["context_unknown"] = ctx_unknown
        if era is None or unknown or ctx_unknown or not tot:
            # the placement could not be measured (no timing metadata, no in-game start in the log, presents whose
            # timing or context is unknown, no window): a named measurement FAIL, never NO DATA
            if era_why:
                r["placement_error"] = era_why
            elif not tot:
                r["placement_error"] = "no span window was dumped"
            elif unknown:
                r["placement_error"] = "%d of %d presents have unknown timing" % (unknown, tot)
            else:
                r["placement_error"] = ("%d of %d presents at or after the in-game start have no [PC_CONTROLS] context "
                                        "before them (whether they are in play is unknown)" % (ctx_unknown, tot))
            r["line"] += " | PLACEMENT UNMEASURED: " + r["placement_error"]
            r["verdict"] = "FAIL"
    if strict and r.get("not_in_play") and passing_but_for_play and r["roll_health"].get("status") == "PASS" \
            and mk is None and not r.get("placement_error"):
        # C2 2026-10-23: a strict gate is never re-placed - a span whose ONE failure is that it was MEASURED out of play
        # has no verdict: NO DATA (never a PASS), the reason recorded; the runner may declare one fresh strict gate. Any
        # other failure (flicker outliers, coverage, the scene floor, the roll's health, an unmeasured placement) stays a
        # FAIL - and so does a span the tape's dump mark placed (mk): that miss is a finding, never re-rolled.
        r["verdict"] = "NO DATA"
        r["no_data_reason"] = "the span is not in play (in play %d of %d): a strict gate is not re-placed" % (n, tot)
        r["line"] += " -> NO DATA (strict: not re-placed; the runner may declare one fresh gate)"
    return r


COUNT_FIELD = re.compile(r"^\d{1,12}$")
SIGNED_FIELD = re.compile(r"^-?\d{1,12}$")


def table_int(parts, i, what, signed=False):
    """A REQUIRED numeric field of a freeze_diff table row (C2's parse-boundary rule; Astra e1cca492 r5 P1: a malformed
    count used to read as 0 - a PASS). Missing or not a number -> ValueError: the freeze half FAILS with freeze_error."""
    if i >= len(parts):
        raise ValueError("freeze table row %s: no %s field" % (parts[0][:20], what))
    v = parts[i]
    if not (SIGNED_FIELD if signed else COUNT_FIELD).match(v):
        raise ValueError("freeze table row %s: %s is %r, not a count" % (parts[0][:20], what, v[:40]))
    return int(v)


# STRICT LOG INTEGRITY (lane C r13 .. r23) lives in log_records.py since r24 (Astra dac76f1 r23 P1: flicker_diff's own
# plumbing skipped a spliced mark this gate refused) - ONE definition the strict gate and both standalone detectors read
# their logs by. Re-exported here: the gate's halves, its callers and the tests use these names.
from log_records import (LOG_LINE, INNER_TS, RECORD_START, MSG_TAG, RECORD_FORMS, UNTAGGED_FORMS,  # noqa: E402,F401
                         VERDICT_TAG, VERDICT_NAMES, VERDICT_NAME, INTERVAL, brackets_pair, SECOND_ENVELOPE,
                         LEVEL_PIECE, CATEGORY_PIECE, TAG_LIKE, LOG_BACKSTEP_S, log_integrity)


def table_rows(table):
    """{freeze: its row's fields} of a freeze_diff table. Lane C r11 (Astra 8d20c07 r10 / conv10 P2: a `pair |` row of a
    freeze without a row disappeared from grading): every per-pair row (`pair |` / `compared |`) names a freeze that has a
    row, else it raises."""
    rows, named = {}, set()
    for ln in table.splitlines():
        parts = [s.strip() for s in ln.strip().split("|")]
        if parts[0].isdigit():
            if "shown stamp moved" in ln and not MOVED_ROW.match(ln.strip()):
                # r17 (Astra 5fb5a2c r16 P2: a cut `(shown stamp moved` row exempted its freeze): the exemption is freeze_diff's
                # WHOLE moved-stamp row or none
                raise ValueError("a freeze table moved-stamp row that cannot be read: %r" % ln.strip()[:160])
            rows.setdefault(int(parts[0]), parts)
        elif parts[0] in ("pair", "compared") and len(parts) > 1 and parts[1].isdigit():
            named.add(int(parts[1]))
    orphans = sorted(named - set(rows))
    if orphans:
        raise ValueError("a per-pair row names freeze %d, which has no row in the freeze table" % orphans[0])
    return rows


def pair_chains(table):
    """{freeze: [(present a, present b), ...]} of a plain table's per-pair rows: every `compared |` row plus every size-change
    `pair |` row (pixels -1: freeze_diff prints no compared row for it), and {freeze: [(a, b)]} of the other `pair |` rows
    (the differing pairs, each also compared). Rows that cannot be read raise (lane C r12)."""
    chains, differing = {}, {}
    for ln in table.splitlines():
        s = ln.strip()
        head = s.split("|")[0].strip()
        if head == "compared":
            m = COMPARED_ROW.match(s)
            if not m:
                raise ValueError("a freeze table compared row that cannot be read: %r" % s[:160])
            chains.setdefault(int(m.group(1)), []).append((int(m.group(2)), int(m.group(3))))
        elif head == "pair":
            m = PAIR_ROW.match(s)
            if not m:
                raise ValueError("a freeze table pair row that cannot be read: %r" % s[:160])
            ab = (int(m.group(2)), int(m.group(3)))
            (chains if int(m.group(4)) < 0 else differing).setdefault(int(m.group(1)), []).append(ab)
    return chains, differing


def freeze_table_complete(table, fresh=False):
    """(complete, why) for a freeze_diff --exact table: it carries exactly ONE FREEZE_EXACT line (freeze_diff prints
    one), that line is a complete PASS / FAIL / NO DATA record (freeze_record; not UNREADABLE), the freeze rows before it
    add up to the record's frozen presents and consecutive pairs, and no graded row follows it. A table that is not
    complete is never graded as the rows it has (Astra e1cca492 r5 P1; e7b0cca r7 P1-4: every record line is inspected,
    a cut or bare one is no record)."""
    lines = table.splitlines()
    at = [i for i, ln in enumerate(lines) if ln.lstrip().startswith("FREEZE_EXACT")]
    if not at:
        return False, "no FREEZE_EXACT record: freeze_diff did not complete"
    if len(at) > 1:
        return False, "%d FREEZE_EXACT lines (freeze_diff prints one record): %r" % (len(at), lines[at[-1]][:80])
    ln = lines[at[0]]
    rec = freeze_record(ln)
    if rec is None:   # a partial, cut or unknown record (`FREEZE_EXACT UNREAD`, a bare `FREEZE_EXACT PASS`, ...)
        return False, "a FREEZE_EXACT line that is not a complete record: %r" % ln[:80]
    if rec.re is FREEZE_UNREADABLE:
        return False, ln[:TEXT_MAX]
    frames = pairs = 0
    for row in lines[:at[0]]:
        parts = [s.strip() for s in row.split("|")]
        if parts[0].isdigit():
            frames += table_int(parts, 1, "frames")
            pairs += table_int(parts, 2, "pairs")
    if (frames, pairs) != (int(rec.group("frames")), int(rec.group("pairs"))):
        return False, ("its rows add up to %d frozen presents / %d pairs, its record says %s / %s"
                       % (frames, pairs, rec.group("frames"), rec.group("pairs")))
    # r11 (Astra 8d20c07 r10 P2): the record's differing-freeze count and list are its rows' - the freezes with a differing
    # pair (plain: column 3; A/B: the differing STATIC pairs, column 9), never a row whose shown stamp moved (freeze_diff
    # lists none of those) - and every per-pair row belongs to a freeze row (table_rows)
    rows = table_rows("\n".join(lines[:at[0]]))
    col = 9 if rec.re is FREEZE_AB else 3
    bad = sorted(f for f, p in rows.items() if not (len(p) > 3 and "shown stamp moved" in p[3])
                 and table_int(p, col, "differing pairs") > 0)
    ids = [] if rec.group("ids") == "-" else [int(x) for x in rec.group("ids").split(",")]
    if int(rec.group("nbad")) != len(ids) or ids != bad:
        return False, ("its record lists %s freeze(s) with a differing pair (%s), its rows %d (%s)"
                       % (rec.group("nbad"), rec.group("ids"), len(bad), ",".join(map(str, bad)) or "-"))
    # r12 (Astra 190f7a5 conv11 P2): each freeze's own pair inventory - a row's pairs are its frames - 1 (consecutive
    # frozen presents), and in the plain table its compared rows (+ its size-change pair rows) are exactly that chain of
    # consecutive pairs, each differing pair among them; a freeze whose shown stamp moved carries none (freeze_diff
    # prints none). r13: the chain is REQUIRED of a fresh table (fresh: this run's own freeze_diff, which prints every
    # compared row - the strict path) and enforced on any table that carries compared rows; a STORED table with none (an
    # older freeze_diff's, e.g. the archived p1a_gate2 / zpd4 gates) is a legacy table, graded as stored - never a strict
    # verdict (reused / readonly) - so the legacy reading stays separate from the current one (Astra conv11's advice).
    for f, p in rows.items():
        if table_int(p, 2, "pairs") != table_int(p, 1, "frames") - 1:
            return False, "freeze %d: %s pairs for %s frames (consecutive presents give frames - 1)" % (f, p[2], p[1])
    has_cmp = any(ln.strip().startswith("compared") for ln in lines[:at[0]])
    if rec.re is FREEZE_PLAIN and (fresh or has_cmp):
        chains, differing = pair_chains("\n".join(lines[:at[0]]))
        for f, p in rows.items():
            moved = len(p) > 3 and "shown stamp moved" in p[3]
            ch = sorted(chains.get(f, []))
            want = 0 if moved else int(p[2])
            if (len(ch) != want or len(set(ch)) != len(ch) or any(b != a + 1 for a, b in ch)
                    or any(ch[i + 1][0] != ch[i][1] for i in range(len(ch) - 1))):
                return False, ("freeze %d: its compared / size-change rows are not its %d consecutive pairs (%d rows)"
                               % (f, want, len(ch)))
            dif = differing.get(f, [])
            if len(set(dif)) != len(dif):   # r13 (Astra 762d3d4 r12 / conv12 P2): one row per differing pair
                return False, "freeze %d: a differing pair row repeated" % f
            if not set(dif) <= set(ch):
                return False, "freeze %d: a differing pair row that is not one of its compared pairs" % f
    for row in lines[at[0] + 1:]:   # nothing graded may follow the record (freeze_diff prints none)
        head = row.split("|")[0].strip()
        if head.isdigit() or head in ("pair", "compared"):
            return False, "a table row after the FREEZE_EXACT record"
    return True, rec.group("verdict")


def recorded_table(table):
    """The completion rule at EVERY entry to the table grader (Astra 6fb621c r6: the ledger's direct call skipped it):
    a table with any FREEZE_EXACT line must be complete (freeze_table_complete) -> its record's verdict word; a table
    with none is graded as stored -> None (never a strict verdict: a stored table is reused / readonly). Lane C r8
    (Astra e7b0cca r7 P1-4): no older-format fallback - a bare or count-less record is incomplete. An incomplete table
    raises."""
    if not any(ln.lstrip().startswith("FREEZE_EXACT") for ln in table.splitlines()):
        return None
    ok, why = freeze_table_complete(table)
    if not ok:
        raise ValueError("an incomplete freeze table: %s" % why)
    return why


def grade_freeze_table(d, log, table, allow_pixels=64, allow_spike=8, r=None, quiet=False):
    """Grade freeze_diff.py's table output (the era filter + the speck tolerance) WITHOUT running it or
    writing anything: grade_freeze() calls it on a fresh table, tools/native_ledger.py on the stored
    `<dir>_freeze.txt`, so the ledger and the gate grade by one rule. The world rule is grade_freeze_world.
    Guarded (Astra e1cca492 r5: the ledger calls this directly): a table or log it cannot read - a malformed required
    field (table_int), a count past int's limit - makes r a FAIL with freeze_error, never an exception."""
    r = r if r is not None else {"dir": d, "verdict": "NO DATA"}
    try:
        r["table_record"] = recorded_table(table)
        return _grade_freeze_table(d, log, table, allow_pixels, allow_spike, r, quiet)
    except Exception as e:
        r["verdict"] = "FAIL"
        r["freeze_error"] = err_text(e)
        r["line"] = "the freeze table could not be graded (%s)" % r["freeze_error"]
        return r


def _grade_freeze_table(d, log, table, allow_pixels, allow_spike, r, quiet):
    # Freeze begin times against the ingame era; freeze windows (begin -> end) against the movies' wall windows.
    ig, t0 = ingame_us(d, log)
    if ig is None:
        # lane C r11 (Astra 8d20c07 conv10 P1: perf/meta.txt removed -> no era -> the old 110 s wall-clock fallback
        # excused a defective in-game freeze, strict exit 0): a roll whose in-game era cannot be established places no
        # freeze - the half FAILS, named (a save-started roll that never reached play included: never a PASS)
        raise ValueError("the roll's in-game era is unknown (%s): no freeze can be placed against it"
                         % ("no perf t0: perf/meta.txt epoch_ms_t0 missing or unreadable" if not t0
                            else "no era marker in the log"))
    begins, ends, movies, present_t = {}, {}, [], {}
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace")
            m = FZ_BEGIN.search(line)
            e = None if m else FZ_END.search(line)
            mv = None if (m or e) else MOVIE_EOM.search(line)
            dp = None if (m or e or mv) else FZ_DUMP.search(line)
            if not (m or e or mv or dp):
                # a freeze's begin / end record that cannot be read never silently drops the freeze's timing (Astra
                # 6fb621c r6: a malformed `begin at` read the freeze as not in-game and excused its defect)
                if "[SIM_FREEZE] #" in line and (" begin at " in line or " end: " in line):
                    raise ValueError("a [SIM_FREEZE] record that cannot be read: %r" % line.strip()[:160])
                continue
            ts = perf_report.LOG_TS.match(line)
            t_us = None
            if ts and t0:
                dt = datetime.datetime.strptime(ts.group(1), "%Y-%m-%d %H:%M:%S.%f")
                t_us = int((dt.timestamp() * 1000 - t0) * 1000)
            elif t0 and (m or e):
                # lane C r8 (Astra e7b0cca r7 P1-1): a freeze's begin / end record whose log time cannot be read has an
                # UNKNOWN place against the in-game era and the movie windows - never "not in-game" (that excused a
                # defective freeze: strict exit 0). The roll's timing is unreadable: the freeze half FAILS, named.
                raise ValueError("a [SIM_FREEZE] record without a readable log time: %r" % line.strip()[:160])
            if m:
                if int(m.group(1)) in begins:   # one begin per freeze: a second never replaces the first's timing (r8)
                    raise ValueError("freeze %s has two [SIM_FREEZE] begin records" % m.group(1))
                begins[int(m.group(1))] = (float(m.group(2)), t_us)
            elif e:
                if int(e.group(1)) in ends:
                    raise ValueError("freeze %s has two [SIM_FREEZE] end records" % e.group(1))
                ends[int(e.group(1))] = t_us
            elif mv:
                if t_us is not None and int(mv.group(3)) >= 2:  # a movie (perf_report.INGAME_MOVIE: >= 2 frames)
                    movies.append((int(mv.group(1)), t_us - int(float(mv.group(2)) * 1e6), t_us))
            elif t_us is not None:
                present_t[int(dp.group(2))] = t_us
    # freeze_diff.py's per-pair rows of the differing pairs (a table without them predates the rows)
    pair_rows = {}
    per_pair = False   # a table with freeze_diff's per-pair rows (`pair |` / `compared |`): their counts are reconciled
    for ln in table.splitlines():
        s = ln.strip()
        head = s.split("|")[0].strip()
        if head in ("pair", "compared"):
            per_pair = True
        pm = PAIR_ROW.match(s)
        if pm:
            prs = pair_rows.setdefault(int(pm.group(1)), [])
            if any((a, b) == (int(pm.group(2)), int(pm.group(3))) for a, b, _n, _m in prs):
                # r13 (Astra 762d3d4 r12 / conv12 P2: a duplicate stood in for an outside-movie defect's row)
                raise ValueError("a freeze table pair row repeated: %r" % s[:160])
            prs.append((int(pm.group(2)), int(pm.group(3)), int(pm.group(4)), int(pm.group(5))))
        elif head == "pair":   # a per-pair row that cannot be read never disappears (Astra 6fb621c r6)
            raise ValueError("a freeze table pair row that cannot be read: %r" % s[:160])
        elif head == "compared" and not COMPARED_ROW.match(s):   # r11 (conv10 P2): parsed whole, like the pair rows
            raise ValueError("a freeze table compared row that cannot be read: %r" % s[:160])
    table_rows(table)   # r11 (Astra 8d20c07 r10 / conv10 P2): every per-pair row belongs to a freeze row

    def freeze_window(fid, t_begin):
        t_end = ends.get(fid)
        if t_end is None or t_end < t_begin:
            t_end = t_begin + FZ_HOLD_US
        return t_begin, t_end

    def movie_over(fid, t_begin):
        """The movie playing during freeze fid (begin .. its end line, else FZ_HOLD_US), or None."""
        if t_begin is None:
            return None
        tb, te = freeze_window(fid, t_begin)
        for num, start, end in movies:
            if start < te and end > tb:
                return num, start, end
        return None

    def pair_in_movie(pair, movie):
        """A differing pair touches the movie window: either present inside it, or the two straddling it. A present
        without a logged time cannot be placed: the pair is graded (never excused by guesswork)."""
        ta, tb = present_t.get(pair[0]), present_t.get(pair[1])
        if ta is None or tb is None:
            return False
        return min(ta, tb) < movie[2] and max(ta, tb) > movie[1]
    rows, bad, counted = [], [], 0
    # --ab-control tables (freeze_diff.py, the per-present A/B): the pair p->p+1 is graded only when its
    # same-transport control p-1->p+1 is exact; the unsettled pairs are reported, a freeze with fewer than
    # MIN_STATIC_PAIRS static pairs is NO DATA, and a roll with fewer than MIN_STATIC_ROLL static pairs in all is
    # NO DATA - never PASS by having nothing left to compare. The thresholds (allow_pixels / allow_spike) are the same.
    ab_control = " | static pairs | " in table.splitlines()[0] if table.strip() else False
    unsettled, static_total, unsettled_total = [], 0, 0
    worst_control = None
    for line in table.splitlines():
        parts = [s.strip() for s in line.split("|")]
        if not parts[0].isdigit():
            continue
        if len(parts) < 4:   # a freeze row (its first field is the freeze) cut short is malformed: never skipped
            raise ValueError("freeze table row %s: %d fields" % (parts[0][:20], len(parts)))
        fid = int(parts[0])
        if any(rw["freeze"] == fid for rw in rows):   # one row per freeze (freeze_diff prints one; r8: one-to-one)
            raise ValueError("freeze %d has two rows in the freeze table" % fid)
        if fid not in begins:   # a freeze without its begin record has no timing: never excluded on that (Astra r6)
            raise ValueError("freeze %d has no [SIM_FREEZE] begin record in the log" % fid)
        wall, t_us = begins[fid]
        if t_us is None:   # unknown timing never excludes a freeze (lane C r8, Astra e7b0cca r7 P1-1)
            raise ValueError("freeze %d's begin record has no log time: its place against the in-game era is unknown"
                             % fid)
        # r11: the era is always known here (an unknown one raised above) - no wall-clock fallback excludes a freeze
        ingame = t_us >= ig
        row = {"freeze": fid, "wall_s": wall, "ingame": ingame, "line": line.strip()}
        movie = movie_over(fid, t_us) if ingame else None
        # Astra merge review P2 (2026-10-15): a movie excuses only what it covers. Covering the whole hold: nothing
        # froze (the FMV on its media clock), the freeze is not graded. Covering part of it: the pairs outside its
        # window are graded (freeze_diff.py's per-pair rows); with no such rows (an older table, an A/B table) the
        # freeze is graded whole, never excused.
        full = False
        ndiff_row = None if "shown stamp moved" in line else table_int(parts, 3, "differing pairs")
        # with per-pair rows, every differing pair has one (freeze_diff prints them all): a count that does not match is
        # a table that cannot be graded - a missing row could hide the defect a movie does not cover (Astra 6fb621c r6)
        if per_pair and ndiff_row is not None and len(pair_rows.get(fid, [])) != ndiff_row:
            raise ValueError("freeze %d: %d differing pairs but %d pair rows" % (fid, ndiff_row,
                                                                                 len(pair_rows.get(fid, []))))
        if movie:
            tb, te = freeze_window(fid, t_us)
            full = movie[1] <= tb and movie[2] >= te
            span = "movie#%d (%.1f-%.1f s)" % (movie[0], movie[1] / 1e6, movie[2] / 1e6)
        if "shown stamp moved" in line:
            row["note"] = "stamp moved (not a frozen window)"
        elif movie and full:
            # an FMV runs on the media clock while the sim is held: its frames change, nothing froze (not graded)
            row["movie"] = movie[0]
            row["note"] = "%s plays through all of it: the media clock, not a frozen window" % span
        elif movie and not ab_control and (fid in pair_rows or not ndiff_row):
            inside = [p for p in pair_rows.get(fid, []) if pair_in_movie(p, movie)]
            outside = [p for p in pair_rows.get(fid, []) if not pair_in_movie(p, movie)]
            counted += 1
            row["movie_partial"] = movie[0]
            row["differing_pairs"] = len(outside)
            row["note"] = "%s overlaps part of it: %d differing pair(s) inside excused, %d outside graded" % (
                span, len(inside), len(outside))
            if any(n < 0 or n > allow_pixels or md > allow_spike for _a, _b, n, md in outside):
                bad.append(fid)
        elif ingame and ab_control:   # every A/B row has its 17 fields (a short one is malformed: FAIL, not plain)
            nstatic = table_int(parts, 8, "static pairs")
            ndiff = table_int(parts, 9, "differing static pairs")
            nuns = table_int(parts, 13, "unsettled pairs")
            cpx = table_int(parts, 15, "control pixels")
            cmd = table_int(parts, 16, "control maxdiff")
            trailing = table_int(parts, 17, "trailing static pairs") if len(parts) > 17 else nstatic
            static_total += nstatic
            unsettled_total += nuns
            row.update({"static_pairs": nstatic, "unsettled_pairs": nuns,
                        "control_worst": ("%s %d px at maxdiff %d" % (parts[14], cpx, cmd)) if nuns else "-"})
            if nuns and (worst_control is None or cpx > worst_control[1]):
                worst_control = (fid, cpx, cmd, parts[14])
            px = table_int(parts, 11, "static pixels")
            md = table_int(parts, 12, "static maxdiff")
            row["trailing_static"] = trailing
            if ndiff and (px > allow_pixels or md > allow_spike):
                # a differing static pair beyond the tolerance is a finding whatever else the window did
                counted += 1
                row["differing_pairs"] = ndiff
                bad.append(fid)
            elif nstatic < MIN_STATIC_PAIRS:
                row["note"] = ("unsettled: %d static pairs of %s (need %d), %d excluded, worst control %s - NO DATA"
                               % (nstatic, parts[2], MIN_STATIC_PAIRS, nuns, row["control_worst"]))
                unsettled.append(fid)
            elif nuns and cmd > allow_spike:
                # an excluded pair whose control exceeds the spike tolerance: the frame MOVED (a dropped draw, a black
                # flip), it did not settle (LSB transients stay within it)
                row["note"] = ("moved: a control at maxdiff %d > the spike tolerance %d (%s), %d static pairs - NO DATA"
                               % (cmd, allow_spike, row["control_worst"], nstatic))
                unsettled.append(fid)
            else:
                counted += 1
                row["differing_pairs"] = ndiff
                if ndiff:
                    row["speck"] = "worst static pair %d px at maxdiff %d (within --allow-pixels %d / --allow-spike %d)" % (
                        px, md, allow_pixels, allow_spike)
        elif ingame:
            counted += 1
            ndiff = ndiff_row
            row["differing_pairs"] = ndiff
            if ndiff:
                px = table_int(parts, 5, "pixels", signed=True)
                md = table_int(parts, 6, "maxdiff")
                if 0 <= px <= allow_pixels and md <= allow_spike:   # a negative count (-1: a size change) is never a speck
                    row["speck"] = "worst pair %d px at maxdiff %d (within --allow-pixels %d / --allow-spike %d)" % (
                        px, md, allow_pixels, allow_spike)
                else:
                    bad.append(fid)
        if movie and not full and "movie_partial" not in row and "shown stamp moved" not in line:
            row["note"] = ((row["note"] + "; ") if row.get("note") else "") + (
                "%s overlaps part of it: no per-pair rows, graded whole" % span)
        rows.append(row)
    verdict = "FAIL" if bad else ("PASS" if counted >= 3 else "NO DATA")
    if ab_control and verdict == "PASS" and static_total < MIN_STATIC_ROLL:
        verdict = "NO DATA"
    r.update({"ingame_from_us": ig, "freezes": rows, "counted": counted, "bad": bad, "verdict": verdict})
    during_movie = [row["freeze"] for row in rows if "movie" in row]
    if during_movie:
        r["movie_excluded"] = during_movie
    part_movie = [row["freeze"] for row in rows if "movie_partial" in row]
    if part_movie:
        r["movie_partial"] = part_movie
    if ab_control:
        r.update({"ab_control": True, "unsettled": unsettled, "static_pairs": static_total, "unsettled_pairs": unsettled_total,
                  "control_line": "A/B control: %d static pairs graded, %d unsettled pairs excluded%s%s" % (
                      static_total, unsettled_total,
                      (", %d freezes NO DATA (%s)" % (len(unsettled), ",".join(map(str, unsettled)))) if unsettled else "",
                      (", worst control freeze %d %s %d px at maxdiff %d" % (worst_control[0], worst_control[3], worst_control[1], worst_control[2])) if worst_control else "")})
    if not rows:
        with open(log, "rb") as f:
            txt = f.read()
        if b"[SIM_FREEZE] #" in txt and b"[SIM_FREEZE_DUMP]" not in txt:
            r["note"] = ("the sim froze but no frozen present was dumped: this exe lacks SR_PRESENT_DUMP_FREEZE=2 "
                         "(the dense freeze dump, iter-467 flicker instruments) - build it from a tree that has it")
            if not quiet:
                print("freeze : " + r["note"])
    return r


def grade_freeze_world(d, log, r, min_world=wf.MIN_WORLD_FREEZES, max_menu=wf.MAX_MENU_FREEZES):
    """The world rule on a graded freeze roll (r from grade_freeze_table): fewer than min_world graded freezes
    showing the world -> FAIL, whatever the pixels say (an exact freeze over a black world proves nothing);
    more than max_menu of them the pause menu over the world -> FAIL (a static menu over a dimmed world is not
    the draw path under test: the rest must be gameplay)."""
    ws = wf.freeze_world(d, log, r.get("freezes", []))
    n = wf.world_count(ws)
    r["world"] = ws
    r["world_drawn"] = n
    r["world_line"] = wf.world_line(ws)
    if n < min_world and r.get("counted"):
        r["world_fail"] = "WORLD NOT DRAWN: the world shows in %d graded freezes (need %d)" % (n, min_world)
        r["verdict"] = "FAIL"
    menu = len(ws.get("menu_world", []))
    r["menu_drawn"] = menu
    if max_menu is not None and menu > max_menu and r.get("counted") and not r.get("world_fail"):
        r["world_fail"] = ("MENU-HEAVY: %d of the graded freezes show the pause menu over the world (at most %d; "
                           "the rest must be gameplay)" % (menu, max_menu))
        r["verdict"] = "FAIL"
    return grade_roll_health(log, r)


MIN_STATIC_PAIRS = 20   # the per-present A/B: static pairs a freeze needs to be graded (freeze_diff.py has the same)
MIN_STATIC_ROLL = 200   # ... and a roll in all (10 freezes' worth), else NO DATA


def grade_freeze(d, allow_maxdiff, allow_pixels=64, allow_spike=8, min_world=wf.MIN_WORLD_FREEZES, reuse=False,
                 readonly=False, ab=False, max_menu=wf.MAX_MENU_FREEZES, strict=False):
    """The freeze half: freeze_diff.py's table (fresh, or the stored <d>_freeze.txt with reuse / readonly), graded
    for pixels (grade_freeze_table) and for the world (grade_freeze_world). strict (a strict gate, C2 2026-10-22):
    never the stored table - freeze_diff runs fresh; the result records reused / readonly. A FRESH table is graded
    only when it is complete (freeze_table_complete) and freeze_diff's exit agrees with its record; a stored table
    whose FREEZE_EXACT record carries its counts (or is UNREADABLE) must be complete too (one without - a fixture or an
    older freeze_diff - is graded as stored, recorded as table_record None). Guarded: any exception is a FAIL with
    freeze_error."""
    return guarded_half("freeze", d, readonly, _grade_freeze, d, allow_maxdiff, allow_pixels, allow_spike, min_world,
                        reuse, readonly, ab, max_menu, strict)


def _grade_freeze(d, allow_maxdiff, allow_pixels, allow_spike, min_world, reuse, readonly, ab, max_menu, strict):
    r = {"dir": d, "verdict": "NO DATA", "reused": False, "readonly": bool(readonly)}
    if strict:
        reuse = False
    log = run_log(d)
    if not log:
        return r
    if strict:   # r13: STRICT LOG INTEGRITY (freeze_error)
        log_integrity(log)
    stored = d + "_freeze.txt"
    if (reuse or readonly) and os.path.exists(stored):
        table = wf.read_text(stored)
        r["reused"] = True   # its completion rule (recorded_table) applies inside grade_freeze_table, for every caller
    elif readonly:
        return r
    else:
        frames = os.path.join(d, "drive", "attempt1", "frames")
        p = subprocess.run([sys.executable, os.path.join(HERE, "freeze_diff.py"), log, frames, "--out", d + "_freeze",
                            "--exact", "--allow-maxdiff", str(allow_maxdiff)] + (["--ab-control"] if ab else []),
                           cwd=REPO, capture_output=True, text=True, errors="replace")
        try:
            with open(stored, "w", encoding="utf-8", errors="replace") as f:
                f.write((p.stdout or "") + (p.stderr or ""))
        except Exception as e:   # the stored copy is evidence; the table below is this run's own stdout
            r["stored_write_error"] = err_text(e)
        table = p.stdout or ""
        ok, why = freeze_table_complete(table, fresh=True)   # r13: this run's table carries every compared row
        if not ok:
            raise ValueError("an incomplete fresh freeze_diff run (exit %s): %s" % (p.returncode, why))
        # freeze_diff --exact exits 0 on PASS, 1 on FAIL; NO DATA is 1 without pairs, 0 for an A/B roll whose pairs are
        # all unsettled
        if p.returncode not in {"PASS": (0,), "FAIL": (1,), "NO DATA": (0, 1)}[why]:
            raise ValueError("freeze_diff exited %s after a FREEZE_EXACT %s record: not a completed run" % (p.returncode, why))
    grade_freeze_table(d, log, table, allow_pixels, allow_spike, r, quiet=readonly)
    return grade_freeze_world(d, log, r, min_world, max_menu)


def mark_present(d):
    """The present the tape's dump mark opened the span window at (the exe logs it), or None: no mark seen (an exe
    without SR_PRESENT_DUMP_FROM_MARK, or a tape that never reached `mark dump`)."""
    log = run_log(d)
    if not log:
        return None
    m = MARK_LINE.search(wf.read_text(log))
    return int(m.group(1)) if m else None


def play_failed(d):
    """perf_run's record of a play tape that died (harness_play exited non-zero), or None. A record that exists but
    cannot be read still means the tape died (fail closed, Astra r5): its text says so."""
    p = os.path.join(d, "play_failed.txt")
    if not os.path.lexists(p):
        return None
    try:
        return wf.read_text(p).strip() or "play_failed.txt is empty"
    except Exception as e:
        return "play_failed.txt is unreadable (%s)" % err_text(e)


def replace_from(d, settle_s=40.0, cap=360):
    """OLD EXES ONLY (no dump mark): where a re-placed span should open -> (present, reason) or (None, reason).
    settle_s after the span roll's own in-game start (the opening movie's END OF MEDIA), moved past any stretch the
    roll's log shows outside the gameplay context (a pause, a menu) to the next gameplay one. The present count is
    THIS roll's; the re-roll is another boot (over 64 archived gates the HUD came at p9,767-p21,251), so the window
    can still land anywhere - the reason says so, and a mark-capable exe never comes here."""
    log = run_log(d)
    if not log:
        return None, "no span log"
    t = era_start_ms(d, log)
    if t is None:
        return None, "the span roll has no in-game era marker (no opening-movie EOM after the HUD)"
    t += settle_s * 1000.0
    timeline = wf.contexts(wf.read_text(log))
    moved = ""
    if wf.context_at(timeline, wf.ms_ts(t)) != "gameplay":
        nxt = [wf.ts_ms(ts) for ts, c in timeline if c == "gameplay" and wf.ts_ms(ts) > t]
        if not nxt:
            return None, "the span roll's log shows no gameplay context after %d s into play" % settle_s
        moved = ", moved %.1f s on past a %s stretch" % ((nxt[0] - t) / 1000.0, wf.context_at(timeline, wf.ms_ts(t)))
        t = nxt[0] + 2000.0
    p, total = wf.presents_at(log, t)
    if p is None:
        return None, "no present counter in the span roll's log ([NATIVE_METER] windows / [GPU_CP] census)"
    if total is not None and p + cap + 600 > total:
        return None, "the run ends at present %d, too soon after play (%d) for a %d-present span" % (total, p, cap)
    return p, ("%d s after the in-game start%s (present %d of %d; a present count from another boot: this exe has no "
               "dump mark)" % (settle_s, moved, p, total))


def write_span_clock(out, span_dir):
    """Fable 32 (7): the span's dump-index-to-mtime CSV IN THE FG ARCHIVE (<out>/span_dump_times.csv: present, file, t_ms =
    the dump file's mtime in ms - the frames themselves live on J:, which may be cleaned) and where the roll's S/P
    per-frame data is (span/perf/frames.csv, kept in the archive) -> the record flicker_gate.json carries. Readonly
    evidence (option 3's clock): no pass / fail reads it."""
    fdir = os.path.join(span_dir, "drive", "attempt1", "frames")
    rec = {"dump_times_csv": None, "presents": 0, "sp_frames_csv": None}
    sp = os.path.join(span_dir, "perf", "frames.csv")
    if os.path.exists(sp):
        rec["sp_frames_csv"] = os.path.relpath(sp, out).replace("\\", "/")
    try:
        names = os.listdir(fdir)
    except OSError as e:
        rec["why"] = "no span frames: %s" % e
        return rec
    rows, skipped = [], 0
    for f in names:
        try:   # a name that cannot be read as a dump (a present number too long for int, a name not encodable as
            #  UTF-8, a file that vanished) is skipped and counted, never an exception
            m = re.search(r"_p(\d+)_", f)
            if m and f.lower().endswith((".bmp", ".png")):
                f.encode("utf-8")
                rows.append((int(m.group(1)), f, os.path.getmtime(os.path.join(fdir, f)) * 1000.0))
        except Exception:
            skipped += 1
    rows.sort()
    p = os.path.join(out, "span_dump_times.csv")
    with open(p, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")   # a name holding a comma or a quote is quoted, never a broken row
        w.writerow(["present", "file", "t_ms"])
        w.writerows([pn, f, "%.3f" % t] for pn, f, t in rows)
    rec.update(dump_times_csv=os.path.basename(p), presents=len(rows))
    if skipped:
        rec["skipped_names"] = skipped
    return rec


def grade(out, span_max=5, allow_maxdiff=1, allow_pixels=64, allow_spike=8, min_world=wf.MIN_WORLD_FREEZES,
          in_play_min=wf.SPAN_IN_PLAY_MIN, only=None, reuse=False, readonly=False, max_menu=wf.MAX_MENU_FREEZES,
          strict=False):
    """Grade a gate dir (span/ + freeze/) -> the result dict main() writes as flicker_gate.json. strict: a strict gate
    (C2 2026-10-22) - no stored output is graded, both halves are recomputed."""
    res = {}
    # C2's parse-boundary rule, the last line of defence: a half whose grading raises (a log, table or record this
    # code cannot parse) is a FAIL with <half>_error - the gate always writes its JSON, never a traceback
    if only != "freeze" and os.path.isdir(os.path.join(out, "span")):
        try:
            res["span"] = grade_span(os.path.join(out, "span"), span_max, in_play_min=in_play_min, reuse=reuse,
                                     readonly=readonly, strict=strict)
        except Exception as e:
            res["span"] = {"dir": os.path.join(out, "span"), "verdict": "FAIL", "span_error": err_text(e),
                           "reused": None, "readonly": bool(readonly),
                           "line": "the span could not be graded (%s)" % err_text(e)}
    if only != "span" and os.path.isdir(os.path.join(out, "freeze")):
        try:
            res["freeze"] = grade_freeze(os.path.join(out, "freeze"), allow_maxdiff, allow_pixels, allow_spike,
                                         min_world=min_world, reuse=reuse, readonly=readonly, max_menu=max_menu,
                                         strict=strict)
        except Exception as e:
            res["freeze"] = {"dir": os.path.join(out, "freeze"), "verdict": "FAIL", "freeze_error": err_text(e),
                             "reused": None, "readonly": bool(readonly),
                             "line": "the freeze could not be graded (%s)" % err_text(e)}
    # lane C r18 (Astra 7c872fc conv17 P1: a full gate PASSED with its freeze/ folder absent): every REQUESTED half is
    # graded - one whose roll folder is missing is a named FAIL, never left out of the verdict (`--only` asks for one)
    for k in [h for h in ("span", "freeze") if only != {"span": "freeze", "freeze": "span"}[h]]:
        if k not in res:
            res[k] = {"dir": os.path.join(out, k), "verdict": "FAIL", "%s_error" % k: "the %s roll's folder is missing" % k,
                      "reused": None, "readonly": bool(readonly), "line": "the %s roll's folder is missing" % k}
    verdicts = [res[k]["verdict"] for k in ("span", "freeze") if k in res]
    res["verdict"] = "FAIL" if "FAIL" in verdicts else ("PASS" if verdicts and all(v == "PASS" for v in verdicts) else "NO DATA")
    res["world_rule"] = {"min_world_freezes": min_world, "max_menu_freezes": max_menu, "span_in_play_min": in_play_min,
                         "scene_luma_min": wf.SCENE_LUMA_MIN, "scene_dark_max": wf.SCENE_DARK_MAX,
                         "side_luma_min": wf.SIDE_LUMA_MIN, "side_dark_max": wf.SIDE_DARK_MAX,
                         # lane C r29c: every grading limit the verdict used (strict_fg: the canonical table)
                         "span_max": span_max, "allow_maxdiff": allow_maxdiff, "allow_pixels": allow_pixels,
                         "allow_spike": allow_spike, "only": only}
    try:   # the measurement cache is a convenience: a failed write is recorded, never the end of the grade
        wf.save_cache()
    except Exception as e:
        res["cache_error"] = err_text(e)
    return res


def main():
    for stream in (sys.stdout, sys.stderr):   # a line the console cannot encode (U+FFFD from a damaged record) is escaped
        try:
            stream.reconfigure(errors="backslashreplace")
        except Exception:
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--out")
    ap.add_argument("--gates", default="")
    ap.add_argument("--only", choices=["span", "freeze"], default=None)
    ap.add_argument("--instance", default="flicker")
    ap.add_argument("--span-from", type=int, default=20000)
    ap.add_argument("--span-cap", type=int, default=360)
    ap.add_argument("--span-max", type=int, default=5,
                    help="counted outlier presents tolerated on the span (default 5: the span is the coarse screen - fast "
                         "near objects and blur pulses score up to 5 on a clean exe in busy scenes; the dense freeze is the "
                         "exact oracle and decides)")
    ap.add_argument("--allow-maxdiff", type=int, default=1, help="freeze: per-channel noise floor (default 1)")
    ap.add_argument("--allow-pixels", type=int, default=64,
                    help="freeze: a worst pair changing at most this many pixels ...")
    ap.add_argument("--allow-spike", type=int, default=8,
                    help="... by at most this many levels is a speck, not a defect (default 64 px / 8 levels)")
    ap.add_argument("--min-world", type=int, default=wf.MIN_WORLD_FREEZES,
                    help="freeze: graded freezes that must show the 3D world (default %d)" % wf.MIN_WORLD_FREEZES)
    ap.add_argument("--span-in-play", type=float, default=wf.SPAN_IN_PLAY_MIN,
                    help="span: share of its presents that must be in play (default %.1f)" % wf.SPAN_IN_PLAY_MIN)
    ap.add_argument("--no-replace", action="store_true", help="span: do not re-place and re-roll a span that missed play")
    ap.add_argument("--replace-settle", type=float, default=40.0,
                    help="span re-placement: open the window this many seconds after the in-game start (default 40: over the 33 archived span rolls a window opened there is in play in all 33; at 20 s, 9 hit a pause-menu blip)")
    ap.add_argument("--from-save", default="", help="SAVE-STARTED gate: both rolls LOAD GAME the newest slot under this content root (perf_run.py --from-save; the root is only read)")
    ap.add_argument("--save-label", default="", help="--from-save: the step the save was taken at, for the record "
                    "(default: chapter N, slot S from the save)")
    ap.add_argument("--save-settings", action="store_true", help="--from-save: also seed the root's settings/")
    ap.add_argument("--tape", default="", help="--from-save: both rolls play this tape AS IS instead of the gate tape "
                    "made save-started (it carries its own SAVE_PRELUDE awaits and a `mark dump` for the span; p2.zpd's "
                    "tools/perf/zpd_play.txt, whose rolls must issue occlusion queries)")
    ap.add_argument("--grade", help="re-grade an existing --out directory")
    ap.add_argument("--reuse", action="store_true",
                    help="with --grade: grade the stored freeze_diff / flicker_diff outputs instead of re-running them")
    ap.add_argument("--repo", default=REPO,
                    help="the checkout the rolls run from (it must have the game disc beside it; a worktree does "
                         "not - default: this tool's checkout)")
    ap.add_argument("--stage", default="", help="both rolls stage the game's read set in this SSD folder "
                    "(perf_run --stage; tools/perf/stage_roll.py)")
    ap.add_argument("--stage-source", default="", help="--stage: the folder holding the disc to stage from")
    a = ap.parse_args()
    out = a.grade or a.out
    if not out:
        ap.error("--out (with --exe) or --grade")
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    strict = strict_gate(out)   # C2 2026-10-22: a strict gate grades no stored output, whatever --reuse says
    if strict and (a.grade or os.path.lexists(os.path.join(out, "flicker_gate.json"))):
        # lane C r33 (C2 2026-10-23, after Astra / conv r32): a strict folder is SINGLE-USE - its receipt is the record
        # of its ONE fresh run (the runner gives every strict gate a new folder). A --grade of a strict folder, or a run
        # into one that already holds a receipt, is refused BEFORE anything is written or rolled: a later invocation
        # never writes a strict verdict, so the folder's receipt stays its fresh run's whatever that invocation does
        print("strict : %s present - a strict folder is single-use (C2 2026-10-23): %s refused; nothing written, "
              "nothing rolled" % (STRICT_RULE, "--grade" if a.grade else "a run into a folder that already holds "
                                  "flicker_gate.json"))
        return 1
    if strict:
        # lane C r29c (Astra / conv r30 P1; C2 2026-10-23): a strict gate accepts NO grading option at all - any of
        # STRICT_LIMITS on the command line, even at its default value, is a strict_params_error FAIL, recorded before
        # anything rolls (a real run with --span-in-play 0 reached PASS). argparse fills an omitted option from the
        # parser-level default: a sentinel there tells an option given from one omitted.
        unset = object()
        ap.set_defaults(**{k: unset for k in STRICT_LIMITS})
        given = ap.parse_args()
        passed = ["--%s" % k.replace("_", "-") for k in STRICT_LIMITS if getattr(given, k) is not unset]
        if passed:
            why = ("a strict gate accepts no grading option (C2 2026-10-23): %s given - it grades by the canonical "
                   "limits only" % ", ".join(passed))
            print("strict : %s present - FAIL: %s" % (STRICT_RULE, why))
            rec = {"verdict": "FAIL", "strict_params_error": why, "regrade": bool(a.grade),
                   "strict": {"marker": STRICT_RULE}}
            # the common receipt writer; the folder holds no earlier receipt (single-use, above): a refusal that cannot
            # be recorded leaves no receipt - a strict FAIL as well
            write_error = publish_receipt(out, json.dumps(rec, indent=1))
            if write_error:
                print("strict : the refusal could not be recorded (%s) - no receipt, the gate FAILS" % write_error)
            return 1
        print("strict : %s present - no stored output is graded, both halves are recomputed by this code%s"
              % (STRICT_RULE, " (--reuse ignored)" if a.reuse else ""))
    meta = {"exe": a.exe, "gates": a.gates}
    attempts = []
    if a.grade:
        prev = os.path.join(out, "flicker_gate.json")
        keep = os.path.join(out, "flicker_gate.pre_world.json")
        old, prior_error, keep_error = {}, None, None
        if os.path.exists(prev):
            # a parse boundary (C2 2026-10-22): an unreadable prior JSON is recorded and nothing of it is kept
            old, prior_error = load_json_record(prev, "the prior flicker_gate.json")
            if old is not None and "world_rule" not in old and not os.path.exists(keep):
                try:   # evidence (Astra r5): the pre-world copy failing is recorded, the regrade goes on
                    shutil.copyfile(prev, keep)   # the verdict as graded before the world rule, kept beside the new one
                except Exception as e:
                    keep_error = err_text(e)
            old = old or {}
        exe0, gates0 = old.get("exe"), old.get("gates")
        meta = {"exe": a.exe or (exe0 if isinstance(exe0, str) else None),
                "gates": a.gates or (gates0 if isinstance(gates0, str) else "")}
        if prior_error:
            meta["prior_json_error"] = prior_error
        if keep_error:
            meta["pre_world_copy_error"] = keep_error
        ck = old.get("span_clock")
        if isinstance(ck, dict):   # a regrade keeps the roll's clock evidence (it cannot be re-taken)
            meta["span_clock"] = ck
        elif ck is not None:
            meta["span_clock_error"] = "the prior span_clock record is a %s, not an object - dropped" % type(ck).__name__
        at = old.get("span_attempts", [])
        attempts = at if isinstance(at, list) else []
        if not isinstance(at, list):
            meta["span_attempts_error"] = "the prior span_attempts record is a %s, not a list - dropped" % type(at).__name__
    else:
        if not a.exe:
            ap.error("--exe")
        base = BASE.format(inst=a.instance) + ("," + a.gates if a.gates else "")
        fs = os.path.abspath(a.from_save) if a.from_save else None
        if a.tape and not fs:
            raise SystemExit("--tape is for a save-started gate (--from-save): the new-game gate plays its own tape")
        tape = (os.path.abspath(a.tape) if a.tape else save_tape(out)) if fs else None
        meta["from_save"] = fs
        if a.tape:
            meta["tape"] = tape
        stage_kw = {}   # only when staging: an unstaged gate's rolls and JSON are exactly as before
        if a.stage:
            stage_kw = {"stage": os.path.abspath(a.stage),
                        "stage_source": os.path.abspath(a.stage_source) if a.stage_source else None}
            meta["stage"] = stage_kw
        if a.only != "freeze":
            span_dir = os.path.join(out, "span")
            frm = a.span_from
            meta["span_exit"] = roll(a.exe, span_dir, base + "," + SPAN.format(frm=frm, cap=a.span_cap), a.repo,
                                     from_save=fs, tape=tape, save_settings=a.save_settings, **stage_kw)
            try:   # orchestration's measurements are parse boundaries too (Astra e1cca492 r5 P1): a failure is recorded,
                #  the span is never re-placed on data that could not be read, and the grade FAILS the span
                n, tot = span_in_play(span_dir)
                mk = mark_present(span_dir)
                attempts.append({"from": ("mark p%d" % mk) if mk is not None else frm, "in_play": [n, tot]})
                if mk is not None and (not tot or n < a.span_in_play * tot):
                    # placed in the roll by the tape: missing play here is a finding, not a placement guess to retry
                    print("span   : in play %d of %d from the tape's dump mark (p%d) - not re-placed" % (n, tot, mk))
                if mk is None and not tot:
                    # no window at all: a mark-capable exe whose tape never reached `mark dump` (play_failed.txt says
                    # why), or a run that ended before the present number - a re-roll at a number cannot fix either
                    pf = play_failed(span_dir)
                    print("span   : no span window was dumped (no dump mark%s) - not re-placed" % (
                        ": " + pf.splitlines()[-1][:120] if pf else ""))
                if strict and mk is None and tot and n < a.span_in_play * tot:
                    # C2 2026-10-23 (after Astra / conv r24 - r26: every round found another hole in the strict
                    # first-span set-aside): a STRICT gate is never re-placed. Its span is graded as rolled - a span
                    # that is not in play is NO DATA (never a PASS), the reason recorded; the RUNNER may start ONE fresh
                    # strict gate, declared in its log before it starts (strict_fg exit 2). Non-strict / diagnostic
                    # gates keep the automatic re-placement below.
                    print("span   : in play %d of %d from p%d - a strict gate is not re-placed (its span is NO DATA; the "
                          "runner may declare one fresh gate)" % (n, tot, frm))
                    attempts[-1]["replace"] = "strict: not re-placed (C2 2026-10-23)"
                if not strict and mk is None and tot and n < a.span_in_play * tot and not a.no_replace:
                    frm2, why = replace_from(span_dir, a.replace_settle, a.span_cap)
                    print("span   : in play %d of %d from p%d - %s" % (n, tot, frm, (
                        "re-placing at p%d (%s)" % (frm2, why)) if frm2 else "cannot re-place: " + why))
                    attempts[-1]["replace"] = why
                    if frm2:
                        for suffix in ("", ".txt", "_flicker.txt", "_flicker_worst.jpg"):
                            if not os.path.exists(span_dir + suffix):
                                continue
                            if not suffix:   # the roll's own folder must move (the re-roll writes a fresh one there)
                                os.replace(span_dir, os.path.join(out, "span_try1"))
                                continue
                            try:   # lane C r9 (conv8 P2): the first try's console / stored output / image are evidence:
                                os.replace(span_dir + suffix, os.path.join(out, "span_try1" + suffix))
                            except Exception as e:   # a failed move is recorded, never an end to the re-placement
                                meta.setdefault("span_try1_move_errors", []).append(
                                    "span%s: %s" % (suffix, err_text(e)))
                        meta["span_exit"] = roll(a.exe, span_dir, base + "," + SPAN.format(frm=frm2, cap=a.span_cap),
                                                 a.repo, from_save=fs, tape=tape, save_settings=a.save_settings,
                                                 **stage_kw)
                        n2, tot2 = span_in_play(span_dir)
                        attempts.append({"from": frm2, "in_play": [n2, tot2]})
            except Exception as e:
                meta["span_orchestration_error"] = err_text(e)
                print("span   : the roll could not be measured (%s) - not re-placed" % err_text(e))
            try:   # Fable 32 (7): the graded span's clock, archived - evidence only, it never stops the grading
                meta["span_clock"] = write_span_clock(out, span_dir)
            except Exception as e:
                meta["span_clock"] = {"error": "the span clock could not be written (%s)" % err_text(e)}
        if a.only != "span":
            meta["freeze_exit"] = roll(a.exe, os.path.join(out, "freeze"), base + "," + FREEZE, a.repo,
                                       from_save=fs, tape=tape, save_settings=a.save_settings, **stage_kw)
    res = dict(meta)
    # C2 2026-10-23: a --grade regrade is never a strict verdict - the receipt says which this is (strict_fg FAILS true)
    res["regrade"] = bool(a.grade)
    if strict:
        res["strict"] = {"marker": STRICT_RULE, "stored_outputs": "ignored: both halves recomputed (C2 2026-10-22)"}
    res["start"] = start_record(out, a.save_label or None)
    if attempts:
        res["span_attempts"] = attempts
    res.update(grade(out, a.span_max, a.allow_maxdiff, a.allow_pixels, a.allow_spike, a.min_world, a.span_in_play,
                     a.only, reuse=bool(a.grade and a.reuse), strict=strict))
    if meta.get("span_orchestration_error") and "span" in res:   # the roll could not be measured: never a PASS
        res["span"]["verdict"] = "FAIL"
        res["span"]["line"] = res["span"].get("line", "") + " | ROLL NOT MEASURED: " + meta["span_orchestration_error"]
        res["verdict"] = "FAIL"
    for k in ("span", "freeze"):
        exit_code = res.get(k + "_exit", 0)
        if k in res and exit_code != 0:
            res[k]["verdict"] = "FAIL"
            res[k]["line"] = res[k].get("line", "") + " | ROLL EXIT %s" % exit_code
            res["verdict"] = "FAIL"
        why = play_failed(os.path.join(out, k)) if k in res else None
        if why:
            # the tape died (an await timed out, a hold_ctx never confirmed): the roll is not the one specified
            res[k]["play_failed"] = why
            res[k]["verdict"] = "FAIL"
            res[k]["line"] = res[k].get("line", "") + " | PLAY TAPE FAILED: " + why.splitlines()[-1][:160]
            res["verdict"] = "FAIL"
    sp_rec = res.get("span")
    if isinstance(sp_rec, dict) and sp_rec.get("verdict") == "FAIL" and "no_data_reason" in sp_rec:
        # a failure found after the grade (the roll's exit, its play tape, the roll not measured) outranks a strict span's
        # not-in-play NO DATA: the span FAILS, and its record no longer carries a NO DATA reason (strict_fg: a FAIL)
        sp_rec["no_data_overridden"] = sp_rec.pop("no_data_reason")
        sp_rec["line"] = sp_rec.get("line", "") + " -> FAIL (it outranks the not-in-play NO DATA)"
    try:   # the record is serialised first: a value it cannot hold never leaves a half-written JSON
        text = json.dumps(res, indent=1)
    except Exception as e:
        res["verdict"] = "FAIL"
        text = json.dumps({"verdict": "FAIL", "dump_error": err_text(e), "strict": res.get("strict")}, indent=1)
    # the receipt is written atomically (a temp file, then a rename); if it cannot be written, the computed result is
    # still printed and the gate FAILS, and no earlier flicker_gate.json may stand for this run (Astra 6fb621c r6)
    write_error = publish_receipt(out, text)
    if write_error:
        res["verdict"] = "FAIL"
    if "span" in res:
        print("span   : %s %s" % (res["span"]["verdict"], res["span"].get("line", "")))
    if "freeze" in res:
        fz = res["freeze"]
        if fz.get("roll_health", {}).get("status") not in (None, "PASS") or fz.get("freeze_error"):
            print("freeze : " + fz.get("line", ""))   # r12 (conv11 P3): a guarded parse failure names its cause here too
        print("freeze : %s counted=%s bad=%s %s%s%s (allow-maxdiff %d; freezes before the ingame era excluded)"
              % (fz["verdict"], fz.get("counted"), fz.get("bad"), fz.get("world_line", ""),
                 (" -> " + fz["world_fail"]) if fz.get("world_fail") else "",
                 (" -> PLAY TAPE FAILED: " + fz["play_failed"].splitlines()[-1][:160]) if fz.get("play_failed") else "",
                 a.allow_maxdiff))
    print("start  : %s%s" % (res["start"]["label"], (" (run_class %s)" % res["start"]["run_class"])
                             if res["start"].get("run_class") else ""))
    if write_error:
        print("receipt: flicker_gate.json NOT WRITTEN (%s) - no receipt, the gate FAILS" % write_error)
    print("FLICKER_GATE %s" % res["verdict"])
    return 0 if res["verdict"] == "PASS" else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as exc:   # the last line of defence: one named FAIL line (where: the innermost frame), exit 1
        import traceback
        tb = traceback.extract_tb(exc.__traceback__)
        print("FLICKER_GATE FAIL: the gate could not complete (%s, at %s)" % (
            err_text(exc), ("%s:%d" % (os.path.basename(tb[-1].filename), tb[-1].lineno)) if tb else "?"))
        sys.exit(1)
