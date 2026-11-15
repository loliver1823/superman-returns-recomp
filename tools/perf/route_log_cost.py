#!/usr/bin/env python3
"""route_log_cost.py - the pre-registered overhead check of the a2 log pin (the coordinator, 2026-10-22;
s4-audit archive/step4_claude_20261018/FLIGHT_ROUTE_DESIGN.md section 8). Every a2 roll of ours carries
canary_routes.A2_LOG_PIN (SR_LOG=dev). Does that move the a2 flight scene's fps by more than K's own spread?

    python tools/perf/route_log_cost.py --route <frozen a2 route file> --route-sha256 <its frozen sha256>
        --with <3 run dirs> --without <3 run dirs> --out <result.json>

The runs are canary_compare run folders of route_repeat's k experiment: the K exe 12da45b9c7d5-canary, config
ours-1x-unc, the frozen route file, tape and scene windows, no --survey and no --extra-gates.
- The WITH arm carries the log pin (run.json log_pin = A2_LOG_PIN, log_pin_off false).
- The WITHOUT arm is --no-a2-log-pin (log_pin null, log_pin_off true).
- 3 + 3 distinct runs (folders and contents), launched INTERLEAVED: ordered by each run's first event time, the arms
  alternate (either arm first).
- Evidence (route_repeat.load_run's rules): every run's flight fps is measured (the probe lost no record and its
  presents cover the window) and its flight shot is present and not blank.
Anything else is INVALID (exit 2).

The statistic. F = the flight scene's present fps (canary_compare's per-scene fps, the quantity route_repeat's CV uses).
  D = mean(F, with) - mean(F, without)
  S = max(F, without) - min(F, without)   (K's spread without the pin)
STANDS if |D| <= S (the pin stands); else BACK (it goes back to the coordinator before any target is set). Both are
compared unrounded. Reported, not graded: every scene's mean fps per arm and their difference, and the with arm's
spread. The flight-view criterion (audit/route/flight_view_check.py, frozen) is run on the same rolls beside this
tool: F1 on all six, F2 / F3 on the with arm.
Exit 0 STANDS, 1 BACK, 2 INVALID or unusable input.
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import canary_routes as routes  # noqa: E402
import route_repeat as rr  # noqa: E402

ARM_N = 3
SCENES = ("street", "on_foot") + routes.POST_SCENES


def first_event_t(run_dir):
    """The run's first event time (canary_compare's Events: time.time()); None when no event has one."""
    with open(os.path.join(run_dir, "events.jsonl"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                t = json.loads(line).get("t")
                if rr._finite(t):
                    return float(t)
    return None


def arm_pins(route_path, route_sha256):
    """(with, without): route_repeat's k experiment at N = 3 per arm; the without arm has no log pin."""
    k = rr.pin_of(route_path, route_sha256, "k")
    with_pin = dict(k, n=ARM_N)
    return with_pin, dict(with_pin, log_pin=None, log_pin_off=True)


def problems_of(with_runs, without_runs, with_pin, without_pin):
    """Why these rolls are not the pre-registered check ([] = they are)."""
    out = rr.identity_problems(with_runs, with_pin) + rr.identity_problems(without_runs, without_pin)
    for r in without_runs:
        if r.get("log_pin_off") is not True:
            out.append(f"{os.path.basename(r['run'])}: not a --no-a2-log-pin run (log_pin_off {r.get('log_pin_off')!r})")
    both = with_runs + without_runs
    if len({r["realpath"] for r in both}) != len(both) or len({r["content"] for r in both}) != len(both):
        out.append("a run is in both arms, or two folders hold the same run")
    order = sorted(((r["t_first"], arm, os.path.basename(r["run"])) for arm, rs in (("with", with_runs),
                    ("without", without_runs)) for r in rs), key=lambda x: (x[0] is None, x[0] or 0.0))
    if any(t is None for t, _, _ in order):
        out.append("a run has no event time (the interleaving cannot be checked)")
    elif len({t for t, _, _ in order}) != len(order):
        # a tie has no order: a stable sort would invent one (Astra pin r1 #4)
        out.append("two runs share a first event time (the interleaving cannot be checked)")
    elif any(order[i][1] == order[i + 1][1] for i in range(len(order) - 1)):
        out.append("the arms were not interleaved: " + ", ".join(f"{n} ({a})" for _, a, n in order))
    for r in both:
        name = os.path.basename(r["run"])
        if r["fps"].get("flight") is None:
            out.append(f"{name}: no measured flight fps (probe lost {r.get('probe_lost')!r}, coverage "
                       f"{r['coverage'].get('flight')})")
        if "flight" not in r["shots"]:
            out.append(f"{name}: no flight shot ({'blank' if 'flight' in r['blank_shots'] else 'missing'})")
    return out


