#!/usr/bin/env python3
"""Tests for p2_trace.py's pacing block (kind 29) and pacing_cmp.py, on synthetic records (no trace file, no game).

    python tools/perf/test_pacing.py

The rule has three outcomes (Astra round 3): a statistical check PASSES only when its bootstrap bound on the
regression is inside the practical margin, FAILS when the bound is clearly beyond it, and is INCONCLUSIVE otherwise.
Negative controls must FAIL their named check: regressions that keep the mean and the median (Astra round 1: steady
2 vblanks against alternating 0 / 4, a 100x wait spike on 25% of presents, a 2% vblank tail, 10x spikes on 2%, a
doubled-gate rate several times the control's, 100,000 missing presents, duplicate ids), severity (Astra round 2: one
100-vblank frame in 2,048, a 200-vblank stall with 99 zero catch-up frames, pacing records that stop early), a hard
failure with an unrelated calibration failure (Astra round 3: still a FAIL). Underpowered differences are
INCONCLUSIVE, never PASS (Astra round 3: 50 -> 75 delayed gates and 20 -> 35 slow waits in 2,048), and the same
differences at the plan's window are FAILs. CALIBRATION: independent same-distribution pairs at the plan's window
(vblank jitter WITH 6+ vblank hitches, ~2.5% delayed gate ISRs, a 99% / 1% wait mixture or a lognormal one) pass at
least 95% of the time and never FAIL; hitch rates compare across unequal windows.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import p2_trace as pt  # noqa: E402
import pacing_cmp as pc  # noqa: E402

N = pt.PACING_TAIL  # pairs in a default window
W = pc.PLAN_WINDOW  # pairs in the plan's window


def records(n, vbl=lambda k: 2, gates=lambda k: 1, wait=lambda k: 300, lead=500, skip=(), dup=(), first_id=0,
            presents_to=None):
    """lead presents of boot pacing (one vblank, no gate, no wait), then n gameplay presents; vbl / gates / wait give
    gameplay present k's vblanks elapsed, gates counted and throttle wait; skip: gameplay indices with no record;
    dup: gameplay indices recorded twice; presents_to: kind-13 present records for every id up to this one (the
    presents the trace saw, independent of the kind-29 stream)."""
    rows, v, g = [], 1000, 7
    for i in range(lead + n):
        k = i - lead
        if k >= 0:
            v += vbl(k)
            g += gates(k)
            w = wait(k)
        else:
            v += 1
            w = 0
        if k in skip:
            continue
        rows.append((first_id + i, 29, v, g, w))
        if k in dup:
            rows.append((first_id + i, 29, v + 7, g, w))  # a second record of the same id (the first is used)
    if presents_to is not None:
        rows += [(i, 13, 0, 0, 0) for i in range(first_id, presents_to + 1)]
    recs = np.zeros(len(rows), dtype=pt.REC)
    for j, (pres, kd, vb, ga, wt) in enumerate(rows):
        recs[j]["present"], recs[j]["kind"], recs[j]["f"], recs[j]["a"], recs[j]["b"] = pres, kd, vb, ga, wt
    return recs


def trace(n, **kw):
    recs = records(n, **kw)
    return pt.pacing(recs, recs["kind"])


def block(vbl, gates, wait):
    """a pacing block built straight from samples (a full window, nothing missing)."""
    vbl, gates, wait = np.asarray(vbl), np.asarray(gates), np.asarray(wait)
    return {"pairs": int(vbl.size), "window_expected": int(vbl.size) + 1, "missing_presents": 0, "duplicate_ids": 0,
            "vblanks_per_present": {"mean": float(vbl.mean())},
            "gates_per_present": {"mean": float(gates.mean()), "over_one": int((gates > 1).sum())},
            "samples": {"vblanks": vbl.tolist(), "gates": gates.tolist(), "wait": wait.tolist()}}


def delayed(n, count, rng=None):
    """gates per pair with `count` delayed ISRs (0 then 2: the mean stays 1), at random or evenly spread positions."""
    g = np.ones(n, dtype=int)
    if rng is None:
        pos = np.linspace(0, n - 2, count).astype(int) // 2 * 2
    else:
        pos = np.sort(rng.choice(np.arange(0, n - 1, 2), size=count, replace=False))
    g[pos], g[pos + 1] = 0, 2
    return g


def scaled(count):
    """a count in a 2,048-pair window scaled to the plan's window (the same rate)."""
    return int(round(count * W / float(N)))


