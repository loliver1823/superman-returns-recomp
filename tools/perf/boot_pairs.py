#!/usr/bin/env python3
"""boot_pairs.py - boot.device_create's boot_order_gap: a paired ON / OFF boot DIAGNOSTIC (dcb, 2026-10-14; rewritten
after Astra's round-1 review the same day to the rule in archive/dcb/astra/round1.txt).

    python tools/perf/boot_pairs.py --pair on:<roll> off:<roll> --pair off:<roll> on:<roll> --pair on:<roll> off:<roll>
                                    [--budget 1.0] [--out <txt>] [--json <json>]

Each --pair names its two rolls IN THE ORDER THEY WERE DECLARED TO RUN (arm:path; a perf_run roll folder with
drive/attempt1/{run.log,steps.log,provenance.json,outcome.json}). The tool checks that order against the logs.

What a diagnostic of this size can and cannot say: it can show a regression in these pairs (OPEN) or fail to see
one ("NO REGRESSION OBSERVED IN THESE PAIRS"). It never says the gap is resolved, within noise or absent: that
needs a separately pre-registered, adequately powered paired design (its own evaluator, its own output tag
BOOT_EQUIV - this tool never prints that tag), which the ledger's resolver requires.

THE RULE (Astra's recommended dcc5 rule, round 1, 2026-10-14):
  Design (else INCONCLUSIVE - the set is not evidence): equal arms, one ON and one OFF per pair; distinct paths; one
    exe hash; identical effective SR_* configuration except SR_NATIVE_DEVCREATE (path-valued keys aside) and the same
    save seeding, parsed for EVERY roll from its steps.log (a roll without the line is named, never matched as None);
    ON engaged (SR_NATIVE_DEVCREATE=1, not CHECK, and a [NATIVE_DEVCREATE] ring init line or a first [GPU_CP] census
    with packets=0) and OFF not (the gate unset and the ME_INIT parsed); each pair's start times in the declared order
    and the pairs in sequence.
  Markers (else INCONCLUSIVE, the missing evidence named): provenance.json and run.log present, the swap-chain line,
    the CP thread up, the [SHADER_CACHE] boot load done line, the attach and the CP's wait status, with consistent
    timestamps (swap < load end <= attach; a recorded wait within 1.0 s of [CP up, attach]); no wait line counts as 0
    only when the wait was enabled and the attach came before the CP started.
    THE ASYNC LOAD (render/boot-attach, dev f9c08c2d; amended on native/devcreate-close 2026-10-14): a roll whose
    "[SHADER_CACHE] boot load on its own thread" line says the load "runs on the loader thread" loads the shader cache
    off the attach's path, so its load end has no order with the swap chain or the attach (on J: it ends before the
    swap chain, on a slow disk after the attach): its timestamps must read swap < attach only, and its uncovered wait U
    is the whole interval [CP up, attach] (the load no longer explains any of it; read from the validated interval,
    never from the recorded number alone). A load that RAN ON THIS THREAD (no loader thread could be created), the
    SR_SHADER_LOAD_ASYNC=0 line, or no load-mode line at all (an exe before render/boot-attach) keep the order above.
    The scan reads on until the 'boot load done' line (or EOF), as boot_census.py does (6286b120), so a load ending
    late is never lost to the early stop. Astra round 1 (P2-6): EVERY recorded wait, 0.0 ms included, must match
    [CP up, attach] within 1.0 s, and a load of L s ending at t must have begun no earlier than 0.1 s before the log's
    first line (else INCONCLUSIVE: an infeasible log).
  OPEN, never discarded or replaced: a crash (outcome not SUCCESS, or outcome.json missing), no HUD request, or a TIMED
    OUT backend wait; an OPEN verdict also names any missing evidence.
  Endpoints, paired ON - OFF per pair: time to the HUD request (PRIMARY, unadjusted), time to the attach, the boot-load
    duration, and attach -> HUD. Diagnostics only (no part of the verdict but the uncovered wait): the backend wait,
    H' = HUD - wait, and the uncovered wait U (the part of [CP up, attach] outside [load end - L, load end]).
  NO REGRESSION OBSERVED IN THESE PAIRS when every paired difference of every endpoint is <= the budget (fixed before
    execution: --budget, default 1.0 s; a non-finite or negative budget is rejected) and every ON roll's U <= 1.0 s;
    OPEN otherwise, naming what exceeded (comparisons fail closed: a nan difference is over).
Read-only; no game runs.
"""
import argparse
import json
import math
import os
import re
import sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
STEP = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] saves: SR_CONTENT_DIR=\S+ \((.*)\)")
MARKS = {
    "swap": re.compile(r"\[HIGHFPS\] swap chain ALLOW_TEARING"),
    "cpup": re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] command processor thread up"),
    "loaddone": re.compile(r"\[SHADER_CACHE\] boot load done: ([0-9.]+) ms"),
    "attach": re.compile(r"\[DISPLAY\] PM4 live backend attached"),
    "waited": re.compile(r"\[DISPLAY\] CP waited ([0-9.]+) ms for the live backend \(([^)]*)\)"),
    "hud": re.compile(r"\[APT_REQ\] #\d+ path='screens/hud/main_hud'"),
    # WP1 (render/boot-attach): the CP's wait for the installed shader cache AFTER the attach, before its first command -
    # the post-attach stall the stall criterion's event C reads (native/devcreate-fix, Astra round 1); r["C"] in seconds
    "cpstart": re.compile(r"\[SHADER_CACHE\] CP start: waited ([0-9.]+) ms for the installed shader cache"),
    "dcring": re.compile(r"\[NATIVE_DEVCREATE\] ring init native"),
    "cpcensus": re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census .* packets=(\d+) "),
    # the load mode (render/boot-attach): which order the load end keeps with the swap chain and the attach
    "asyncload": re.compile(r"\[SHADER_CACHE\] boot load on its own thread \(SR_SHADER_LOAD_ASYNC=1[^)]*\): .*the load "
                            r"(runs on the loader thread|RAN ON THIS THREAD)"),
    "syncload": re.compile(r"\[SHADER_CACHE\] SR_SHADER_LOAD_ASYNC=0: the boot load runs inside"),
    # parse-only (no rule here): the attach line's black-world race fields, graded by tools/perf/boot_equiv.py
    "attachrace": re.compile(r"\[DISPLAY\] PM4 live backend attached: guest VdSwaps before it (\d+), draw commands "
                             r"queued (\d+)"),
}
LOADER_THREAD = "runs on the loader thread"
VOLATILE = ("SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR", "SR_NATIVE_DEVCREATE")
U_MAX = 1.0
WAIT_SLACK = 1.0
LOAD_SLACK = 0.1  # s: a load may begin at most this long before the log's first line (its clock vs the log's)
# native_proof's ABORT markers (scan_abort) and the game's fatal line: a roll whose log holds one failed, whatever its
# outcome.json says (Astra round 1 on native/devcreate-close, P1-2)
ABORT_MARKERS = (("CP stall", re.compile(r"\[CP_WAIT\] stalled")), ("GPU hang trigger", re.compile(r"\[GPU_HANG\] ==== trigger")),
                 ("thread fatal", re.compile(r"THREAD_FATAL")), ("indirect-call failure", re.compile(r"\[ICALL_FAIL\]")),
                 ("fatal", re.compile(r"\[FATAL\]")))
