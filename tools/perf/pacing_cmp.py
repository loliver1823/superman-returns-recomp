#!/usr/bin/env python3
"""pacing_cmp.py - p2.transport's `pacing` check: the composed transport's present pacing against a control roll's,
from the two rolls' P2 trace summaries (tools/perf/p2_trace.py --json, its `pacing` block: the kind-29 records -
dev+0x3B14 vblanks, dev+0x3B20 gate count, dev+0x4E4C throttle wait - one per present, and their samples).

    python tools/perf/pacing_cmp.py <control summary.json> <target summary.json>
                                    [--calibration <second control summary.json>] [--json out.json]

THE ORACLE (NATIVE_ROADMAP P2.7 item 5: "vblanks per present, the gate count and the throttle wait must match between
ring and ringless"; the orchestrator's decision of 2026-10-14: the composed roll against the all-OFF roll, the Xenia
golden reported only). Both rolls run the console pacing path (SR_HIGHFPS=0), uninstrumented.

PRE-REGISTERED (2026-10-14, before any composed roll; revised after Astra's rounds 1-3 the same day). Every check has
three outcomes, and uncertainty never becomes acceptance:
  * a STATISTICAL check bootstraps the difference target - control (BOOT resamples of each roll, a fixed seed so a
    verdict is reproducible) and takes its one-sided CONF (95%) bounds [lower, upper]. It PASSES only when the upper
    bound of the regression is inside the practical margin (two-sided checks: both bounds inside +-margin); it FAILS
    when the lower bound is beyond the margin (clear exceedance); anything else - overlapping bounds - is
    INCONCLUSIVE. The practical margins: the vblanks mean +-0.05; the share of presents above the control's p99
    vblanks +0.5 points; the hitch RATE (presents at >= HITCH_V = 6 vblanks, ~100 ms at 60 Hz, over the pairs) +0.2
    points; the doubled-gate proportion +0.5 points; the throttle wait p50 +-max(15%, 64); p95 / p99 by their
    exceedance dual - the share of presents slower than the control's p95 / p99 + max(25%, 64) - +1 / +0.5 points (a
    quantile of a two-valued distribution flips at its mixing fraction; the share does not); the share above 2x the
    control's p99 + 64 +0.5 points;
  * a FIXED check passes or fails: severity - the max vblanks per present at most max(the control's max, HITCH_V) +
    MAX_ALLOW_V (2), the max throttle wait at most WAIT_MAX_X (4x) the control's + 64 (a single hitch is a regression
    however rare); sanity - coverage (>= MIN_PAIRS consecutive-id pairs, <= 1% of the window's ids missing, no duplicate
    id), gates per present within 0.02 of 1 in each roll (82111E38 runs once per gated present), the vblank
    distributions' total variation distance <= 0.05.
VERDICT: any FAIL -> FAIL (a hard failure stays a FAIL, whatever the calibration says); else any INCONCLUSIVE ->
INCONCLUSIVE; else PASS. CALIBRATION (--calibration): a second all-OFF control from the same session graded against the
first under the same rules; it only gates PASS - a would-be PASS whose calibration is not a PASS is INCONCLUSIVE.
SAMPLE SIZE: for every statistical check the output states the presents a roll needs for the check to PASS with 95%
probability when nothing changed, n * (2 * 1.645 * se / margin)^2 from the bootstrap's own standard error. The plan's
pacing window PLAN_WINDOW (28,672 present ids) is sized from these on the calibration fixtures (test_pacing.py: the
doubled-gate proportion at a 2.5% base rate needs the most, ~21,000-24,000); at 2,048 presents most same-distribution
pairs are INCONCLUSIVE, never PASS.
Exit 0 = PASS, 1 = FAIL, 2 = a summary without a pacing block or its samples, 3 = INCONCLUSIVE.
"""
import argparse
import json
import math
import sys

import numpy as np

