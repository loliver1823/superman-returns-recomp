#!/usr/bin/env python3
"""boot_attach_eval.py - grade render/boot-attach's live boot pairs (docs/NATIVE_ROADMAP.md "Boot attach").

    python tools/perf/boot_attach_eval.py [--warmup <bat_warmup>] --pair on:<bat_on1> off:<bat_off1> \
                                          --pair off:<bat_off2> on:<bat_on2> --pair on:<bat_on3> off:<bat_off3> \
                                          [--budget=1.0] [--out <txt>] [--json <json>]
    python -m unittest discover -s tools/perf -p test_boot_attach_eval.py      # the rule's tests

ON = the default build (SR_SHADER_LOAD_ASYNC unset or 1: the shader cache loads on its own thread); OFF =
SR_SHADER_LOAD_ASYNC=0 (the old order: the whole load inside the backend init, before the attach). Each --pair names
its two rolls IN THE ORDER THEY WERE DECLARED TO RUN (arm:path; a perf_run roll folder with
drive/attempt1/{run.log, steps.log, provenance.json, outcome.json}); native_proof.py --plan boot_attach runs them in
that order. The tool checks the order against the logs.

What three pairs can and cannot say: a regression in these pairs (OPEN), or none seen ("NO REGRESSION OBSERVED IN
THESE PAIRS"), with the measured bounds of the ON rolls. It never says faster, resolved, equivalent, or that the attach
"did not wait for the disk": the probe (a directory listing) and the loader thread's start stay on init's path, a
thread-creation failure runs the load there, and a short CP wait does not bound the attach. The bounds are what the
logs measure, on one clock (ms since the process was created): the attach, the backend init's window, the probe's
duration and the loader thread's start.

THE RULE (registered with the plan before any roll; the design, marker, OPEN and budget rules are tools/perf/
boot_pairs.py's on native/devcreate-boot, Astra rounds 1-2, ported - not imported):
  Design (else INCONCLUSIVE - the set is not evidence): exactly three pairs, declared ON/OFF, OFF/ON, ON/OFF (the
    registered alternation; bat1 and bat2) or its mirror OFF/ON, ON/OFF, OFF/ON (the boot_attach plan's order from
    2026-10-14: every set before it put an ON roll in the first graded slot, and bat2 measured a first-launch-per-
    invocation excess, so position and arm were confounded - the mirror, after an in-invocation warm-up, separates
    them); distinct paths; one exe hash; identical effective SR_* configuration except
    SR_SHADER_LOAD_ASYNC and the path-valued keys; the same save seeding, parsed for EVERY roll from its steps.log (a
    roll without the line is named, never matched as None); each roll's arm engaged - ON: the flag unset / 1 and the
    "boot load on its own thread" line; OFF: the flag 0, the "SR_SHADER_LOAD_ASYNC=0" line and the attach after the
    load's end (the old order, else the OFF arm is not the baseline); each pair's start times in the declared order
    and the six rolls in sequence.
  Markers (else INCONCLUSIVE, the missing evidence named): provenance.json, run.log, the backend init line with its
    process window, the CP thread up, the attach with its process time and the cache's state read just after it, the
    CP start (WP1) line with its "ready after WP1" time, the [GPU_CP] first-command line, the installed and translator
    stage lines with their ends, the loader's start line, and ON the probe's duration; the CP's backend-wait status.
    Every RECORDED wait (0.0 ms included) must lie within 1.0 s of [CP up, attach]; no wait line counts as 0 only when
    the wait was enabled and the attach came before the CP started. A log that contradicts the build's sampling order
    (the attach read the cache loaded before stage 1's end; the first command before WP1's readiness) is named.
  OPEN, never discarded or replaced (an OPEN verdict also names any missing evidence): a crash or timeout (outcome not
    SUCCESS, or outcome.json missing), no HUD request, a [FATAL] line, a backend wait that did not end "attached"
    (INIT FAILED; TIMED OUT in logs of the earlier 60 s limit), an attach after guest VdSwaps or with draws queued (the
    black-world race), a first command consumed without the backend, a CP that started with no installed cache, and
    an ON roll whose load ran on init's thread (no thread could be created) or on the main thread (the boot-load
    lines name both): the decoupling was not in effect.
  Endpoints, paired ON - OFF per pair: time to the HUD request (PRIMARY; from the log's first line), the attach and the
    CP's first consumed command (process-relative). NO REGRESSION OBSERVED IN THESE PAIRS when every paired difference
    of every endpoint is <= the budget (fixed before execution: --budget, default 1.0 s; a non-finite or negative
    budget is rejected); OPEN otherwise, naming what exceeded (comparisons fail closed: a nan difference is over).
  Reported, not graded: the ON bounds (max attach, init end, probe, loader-thread start, CP ready after WP1, CP first
    command; process ms) and in how many ON rolls the installed cache was still loading at the attach - read from the
    attach line's own cache state, sampled just after the attach (stages only advance), never from a timestamp
    comparison (decoupling witnessed in k of 3; a fast load witnesses nothing either way); per roll the CP's backend
    wait, its still-waiting warnings and any [HOST_WAIT_STALL] escalations, the WP1 wait, the stage durations.
  THE DECLARED WARM-UP (native/devcreate-close, 2026-10-14; bat1's pair 1 was the session's first roll): --warmup
    <roll> names one roll declared before the set and run before pair 1 (native_proof.py --plan boot_attach, step
    'warmup': the default build, the ON arm's configuration). It is NEVER GRADED: no endpoint, OPEN class, budget or
    bound reads it; its numbers are printed as information on their own line. When it is named, the set is
    INCONCLUSIVE unless the warm-up was run, is not also a graded roll, ran before pair 1's first roll, on the set's
    exe with the ON arm's configuration (SR_SHADER_LOAD_ASYNC unset or 1) and the same save seeding, reached the
    HUD (it warmed the whole boot path; a warm-up that crashed or stopped short is named, never an OPEN of the set),
    its log ENDED before the first graded roll's log began (Astra round 1 on native/devcreate-close, P2-7: a start-only
    check let a warm-up still booting at the first graded launch through), its run row is DONE, and it and every
    graded roll that ran are roll rows of ONE native_proof run (tools/perf/proof_rows.py: the *_proof.json beside them
    whose `out` is that folder, rows matched by CANONICAL path - never basename - the run's exe and start time
    matching every roll; P1-2): bat2 found the first launch of EACH invocation ~1 s late to its first CP command
    (+1385 / +2092 / +1950 ms against +1017..+1143), so a warm-up in its own invocation (bat2w) absorbs nothing.
    Without --warmup the rule is as before (bat1's form) plus the failed-roll rule below.
  FAILED ROLLS (P1-2, with or without --warmup): a graded roll whose row in that run is not DONE (ABORT / FAIL), or
    whose log holds an abort marker anywhere (THREAD_FATAL, ICALL_FAIL, a CP stall, a GPU hang trigger, [FATAL]:
    boot_pairs.ABORT_MARKERS), is OPEN whatever its outcome.json says.
  POSITION (information, never graded): the first graded roll's CP first command and time to the HUD beside the
    median of the other graded rolls of its arm, and the warm-up's.
Read-only; no game runs. Exit 0 with a verdict; 2 for bad arguments.
"""
import argparse
import json
import math
import os
import re
import sys
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boot_pairs as bp  # noqa: E402  (abort_marker only: the run log's failure markers, one list for every tool)
import proof_rows as prows  # noqa: E402  (which run took the rolls: canonical rows, the run's identity)

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
STEP = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] saves: SR_CONTENT_DIR=\S+ \((.*)\)")
P = r"\+(-?[0-9.]+)"  # a process-relative ms field (-1 = the clock was not readable)
MARKS = {
    "begin_on": re.compile(r"\[SHADER_CACHE\] boot load on its own thread \(SR_SHADER_LOAD_ASYNC=1, default 1\): "
                           r"the probe of .* says (an installed cache|NO installed cache) \(probe ([0-9.]+) ms, from "
                           r"process " + P + r" ms\); the load (runs on the loader thread|RAN ON THIS THREAD)"),
    "begin_off": re.compile(r"\[SHADER_CACHE\] SR_SHADER_LOAD_ASYNC=0: the boot load runs inside"),
    # the dev merge: dcb's per-load lines and render/boot-attach's stage fields are one line per stage
    "loader": re.compile(r"\[SHADER_CACHE\] boot load: started at process " + P + r" ms on thread (\d+) \(main thread "
                         r"(\d+)\), named '([^']*)'"),
    "s1": re.compile(r"\[SHADER_CACHE\] boot load: installed stage ([0-9.]+) ms \(.*\), ended at process " + P + " ms"),
    "s2": re.compile(r"\[SHADER_CACHE\] boot load done: ([0-9.]+) ms on thread \d+ \(main thread \d+\): installed cache "
                     r"[0-9.]+ ms[^,]*, runtime translator ([0-9.]+) ms .*; ended at process " + P + " ms"),
    "init": re.compile(r"\[DISPLAY\] backend init ([0-9.]+) ms \(.*process " + P + " to " + P + r" ms\)"),
    "cpup": re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] command processor thread up"),
    "attach": re.compile(r"\[DISPLAY\] PM4 live backend attached: guest VdSwaps before it (\d+), draw commands queued "
                         r"(\d+); process " + P + r" ms; installed shader cache just after the attach: "
                         r"(loaded|still loading)"),
    "waited": re.compile(r"\[DISPLAY\] CP waited ([0-9.]+) ms for the live backend \(([^)]*)\)"
                         r"(?:, (\d+) still-waiting warnings)?"),
    "cpstart": re.compile(r"\[SHADER_CACHE\] CP start: waited ([0-9.]+) ms for the installed shader cache \(([a-z ]+); "
                          r"translator stage ([a-z ]+)\), ready after WP1 at process " + P + " ms"),
    "cpfirst": re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] first command consumed at process " + P + r" ms \(.*the live backend "
                          r"(attached|NOT attached)\)"),
    "hoststall": re.compile(r"\[HOST_WAIT_STALL\] ==== the CP has waited"),
    "hud": re.compile(r"\[APT_REQ\] #\d+ path='screens/hud/main_hud'"),
    "fatal": re.compile(r"\[FATAL\]"),
}
# Path-valued keys differ per roll by construction; SR_SHADER_LOAD_ASYNC is the arm.
VOLATILE = ("SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR", "SR_SHADER_LOAD_ASYNC")
DESIGN = [["on", "off"], ["off", "on"], ["on", "off"]]
MIRROR = [["off", "on"], ["on", "off"], ["off", "on"]]  # the plan's order since 2026-10-14 (the position effect)
WAIT_SLACK = 1.0
ORDER_SLACK = 0.005  # the CP-up line and its attached() check are microseconds apart; the log has ms
ENDPOINTS = (("hud", "time to HUD (primary)"), ("attach_s", "attach (process)"), ("cpfirst_s", "CP first command (process)"))
LINE_NAMES = {"init": "backend init (with its process window)", "cpup": "CP thread up",
              "attach": "attach (with its process time and the cache's state just after it)",
              "cpstart": "CP start (WP1, ready after WP1)", "cpfirst": "CP first command consumed",
              "s1": "installed stage end", "s2": "translator stage end", "loader": "loader start"}


