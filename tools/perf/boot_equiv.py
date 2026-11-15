#!/usr/bin/env python3
"""boot_equiv.py - BOOT_EQUIV: boot.device_create's residual unknown boot_order_gap, the MEAN criterion: the
pre-registered, powered paired design dcl-v2 (docs/NATIVE_ROADMAP.md, boot.device_create "CLOSE DESIGN"; native/
devcreate-close, 2026-10-14; dcl-v1 revised after Astra round 1 before any roll; the run: tools/perf/native_proof.py
--plan devcreate_close). The intermittent-stall criterion is a SEPARATE tool and resolver: tools/perf/boot_stall.py
(BOOT_STALL). The unknown resolves only when both pass.

    python tools/perf/boot_equiv.py --warmup <roll> --pair on:<roll> off:<roll> --pair off:<roll> on:<roll> ...
                                    (the design's pairs, declared in run order) [--design dcl-v2 | dcf-v1]
                                    [--out <txt>] [--json <json>]
    python -m unittest discover -s tools/perf -p test_boot_equiv.py      # the rule's tests and its mutant check

Two registered designs (DESIGNS; the ledger item names the design and its pair count): dcl-v2 - 8 pairs, the gate
default OFF (ON = SR_NATIVE_DEVCREATE=1, the warm-up the OFF arm); dcf-v1 (native/devcreate-fix, 2026-10-15, the bake
plan devcreate_bake) - 4 pairs, the gate default ON (ON = the key unset, OFF = SR_NATIVE_DEVCREATE=0, the warm-up the
default build with the key unset). The rule below is the same under both but for the pair count and what "ON" and "OFF"
read as in a roll's provenance (boot_pairs.arm_of); the comment at DESIGNS gives the 4-pair reasoning.

Each --pair names its two rolls IN THE ORDER THEY WERE DECLARED TO RUN (arm:path; a perf_run roll folder with
drive/attempt1/{run.log, steps.log, provenance.json, outcome.json}); --warmup names the declared warm-up roll. The
last line printed is the verdict: exactly one of EQUIVALENT, OPEN, INCONCLUSIVE. Only an EQUIVALENT line carries the
tag BOOT_EQUIV (the tag the ledger's resolver boot_order.powered requires); OPEN and INCONCLUSIVE lines never print it.
tools/perf/boot_pairs.py stays the 3-pair diagnostic that never prints the tag; this evaluator reads every roll with
boot_pairs' own roll() (its markers, its OPEN classes, its gate engagement, its async-load amendment), so the two
can never read a log differently.

THE RULE (frozen before any roll; the reasons and the measured spreads are in the roadmap note):
  Design dcl-v2 / dcf-v1 (else INCONCLUSIVE - the set is not evidence):
    - exactly the design's pairs (dcl-v2: 8; dcf-v1: 4), declared ON/OFF, OFF/ON, ON/OFF, ... (pair k ON-first when k
      is odd: ABBA blocks, each arm first in half the pairs); one ON and one OFF per pair; every path distinct;
    - a declared warm-up roll (--warmup), never graded: the OFF arm's configuration (SR_NATIVE_DEVCREATE unset - the
      default build), the same exe, exe path and save seeding as the set; it reached the HUD and its log ENDED before
      the first graded roll's log began (Astra P2-7: a start-only check let a warm-up still booting at the first
      graded launch through); its run row DONE;
    - ONE native_proof run took the warm-up and every graded roll (tools/perf/proof_rows.py: the *_proof.json beside
      them whose `out` is that folder, rows matched by CANONICAL path - never basename - and the run's exe and start
      time matching every roll; Astra P1-2): bat2 found the first launch of EACH invocation ~1 s late to its first CP
      command, so only a warm-up inside the set's own invocation takes that launch;
    - one exe hash and one exe path (one staged read set) for every roll, the warm-up included; identical effective
      SR_* configuration except SR_NATIVE_DEVCREATE and the path-valued keys; the same save seeding, parsed for
      every roll from its steps.log;
    - every graded roll's arm engaged (boot_pairs: ON = SR_NATIVE_DEVCREATE=1, not CHECK, and the ring init line or
      a first census with packets=0; OFF = the gate unset and the ME_INIT parsed);
    - each pair's start times in the declared order and the rolls in the declared sequence;
    - a roll that was never run (no roll folder) is missing evidence, named.
  Markers (else INCONCLUSIVE): boot_pairs' (provenance, run.log, swap chain, CP up, boot load done, the attach and
    the wait status, consistent and FEASIBLE timestamps - every recorded wait, 0.0 ms included, matching [CP up,
    attach]; a load beginning no earlier than the log), and the attach line's VdSwaps / queued-draws fields.
  OPEN, never discarded or replaced (an OPEN verdict also names any missing evidence):
    - in any graded roll: boot_pairs' OPEN classes (a crash or timeout - outcome not SUCCESS, or outcome.json missing
      in a roll folder - no HUD request, a backend wait that did not end "attached"); the run's row for it not DONE
      (ABORT / FAIL); an abort marker anywhere in its log (THREAD_FATAL, ICALL_FAIL, a CP stall, a GPU hang trigger,
      [FATAL]) - whatever its outcome.json says (Astra P1-2);
    - an attach after guest VdSwaps or with draw commands queued (the black-world race);
    - an ON roll whose backend attached more than 5.0 s after its log began, or whose uncovered wait exceeds 1.0 s
      (boot_stall.py's events A / W, one definition);
    - any pair whose ON - OFF difference exceeds the stall cap (5.0 s) on any endpoint - read even when the design is
      INCONCLUSIVE: a measured stall is never hidden behind a design fault;
    - with the design whole, an endpoint whose mean paired difference exceeds its margin.
  Statistic, per endpoint (paired ON - OFF, seconds; fixed margins): time to the HUD request 2.0 (PRIMARY, from the
    log's first line, unadjusted), time to the attach 0.5, the boot-load duration 0.5, attach -> HUD 2.0. The
    one-sided 95% upper confidence bound of the mean difference, mean + t(0.95, n-1) * s / sqrt(n) (Student t, the
    table below; s the sample SD, n-1 df).
  POSITION, per endpoint (Astra P2-8: ON-first is NOT conservative for every endpoint - a first-slot delay of the
    attach alone LOWERS attach -> HUD): the in-invocation warm-up takes the invocation's first launch; then, for each
    endpoint, the first graded roll's value must lie within that endpoint's margin of the median of the other graded
    rolls of its arm, in EITHER direction - else INCONCLUSIVE (a position effect on that endpoint). An effect smaller
    than the margin on that one roll moves the 8-pair mean by less than margin / 8.
  INCONCLUSIVE also: with nothing OPEN and the design whole, an endpoint whose upper bound reaches its margin (the set
    was under-powered as run: its paired spread was larger than the design assumed), a pair whose OFF roll is slower
    than its ON roll by more than the stall cap, or an OFF roll that attached more than 5.0 s late (an OFF-arm stall:
    the conditions were not the design's, and a negative outlier must not help the bound).
  EQUIVALENT (tag BOOT_EQUIV): the design whole, every marker, nothing OPEN, no position effect, every endpoint's upper
    bound below its margin. It covers ONLY the mean paired regression of each endpoint (one-sided 95%, on this exe,
    from this read set, in this scenario). It does NOT bound an intermittent stall: with no stall in 8 ON boots the
    exact 95% bound on a stall rate is 31.23% (independent boots assumed) - BOOT_STALL's 29-boot criterion is the
    stall claim.
Read-only; no game runs. Exit 0 with a verdict; 2 for bad arguments.
"""
import argparse
import json
import math
import os
import statistics
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_pairs as bp  # noqa: E402  (the markers, OPEN classes and gate engagement: one parser for both tools)
import boot_stall as bs  # noqa: E402  (the stall events A / W: one definition)
import proof_rows as pr  # noqa: E402  (which run took the rolls: canonical rows, the run's identity)