def slow(n, count, base=300, value=900):
    """n + 1 waits with `count` slow ones evenly spread."""
    w = np.full(n + 1, base)
    w[np.linspace(0, n, count).astype(int)] = value
    return w


def same_distribution(rng, continuous, n=None):
    """one roll of the calibration fixture: vblank jitter with 6+ vblank hitches (~0.1%), ~2.5% delayed gate ISRs,
    a 99% / 1% wait mixture or a lognormal wait."""
    n = n or W
    vbl = rng.choice([1, 2, 3, 6, 7, 8], size=n, p=[0.01, 0.969, 0.02, 0.0005, 0.0003, 0.0002])
    gates = delayed(n, int(rng.binomial(n // 2, 0.05)), rng)
    if continuous:
        wait = np.round(rng.lognormal(np.log(300.0), 0.3, size=n + 1))
    else:
        wait = np.where(rng.random(n + 1) < 0.01, 900, 300)
    return block(vbl, gates, wait)


def run(name, fn):
    try:
        fn()
        print("PASS " + name)
        return 0
    except AssertionError as e:
        print("FAIL %s: %s" % (name, str(e)[:600]))  # (a summary's samples would run to thousands of numbers)
        return 1


def not_pass(r):
    return {k: v for k, v in r["states"].items() if v != "PASS"}


def main():
    fails = 0
    base = trace(3000)

    def block_shape():
        assert base["pairs"] == N and base["missing_presents"] == 0 and base["duplicate_ids"] == 0, base
        assert base["vblanks_per_present"]["mean"] == 2.0 and base["gates_per_present"]["mean"] == 1.0, base
        assert base["gates_per_present"]["over_one"] == 0 and base["throttle_wait"]["p50"] == 300, base
        assert len(base["samples"]["vblanks"]) == N and len(base["samples"]["wait"]) == N + 1
        assert base["whole_run_vblanks_mean"] < 2.0, base  # the boot lead is outside the graded window only
    fails += run("the pacing block over the gameplay tail (boot outside the window)", block_shape)

    def same():
        r = pc.compare(base, trace(3000))
        assert r["verdict"] == "PASS", not_pass(r)
    fails += run("identical pacing passes", same)

    # --- calibration at the plan's window ---
    def calibration_rate():
        rng = np.random.default_rng(7)
        verdicts, why = {}, {}
        for i in range(200):
            r = pc.compare(same_distribution(rng, bool(i % 2)), same_distribution(rng, bool(i % 2)))
            verdicts[r["verdict"]] = verdicts.get(r["verdict"], 0) + 1
            for k, v in not_pass(r).items():
                why[(k, v)] = why.get((k, v), 0) + 1
        print("    same-distribution pairs at the plan window %d: %s; non-PASS checks %s" % (W, verdicts, why))
        assert verdicts.get("PASS", 0) >= 190 and not verdicts.get("FAIL"), (verdicts, why)
    fails += run("independent same-distribution pairs (hitches included) pass >= 95% at the plan window, never FAIL",
                 calibration_rate)

    def window_suffices():
        rng = np.random.default_rng(3)
        needs = [pc.compare(same_distribution(rng, bool(i % 2)), same_distribution(rng, bool(i % 2)))["presents_needed"]
                 for i in range(10)]
        assert max(needs) <= W, (needs, W)
    fails += run("the plan's window covers the presents every check needs on the calibration fixtures", window_suffices)

    def hitch_rates_with_exposure():
        # Astra round 3: 10 six-vblank hitches among 1,024 pairs against 20 among 2,048 (the same rate), and 20
        # against 22 at equal length: never a FAIL (rates with exposure and uncertainty, not raw counts)
        def hitches(n, count):
            v = np.full(n, 2)
            v[np.linspace(0, n - 1, count).astype(int)] = 6
            return v
        for (n1, c1), (n2, c2) in (((2048, 20), (1024, 10)), ((2048, 20), (2048, 22))):
            r = pc.compare(block(hitches(n1, c1), [1] * n1, [300] * (n1 + 1)),
                           block(hitches(n2, c2), [1] * n2, [300] * (n2 + 1)))
            assert r["states"]["vblanks_hitch_rate_match"] != "FAIL" and r["verdict"] != "FAIL", not_pass(r)
        # at the calibration fixture's ~0.1% hitch rate, a 10% relative difference at the plan window PASSES (a 1%
        # hitch rate - Astra's examples - would need ~53,000 presents for the 0.2-point margin: INCONCLUSIVE there)
        r = pc.compare(block(hitches(W, 29), [1] * W, [300] * (W + 1)), block(hitches(W, 32), [1] * W, [300] * (W + 1)))
        assert r["verdict"] == "PASS", not_pass(r)
    fails += run("hitch RATES compare across unequal windows (never a FAIL); 29 vs 32 at the plan window PASS",
                 hitch_rates_with_exposure)

    # --- uncertainty is never acceptance (Astra round 3) ---
    def underpowered_gates():
        r = pc.compare(block([2] * N, delayed(N, 50), [300] * (N + 1)), block([2] * N, delayed(N, 75), [300] * (N + 1)))
        assert r["states"]["gate_doubles_match"] == "INCONCLUSIVE" and r["verdict"] == "INCONCLUSIVE", \
            (r["states"]["gate_doubles_match"], r["stats"]["gate_doubles_match"])
        r = pc.compare(block([2] * W, delayed(W, scaled(50)), [300] * (W + 1)),
                       block([2] * W, delayed(W, scaled(75)), [300] * (W + 1)))
        assert r["states"]["gate_doubles_match"] == "FAIL", r["stats"]["gate_doubles_match"]
    fails += run("50 -> 75 delayed gates in 2,048 is INCONCLUSIVE (not PASS); the same rates at the plan window FAIL",
                 underpowered_gates)

    def underpowered_waits():
        r = pc.compare(block([2] * N, [1] * N, slow(N, 20)), block([2] * N, [1] * N, slow(N, 35)))
        assert r["verdict"] == "INCONCLUSIVE" and "FAIL" not in r["states"].values(), not_pass(r)
        r = pc.compare(block([2] * W, [1] * W, slow(W, scaled(20))), block([2] * W, [1] * W, slow(W, scaled(35))))
        assert r["verdict"] == "FAIL", not_pass(r)
    fails += run("20 -> 35 slow waits in 2,048 is INCONCLUSIVE (not PASS); the same rates at the plan window FAIL",
                 underpowered_waits)

    def round2_examples_at_window():
        # Astra round 2's false failures, at the plan's window: 20 vs 22 slow waits, 50 vs 55 delayed gate pairs
        r = pc.compare(block([2] * W, [1] * W, slow(W, scaled(20))), block([2] * W, [1] * W, slow(W, scaled(22))))
        assert r["verdict"] == "PASS", not_pass(r)
        r = pc.compare(block([2] * W, delayed(W, scaled(50)), [300] * (W + 1)),
                       block([2] * W, delayed(W, scaled(55)), [300] * (W + 1)))
        assert r["verdict"] == "PASS", not_pass(r)
    fails += run("Astra round 2's sampling differences at the plan window PASS", round2_examples_at_window)

    # --- gates ---
    def double():
        r = pc.compare(base, trace(3000, gates=lambda k: 2))
        assert r["states"]["target_one_gate_per_present"] == "FAIL" and r["verdict"] == "FAIL", not_pass(r)
    fails += run("negative: a gate applied twice on every present fails", double)

    def doubles_rate():
        r = pc.compare(block([2] * N, delayed(N, 50), [300] * (N + 1)), block([2] * N, delayed(N, 150), [300] * (N + 1)))
        assert r["states"]["gate_doubles_match"] == "FAIL" and r["verdict"] == "FAIL", r["stats"]["gate_doubles_match"]
    fails += run("negative: delayed gate pairs tripled (2.4% -> 7.3%, the mean kept) fail", doubles_rate)

    def doubles_as_proportions():
        # Astra round 1: 100 doubled among 2,048 pairs (control) against 100 among 1,024 (target)
        r = pc.compare(block([2] * N, delayed(N, 100), [300] * (N + 1)),
                       block([2] * 1024, delayed(1024, 100), [300] * 1025))
        assert r["states"]["gate_doubles_match"] == "FAIL", r["stats"]["gate_doubles_match"]
    fails += run("negative: the same doubled count in half the window fails (proportions, not counts)",
                 doubles_as_proportions)

    # --- vblanks ---
    def rate():
        r = pc.compare(base, trace(3000, vbl=lambda k: 3))
        assert r["states"]["vblanks_match"] == "FAIL", r["stats"]["vblanks_match"]
    fails += run("negative: three vblanks a present against two fails", rate)

    def alternating():
        t = trace(3000, vbl=lambda k: 0 if k % 2 else 4)
        assert t["vblanks_per_present"]["mean"] == 2.0 and t["vblanks_per_present"]["p50"] == 2.0
        r = pc.compare(base, t)
        assert r["states"]["vblanks_distribution_match"] == "FAIL" and r["verdict"] == "FAIL", not_pass(r)
    fails += run("negative: alternating 0 / 4 vblanks against steady 2 (same mean and median) fails", alternating)

    def tail():
        # 2% of presents at 4 vblanks and 2% at 0: mean 2, median 2, distance 0.04, no hitch, max 4
        r = pc.compare(base, trace(3000, vbl=lambda k: 4 if k % 100 < 2 else 0 if k % 100 < 4 else 2))
        assert r["states"]["vblanks_distribution_match"] == "PASS" and r["states"]["vblanks_max_match"] == "PASS"
        assert r["states"]["vblanks_tail_match"] == "FAIL" and r["verdict"] == "FAIL", r["stats"]["vblanks_tail_match"]
    fails += run("negative: a 2% vblank tail (mean, median and distance kept) fails", tail)

    def long_frames():
        # 1% of presents at 10 vblanks, 4% at 0: mean 2.0, median 2 - hitches (>= 6) the control does not have
        t = trace(3000, vbl=lambda k: 10 if k % 100 == 0 else 0 if k % 25 == 1 else 2)
        assert abs(t["vblanks_per_present"]["mean"] - 2.0) < 0.01 and t["vblanks_per_present"]["p50"] == 2.0
        r = pc.compare(base, t)
        assert r["states"]["vblanks_hitch_rate_match"] == "FAIL" and r["verdict"] == "FAIL", r["stats"]["vblanks_hitches"]
    fails += run("negative: a 1% long-frame tail (mean and median kept) fails", long_frames)

    def one_hitch():
        # Astra round 2: one 100-vblank frame among 2,048 (~1.67 s at 60 Hz)
        r = pc.compare(base, trace(3000, vbl=lambda k: 100 if k == 2500 else 2))
        assert r["states"]["vblanks_max_match"] == "FAIL" and r["verdict"] == "FAIL", r["stats"]["vblanks_max"]
    fails += run("negative: one 100-vblank frame in 2,048 fails (severity)", one_hitch)

    def stall_catch_up():
        # Astra round 2: a 200-vblank stall, then 99 zero-vblank catch-up frames: the mean is kept exactly
        t = trace(3000, vbl=lambda k: 200 if k == 2200 else 0 if 2200 < k <= 2299 else 2)
        assert t["vblanks_per_present"]["mean"] == 2.0, t["vblanks_per_present"]
        r = pc.compare(base, t)
        assert r["states"]["vblanks_max_match"] == "FAIL" and r["verdict"] == "FAIL", r["stats"]["vblanks_max"]
    fails += run("negative: a 200-vblank stall with 99 zero catch-up frames (mean kept) fails", stall_catch_up)

    def small_hitch():
        # one 7-vblank frame (the hitch threshold 6, the max allowance 2 above it): not a FAIL
        r = pc.compare(base, trace(3000, vbl=lambda k: 7 if k == 2500 else 2))
        assert r["verdict"] != "FAIL" and r["states"]["vblanks_max_match"] == "PASS", not_pass(r)
    fails += run("one 7-vblank frame is inside the severity allowance (max <= 6 + 2)", small_hitch)

    # --- the throttle wait ---
    def wait():
        r = pc.compare(base, trace(3000, wait=lambda k: 900))
        assert r["states"]["throttle_wait_match"] == "FAIL", r["stats"]["throttle_wait_match"]
    fails += run("negative: a tripled throttle wait fails", wait)

    def wait_spikes():
        # Astra round 1: 100x the wait on 25% of presents; the median stays 300
        r = pc.compare(base, trace(3000, wait=lambda k: 30000 if k % 4 == 0 else 300))
        assert r["states"]["throttle_wait_match"] == "PASS", r["stats"]["throttle_wait_match"]  # the median is blind
        assert (r["states"]["throttle_wait_p95_match"] == "FAIL" and r["states"]["throttle_wait_spikes_match"] == "FAIL"
                and r["states"]["throttle_wait_max_match"] == "FAIL" and r["verdict"] == "FAIL"), not_pass(r)
    fails += run("negative: 100x throttle-wait spikes on 25% of presents (median kept) fail", wait_spikes)

    def rare_spikes():
        # 2% of presents at 10x: the median kept; the p99's exceedance share and the spike share see it (so does the
        # p95's: 2 points above the control's p95 + 25%, against its 1-point margin)
        r = pc.compare(base, trace(3000, wait=lambda k: 3000 if k % 50 == 0 else 300))
        assert r["states"]["throttle_wait_match"] == "PASS", r["stats"]["throttle_wait_match"]
        assert (r["states"]["throttle_wait_p99_match"] == "FAIL" and r["states"]["throttle_wait_spikes_match"] == "FAIL"
                and r["verdict"] == "FAIL"), not_pass(r)
    fails += run("negative: 10x spikes on 2% of presents (the median kept) fail", rare_spikes)

    # --- coverage ---
    def short():
        r = pc.compare(base, trace(300, lead=200))
        assert r["states"]["target_pairs"] == "FAIL", not_pass(r)
    fails += run("negative: a trace of 500 presents is too short", short)

    def gap():
        t = trace(3000, skip={2900})
        assert t["missing_presents"] == 1 and t["pairs"] == N - 2, {k: v for k, v in t.items() if k != "samples"}
        assert pc.compare(base, t)["verdict"] == "PASS"
    fails += run("one missing present is counted and its deltas not averaged across", gap)

    def big_hole():
        # Astra round 1: two retained blocks of 1,024 presents with 100,000 missing present ids between them
        recs = np.concatenate([records(1024, lead=0), records(1024, lead=0, first_id=101024)])
        t = pt.pacing(recs, recs["kind"])
        assert t["window_ids"] == [99999, 102047] and t["missing_presents"] == 1025, \
            {k: v for k, v in t.items() if k != "samples"}  # counted in ids, not as one discontinuity
        r = pc.compare(base, t)
        assert r["states"]["target_pairs"] == "FAIL" and r["verdict"] == "FAIL", not_pass(r)
    fails += run("negative: 100,000 missing presents between two blocks fail coverage", big_hole)

    def trailing():
        # Astra round 2: present records through id 6499, pacing records stopping at 4999 - the window ends at the
        # last present the trace saw, so the 1,500 trailing ids are missing (never an earlier healthy window)
        recs = records(5000, lead=0, presents_to=6499)
        t = pt.pacing(recs, recs["kind"])
        assert t["window_ids"] == [6499 - N, 6499] and t["window_anchor"] == "present records", \
            {k: v for k, v in t.items() if k != "samples"}
        assert t["missing_presents"] == 1500, t["missing_presents"]
        assert pc.compare(base, t)["states"]["target_pairs"] == "FAIL"
        healthy = pt.pacing(*(lambda r: (r, r["kind"]))(records(6500, lead=0, presents_to=6499)))
        assert healthy["missing_presents"] == 0 and pc.compare(base, healthy)["verdict"] == "PASS"
    fails += run("negative: pacing records stopping 1,500 presents before the trace's last present fail", trailing)

    def duplicates():
        t = trace(3000, dup={2950, 2960})
        assert t["duplicate_ids"] == 2 and t["missing_presents"] == 0, {k: v for k, v in t.items() if k != "samples"}
        r = pc.compare(base, t)
        assert r["states"]["target_no_duplicate_ids"] == "FAIL" and r["verdict"] == "FAIL", not_pass(r)
    fails += run("negative: a present id recorded twice fails (graded apart from missing ids)", duplicates)

    # --- the calibration gate (Astra rounds 2 and 3) ---
    def calibration_gate():
        bad = trace(3000, wait=lambda k: 900)  # a "second control" the instrument cannot match with the first
        r = pc.judge(base, trace(3000), bad)
        assert r["verdict"] == "INCONCLUSIVE" and r["composed_verdict"] == "PASS", r["verdict"]
        # a hard failure stays a FAIL whatever the calibration says (Astra round 3: a 100-vblank stall with an
        # unrelated calibration failure)
        r = pc.judge(base, trace(3000, vbl=lambda k: 100 if k == 2500 else 2), bad)
        assert r["verdict"] == "FAIL" and r["calibration"]["verdict"] == "FAIL", r["verdict"]
        r = pc.judge(base, trace(3000, wait=lambda k: 900), trace(3000))
        assert r["verdict"] == "FAIL" and r["calibration"]["verdict"] == "PASS", r["verdict"]
        r = pc.judge(base, trace(3000), trace(3000))
        assert r["verdict"] == "PASS", r["verdict"]
    fails += run("the calibration only gates a PASS (INCONCLUSIVE); a hard failure stays a FAIL", calibration_gate)

    def nodata():
        import json
        import tempfile
        d = os.path.join(os.path.dirname(os.path.dirname(HERE)), "archive", "test_pacing")
        os.makedirs(d, exist_ok=True)
        a = os.path.join(tempfile.mkdtemp(dir=d), "a.json")
        json.dump({"pacing": {"n": 0}}, open(a, "w"))
        assert pc.main([a, a]) == 2
    fails += run("negative: a summary without pacing records is NO_DATA (exit 2)", nodata)

    print("%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