def read_json(p):
    try:
        with open(p, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def pms(s):
    """A process-ms field: a float, or None when the game could not read its clock (-1)."""
    v = float(s)
    return v if v >= 0 else None


def scan(f, r):
    t0 = None
    for line in f:
        m = TS.match(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
        if t0 is None:
            t0 = t
            r["start"] = t
        for k, rx in MARKS.items():
            if k in r:
                continue
            mm = rx.search(line)
            if not mm:
                continue
            r[k] = (t - t0).total_seconds()
            g = mm.groups()
            if k == "begin_on":
                r["probe_says"], r["probe_ms"], r["probe_at"], r["load_where"] = g[0], float(g[1]), pms(g[2]), g[3]
            elif k == "loader":
                r["loader_at"], r["loader_tid"], r["main_tid"], r["loader_thread"] = pms(g[0]), g[1], g[2], g[3]
            elif k == "s1":
                r["s1_ms"], r["s1_end"] = float(g[0]), pms(g[1])
            elif k == "s2":
                r["load_ms"], r["s2_ms"], r["s2_end"] = float(g[0]), float(g[1]), pms(g[2])
            elif k == "init":
                r["init_ms"], r["init_from"], r["init_to"] = float(g[0]), pms(g[1]), pms(g[2])
            elif k == "attach":
                r["swaps_before"], r["queued_before"], r["attach_at"] = int(g[0]), int(g[1]), pms(g[2])
                r["attach_cache"] = g[3]
            elif k == "waited":
                r["W"], r["waitst"] = float(g[0]) / 1000.0, g[1]
                r["W_line"] = True  # a recorded wait, 0.0 included, is checked against the timestamps
                r["warns"] = int(g[2]) if g[2] is not None else None
            elif k == "cpstart":
                r["wp1_ms"], r["wp1_state"], r["wp1_s2"], r["cpready_at"] = float(g[0]), g[1], g[2], pms(g[3])
            elif k == "cpfirst":
                r["cpfirst_at"], r["cpfirst_backend"] = pms(g[0]), g[1]
        if "hoststall" in r:  # count every [HOST_WAIT_STALL] escalation, not only the first
            r["hoststalls"] = r.get("hoststalls", 0) + (1 if MARKS["hoststall"].search(line) else 0)


def roll(arm, path):
    att = os.path.join(path, "drive", "attempt1")
    r = {"arm": arm, "roll": path, "problems": [], "open": []}
    prov = read_json(os.path.join(att, "provenance.json")) or {}
    outc = read_json(os.path.join(att, "outcome.json")) or {}
    r["exe"] = (prov.get("exeSha256") or "").lower()
    r["env"] = prov.get("effectiveSrEnv") or {}
    r["result"] = outc.get("result")
    if not prov:
        r["problems"].append("no provenance.json (exe / SR_* configuration unknown)")
    try:
        with open(os.path.join(att, "steps.log"), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = STEP.match(line)
                if m:
                    r["seed"] = m.group(2)
                    break
    except OSError:
        pass  # no seed parsed: the design check names the roll
    if r["result"] != "SUCCESS":
        r["open"].append("outcome %s" % (r["result"] or "missing (no outcome.json)"))
    try:
        f = open(os.path.join(att, "run.log"), encoding="utf-8", errors="replace")
    except OSError:
        r["problems"].append("no run.log (%s)" % os.path.join(att, "run.log"))
        r["gate"] = "unverified (no run.log)"
        return r
    with f:
        scan(f, r)
    grade(r)
    return r


def grade(r):
    """r['problems'] (INCONCLUSIVE), r['open'] (OPEN) and r['gate'] for one scanned roll."""
    arm, env = r["arm"], r["env"]
    # OPEN
    if "hud" not in r:
        r["open"].append("no HUD request")
    if "fatal" in r:
        r["open"].append("a [FATAL] line")
    if r.get("waitst") and r["waitst"] != "attached":
        r["open"].append("backend wait %s" % r["waitst"])
    if r.get("swaps_before") or r.get("queued_before"):
        r["open"].append("attach after %d guest VdSwaps / %d queued draws (the black-world race)" % (
            r.get("swaps_before", 0), r.get("queued_before", 0)))
    if r.get("wp1_state") and r["wp1_state"] != "loaded":
        r["open"].append("the CP started with the installed cache %s" % r["wp1_state"])
    if arm == "on" and r.get("load_where") == "RAN ON THIS THREAD":
        r["open"].append("ON: the load ran on init's thread (no thread could be created)")
    elif arm == "on" and r.get("loader_tid") and r.get("loader_tid") == r.get("main_tid"):
        r["open"].append("ON: the load ran on the main thread %s" % r["loader_tid"])
    if r.get("cpfirst_backend") == "NOT attached":
        r["open"].append("the CP consumed its first command without the live backend (the black-world path)")
    # markers
    for k in ("init", "cpup", "attach", "cpstart", "cpfirst", "s1", "s2", "loader"):
        if k not in r:
            r["problems"].append("no %s line" % LINE_NAMES[k])
    for k, name in (("init_to", "backend init end"), ("attach_at", "attach"), ("cpready_at", "CP ready after WP1"),
                    ("cpfirst_at", "CP first command"), ("s1_end", "installed stage end"),
                    ("s2_end", "translator stage end")):
        if k in r and r[k] is None:
            r["problems"].append("the %s's process time was not readable (-1)" % name)
    if arm == "on":
        if "begin_on" in r and r.get("probe_ms") is None:
            r["problems"].append("no probe duration")
        if "loader" in r and r.get("loader_at") is None:
            r["problems"].append("the loader start's process time was not readable (-1)")
    # the CP's backend wait (boot_pairs' rule)
    wait_on = (env.get("SR_PM4_WAIT_BACKEND", "1") or "1")[0] != "0"
    if "waited" not in r and "attach" in r and "cpup" in r:
        if wait_on and r["attach"] <= r["cpup"] + ORDER_SLACK:
            r["W"] = 0.0
        else:
            r["problems"].append("no backend-wait line though the attach (%.3f s) followed the CP's start (%.3f s)%s" % (
                r["attach"], r["cpup"], "" if wait_on else " - waiting disabled"))
    # Astra round 2: every RECORDED wait - 0.0 ms included - must match [CP up, attach]; only the no-line case above
    # is a 0 s wait by rule.
    if r.get("W_line") and r.get("W") is not None and r.get("waitst") == "attached" and "cpup" in r and "attach" in r:
        span = r["attach"] - r["cpup"]
        if not abs(span - r["W"]) <= WAIT_SLACK:
            r["problems"].append("recorded backend wait %.2f s vs [CP up, attach] %.2f s" % (r["W"], span))
    # Astra round 2: the stage-1 end is sampled before its publication and the attach line's cache state after the
    # attach, so "loaded" there with the attach before the stage-1 end cannot happen in a log of this build.
    if (r.get("attach_cache") == "loaded" and r.get("attach_at") is not None and r.get("s1_end") is not None and
            not r["s1_end"] <= r["attach_at"]):
        r["problems"].append("the attach read the cache loaded at +%.1f ms but stage 1 ended at +%.1f ms "
                             "(inconsistent log)" % (r["attach_at"], r["s1_end"]))
    if (r.get("cpready_at") is not None and r.get("cpfirst_at") is not None and
            not r["cpready_at"] <= r["cpfirst_at"]):
        r["problems"].append("the CP's first command (+%.1f ms) precedes its WP1 readiness (+%.1f ms)" % (
            r["cpfirst_at"], r["cpready_at"]))
    # endpoints (seconds)
    if r.get("attach_at") is not None:
        r["attach_s"] = r["attach_at"] / 1000.0
    if r.get("cpfirst_at") is not None:
        r["cpfirst_s"] = r["cpfirst_at"] / 1000.0
    # the arm's engagement
    flag = env.get("SR_SHADER_LOAD_ASYNC")
    flag_on = flag is None or (flag or "1")[0] != "0"
    if arm == "on":
        ok = flag_on and "begin_on" in r and "begin_off" not in r
        r["gate"] = "on" if ok else "unverified (SR_SHADER_LOAD_ASYNC=%s, ON line %s, OFF line %s)" % (
            flag, "begin_on" in r, "begin_off" in r)
        if ok and r.get("attach_cache"):
            # the cache's own state read just after the attach (stages only advance): never a timestamp comparison
            r["witness"] = r["attach_cache"] == "still loading"
    else:
        old_order = (r.get("attach_at") is not None and r.get("s2_end") is not None and r["attach_at"] >= r["s2_end"])
        ok = (not flag_on) and "begin_off" in r and "begin_on" not in r and old_order
        r["gate"] = "off" if ok else ("unverified (SR_SHADER_LOAD_ASYNC=%s, OFF line %s, ON line %s, attach after the "
                                      "load's end %s)" % (flag, "begin_off" in r, "begin_on" in r, old_order))


def parse_pair(tokens):
    out = []
    for tok in tokens:
        arm, _, path = tok.partition(":")
        if arm.lower() not in ("on", "off") or not path:
            raise SystemExit("bad pair token %r (want on:<roll> or off:<roll>)" % tok)
        out.append((arm.lower(), path))
    return out


def budget_arg(s):
    """--budget: a finite number of seconds >= 0 (nan compares false and a negative budget is no budget - either
    would let a regression through)."""
    try:
        v = float(s)
    except ValueError:
        raise argparse.ArgumentTypeError("budget %r is not a number" % s)
    if not math.isfinite(v) or v < 0:
        raise argparse.ArgumentTypeError("budget %r must be a finite number of seconds >= 0" % s)
    return v


def fmt(x, w=7, p=1):
    return ("%*.*f" % (w, p, x)) if isinstance(x, (int, float)) and not isinstance(x, bool) else ("%*s" % (w, "-"))


def run_of(warmup, flat):
    """proof_rows.find_run over the warm-up (when declared) and every graded roll: the ONE native_proof run that took
    them, its rows matched by canonical path - never by basename (Astra round 1 on native/devcreate-close, P1-2)."""
    return prows.find_run(([warmup] if warmup else []) + [r["roll"] for r in flat])


def warmup_check(warmup, rolls, flat, norm, run):
    """(the warm-up's roll dict or None, design problems) for a declared warm-up - never graded."""
    if not warmup:
        return None, []
    probs = []
    if os.path.normcase(os.path.abspath(warmup)) in rolls:
        return None, ["the warm-up %s is also a graded roll (the warm-up is never graded)" % warmup]
    if not os.path.isdir(warmup):
        return None, ["the warm-up %s was not run" % warmup]
    w = roll("on", warmup)
    what, line = bp.abort_marker(warmup)
    if w.get("result") != "SUCCESS" or "hud" not in w or what:
        probs.append("the warm-up did not complete to the HUD (%s): the boot path was not warmed" % (
            ", ".join(w["open"] + (["%s in the log (%s)" % (what, line)] if what else [])) or "no HUD request"))
    if run["proof"] and prows.row_verdict(run, warmup) not in (None, "DONE"):
        probs.append("the warm-up's run row says %s" % prows.row_verdict(run, warmup))
    flag = w["env"].get("SR_SHADER_LOAD_ASYNC")
    if not (flag is None or (flag or "1")[0] != "0"):
        probs.append("the warm-up ran SR_SHADER_LOAD_ASYNC=%s (it is declared as the default build, the ON arm)" % flag)
    # Astra P2-7: the warm-up must have ENDED (its log's last line) before the first graded launch began - a start-only
    # check let a warm-up still booting at the first graded launch through
    _wfirst, wlast = prows.log_span(warmup)
    firsts = [f for f in (prows.log_span(r["roll"])[0] for r in flat if os.path.isdir(r["roll"])) if f is not None]
    if wlast is None:
        probs.append("the warm-up's end is unknown (no timestamped run.log line)")
    elif firsts and not wlast < min(firsts):
        probs.append("the warm-up had not ended (its log runs to %s) before the first graded launch (%s)" % (
            wlast.isoformat(sep=" "), min(firsts).isoformat(sep=" ")))
    if set(r["exe"] for r in flat) != {w["exe"]}:
        probs.append("the warm-up's exe %s is not the set's" % (w["exe"][:12] or "unknown"))
    if any(json.dumps(norm(w["env"]), sort_keys=True) != json.dumps(norm(r["env"]), sort_keys=True) for r in flat):
        probs.append("the warm-up's effective SR_* configuration differs from the set's")
    if w.get("seed") is None or any(r.get("seed") != w.get("seed") for r in flat):
        probs.append("the warm-up's save seeding is not the set's (%s)" % w.get("seed"))
    if run["problems"] or not run["proof"]:
        probs.append("the warm-up and the set are not rows of one native_proof run (%s): a warm-up absorbs the "
                     "invocation's first launch only inside that invocation (bat2)" % "; ".join(run["problems"]))
    return w, probs


def position_line(pairs, R, W):
    """Information, never graded (bat2, 2026-10-14: the first roll of each native_proof invocation consumed its first CP
    command ~1 s late - +1385 / +2092 / +1950 ms against +1017..+1143 in the ten later rolls): the first graded roll's
    CP first command and time to the HUD against the median of the other graded rolls of ITS arm, and the warm-up's."""
    if not pairs or not R or not R[0]:
        return ""
    arm, first = pairs[0][0][0], R[0][0]
    rest = [r for pr, rr in zip(pairs, R) for (a, _), r in zip(pr, rr) if a == arm and r is not first]

    def med(k):
        v = sorted(x for x in (r.get(k) for r in rest) if isinstance(x, (int, float)) and not isinstance(x, bool))
        return None if not v else v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2.0
    return ("position (information, never graded): the first graded roll (pair 1 %s) CP first command +%s ms, HUD %s s; "
            "the other %s rolls' median +%s ms, %s s%s" % (
                arm.upper(), fmt(first.get("cpfirst_at"), 0, 0), fmt(first.get("hud"), 0, 2), arm.upper(),
                fmt(med("cpfirst_at"), 0, 0), fmt(med("hud"), 0, 2),
                "; the warm-up (the invocation's first launch) +%s ms, %s s" % (
                    fmt(W.get("cpfirst_at"), 0, 0), fmt(W.get("hud"), 0, 2)) if W is not None else ""))


def evaluate(pairs, budget, warmup=None):
    """pairs: [[(arm, path), (arm, path)], ...] in declared run order; warmup: the declared warm-up roll (never
    graded) or None. Returns (lines, verdict, dump)."""
    design = []
    rolls = {}
    declared = [[arm for arm, _ in pr] for pr in pairs]
    if declared not in (DESIGN, MIRROR):
        design.append("the registered design is three pairs declared ON/OFF, OFF/ON, ON/OFF or the mirror OFF/ON, "
                      "ON/OFF, OFF/ON (got %s)" % (", ".join("/".join(a.upper() for a in d) for d in declared)))
    for i, pr in enumerate(pairs):
        if sorted(arm for arm, _ in pr) != ["off", "on"]:
            design.append("pair %d is not one ON and one OFF" % (i + 1))
        for arm, path in pr:
            key = os.path.normcase(os.path.abspath(path))
            if key in rolls:
                design.append("path used twice: %s" % path)
            rolls[key] = roll(arm, path)
    R = [[rolls[os.path.normcase(os.path.abspath(p))] for _, p in pr] for pr in pairs]
    # a path used twice maps both slots to one roll dict: grade it under its last arm only (the design names it)
    flat = [r for pr in R for r in pr]
    exes = set(r["exe"] for r in flat)
    if len(exes) != 1 or "" in exes:
        design.append("exe hashes %s (want exactly one)" % sorted(x[:12] for x in exes))
    norm = lambda e: {k: v for k, v in e.items() if k not in VOLATILE}
    if len(set(json.dumps(norm(r["env"]), sort_keys=True) for r in flat)) != 1:
        keys = sorted(set(k for r in flat for k in norm(r["env"])
                          if len(set(json.dumps(norm(x["env"]).get(k)) for x in flat)) != 1))
        design.append("the effective SR_* configuration differs beyond SR_SHADER_LOAD_ASYNC (%s)" % ", ".join(keys))
    unseeded = [r["roll"] for r in flat if r.get("seed") is None]
    if unseeded:
        design.append("save seeding not recorded for %s" % ", ".join(unseeded))
    elif len(set(r["seed"] for r in flat)) != 1:
        design.append("save seeding differs (%s)" % sorted(set(r["seed"] for r in flat)))
    for i, (pr, rr) in enumerate(zip(pairs, R)):
        for (arm, _), r in zip(pr, rr):
            if r.get("gate") != arm:
                design.append("pair %d %s roll's arm: %s" % (i + 1, arm.upper(), r.get("gate", "unknown")))
        if all("start" in r for r in rr) and not rr[0]["start"] < rr[1]["start"]:
            design.append("pair %d did not run in its declared order" % (i + 1))
    starts = [r.get("start") for r in flat]
    if all(starts) and any(starts[k] >= starts[k + 1] for k in range(len(starts) - 1)):
        design.append("the rolls did not run in the declared sequence")
    run = run_of(warmup, flat)
    W, wprobs = warmup_check(warmup, rolls, flat, norm, run)  # never graded: only the design reads it
    design += wprobs
    # Astra P1-2: a graded roll's run row that is not DONE (ABORT / FAIL), or an abort marker anywhere in its log
    # (THREAD_FATAL, ICALL_FAIL, a CP stall, a GPU hang trigger), is a failed roll whatever its outcome.json says
    for r in flat:
        v = prows.row_verdict(run, r["roll"]) if run["proof"] else None
        if v not in (None, "DONE"):
            row = run["rows"][prows.key(r["roll"])]
            r["open"].append("the run's row says %s (%s)" % (v, str(row.get("detail", ""))[:100]))
        what, line = bp.abort_marker(r["roll"])
        if what and not (what == "fatal" and "fatal" in r):  # a [FATAL] the scan read is its own OPEN class already
            r["open"].append("%s in the log (%s)" % (what, line))

    lines = ["pair arm    W(s) wait-st  warns stalls  init-ms init-end  probe  loader   s1-end   s2-end   attach"
             "  cp-ready  cp-first      HUD  witness  roll"]
    if W is not None:
        lines.append("   0 WUP %s %-8s %5s %6s %s %s %s %s %s %s %s %s %s %s %8s  %s  (warm-up: never graded)" % (
            fmt(W.get("W"), 7, 2), W.get("waitst", "-"), "-" if W.get("warns") is None else W["warns"],
            W.get("hoststalls", 0), fmt(W.get("init_ms"), 8), fmt(W.get("init_to"), 8), fmt(W.get("probe_ms"), 6),
            fmt(W.get("loader_at"), 7), fmt(W.get("s1_end"), 8), fmt(W.get("s2_end"), 8), fmt(W.get("attach_at"), 8),
            fmt(W.get("cpready_at"), 9), fmt(W.get("cpfirst_at"), 9), fmt(W.get("hud"), 8, 2), "-", W["roll"]))
    for i, (pr, rr) in enumerate(zip(pairs, R)):
        for (arm, _), r in zip(pr, rr):
            lines.append("%4d %-3s %s %-8s %5s %6s %s %s %s %s %s %s %s %s %s %s %8s  %s" % (
                i + 1, arm.upper(), fmt(r.get("W"), 7, 2), r.get("waitst", "-"),
                "-" if r.get("warns") is None else r["warns"], r.get("hoststalls", 0), fmt(r.get("init_ms"), 8),
                fmt(r.get("init_to"), 8), fmt(r.get("probe_ms"), 6), fmt(r.get("loader_at"), 7),
                fmt(r.get("s1_end"), 8), fmt(r.get("s2_end"), 8), fmt(r.get("attach_at"), 8),
                fmt(r.get("cpready_at"), 9), fmt(r.get("cpfirst_at"), 9), fmt(r.get("hud"), 8, 2),
                {True: "yes", False: "no"}.get(r.get("witness"), "-"), r["roll"]))
    opens = ["%s: %s" % (r["roll"], ", ".join(r["open"])) for r in flat if r["open"]]
    probs = ["%s: %s" % (r["roll"], ", ".join(r["problems"])) for r in flat if r["problems"]]
    diffs = {}
    head = "BOOT_ATTACH pairs=%d budget=%.1f" % (len(pairs), budget)
    if opens:
        verdict = head + " -> OPEN (kept, never discarded): " + "; ".join(opens) + (
            " | also missing / inconsistent evidence: " + "; ".join(design + probs) if design or probs else "")
    elif design or probs:
        verdict = head + " -> INCONCLUSIVE: " + "; ".join(design + probs)
    else:
        over = []
        for key, name in ENDPOINTS:
            d = []
            for rr, pr in zip(R, pairs):
                arms = [arm for arm, _ in pr]
                d.append(rr[arms.index("on")][key] - rr[arms.index("off")][key])
            diffs[key] = d
            lines.append("paired ON - OFF %-28s %s" % (name, " ".join("%+.2f" % x for x in d)))
            if any(not (x <= budget) for x in d):  # fail closed: a nan difference is over
                over.append("%s %s" % (name, " ".join("%+.2f" % x for x in d)))
        on = [r for pr, rr in zip(pairs, R) for (arm, _), r in zip(pr, rr) if arm == "on"]
        witnessed = sum(1 for r in on if r.get("witness"))
        bounds = ("ON bounds (ms since process start): attach <= %.0f, backend init ended <= %.0f, probe <= %.1f ms, "
                  "loader thread started <= %.0f, CP ready after WP1 <= %.0f, CP first command <= %.0f; the installed "
                  "cache still loading at the attach (read just after it) in %d of %d ON rolls; host-wait stall "
                  "escalations ON %d, OFF %d" % (
                      max(r["attach_at"] for r in on), max(r["init_to"] for r in on), max(r["probe_ms"] for r in on),
                      max(r["loader_at"] for r in on), max(r["cpready_at"] for r in on),
                      max(r["cpfirst_at"] for r in on), witnessed, len(on), sum(r.get("hoststalls", 0) for r in on),
                      sum(r.get("hoststalls", 0) for r in flat if r["arm"] == "off")))
        lines.append(bounds)
        head += " max_dHUD=%+.2f max_dAttach=%+.2f max_dCpFirst=%+.2f" % (
            max(diffs["hud"]), max(diffs["attach_s"]), max(diffs["cpfirst_s"]))
        verdict = head + "; " + bounds + (" -> OPEN: " + "; ".join(over) if over else
                                           " -> NO REGRESSION OBSERVED IN THESE PAIRS")
    pos = position_line(pairs, R, W)
    if pos:
        lines.append(pos)
    if W is not None:  # information only: the warm-up is never graded
        lines.append("warm-up (never graded): %s HUD %s s, attach +%s ms, CP ready after WP1 +%s ms, CP first "
                     "command +%s ms, backend wait %s s" % (
                         W["roll"], fmt(W.get("hud"), 0, 2), fmt(W.get("attach_at"), 0, 0), fmt(W.get("cpready_at"), 0, 0),
                         fmt(W.get("cpfirst_at"), 0, 0), fmt(W.get("W"), 0, 2)))
    lines.append(verdict)
    clean = lambda r: {k: (v.isoformat() if isinstance(v, datetime) else v) for k, v in r.items() if k != "env"}
    dump = {"pairs": [[clean(r) for r in rr] for rr in R], "design": design, "diffs": diffs, "verdict": verdict,
            "budget": budget, "warmup": clean(W) if W is not None else None}
    return lines, verdict, dump


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pair", nargs=2, action="append", required=True, metavar=("ARM:ROLL", "ARM:ROLL"))
    ap.add_argument("--budget", type=budget_arg, default=1.0,
                    help="the per-endpoint regression budget in seconds (finite, >= 0), fixed before running")
    ap.add_argument("--warmup", help="the declared warm-up roll (run before pair 1, never graded)")
    ap.add_argument("--out")
    ap.add_argument("--json")
    a = ap.parse_args()
    pairs = [parse_pair(p) for p in a.pair]
    lines, verdict, dump = evaluate(pairs, a.budget, a.warmup)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dump, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