# THE REGISTERED DESIGNS (--design; the ledger's resolver item names the design and its pair count):
#   dcl-v2 (native/devcreate-close, 2026-10-14): 8 pairs; the gate default OFF - ON = SR_NATIVE_DEVCREATE=1, OFF = the
#     key unset; the warm-up is the OFF arm (the default build of that time).
#   dcf-v1 (native/devcreate-fix, 2026-10-15; the bake plan native_proof --plan devcreate_bake): 4 pairs; the gate
#     default ON - ON = the default (the key unset), OFF = SR_NATIVE_DEVCREATE=0; the warm-up is the default build (the
#     key unset: pinning it either way is a design fault). The same markers, endpoints, margins, OPEN classes, position
#     rule and statistic. Why 4 pairs and not 8: the stalls the unknown was written on are root-caused
#     (docs/NATIVE_ROADMAP.md "boot.device_create BAKE PLAN": the installed shader cache's synchronous boot load on the
#     attach path, in BOTH arms, gone since render/boot-attach f9c08c2d), so the set is a regression guard with a known
#     noise model, not a search: on J: with an in-invocation warm-up the paired HUD spread is ~0.5 s (bat1 / bat2 pairs
#     2-3: +0.03 / -0.97 / -0.16 / +0.21), so with n = 4 the bound sits t(0.95, 3) x 0.53 / 2 = 0.62 s above the mean
#     against the 2.0 s margin (0.89 s at n = 3); the order ON/OFF, OFF/ON, ON/OFF, OFF/ON still puts each arm first
#     twice. The stall claim stays boot_stall.py's (design dcf-stall-v1).
DESIGNS = {"dcl-v2": {"pairs": 8, "baked": False}, "dcf-v1": {"pairs": 4, "baked": True}}
DESIGN_ID = "dcl-v2"   # the default design (--design); the registry above is the one place its numbers live
N_PAIRS = DESIGNS[DESIGN_ID]["pairs"]
BAKED = DESIGNS[DESIGN_ID]["baked"]   # the gate's default under the selected design (boot_pairs.arm_of)
DESIGN = [["on", "off"] if k % 2 == 0 else ["off", "on"] for k in range(N_PAIRS)]  # pair 1 ON/OFF, pair 2 OFF/ON...