GATE = "SR_NATIVE_DEVCREATE"


def arm_of(env, baked=False):
    """The arm a roll's effective SR_* env declares for the devcreate gate: "on", "off", or None when SR_AB_TOGGLE names
    the gate (the per-present A/B roll: neither arm at boot).
    baked=False (the exes of 2026-10-10..14, SR_NATIVE_DEVCREATE default OFF): ON = "1"; anything else (unset, "0") OFF.
    baked=True (default ON since native/devcreate-fix, 2026-10-15): OFF = "0"; anything else (unset, "1") ON."""
    env = env or {}
    if GATE in (env.get("SR_AB_TOGGLE") or "").split("+"):
        return None
    v = env.get(GATE)
    if baked:
        return "off" if v == "0" else "on"
    return "on" if v == "1" else "off"


def declared_on_boot(env, baked=False):
    """An ON boot of the devcreate gate as the stall criterion counts them: the arm declared ON and not a CHECK roll
    (SR_NATIVE_FLUSH_CHECK=1 runs the guest bodies - the ports never engage)."""
    return arm_of(env, baked) == "on" and (env or {}).get("SR_NATIVE_FLUSH_CHECK") != "1"


def abort_marker(path):
    """(what, line) of the first abort marker in a roll's whole run.log, or (None, None); a missing log -> (None, None)
    (the missing log is its own problem)."""
    try:
        with open(os.path.join(path, "drive", "attempt1", "run.log"), "rb") as f:
            for raw in f:
                line = raw.decode("utf-8", "replace")
                for what, rx in ABORT_MARKERS:
                    if rx.search(line):
                        return what, line.strip()[:160]
    except OSError:
        pass
    return None, None