def grade(with_runs, without_runs):
    """The verdict of rolls that ARE the check (problems_of(...) == [])."""
    fw = [r["fps"]["flight"] for r in with_runs]
    fn = [r["fps"]["flight"] for r in without_runs]
    d = statistics.mean(fw) - statistics.mean(fn)
    s = max(fn) - min(fn)
    scenes = {}
    for scene in SCENES:
        vw = [r["fps"].get(scene) for r in with_runs]
        vn = [r["fps"].get(scene) for r in without_runs]
        ok = all(v is not None for v in vw + vn)
        scenes[scene] = {"with": vw, "without": vn,
                         "mean_with": rr._r(statistics.mean(vw)) if ok else None,
                         "mean_without": rr._r(statistics.mean(vn)) if ok else None,
                         "difference": rr._r(statistics.mean(vw) - statistics.mean(vn)) if ok else None}
    return {"verdict": "STANDS" if abs(d) <= s else "BACK", "flight_with": fw, "flight_without": fn,
            "difference": d, "spread_without": s, "spread_with": max(fw) - min(fw), "scenes": scenes,
            "rule": "STANDS if |mean(F, with) - mean(F, without)| <= max(F, without) - min(F, without), unrounded"}


def load(run_dir):
    r = rr.load_run(run_dir)
    try:
        r["t_first"] = first_event_t(run_dir)
    except (OSError, ValueError) as error:
        raise rr.Unusable(f"{run_dir}: {error}") from error
    return r


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--route", required=True, help="the frozen a2 route file")
    ap.add_argument("--route-sha256", required=True, help="its frozen sha256 (FLIGHT_ROUTE_DESIGN.md)")
    ap.add_argument("--with", dest="with_runs", nargs="+", required=True, help="the log-pinned rolls")
    ap.add_argument("--without", dest="without_runs", nargs="+", required=True, help="the --no-a2-log-pin rolls")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        with_pin, without_pin = arm_pins(a.route, a.route_sha256)
        with_runs = [load(d) for d in a.with_runs]
        without_runs = [load(d) for d in a.without_runs]
    except (rr.Unusable, OSError, ValueError, KeyError) as error:
        print(f"route_log_cost: unusable input: {error}", file=sys.stderr)
        return 2
    bad = problems_of(with_runs, without_runs, with_pin, without_pin)
    result = {"verdict": "INVALID", "problems": bad} if bad else grade(with_runs, without_runs)
    result["experiment"] = rr._pin_record(with_pin)
    result["runs"] = {"with": [os.path.abspath(d) for d in a.with_runs],
                      "without": [os.path.abspath(d) for d in a.without_runs]}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    if bad:
        print(f"route_log_cost: INVALID {bad}")
        return 2
    print(f"flight fps with {result['flight_with']} without {result['flight_without']}: difference "
          f"{result['difference']:.3f}, spread without {result['spread_without']:.3f} -> {result['verdict']}")
    return 0 if result["verdict"] == "STANDS" else 1


if __name__ == "__main__":
    sys.exit(main())