def select_design(design_id):
    """Select a registered design (--design): its pair count, alternation and gate default."""
    global DESIGN_ID, N_PAIRS, BAKED, DESIGN
    d = DESIGNS[design_id]
    DESIGN_ID, N_PAIRS, BAKED = design_id, d["pairs"], d["baked"]
    DESIGN = [["on", "off"] if k % 2 == 0 else ["off", "on"] for k in range(N_PAIRS)]


ENDPOINTS = (("hud", "time to HUD (primary)", 2.0), ("attach", "time to attach", 0.5),
             ("L", "boot-load duration", 0.5), ("a2h", "attach -> HUD", 2.0))
STALL_CAP = 5.0
U_MAX = bs.U_MAX
ATTACH_STALL = bs.ATTACH_STALL
TAG = "BOOT_EQUIV"
# one-sided 95% quantiles of Student's t by degrees of freedom (frozen; test_boot_equiv checks them against scipy)
T95 = {1: 6.3138, 2: 2.9200, 3: 2.3534, 4: 2.1318, 5: 2.0150, 6: 1.9432, 7: 1.8946, 8: 1.8595, 9: 1.8331,
       10: 1.8125, 11: 1.7959, 12: 1.7823, 13: 1.7709, 14: 1.7613, 15: 1.7531, 16: 1.7459, 17: 1.7396, 18: 1.7341,
       19: 1.7291, 20: 1.7247, 21: 1.7207, 22: 1.7171, 23: 1.7139, 24: 1.7109, 25: 1.7081, 26: 1.7056, 27: 1.7033,
       28: 1.7011, 29: 1.6991, 30: 1.6973}


def grade_roll(path, arm):
    """boot_pairs' roll() plus this design's extras: a roll never run, the exe path, the black-world race, an abort
    marker anywhere in the log."""
    if not os.path.isdir(path):
        return {"roll": path, "arm": arm, "notrun": True, "problems": ["not run (no roll folder)"], "open": []}
    r = bp.roll(path, baked=BAKED)
    r["arm"] = arm
    prov = pr.read_json(os.path.join(path, "drive", "attempt1", "provenance.json")) or {}
    r["exe_path"] = os.path.normcase(prov.get("exePath") or "")
    if r.get("swaps_before") or r.get("queued_before"):
        r["open"].append("attach after %d guest VdSwaps / %d queued draws (the black-world race)" % (
            r.get("swaps_before", 0), r.get("queued_before", 0)))
    elif "attach" in r and "swaps_before" not in r:
        r["problems"].append("the attach line has no VdSwaps / queued-draws fields")
    what, line = bp.abort_marker(path)
    if what:
        r["open"].append("%s in the log (%s)" % (what, line))
    return r