ENDPOINTS =(("hud", "time to HUD (primary)"), ("attach", "time to attach"), ("L", "boot-load duration"),
             ("a2h", "attach -> HUD"))


def read_json(p):
    try:
        with open(p, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def roll(path, baked=False):
    """One roll's boot markers, endpoints, OPEN classes and gate state. baked: the devcreate gate's default (arm_of) -
    False for the sets of 2026-10-10..14 (dcl-v2), True since the bake (native/devcreate-fix, dcf-v1)."""
    att = os.path.join(path, "drive", "attempt1")
    r = {"roll": path, "problems": [], "open": []}
    prov = read_json(os.path.join(att, "provenance.json")) or {}
    outc = read_json(os.path.join(att, "outcome.json")) or {}
    r["exe"] = (prov.get("exeSha256") or "").lower()
    r["exepath"] = prov.get("exePath") or ""   # the exe as launched (a staged roll's: under the --stage root)
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
        pass  # no seed parsed: main() names the roll (a missing seed never matches the others' by being None alike)
    t0 = None
    try:
        f = open(os.path.join(att, "run.log"), encoding="utf-8", errors="replace")
    except OSError:
        # Astra round 2: the missing evidence named, INCONCLUSIVE (a recorded crash in outcome.json stays OPEN)
        r["problems"].append("no run.log (%s)" % os.path.join(att, "run.log"))
        r["gate"] = "unverified (no run.log)"
        if r["result"] != "SUCCESS":
            r["open"].append("outcome %s" % (r["result"] or "missing"))
        return r
    with f:
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
                if mm:
                    r[k] = (t - t0).total_seconds()
                    if k == "loaddone":
                        r["L"] = float(mm.group(1)) / 1000.0
                    elif k == "waited":
                        r["W"], r["waitst"] = float(mm.group(1)) / 1000.0, mm.group(2)
                    elif k == "cpstart":
                        r["C"] = float(mm.group(1)) / 1000.0
                    elif k == "cpcensus":
                        r["cppackets"] = int(mm.group(1))
                    elif k == "asyncload":
                        r["asyncwhere"] = mm.group(1)
                    elif k == "attachrace":
                        r["swaps_before"], r["queued_before"] = int(mm.group(1)), int(mm.group(2))
            if "hud" in r and "cpcensus" in r and "loaddone" in r:  # a load ending late is read (else to EOF)
                break
    # the outcome classes
    if r["result"] != "SUCCESS":
        r["open"].append("outcome %s" % (r["result"] or "missing"))
    if "hud" not in r:
        r["open"].append("no HUD request")
    if r.get("waitst") and r["waitst"] != "attached":
        r["open"].append("backend wait %s" % r["waitst"])
    # markers and consistency
    for k in ("swap", "cpup", "loaddone", "attach"):
        if k not in r:
            r["problems"].append("no %s line" % k)
    wait_on = (r["env"].get("SR_PM4_WAIT_BACKEND", "1") or "1")[0] != "0"
    if "waited" not in r and "attach" in r and "cpup" in r:
        if wait_on and r["attach"] <= r["cpup"]:
            r["W"] = 0.0
        else:
            r["problems"].append("no wait line though the attach (%.2f s) followed the CP's start (%.2f s)%s" % (
                r["attach"], r["cpup"], "" if wait_on else " - waiting disabled"))
    # the load mode: on the loader thread (render/boot-attach's default) the load is off the attach's path
    r["load_mode"] = ("loader thread" if r.get("asyncwhere") == LOADER_THREAD and "syncload" not in r else
                      "init thread (RAN ON THIS THREAD)" if r.get("asyncwhere") else
                      "inside the backend init (SR_SHADER_LOAD_ASYNC=0)" if "syncload" in r else
                      "inside the backend init (no load-mode line)")
    off_path = r["load_mode"] == "loader thread"
    if off_path:
        if all(k in r for k in ("swap", "attach")) and not r["swap"] < r["attach"] + 0.001:
            r["problems"].append("timestamps out of order (swap %.2f, attach %.2f; the load on the loader thread)" % (
                r["swap"], r["attach"]))
    elif all(k in r for k in ("swap", "loaddone", "attach")):
        if not (r["swap"] < r["loaddone"] <= r["attach"] + 0.001):
            r["problems"].append("timestamps out of order (swap %.2f, load end %.2f, attach %.2f)" % (
                r["swap"], r["loaddone"], r["attach"]))
    # Astra round 1 on native/devcreate-close (P2-6): EVERY recorded wait - 0.0 ms included - must match the interval
    # [CP up, attach] (0 when the attach came first); a truthiness test let "CP waited 0.0 ms" stand beside 25 s
    if "waited" in r and "cpup" in r and "attach" in r and r.get("waitst") == "attached":
        span = max(0.0, r["attach"] - r["cpup"])
        if not abs(span - r["W"]) <= WAIT_SLACK:
            r["problems"].append("recorded wait %.2f s vs [CP up, attach] %.2f s" % (r["W"], span))
    # ...and the load must fit in the log: a load of L seconds that ended at t began at t - L, never before the log's
    # first line (a 20 s load "done" 0.3 s after the start is an inconsistent log)
    if "loaddone" in r and "L" in r and not r["loaddone"] - r["L"] >= -LOAD_SLACK:
        r["problems"].append("infeasible load: %.2f s done at +%.2f s would have begun before the log's first line" % (
            r["L"], r["loaddone"]))
    # endpoints and diagnostics
    if "attach" in r and "hud" in r:
        r["a2h"] = r["hud"] - r["attach"]
    if "hud" in r and "W" in r:
        r["Hp"] = r["hud"] - r["W"]
    if off_path and "cpup" in r and "attach" in r:
        # off the attach's path the load explains none of the wait: the uncovered wait IS the validated interval
        # [CP up, attach] (never the recorded number alone - P2-6)
        r["U"] = max(0.0, r["attach"] - r["cpup"])
    elif all(k in r for k in ("cpup", "attach", "loaddone", "L", "W")):
        if r["W"] > 0:
            a, b = r["cpup"], r["attach"]
            c, d = r["loaddone"] - r["L"], r["loaddone"]
            r["U"] = max(0.0, (b - a) - max(0.0, min(b, d) - max(a, c)))
        else:
            r["U"] = 0.0
    # gate state: the declared arm (arm_of: the gate's default decides what "unset" means) against the log's
    # engagement - ON = the ring init line or a first census with packets=0, OFF = the guest's ME_INIT parsed
    arm = arm_of(r["env"], baked)
    r["declared"] = arm
    on = arm == "on"
    check = r["env"].get("SR_NATIVE_FLUSH_CHECK") == "1"
    engaged = "dcring" in r or r.get("cppackets") == 0
    me_init = r.get("cppackets", 0) >= 1 and "dcring" not in r
    r["gate"] = "on" if (on and not check and engaged) else "off" if (arm == "off" and me_init) else \
        "ab (SR_AB_TOGGLE names the gate: neither arm at boot)" if arm is None else \
        "check (SR_NATIVE_FLUSH_CHECK=1: the guest bodies run)" if (on and check) else \
        "unverified (gate %s, CHECK %s, ring init line %s, first census packets %s)" % (
            r["env"].get(GATE), check, "dcring" in r, r.get("cppackets"))
    return r


def parse_pair(tokens):
    if len(tokens) != 2:
        raise SystemExit("--pair takes two arm:path tokens in run order")
    out = []
    for tok in tokens:
        arm, _, path = tok.partition(":")
        if arm.lower() not in ("on", "off") or not path:
            raise SystemExit("bad pair token %r (want on:<roll> or off:<roll>)" % tok)
        out.append((arm.lower(), path))
    return out


def budget_arg(s):
    """--budget: a finite number of seconds >= 0 (Astra round 2: nan compares false and a negative budget is no
    budget - either would let a +30 s regression through)."""
    try:
        v = float(s)
    except ValueError:
        raise argparse.ArgumentTypeError("budget %r is not a number" % s)
    if not math.isfinite(v) or v < 0:
        raise argparse.ArgumentTypeError("budget %r must be a finite number of seconds >= 0" % s)
    return v


def fmt(x, w=6, p=1):
    return ("%*.*f" % (w, p, x)) if isinstance(x, (int, float)) else ("%*s" % (w, "-"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pair", nargs=2, action="append", required=True, metavar=("ARM:ROLL", "ARM:ROLL"))
    ap.add_argument("--budget", type=budget_arg, default=1.0,
                    help="the per-endpoint regression budget in seconds (finite, >= 0), fixed before running")
    ap.add_argument("--out")
    ap.add_argument("--json")
    ap.add_argument("--baked", action="store_true",
                    help="the devcreate gate default ON (since native/devcreate-fix): ON = unset, OFF = SR_NATIVE_DEVCREATE=0")
    a = ap.parse_args()
    pairs = [parse_pair(p) for p in a.pair]
    design = []
    rolls = {}
    for i, pr in enumerate(pairs):
        arms = sorted(arm for arm, _ in pr)
        if arms != ["off", "on"]:
            design.append("pair %d is not one ON and one OFF" % (i + 1))
        for arm, path in pr:
            if os.path.normcase(os.path.abspath(path)) in rolls:
                design.append("path used twice: %s" % path)
            rolls[os.path.normcase(os.path.abspath(path))] = roll(path, baked=a.baked)
    R = [[rolls[os.path.normcase(os.path.abspath(p))] for _, p in pr] for pr in pairs]
    flat = [r for pr in R for r in pr]
    exes = set(r["exe"] for r in flat)
    if len(exes) != 1 or "" in exes:
        design.append("exe hashes %s (want exactly one)" % sorted(x[:12] for x in exes))
    norm = lambda e: {k: v for k, v in e.items() if k not in VOLATILE}
    if len(set(json.dumps(norm(r["env"]), sort_keys=True) for r in flat)) != 1:
        design.append("the effective SR_* configuration differs beyond SR_NATIVE_DEVCREATE")
    unseeded = [r["roll"] for r in flat if r.get("seed") is None]
    if unseeded:  # every roll needs its own parsed policy ({None} alone has one element)
        design.append("save seeding not recorded for %s" % ", ".join(unseeded))
    elif len(set(r["seed"] for r in flat)) != 1:
        design.append("save seeding differs (%s)" % sorted(set(r["seed"] for r in flat)))
    for i, (pr, rr) in enumerate(zip(pairs, R)):
        for (arm, path), r in zip(pr, rr):
            if r.get("gate") != arm:
                design.append("pair %d %s roll's gate state: %s" % (i + 1, arm.upper(), r.get("gate", "unknown")))
        if all("start" in r for r in rr) and not rr[0]["start"] < rr[1]["start"]:
            design.append("pair %d did not run in its declared order" % (i + 1))
    starts = [r.get("start") for r in flat]
    if all(starts) and any(starts[k] >= starts[k + 1] for k in range(len(starts) - 1)):
        design.append("the rolls did not run in the declared sequence")
    lines = ["pair arm   W  wait-st   Tcp     E      L  attach      U     HUD     H'  attach->HUD  load   roll"]
    for i, (pr, rr) in enumerate(zip(pairs, R)):
        for (arm, _), r in zip(pr, rr):
            lines.append("%4d %-3s %s %-9s %s %s %s %s %s %s %s %s  %-6s %s" % (
                i + 1, arm.upper(), fmt(r.get("W"), 5), r.get("waitst", "-"), fmt(r.get("cpup"), 5), fmt(r.get("loaddone")),
                fmt(r.get("L")), fmt(r.get("attach"), 7), fmt(r.get("U"), 6), fmt(r.get("hud"), 7), fmt(r.get("Hp")),
                fmt(r.get("a2h"), 11), "async" if r.get("load_mode") == "loader thread" else "inline", r["roll"]))
    opens = ["%s: %s" % (r["roll"], ", ".join(r["open"])) for r in flat if r["open"]]
    probs = ["%s: %s" % (r["roll"], ", ".join(r["problems"])) for r in flat if r["problems"]]
    diffs = {}
    head = "BOOT_PAIRS pairs=%d budget=%.1f" % (len(pairs), a.budget)
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
                on = rr[[arm for arm, _ in pr].index("on")]
                off = rr[[arm for arm, _ in pr].index("off")]
                d.append(on[key] - off[key])
            diffs[key] = d
            lines.append("paired ON - OFF %-22s %s" % (name, " ".join("%+.2f" % x for x in d)))
            if any(not (x <= a.budget) for x in d):  # fail closed: a nan difference is over
                over.append("%s %s" % (name, " ".join("%+.2f" % x for x in d)))
        u_on = [r["U"] for pr, rr in zip(pairs, R) for (arm, _), r in zip(pr, rr) if arm == "on"]
        if any(not (u <= U_MAX) for u in u_on):
            over.append("ON uncovered wait %s" % " ".join("%.2f" % u for u in u_on))
        head += " max_dHUD=%+.2f max_dAttach=%+.2f max_dLoad=%+.2f max_dA2H=%+.2f max_U_on=%.2f" % (
            max(diffs["hud"]), max(diffs["attach"]), max(diffs["L"]), max(diffs["a2h"]), max(u_on))
        verdict = head + (" -> OPEN: " + "; ".join(over) if over else " -> NO REGRESSION OBSERVED IN THESE PAIRS")
    lines.append(verdict)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    if a.json:
        dump = {"pairs": [[{k: (v.isoformat() if isinstance(v, datetime) else v) for k, v in r.items() if k != "env"}
                           for r in rr] for rr in R], "design": design, "diffs": diffs, "verdict": verdict}
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dump, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