MIN_PAIRS, MAX_MISSING_SHARE = 1000, 0.01
GATE_TOL, VBL_TVD = 0.02, 0.05
BOOT, CONF, SEED, CHUNK = 1000, 0.95, 20261014, 100
Z = 1.6448536269514722  # the one-sided 95% normal quantile (for the sample-size statement)
# practical margins
VBL_MEAN_M, TAIL_M, TAIL95_M, HITCH_M, DOUBLES_M = 0.05, 0.005, 0.01, 0.002, 0.005
# the pacing window the plan's rolls use (present ids): sized so that every statistical check is conclusive with 95%
# probability on the calibration fixtures (presents_needed; test_pacing.py) - the doubled-gate proportion at a 2.5%
# base rate needs the most
PLAN_WINDOW = 28672
WAIT_ABS, WAIT_P50_REL, WAIT_TAIL_REL, SPIKE_X = 64.0, 0.15, 0.25, 2.0
# severity
HITCH_V, MAX_ALLOW_V, WAIT_MAX_X = 6, 2, 4.0
GOLDEN_VBL_GAMEPLAY = 2.48
QS = (0.5, 0.95, 0.99)


def pacing_of(path):
    d = json.load(open(path, encoding="utf-8"))
    p = d.get("pacing") or {}
    s = p.get("samples") or {}
    return p if p.get("pairs") and s.get("vblanks") and s.get("wait") else None


def tvd(a, b):
    """total variation distance between two samples' value distributions (0 = identical, 1 = disjoint)."""
    va, ca = np.unique(a, return_counts=True)
    vb, cb = np.unique(b, return_counts=True)
    keys = np.union1d(va, vb)
    pa = np.zeros(keys.size)
    pb = np.zeros(keys.size)
    pa[np.searchsorted(keys, va)] = ca / float(a.size)
    pb[np.searchsorted(keys, vb)] = cb / float(b.size)
    return 0.5 * float(np.abs(pa - pb).sum())


def boot(x, rng, stats):
    """The bootstrap of statistics of one sample, drawn as multinomial counts over its distinct values (exactly a
    resample with replacement, in O(distinct values) memory): stats = {name: f(vals, counts (B, k), n) -> (B,)}."""
    vals, cnt = np.unique(x, return_counts=True)
    n = int(x.size)
    out = {k: [] for k in stats}
    for lo in range(0, BOOT, CHUNK):
        draws = rng.multinomial(n, cnt / float(n), size=min(CHUNK, BOOT - lo))
        for k, f in stats.items():
            out[k].append(f(vals, draws, n))
    return {k: np.concatenate(v) for k, v in out.items()}


def share_above(thr):
    return lambda vals, d, n: d[:, vals > thr].sum(axis=1) / float(n)


def share_at_least(thr):
    return lambda vals, d, n: d[:, vals >= thr].sum(axis=1) / float(n)


def mean_of(vals, d, n):
    return (d * vals[None, :]).sum(axis=1) / float(n)


def quantile_of(q):
    """numpy's default (linear) quantile of each resample, from its counts."""
    def f(vals, d, n):
        cum = d.cumsum(axis=1)
        h = (n - 1) * q
        i, j = int(math.floor(h)), int(math.ceil(h))
        lo = vals[np.argmax(cum > i, axis=1)]
        hi = vals[np.argmax(cum > j, axis=1)]
        return lo + (hi - lo) * (h - i)
    return f