def median(vals):
    v = sorted(x for x in vals if isinstance(x, (int, float)) and not isinstance(x, bool))
    return None if not v else v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2.0


def t95(df):
    return T95.get(df, 1.6449) if df >= 1 else float("nan")


def stats_of(d):
    """(mean, sd, upper bound) of a list of paired differences; nan when undefined."""
    n = len(d)
    if n == 0:
        return float("nan"), float("nan"), float("nan")
    m = statistics.fmean(d)
    if n < 2:
        return m, float("nan"), float("nan")
    s = statistics.stdev(d)
    return m, s, m + t95(n - 1) * s / math.sqrt(n)


def fmt(x, w=7, p=2):
    return ("%*.*f" % (w, p, x)) if isinstance(x, (int, float)) and not isinstance(x, bool) else ("%*s" % (w, "-"))


def sgn(x):
    return ("%+.2f" % x) if isinstance(x, (int, float)) and not math.isnan(x) else "nan"


def evaluate(pairs, warmup):
    """pairs: [[(arm, path), (arm, path)], ...] in declared run order; warmup: a path or None.
    -> (lines, verdict line, dump)."""
    design, opens = [], []
    seen = {}
    declared = [[arm for arm, _ in p] for p in pairs]
    if declared != DESIGN:
        design.append("the registered design %s is %d pairs declared %s (got %d: %s)" % (
            DESIGN_ID, N_PAIRS, ", ".join("/".join(a.upper() for a in d) for d in DESIGN[:3]) + ", ...", len(pairs),
            ", ".join("/".join(a.upper() for a in d) for d in declared) or "none"))
    R = []
    for i, p in enumerate(pairs):
        if sorted(arm for arm, _ in p) != ["off", "on"]:
            design.append("pair %d is not one ON and one OFF" % (i + 1))
        row = []
        for arm, path in p:
            if pr.key(path) in seen:
                design.append("path used twice: %s" % path)
            seen[pr.key(path)] = True
            row.append(grade_roll(path, arm))
        R.append(row)
    W = None
    if not warmup:
        design.append("no declared warm-up roll (--warmup): the first-roll effect is not separated from the arm")
    else:
        if pr.key(warmup) in seen:
            design.append("the warm-up %s is also a graded roll (the warm-up is never graded)" % warmup)
        W = grade_roll(warmup, "on" if BAKED else "off")  # the default build's arm under the design
    flat = [r for row in R for r in row]
    ran = [r for r in flat if not r.get("notrun")]
    allr = ran + ([W] if W and not W.get("notrun") else [])
    # ONE native_proof run took them all (canonical rows, the run's identity); its row verdicts
    run = pr.find_run(([warmup] if warmup else []) + [r["roll"] for r in flat])
    design += run["problems"]
    if run["proof"]:
        for r in flat:
            v = pr.row_verdict(run, r["roll"])
            if v is None:  # no row: a folder the run does not record is in run["problems"]; no folder - not run
                continue
            if v == "SKIP" and not r.get("notrun"):
                design.append("%s: the run skipped it, yet a roll folder is there" % r["roll"])
            elif v not in ("DONE", "SKIP"):
                row = run["rows"][pr.key(r["roll"])]
                opens.append("%s: the run's row says %s (%s)" % (r["roll"], v, str(row.get("detail", ""))[:100]))
    # the warm-up: the OFF arm's configuration, its run row DONE, reached the HUD and ENDED before the first graded launch
    if W is not None:
        if W.get("notrun"):
            design.append("the warm-up %s was not run" % warmup)
        else:
            if W.get("result") != "SUCCESS" or "hud" not in W or W.get("open"):
                design.append("the warm-up %s did not complete to the HUD (%s): the boot path was not warmed" % (
                    warmup, ", ".join(W["open"]) or "no HUD request"))
            if BAKED:
                if bp.GATE in W["env"]:  # the default build: the key unset (=0 is the OFF arm, =1 a pin of the default)
                    design.append("the warm-up pins SR_NATIVE_DEVCREATE=%s (design %s declares it as the default build, "
                                  "the key unset)" % (W["env"].get(bp.GATE), DESIGN_ID))
            elif W["env"].get("SR_NATIVE_DEVCREATE") == "1":
                design.append("the warm-up ran SR_NATIVE_DEVCREATE=1 (it is declared as the OFF arm, the default build)")
            if run["proof"] and pr.row_verdict(run, warmup) not in (None, "DONE"):  # None: in run["problems"]
                design.append("the warm-up's run row says %s" % pr.row_verdict(run, warmup))
            _wfirst, wlast = pr.log_span(warmup)
            firsts = [pr.log_span(r["roll"])[0] for r in ran]
            firsts = [f for f in firsts if f is not None]
            if wlast is None:
                design.append("the warm-up's end is unknown (no timestamped run.log line)")
            elif firsts and not wlast < min(firsts):
                design.append("the warm-up had not ended (its log runs to %s) before the first graded launch (%s)" % (
                    wlast.isoformat(sep=" "), min(firsts).isoformat(sep=" ")))
    # one exe, one exe path, one configuration, one save seeding (the warm-up included)
    exes = set(r["exe"] for r in allr)
    if allr and (len(exes) != 1 or "" in exes):
        design.append("exe hashes %s (want exactly one)" % sorted(x[:12] for x in exes))
    paths = set(r.get("exe_path", "") for r in allr)
    if allr and (len(paths) != 1 or "" in paths):
        design.append("exe paths %s (want one: one staged read set)" % sorted(paths))
    norm = lambda e: {k: v for k, v in e.items() if k not in bp.VOLATILE}
    if allr and len(set(json.dumps(norm(r["env"]), sort_keys=True) for r in allr)) != 1:
        keys = sorted(set(k for r in allr for k in norm(r["env"])
                          if len(set(json.dumps(norm(x["env"]).get(k)) for x in allr)) != 1))
        design.append("the effective SR_* configuration differs beyond SR_NATIVE_DEVCREATE (%s)" % ", ".join(keys))
    unseeded = [r["roll"] for r in allr if r.get("seed") is None]
    if unseeded:
        design.append("save seeding not recorded for %s" % ", ".join(unseeded))
    elif allr and len(set(r["seed"] for r in allr)) != 1:
        design.append("save seeding differs (%s)" % sorted(set(r["seed"] for r in allr)))
    for i, (p, rr) in enumerate(zip(pairs, R)):
        for (arm, _), r in zip(p, rr):
            if not r.get("notrun") and r.get("gate") != arm:
                design.append("pair %d %s roll's gate state: %s" % (i + 1, arm.upper(), r.get("gate", "unknown")))
        if all(r.get("start") for r in rr) and not rr[0]["start"] < rr[1]["start"]:
            design.append("pair %d did not run in its declared order" % (i + 1))
    starts = [r.get("start") for r in flat if r.get("start")]
    if any(starts[k] >= starts[k + 1] for k in range(len(starts) - 1)):
        design.append("the rolls did not run in the declared sequence")
    # OPEN classes of the graded rolls; the stall events A / W (ON: OPEN; OFF late attach: the conditions); markers
    off_stalls = []
    for r in ran:
        if r["open"]:
            opens.append("%s: %s" % (r["roll"], ", ".join(r["open"])))
        late = isinstance(r.get("attach"), (int, float)) and not r["attach"] <= ATTACH_STALL
        if r["arm"] == "on" and late:
            opens.append("%s: ON attach at +%.2f s (> %.1f s: a late attach)" % (r["roll"], r["attach"], ATTACH_STALL))
        elif late:
            off_stalls.append("%s: OFF attach at +%.2f s (> %.1f s)" % (r["roll"], r["attach"], ATTACH_STALL))
        if r["arm"] == "on" and "U" in r and not r["U"] <= U_MAX:
            opens.append("%s: ON uncovered wait %.2f s > %.1f s" % (r["roll"], r["U"], U_MAX))
    probs = ["%s: %s" % (r["roll"], ", ".join(r["problems"])) for r in flat if r["problems"]]

    # paired differences (complete pairs only) and the stall cap - read whatever the design
    ends = {}
    lines = ["pair arm      W wait-st    attach       L      U      HUD    a2h  gate     roll"]
    if W is not None:
        lines.append("   0 WUP %s %-9s %s %s %s %s %s  %-8s %s  (warm-up: never graded)" % (
            fmt(W.get("W"), 6), W.get("waitst", "-"), fmt(W.get("attach"), 8), fmt(W.get("L")),
            fmt(W.get("U"), 6), fmt(W.get("hud"), 8), fmt(W.get("a2h"), 6), str(W.get("gate", "-"))[:8], warmup))
    for i, (p, rr) in enumerate(zip(pairs, R)):
        for (arm, path), r in zip(p, rr):
            lines.append("%4d %-3s %s %-9s %s %s %s %s %s  %-8s %s" % (
                i + 1, arm.upper(), fmt(r.get("W"), 6), r.get("waitst", "-"), fmt(r.get("attach"), 8),
                fmt(r.get("L")), fmt(r.get("U"), 6), fmt(r.get("hud"), 8), fmt(r.get("a2h"), 6),
                "not run" if r.get("notrun") else str(r.get("gate", "-"))[:8], path))
    for key, name, margin in ENDPOINTS:
        d = []
        for k, (p, rr) in enumerate(zip(pairs, R)):
            arms = [arm for arm, _ in p]
            if sorted(arms) != ["off", "on"]:
                continue
            on, off = rr[arms.index("on")], rr[arms.index("off")]
            if key in on and key in off:
                x = on[key] - off[key]
                d.append(x)
                if not x <= STALL_CAP:  # fail closed: a nan difference is over
                    opens.append("pair %d %s ON - OFF %s s over the %.1f s stall cap" % (k + 1, name, sgn(x), STALL_CAP))
                elif x < -STALL_CAP:
                    off_stalls.append("pair %d %s ON - OFF %s s (the OFF roll stalled)" % (k + 1, name, sgn(x)))
        m, s, ucb = stats_of(d)
        ends[key] = {"name": name, "margin": margin, "diffs": d, "n": len(d), "mean": m, "sd": s, "ucb95": ucb}
        lines.append("paired ON - OFF %-22s %s | n=%d mean %s sd %s ucb95 %s margin %.1f" % (
            name, " ".join("%+.2f" % x for x in d) or "-", len(d), sgn(m), fmt(s, 0), sgn(ucb), margin))
    # position, per endpoint: the first graded roll against the median of the other graded rolls of its arm
    position = []
    if pairs and R and R[0] and not R[0][0].get("notrun"):
        arm0, first = pairs[0][0][0], R[0][0]
        others = [r for p, rr in zip(pairs, R) for (a, _), r in zip(p, rr) if a == arm0 and r is not first]
        parts = []
        for key, name, margin in ENDPOINTS:
            med = median(r.get(key) for r in others)
            if isinstance(first.get(key), (int, float)) and med is not None:
                dev = first[key] - med
                parts.append("%s %s (median of the other %s rolls %s, dev %s)" % (
                    key, fmt(first[key], 0), arm0.upper(), fmt(med, 0), sgn(dev)))
                if not abs(dev) <= margin:
                    position.append("the first graded roll (pair 1 %s) %s %s s from its arm's median (> the %.1f s "
                                    "margin): a position effect" % (arm0.upper(), name, sgn(dev), margin))
        lines.append("position, per endpoint: %s%s" % ("; ".join(parts), (
            "; the warm-up (the invocation's first launch) HUD %s s" % fmt(W.get("hud"), 0))
            if W is not None and not W.get("notrun") else ""))
    complete = not design and not probs
    head = "design=%s pairs=%d" % (DESIGN_ID, len(pairs))
    if opens:
        verdict, why = "OPEN", "(kept, never discarded): " + "; ".join(opens) + (
            " | also missing / inconsistent evidence: " + "; ".join(design + probs) if design or probs else "") + (
            " | also an OFF-arm stall: " + "; ".join(off_stalls) if off_stalls else "")
    elif not complete:
        verdict, why = "INCONCLUSIVE", "(not evidence): " + "; ".join(design + probs)
    else:
        over = [("%s mean %s > margin %.1f" % (e["name"], sgn(e["mean"]), e["margin"]))
                for e in ends.values() if not e["mean"] <= e["margin"]]
        short = [("%s ucb95 %s >= margin %.1f (sd %s, n %d)" % (e["name"], sgn(e["ucb95"]), e["margin"],
                                                               fmt(e["sd"], 0), e["n"]))
                 for e in ends.values() if not e["ucb95"] < e["margin"]]
        if over:
            verdict, why = "OPEN", "(kept, never discarded): a mean regression over its margin: " + "; ".join(over)
        elif off_stalls or short or position:
            verdict, why = "INCONCLUSIVE", " | ".join(x for x in (
                ("an OFF-arm stall (the conditions were not the design's): " + "; ".join(off_stalls))
                if off_stalls else "",
                ("a position effect: " + "; ".join(position)) if position else "",
                ("under-powered as run (the paired spread exceeded the design's): " + "; ".join(short))
                if short else "") if x)
        else:
            verdict, why = "EQUIVALENT", (
                "within the tested conditions (exe %s, exe path %s, this scenario): the MEAN paired regression of every "
                "endpoint is below its margin at one-sided 95%% - %s. It does NOT bound an intermittent stall: no stall "
                "in these %d ON boots alone bounds a stall rate only below %.2f%% (exact, independent boots assumed); "
                "the stall claim is BOOT_STALL's" % (
                    sorted(exes)[0][:12], sorted(paths)[0], ", ".join(
                        "%s %s < %.1f" % (k, sgn(e["ucb95"]), e["margin"]) for k, e in ends.items()),
                    len(pairs), 100 * bs.upper95(len(pairs))))
    stem = TAG if verdict == "EQUIVALENT" else "BOOT_SET"
    line = "%s %s -> %s %s" % (stem, head, verdict, why)
    lines.append(line)
    clean = lambda r: {k: (v.isoformat() if isinstance(v, datetime) else v) for k, v in r.items() if k != "env"}
    dump = {"design": DESIGN_ID, "verdict": verdict, "tag": TAG if verdict == "EQUIVALENT" else None,
            "pairs": len(pairs), "registered_pairs": N_PAIRS, "baked": BAKED, "stall_cap": STALL_CAP, "u_max": U_MAX,
            "attach_stall": ATTACH_STALL, "margins": {k: m for k, _n, m in ENDPOINTS},
            "declared": [[[arm, os.path.abspath(p)] for arm, p in pp] for pp in pairs],
            "warmup_roll": os.path.abspath(warmup) if warmup else None, "proof": run["proof"],
            "endpoints": ends, "open": opens, "design_problems": design, "marker_problems": probs,
            "position": position, "warmup": clean(W) if W else None, "rolls": [[clean(r) for r in rr] for rr in R],
            "line": line}
    return lines, line, dump


def parse_pair(tokens):
    out = []
    for tok in tokens:
        arm, _, path = tok.partition(":")
        if arm.lower() not in ("on", "off") or not path:
            raise SystemExit("bad pair token %r (want on:<roll> or off:<roll>)" % tok)
        out.append((arm.lower(), path))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pair", nargs=2, action="append", required=True, metavar=("ARM:ROLL", "ARM:ROLL"))
    ap.add_argument("--warmup", help="the declared warm-up roll (run before pair 1, never graded)")
    ap.add_argument("--design", default="dcl-v2", choices=sorted(DESIGNS),
                    help="the registered design: dcl-v2 (8 pairs, the gate default OFF) or dcf-v1 (4 pairs, the bake: "
                         "the gate default ON, OFF = SR_NATIVE_DEVCREATE=0)")
    ap.add_argument("--out")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    select_design(a.design)
    pairs = [parse_pair(p) for p in a.pair]
    lines, _line, dump = evaluate(pairs, a.warmup)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dump, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