def compare(control, target, seed=SEED):
    rng = np.random.default_rng(seed)
    cs, ts = control["samples"], target["samples"]
    cv, tv = np.asarray(cs["vblanks"], dtype=np.int64), np.asarray(ts["vblanks"], dtype=np.int64)
    cg = np.asarray(cs.get("gates") or [1] * cv.size, dtype=np.int64)
    tg = np.asarray(ts.get("gates") or [1] * tv.size, dtype=np.int64)
    cw, tw = np.asarray(cs["wait"], dtype=np.float64), np.asarray(ts["wait"], dtype=np.float64)
    states, stats = {}, {}

    v99 = float(np.quantile(cv, 0.99))
    cq = np.quantile(cw, QS)
    thr95 = float(cq[1]) + max(WAIT_TAIL_REL * abs(float(cq[1])), WAIT_ABS)
    thr99 = float(cq[2]) + max(WAIT_TAIL_REL * abs(float(cq[2])), WAIT_ABS)
    spike = SPIKE_X * float(cq[2]) + WAIT_ABS
    vstats = {"mean": mean_of, "tail": share_above(v99), "hitch": share_at_least(HITCH_V)}
    gstats = {"double": share_above(1)}
    wstats = {"p50": quantile_of(0.5), "x95": share_above(thr95), "x99": share_above(thr99),
              "spike": share_above(spike)}
    bc = {**boot(cv, rng, vstats), **boot(cg, rng, gstats), **boot(cw, rng, wstats)}
    bt = {**boot(tv, rng, vstats), **boot(tg, rng, gstats), **boot(tw, rng, wstats)}
    n_of = {"mean": cv.size, "tail": cv.size, "hitch": cv.size, "double": cg.size, "p50": cw.size, "x95": cw.size,
            "x99": cw.size, "spike": cw.size}

    def graded(name, key, point, margin, two_sided):
        diff = bt[key] - bc[key]
        lo, hi = float(np.quantile(diff, 1.0 - CONF)), float(np.quantile(diff, CONF))
        se = float(diff.std())
        if two_sided:
            state = "FAIL" if lo > margin or hi < -margin else "PASS" if -margin <= lo and hi <= margin else "INCONCLUSIVE"
        else:
            state = "FAIL" if lo > margin else "PASS" if hi <= margin else "INCONCLUSIVE"
        # the presents a roll needs for this check to PASS with 95% probability when nothing changed
        need = int(math.ceil(n_of[key] * (2 * Z * se / margin) ** 2)) if margin > 0 else None
        stats[name] = {"diff": round(point, 6), "lower": round(lo, 6), "upper": round(hi, 6), "margin": margin,
                       "se": round(se, 6), "presents_needed": need}
        states[name] = state

    def fixed(name, ok):
        states[name] = "PASS" if ok else "FAIL"

    # --- sanity (fixed): coverage, one gate per present, the vblank distribution ---
    for side, p in (("control", control), ("target", target)):
        expected = p.get("window_expected", 0)
        fixed(side + "_pairs", p["pairs"] >= MIN_PAIRS and expected > 0 and
              p.get("missing_presents", expected) <= MAX_MISSING_SHARE * expected)
        fixed(side + "_no_duplicate_ids", p.get("duplicate_ids", 1) == 0)
        fixed(side + "_one_gate_per_present", abs(p["gates_per_present"]["mean"] - 1.0) <= GATE_TOL)
    dist = tvd(cv, tv)
    stats["vblanks_tvd"] = round(dist, 4)
    fixed("vblanks_distribution_match", dist <= VBL_TVD)

    # --- vblanks: the mean (two-sided), the tail above the control's p99, the hitch rate (one-sided) ---
    graded("vblanks_match", "mean", float(tv.mean() - cv.mean()), VBL_MEAN_M, True)
    graded("vblanks_tail_match", "tail", float((tv > v99).mean() - (cv > v99).mean()), TAIL_M, False)
    graded("vblanks_hitch_rate_match", "hitch", float((tv >= HITCH_V).mean() - (cv >= HITCH_V).mean()), HITCH_M, False)
    # --- vblanks: severity (fixed) ---
    cmax, tmax = float(cv.max()), float(tv.max())
    stats["vblanks_max"] = [cmax, tmax]
    stats["vblanks_hitches"] = [int((cv >= HITCH_V).sum()), int((tv >= HITCH_V).sum())]
    fixed("vblanks_max_match", tmax <= max(cmax, HITCH_V) + MAX_ALLOW_V)

    # --- gates: the doubled proportion (one-sided) ---
    graded("gate_doubles_match", "double", float((tg > 1).mean() - (cg > 1).mean()), DOUBLES_M, False)

    # --- the throttle wait: p50 (two-sided), p95 / p99 / the spike share (one-sided), the max (fixed) ---
    tq = np.quantile(tw, QS)
    graded("throttle_wait_match", "p50", float(tq[0] - cq[0]), max(WAIT_P50_REL * abs(cq[0]), WAIT_ABS), True)
    # p95 / p99 by their exceedance dual (a quantile of a two-valued distribution flips at its mixing fraction, so a
    # bootstrap of the quantile itself is unresolvable there): the share of the target's presents slower than the
    # control's p95 / p99 + max(25%, 64) may exceed the control's own share there by at most the tail margin
    graded("throttle_wait_p95_match", "x95", float((tw > thr95).mean() - (cw > thr95).mean()), TAIL95_M, False)
    graded("throttle_wait_p99_match", "x99", float((tw > thr99).mean() - (cw > thr99).mean()), TAIL_M, False)
    graded("throttle_wait_spikes_match", "spike", float((tw > spike).mean() - (cw > spike).mean()), TAIL_M, False)
    stats["throttle_wait_quantiles"] = {"control": [float(x) for x in cq], "target": [float(x) for x in tq],
                                        "p95_threshold": thr95, "p99_threshold": thr99}
    wmax = [float(cw.max()), float(tw.max())]
    stats["throttle_wait_max"] = wmax
    stats["throttle_wait_spike_threshold"] = spike
    fixed("throttle_wait_max_match", wmax[1] <= WAIT_MAX_X * wmax[0] + WAIT_ABS)

    vals = set(states.values())
    verdict = "FAIL" if "FAIL" in vals else "INCONCLUSIVE" if "INCONCLUSIVE" in vals else "PASS"
    needed = [s["presents_needed"] for s in stats.values() if isinstance(s, dict) and s.get("presents_needed")]
    return {"verdict": verdict, "states": states, "checks": {k: v == "PASS" for k, v in states.items()},
            "stats": stats, "presents_needed": max(needed) if needed else None,
            "presents": [int(cv.size), int(tv.size)],
            "vblanks_delta": round(float(tv.mean() - cv.mean()), 4),
            "throttle_wait_p50": [float(cq[0]), float(tq[0])],
            "golden_info": {"xenia_gameplay_vblanks_per_frame": GOLDEN_VBL_GAMEPLAY,
                            "control": control["vblanks_per_present"]["mean"],
                            "target": target["vblanks_per_present"]["mean"]}}


def judge(control, target, calibration=None):
    """compare(control, target); the calibration (control vs a second control) only gates a PASS: a FAIL stays a
    FAIL, a would-be PASS with a calibration that is not a PASS is INCONCLUSIVE."""
    out = compare(control, target)
    if calibration is not None:
        cal = compare(control, calibration)
        out["calibration"] = {k: cal[k] for k in ("verdict", "states", "checks", "stats", "presents_needed")}
        if out["verdict"] == "PASS" and cal["verdict"] != "PASS":
            out["composed_verdict"] = out["verdict"]
            out["verdict"] = "INCONCLUSIVE"
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("control")
    ap.add_argument("target")
    ap.add_argument("--calibration", help="a second all-OFF control summary from the same session")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    c, t = pacing_of(a.control), pacing_of(a.target)
    k = pacing_of(a.calibration) if a.calibration else None
    if not c or not t or (a.calibration and not k):
        out = {"verdict": "NO_DATA", "control": bool(c), "target": bool(t)}
        if a.calibration:
            out["calibration"] = bool(k)
        rc = 2
    else:
        out = judge(c, t, k)
        rc = {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 3}[out["verdict"]]
    print(json.dumps(out, indent=1))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
