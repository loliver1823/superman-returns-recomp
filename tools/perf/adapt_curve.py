#!/usr/bin/env python3
"""adapt_curve.py - eye-adaptation transition curves from SR_PRESENT_LUMA rolls (step 4 item X2, lane D, 2026-10-18).

The game's HDR history moves by (1 - exp2(-0.8744 * c0.x)) once per evaluation (ENGINE_MAP 6g.8d; PS 040D6633), so
with one evaluation per present an uncapped roll adapts faster in wall time than the console's 30 Hz.
SR_HDR_ADAPT_DT=1 (kernel/hdr_adaptation.cpp) scales c0.x by 30 x render_dt. This tool measures the transition each
roll shows in its presented luminance (gfx/present_luma.cpp: <roll>/perf/present_luma.csv, every present, wall time
from t_us) and grades the pre-registered X2 rules (archive/step4_claude_20261018/timing/NOTES.md 2026-10-18 11:55):

  step   the first present whose luma rises >= --rise (30) code levels above the median of the preceding 1.0 s
         (presents before --after seconds of the CSV are not candidates)
  peak   the max luma within 0.5 s after the step; L_final = median luma over [peak + 2.0 s, peak + 3.0 s]
  fit    L(t) = A + B exp(-(t - t_peak) / tau) over [peak, peak + 3.0 s] (least squares; tau grid 5 ms .. 3 s)
  t50 / t90  the first sample with |L - L_final| <= 0.5 / 0.1 of |L_peak - L_final|; R = presents/s in the window
  rules  1 NOISE: the two A rolls' tau differ by > 25 % -> INCONCLUSIVE
         2 DEFECT (=0): tau_A / tau_C >= 2.5 and within +-35 % of R_C / R_A
         3 FIX (=1): tau_D / tau_A in [0.80, 1.25]; t50_D, t90_D within max(20 %, 67 ms) of A's; the RMS of the normalized
           progress (L - L_final) / (L_peak - L_final), D vs A on a 10 ms grid over the window, <= 0.10
         4 CONTROL: B within rule 3's tolerances of A
         ACCEPT only when 2, 3 and 4 hold.
Arms: A = 30 Hz SR_HDR_ADAPT_DT=0, B = 30 Hz =1, C = uncapped =0, D = uncapped =1.
Every grader (grade / seq / freeze; Astra review 2026-10-18) first checks the arms' cadence: C and D at >= 2.5x A's
presents/s, B within 15 % of A, else INCONCLUSIVE. A requested --anchor era that a roll's report.json lacks (or a
missing / malformed report.json) makes the set INCONCLUSIVE. freeze also requires a continuous fit window: consecutive
pn and no rise of the probe's skipped count, or the hold is not gradable.

    python tools/perf/adapt_curve.py grade --arm A=<roll>,<roll> --arm B=... --arm C=... --arm D=... --out <dir>
    python tools/perf/adapt_curve.py curve <roll or present_luma.csv> [--after S | --anchor title]   # one roll
    python tools/perf/adapt_curve.py pilot <roll> --dumps <SR_PRESENT_DUMP_DIR> --anchor title   # the pilot checks
    python tools/perf/adapt_curve.py selftest                                          # synthetic curves, the rules
Writes <out>/adapt_grade.json (+ adapt_curves.png when matplotlib is present). Exit 0 = ACCEPT, 1 = REJECT,
3 = INCONCLUSIVE, 2 = bad input.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import statistics
import sys

RISE = 30.0           # code levels above the preceding second's median
BASE_S = 1.0          # the baseline window before a step candidate
PEAK_S = 0.5          # the peak search after the step
FINAL_S = (2.0, 3.0)  # L_final's window after the peak
FIT_S = 3.0           # the fit window after the peak
GRID_S = 0.010        # the progress grid
TAU_MIN, TAU_MAX, TAU_N = 0.005, 3.0, 400


def find_csv(path):
    if os.path.isfile(path):
        return path
    for cand in (os.path.join(path, "perf", "present_luma.csv"), os.path.join(path, "present_luma.csv")):
        if os.path.isfile(cand):
            return cand
    for dp, _dn, fn in os.walk(path):
        if "present_luma.csv" in fn:
            return os.path.join(dp, "present_luma.csv")
    raise FileNotFoundError("no present_luma.csv under %s" % path)


def load(path, with_pn=False):
    """-> [(t_seconds, luma)] in present order (with_pn: [(t, luma, pn)]), plus the last 'skipped' count."""
    rows, skipped = [], 0
    with open(find_csv(path), encoding="utf-8") as f:
        for r in csv.DictReader(line for line in f if not line.startswith("#")):
            rec = (int(r["t_us"]) / 1e6, float(r["luma"]))
            rows.append(rec + (int(r["pn"]),) if with_pn else rec)
            skipped = int(r.get("skipped") or 0)
    rows.sort(key=lambda x: x[0])
    return rows, skipped


def era_start(roll, era):
    """The perf-clock start (s) of a perf_report era in <roll>/report.json (e.g. 'title' = the guest's PRESS START
    request); the same clock as present_luma.csv's t_us (sr::perf::nowUs). None when the roll has no such era."""
    path = os.path.join(roll, "report.json") if os.path.isdir(roll) else os.path.join(os.path.dirname(
        os.path.dirname(roll)), "report.json")
    try:
        with open(path, encoding="utf-8") as f:
            eras = json.load(f).get("eras", [])
        for e in eras:
            if e.get("era") == era and e.get("t0_s") is not None:
                return float(e["t0_s"])
    except (OSError, ValueError, AttributeError, TypeError):   # missing / malformed: the caller treats None as no anchor
        return None
    return None


def after_for(roll, rows, after, anchor, lead=1.0):
    """The step-search start in seconds after the first record: --after, or (--anchor ERA) the era's start minus lead."""
    if not anchor:
        return after
    t = era_start(roll, anchor)
    if t is None:
        raise ValueError("%s: no '%s' era in report.json" % (roll, anchor))
    return max(after, t - lead - rows[0][0])


def stimulus_check(r, rows):
    """The pilot's pre-registered stimulus rule: a rise of >= 30 levels within <= 3 presents, then a relaxation."""
    if "error" in r:
        return {"pass": False, "why": r["error"]}
    ts = [x[0] for x in rows]
    i = min(range(len(ts)), key=lambda k: abs(ts[k] - (rows[0][0] + r["t_step_s"])))
    rise3 = rows[i][1] - rows[max(0, i - 3)][1]
    relax = r["l_peak"] - r["l_final"]
    return {"rise_within_3_presents": round(rise3, 3), "relaxation": round(relax, 3),
            "pass": rise3 >= RISE and relax >= 5.0}


def instrument_check(roll, dump_dir, rows_n=64, step=2, tol=0.01):
    """The pre-registered probe check: every present dump's mean over the probe's rows / step equals one CSV record's
    luma within tol, at ONE constant present-number offset, in order; and the probe skipped nothing."""
    from PIL import Image
    import re
    recs, skipped = load(roll, with_pn=True)
    by_pn = {pn: l for _t, l, pn in recs}
    out = {"dumps": 0, "matched": 0, "offsets": {}, "worst": 0.0, "skipped": skipped, "misses": []}
    per = []
    names = sorted(n for n in os.listdir(dump_dir) if n.startswith("present_") and n.endswith(".bmp"))
    for n in names:
        m = re.search(r"_p(\d+)_\d+x\d+\.bmp$", n)
        if not m:
            continue
        p = int(m.group(1))
        img = Image.open(os.path.join(dump_dir, n)).convert("RGB")
        w, h = img.size
        px = img.load()
        rows = min(rows_n, h)
        tot, cnt = 0, 0
        for r in range(rows):
            y = ((2 * r + 1) * h) // (2 * rows)
            for x in range(0, w, step):
                R, G, B = px[x, y]
                tot += 299 * R + 587 * G + 114 * B
                cnt += 1
        luma = tot / (1000.0 * cnt)
        out["dumps"] += 1
        per.append((n, p, luma))
    # 2026-10-18 amendment (x2set2): count, per offset, the dumps the record at p + offset matches. Repeated frames
    # (a movie frame shown on two presents) tie several offsets; the first-tie rule of the pilot mis-reported them.
    for off in range(-3, 4):   # the dump counts from its first ready call, the probe from its first call
        ok = [abs(by_pn[p + off] - l) for _n, p, l in per if p + off in by_pn and abs(by_pn[p + off] - l) <= tol]
        out["offsets"][str(off)] = len(ok)
    best = max(out["offsets"].items(), key=lambda kv: kv[1]) if out["offsets"] else ("0", 0)
    out["best_offset"], out["matched"] = int(best[0]), best[1]
    for n, p, l in per:
        r = by_pn.get(p + out["best_offset"])
        if r is None or abs(r - l) > tol:
            out["misses"].append({"dump": n, "luma": round(l, 4), "record": r})
        else:
            out["worst"] = max(out["worst"], abs(r - l))
    out["pass"] = out["dumps"] > 0 and out["matched"] == out["dumps"] and skipped == 0
    return out


# ------------------------------------------------------------------ the title-sequence analysis (amendment x2set2)
SEQ_FIRST = 18.0            # the first title frame: luma above this (halfway black 4 -> first frame 32)
SEQ_GRID = (0.3, 9.0)       # seconds after the first title frame
CUT_MIN, CUT_WIN = 12.0, (2.0, 6.0)   # the settle cut: the largest one-present rise >= 12 levels in [2 s, 6 s]
NOISE_MAX = 4.0


def seq_curve(roll, rows, anchor="title"):
    """One roll on the title sequence: t0 = the first present with luma > SEQ_FIRST at or after the anchor era
    start - 1 s; the curve on the GRID_S grid over SEQ_GRID (linear interpolation); the raw rows after t0."""
    t_era = era_start(roll, anchor)
    if t_era is None:
        return {"error": "no '%s' era in report.json" % anchor, "anchor_missing": True}
    return seq_from_rows(rows, t_era)


def seq_from_rows(rows, t_era):
    """seq_curve's core: rows [(t, luma)] and the anchor era's start on the same clock."""
    t0 = next((t for t, l in rows if t >= t_era - 1.0 and l > SEQ_FIRST), None)
    if t0 is None:   # Astra 2026-10-18 P3: was a NameError (`anchor` is not in scope here)
        return {"error": "no present above %.0f after the anchor era (t %.3f s)" % (SEQ_FIRST, t_era)}
    rel = [(t - t0, l) for t, l in rows if t >= t0]
    if not rel or rel[-1][0] < SEQ_GRID[1]:
        return {"error": "the roll ends %.1f s after the first title frame" % (rel[-1][0] if rel else 0)}
    grid, j = [], 0
    n = int(round((SEQ_GRID[1] - SEQ_GRID[0]) / GRID_S)) + 1
    for g in range(n):
        x = SEQ_GRID[0] + g * GRID_S
        while j + 1 < len(rel) and rel[j + 1][0] < x:
            j += 1
        (x0, y0), (x1, y1) = rel[j], rel[min(j + 1, len(rel) - 1)]
        grid.append(y0 if x1 <= x0 else y0 + (y1 - y0) * (min(max(x, x0), x1) - x0) / (x1 - x0))
    win = [(t, l) for t, l in rel if t <= SEQ_GRID[1]]
    rate = (len(win) - 1) / (win[-1][0] - win[0][0]) if len(win) > 1 and win[-1][0] > win[0][0] else 0.0
    return {"t0_s": round(t0, 4), "era_t0_s": round(t_era, 4), "curve": grid, "rel": rel, "presents_per_s": round(rate, 3)}


def find_cut(rel):
    """The largest one-present rise >= CUT_MIN within CUT_WIN: (t of the present after the rise, rise) or None."""
    best = None
    for (ta, la), (tb, lb) in zip(rel, rel[1:]):
        if CUT_WIN[0] <= tb <= CUT_WIN[1] and lb - la >= CUT_MIN and (best is None or lb - la > best[1]):
            best = (tb, lb - la)
    return best


def settle_half(rel, t_cut):
    """After a cut at t_cut: the time from the post-cut peak (max within 0.3 s) until the luma falls halfway toward
    its level 1.5 s after the cut. None when it never does."""
    post = [(t, l) for t, l in rel if t_cut <= t <= t_cut + 0.3]
    later = [(t, l) for t, l in rel if t >= t_cut + 1.5]
    if not post or not later:
        return None
    tp, lp = max(post, key=lambda x: x[1])
    l15 = later[0][1]
    if lp <= l15:
        return None
    half = lp - 0.5 * (lp - l15)
    return next((t - tp for t, l in rel if t > tp and l <= half), None)


# ------------------------------------------------------------------ the freeze-relaxation analysis (amendment 2, x2set3)
K_PRESENT = 0.8743894696235657 * 0.25 * math.log(2)   # the per-update decay exponent at c0.x = 0.25
HOLD_SKIP_MS, HOLD_MIN_N, HOLD_MIN_B, HOLD_MIN_R2 = 50.0, 8, 0.5, 0.95


STAMP_SKIP, HOLD_MIN_B_STAMP = 3, 0.3   # amendment 3 (x2set4): records after the stamp settles; the gradable |B|
STAMP_TOL_NS = 2_000_000                 # amendment 4 (x2set5): the settled tolerance


def load_holds(path):
    """[(t_s, luma, hold_id, held_ms, stamp or None, pn, skipped)] from a present_luma.csv with the hold columns.
    pn (the probe's present ordinal) and skipped (its cumulative count of presents not sampled) are kept for the
    continuity check (Astra 2026-10-18 P2): a CSV without pn is refused."""
    out = []
    with open(find_csv(path), encoding="utf-8") as f:
        for r in csv.DictReader(line for line in f if not line.startswith("#")):
            st = r.get("stamp")
            if r.get("pn") in (None, ""):
                raise ValueError("%s: a record without pn (the continuity check needs the present ordinals)" % path)
            out.append((int(r["t_us"]) / 1e6, float(r["luma"]), int(r.get("hold") or 0), float(r.get("held_ms") or 0),
                        int(st) if st not in (None, "") else None, int(r["pn"]), int(r.get("skipped") or 0)))
    out.sort(key=lambda x: x[0])
    return out


def frozen_part(pts):
    """Amendment 3: the records of one hold from STAMP_SKIP records after the stamp settles on its final value
    (the first record from which every later stamp equals the last record's). [] when it never settles.
    pts: [(t, luma, stamp, ...)]; returns the same tuples."""
    if not pts or pts[-1][2] is None:
        return []
    last = pts[-1][2]
    k = len(pts) - 1
    # amendment 4: "settled" = within STAMP_TOL_NS of the final stamp (the GameKernel clock's 1 ms grid +- 1 ms)
    while k > 0 and pts[k - 1][2] is not None and abs(pts[k - 1][2] - last) <= STAMP_TOL_NS:
        k -= 1
    return pts[k + STAMP_SKIP:]


def continuity(pts):
    """Astra 2026-10-18 P2: a fit window must hold every present: consecutive pn and no rise of the probe's skipped
    count (a saturated readback ring or an unreadable back buffer). pts: [(t, luma, stamp, pn, skipped)].
    -> None when continuous, else the first discontinuity."""
    for x, y in zip(pts, pts[1:]):
        if x[3] is None or y[3] is None:
            return "no present ordinal"
        if y[3] != x[3] + 1:
            return "present %d follows %d" % (y[3], x[3])
        if y[4] != x[4]:
            return "skipped %d -> %d" % (x[4], y[4])
    return None


def hold_fits(recs, t_from=None):
    """Per hold id (at or after t_from): the exponential fit + gradability. With a stamp column (amendment 3) the fit
    starts STAMP_SKIP records after the render loop's time stops (frozen content); without it (amendment 2) at
    held_ms >= HOLD_SKIP_MS. recs as load_holds returns them. A hold whose fit window misses a present is not
    gradable and carries 'discontinuous' (its rate is not used either)."""
    stamped = any(r[4] is not None for r in recs)
    by = {}
    for t, l, h, ms, st, pn, sk in recs:
        if h and (t_from is None or t >= t_from) and (stamped or ms >= HOLD_SKIP_MS):
            by.setdefault(h, []).append((t, l, st, pn, sk))
    min_b = HOLD_MIN_B_STAMP if stamped else HOLD_MIN_B
    fits = []
    for h, raw in sorted(by.items()):
        pts = frozen_part(raw) if stamped else raw
        if len(pts) < 3:
            continue
        gap = continuity(pts)
        ts, ls = [p[0] for p in pts], [p[1] for p in pts]
        tau, a, b, sse = fit_exp(ts, ls, ts[0])
        mean = statistics.fmean(ls)
        sst = sum((x - mean) ** 2 for x in ls) or 1e-12
        r2 = 1 - sse / sst
        rate = (len(ts) - 1) / (ts[-1] - ts[0]) if ts[-1] > ts[0] else 0.0
        fits.append({"hold": h, "n": len(ts), "tau_s": round(tau, 5), "A": round(a, 3), "B": round(b, 3),
                     "r2": round(r2, 4), "presents_per_s": round(rate, 2), "t1_s": round(ts[0], 3),
                     "discontinuous": gap,
                     "gradable": gap is None and len(ts) >= HOLD_MIN_N and abs(b) >= min_b and r2 >= HOLD_MIN_R2,
                     "curve": [(round(t - ts[0], 4), (l - a) / b if b else 0.0) for t, l in zip(ts, ls)]})
    return fits


# Astra 2026-10-18 P2: an arm's cadence must match its configuration before its tau is graded: the uncapped arms
# (C, D) at >= CADENCE_HFR_MIN x A's presents/s (the defect rule's own 2.5), the 30 Hz control (B) within +-15 % of A.
CADENCE_HFR_MIN, CADENCE_CONTROL = 2.5, 0.15


def cadence_rule(rates):
    """rates: {'A': presents/s, ...} -> the cadence rule (pass = every arm ran at its configured cadence)."""
    ra = rates.get("A") or 0.0
    if ra <= 0 or any(not rates.get(k) for k in "BCD"):
        return {"pass": False, "rates": rates, "why": "an arm has no presents/s"}
    r = {"C_over_A": round(rates["C"] / ra, 3), "D_over_A": round(rates["D"] / ra, 3),
         "B_over_A": round(rates["B"] / ra, 3)}
    r["pass"] = (r["C_over_A"] >= CADENCE_HFR_MIN and r["D_over_A"] >= CADENCE_HFR_MIN and
                 abs(r["B_over_A"] - 1) <= CADENCE_CONTROL)
    if not r["pass"]:
        r["why"] = "C and D need >= %.1f x A's presents/s, B within %d %% of A" % (CADENCE_HFR_MIN, CADENCE_CONTROL * 100)
    return r


def grade_freeze(per_arm):
    """per_arm: {'A': [hold fit lists per roll], ...} -> the amendment-2 verdict."""
    out = {"arms": {}, "rules": {}}
    for k in "ABCD":
        hs = [f for roll in per_arm.get(k, []) for f in roll if f["gradable"]]
        rates = [f["presents_per_s"] for roll in per_arm.get(k, []) for f in roll
                 if f["presents_per_s"] > 0 and not f.get("discontinuous")]
        out["arms"][k] = {"gradable": len(hs), "holds": sum(len(r) for r in per_arm.get(k, [])),
                          "discontinuous": sum(1 for roll in per_arm.get(k, []) for f in roll if f.get("discontinuous")),
                          "tau_s": statistics.median(f["tau_s"] for f in hs) if hs else None,
                          "taus": sorted(f["tau_s"] for f in hs),
                          "presents_per_s": statistics.median(rates) if rates else None}
    a = out["arms"]
    if any(a[k]["gradable"] < 4 for k in "ABCD"):
        out["verdict"] = "INCONCLUSIVE"
        out["reason"] = "gradable holds per arm %s (need 4)" % {k: a[k]["gradable"] for k in "ABCD"}
        return out
    out["rules"]["cadence"] = cadence_rule({k: a[k]["presents_per_s"] for k in "ABCD"})
    if not out["rules"]["cadence"]["pass"]:
        out["verdict"] = "INCONCLUSIVE"
        out["reason"] = "the arms' cadences do not match their configurations: %s" % out["rules"]["cadence"]
        return out
    model_a =1.0 / (a["A"]["presents_per_s"] * K_PRESENT)
    out["model"] = {"tau_A_model_s": round(model_a, 4), "tau_C_model_s": round(1.0 / (a["C"]["presents_per_s"] * K_PRESENT), 4),
                    "tau_D_model_s": round(1.0 / (30.0 * K_PRESENT), 4)}
    ta, tb, tc, td = (a[k]["tau_s"] for k in "ABCD")
    out["rules"]["model"] = {"tau_A_over_model": round(ta / model_a, 4), "pass": abs(ta / model_a - 1) <= 0.25}
    exp = a["C"]["presents_per_s"] / a["A"]["presents_per_s"]
    out["rules"]["defect"] = {"tau_A_over_C": round(ta / tc, 4), "rate_C_over_A": round(exp, 4),
                              "pass": ta / tc >= 2.5 and abs(ta / tc - exp) <= 0.30 * exp}
    out["rules"]["fix"] = {"tau_D_over_A": round(td / ta, 4), "expected": round(a["A"]["presents_per_s"] / 30.0, 4),
                           "pass": 0.85 <= td / ta <= 1.15}
    out["rules"]["control"] = {"tau_B_over_A": round(tb / ta, 4), "pass": 0.85 <= tb / ta <= 1.15}
    if not out["rules"]["model"]["pass"]:
        out["verdict"], out["reason"] = "INCONCLUSIVE", "the hold relaxation does not follow the adaptation law (tau_A %.4f vs model %.4f)" % (ta, model_a)
    elif all(out["rules"][k]["pass"] for k in ("defect", "fix", "control")):
        out["verdict"], out["reason"] = "ACCEPT", "defect reproduced at =0; =1 matches 30 Hz; control holds; model check holds"
    else:
        out["verdict"] = "REJECT"
        out["reason"] = "failed: " + ", ".join(k for k in ("defect", "fix", "control") if not out["rules"][k]["pass"])
    return out


def plot_freeze(per_arm, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    colors = {"A": "#1f4e79", "B": "#7fa7cf", "C": "#b5482f", "D": "#e09a3e"}
    labels = {"A": "A 30 Hz, ADAPT_DT=0", "B": "B 30 Hz, =1", "C": "C uncapped, =0", "D": "D uncapped, =1"}
    fig, ax = plt.subplots(figsize=(9, 5), dpi=120)
    for arm in "ACBD":
        first = True
        for roll in per_arm.get(arm, []):
            for f in roll:
                if f["gradable"]:
                    ax.plot([x for x, _y in f["curve"]], [y for _x, y in f["curve"]], color=colors[arm], lw=0.9,
                            alpha=0.55, label=labels[arm] if first else None)
                    first = False
    ax.set_xlim(0, 1.4)
    ax.axhline(0, color="#999", lw=0.6)
    ax.set_xlabel("wall time into the hold (s)")
    ax.set_ylabel("normalized exposure transient (L - L_settled) / B")
    ax.set_title("Eye adaptation on a frozen scene (SR_DIAG_SIM_FREEZE holds, adaptation running)")
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return True


def mean_curve(curves):
    n = min(len(c) for c in curves)
    return [statistics.fmean(c[i] for c in curves) for i in range(n)]


def grade_seq(per_arm):
    """per_arm: {'A': [seq_curve results], ...} -> the amendment's verdict."""
    out = {"rules": {}}
    for k in "ABCD":
        ok = [r for r in per_arm.get(k, []) if "error" not in r]
        if len(ok) < (2 if k in "AD" else 1):
            out["verdict"], out["reason"] = "INCONCLUSIVE", "arm %s has %d gradable roll(s)" % (k, len(ok))
            return out
        per_arm[k] = ok
    out["rules"]["cadence"] = cadence_rule({k: statistics.median(r["presents_per_s"] for r in per_arm[k]) for k in "ABCD"})
    if not out["rules"]["cadence"]["pass"]:
        out["verdict"] = "INCONCLUSIVE"
        out["reason"] = "the arms' cadences do not match their configurations: %s" % out["rules"]["cadence"]
        return out
    mc = {k: mean_curve([r["curve"] for r in per_arm[k]]) for k in "ABCD"}
    na = rms(per_arm["A"][0]["curve"], per_arm["A"][1]["curve"])
    nd = rms(per_arm["D"][0]["curve"], per_arm["D"][1]["curve"])
    noise = max(na, nd)
    r_ca, r_da, r_ba = rms(mc["C"], mc["A"]), rms(mc["D"], mc["A"]), rms(mc["B"], mc["A"])
    out["rms"] = {"A1_A2": round(na, 3), "D1_D2": round(nd, 3), "noise": round(noise, 3), "C_A": round(r_ca, 3),
                  "D_A": round(r_da, 3), "B_A": round(r_ba, 3)}
    cut = find_cut(per_arm["A"][0]["rel"])
    halves = {}
    if cut:
        for k in "AC":
            hs = []
            for r in per_arm[k]:
                c = find_cut([(t, l) for t, l in r["rel"] if abs(t - cut[0]) <= 0.3])  # the same cut in this roll
                h = settle_half(r["rel"], c[0]) if c else None
                if h is not None:
                    hs.append(h)
            halves[k] = statistics.fmean(hs) if hs else None
    out["cut"] = {"t_s": None if not cut else round(cut[0], 3), "rise": None if not cut else round(cut[1], 2),
                  "half_A_s": halves.get("A"), "half_C_s": halves.get("C")}
    faster = halves.get("A") is not None and halves.get("C") is not None and halves["C"] < halves["A"]
    out["rules"]["noise"] = {"pass": na <= NOISE_MAX}
    out["rules"]["defect"] = {"pass": r_ca >= 3 * noise and r_ca >= 2.0 and faster, "c_settles_faster": faster}
    out["rules"]["fix"] = {"pass": r_da <= 1.5 * noise and r_da <= 0.5 * r_ca}
    out["rules"]["control"] = {"pass": r_ba <= 1.5 * noise}
    if not out["rules"]["noise"]["pass"]:
        out["verdict"], out["reason"] = "INCONCLUSIVE", "A repeats differ by %.2f > %.1f levels RMS" % (na, NOISE_MAX)
    elif all(out["rules"][k]["pass"] for k in ("defect", "fix", "control")):
        out["verdict"], out["reason"] = "ACCEPT", "defect reproduced at =0; =1 matches 30 Hz; control holds"
    else:
        out["verdict"] = "REJECT"
        out["reason"] = "failed: " + ", ".join(k for k in ("defect", "fix", "control") if not out["rules"][k]["pass"])
    out["mean_curves"] = mc
    return out


def fit_exp(ts, ls, t0):
    """Least-squares L = A + B exp(-(t - t0)/tau): -> (tau, A, B, sse)."""
    best = None
    n = len(ts)
    for k in range(TAU_N):
        tau = TAU_MIN * (TAU_MAX / TAU_MIN) ** (k / (TAU_N - 1))
        e = [math.exp(-(t - t0) / tau) for t in ts]
        se, see = sum(e), sum(x * x for x in e)
        sl, sel = sum(ls), sum(x * y for x, y in zip(e, ls))
        det = n * see - se * se
        if abs(det) < 1e-12:
            continue
        b = (n * sel - se * sl) / det
        a = (sl - b * se) / n
        sse = sum((a + b * x - y) ** 2 for x, y in zip(e, ls))
        if best is None or sse < best[3]:
            best = (tau, a, b, sse)
    return best


def first_within(ts, ls, t_peak, l_final, span, frac):
    for t, l in zip(ts, ls):
        if t >= t_peak and abs(l - l_final) <= frac * span:
            return t - t_peak
    return None


def progress_curve(ts, ls, t_peak, l_peak, l_final):
    """(L - L_final) / (L_peak - L_final) on the GRID_S grid over [0, FIT_S] (linear interpolation)."""
    span = l_peak - l_final
    out = []
    j = 0
    pts = [(t - t_peak, (l - l_final) / span) for t, l in zip(ts, ls) if t >= t_peak]
    if len(pts) < 2:
        return out
    for g in range(int(round(FIT_S / GRID_S)) + 1):
        x = g * GRID_S
        while j + 1 < len(pts) and pts[j + 1][0] < x:
            j += 1
        if j + 1 >= len(pts):
            out.append(pts[-1][1])
            continue
        (x0, y0), (x1, y1) = pts[j], pts[j + 1]
        out.append(y0 if x1 == x0 else y0 + (y1 - y0) * (min(max(x, x0), x1) - x0) / (x1 - x0))
    return out


def analyze(rows, after=0.0, rise=RISE):
    """One roll's transition metrics, or {'error': ...}."""
    if len(rows) < 20:
        return {"error": "only %d records" % len(rows)}
    t_first = rows[0][0]
    step = None
    lo = 0
    for i, (t, l) in enumerate(rows):
        if t - t_first < max(after, BASE_S):
            continue
        while rows[lo][0] < t - BASE_S:
            lo += 1
        base = [x[1] for x in rows[lo:i]]
        if len(base) >= 5 and l - statistics.median(base) >= rise:
            step = i
            base_l = statistics.median(base)
            break
    if step is None:
        return {"error": "no rise of >= %.0f levels after %.1f s" % (rise, after)}
    t_step = rows[step][0]
    win = [(t, l) for t, l in rows[step:] if t <= t_step + PEAK_S]
    t_peak, l_peak = max(win, key=lambda x: x[1])
    fin = [l for t, l in rows if t_peak + FINAL_S[0] <= t <= t_peak + FINAL_S[1]]
    fitw = [(t, l) for t, l in rows if t_peak <= t <= t_peak + FIT_S]
    if len(fin) < 5 or len(fitw) < 10:
        return {"error": "the roll ends within %.1f s of the peak" % FIT_S}
    l_final = statistics.median(fin)
    span = abs(l_peak - l_final)
    ts, ls = [x[0] for x in fitw], [x[1] for x in fitw]
    tau, a, b, sse = fit_exp(ts, ls, t_peak)
    mean = statistics.fmean(ls)
    sst = sum((x - mean) ** 2 for x in ls) or 1e-12
    rate = (len(ts) - 1) / (ts[-1] - ts[0]) if ts[-1] > ts[0] else 0.0
    return {"t_step_s": round(t_step - t_first, 4), "t_peak_s": round(t_peak - t_first, 4),
            "baseline": round(base_l, 3), "l_peak": round(l_peak, 3), "l_final": round(l_final, 3),
            "span": round(span, 3), "tau_s": round(tau, 5), "fit_A": round(a, 3), "fit_B": round(b, 3),
            "r2": round(1 - sse / sst, 4), "t50_s": first_within(ts, ls, t_peak, l_final, span, 0.5),
            "t90_s": first_within(ts, ls, t_peak, l_final, span, 0.1), "presents_per_s": round(rate, 3),
            "fit_n": len(ts), "progress": progress_curve(ts, ls, t_peak, l_peak, l_final) if span > 0 else []}


def arm_summary(results):
    ok = [r for r in results if "error" not in r]
    if not ok:
        return {"n": 0}
    def mean(k):
        v = [r[k] for r in ok if r.get(k) is not None]
        return statistics.fmean(v) if v else None
    n = min(len(r["progress"]) for r in ok)
    prog = [statistics.fmean(r["progress"][g] for r in ok) for g in range(n)]
    return {"n": len(ok), "tau_s": mean("tau_s"), "t50_s": mean("t50_s"), "t90_s": mean("t90_s"),
            "presents_per_s": mean("presents_per_s"), "taus": [r["tau_s"] for r in ok], "progress": prog}


def rms(a, b):
    n = min(len(a), len(b))
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(n)) / n) if n else float("inf")


def within(x, ref, frac=0.20, floor=0.067):
    return x is not None and ref is not None and abs(x - ref) <= max(frac * ref, floor)


def matches(arm, ref):
    """Rule 3's tolerances: arm vs the reference arm (A)."""
    ratio = arm["tau_s"] / ref["tau_s"]
    r = rms(arm["progress"], ref["progress"])
    checks = {"tau_ratio": round(ratio, 4), "tau_ratio_ok": 0.80 <= ratio <= 1.25,
              "t50_ok": within(arm["t50_s"], ref["t50_s"]), "t90_ok": within(arm["t90_s"], ref["t90_s"]),
              "progress_rms": round(r, 4), "progress_rms_ok": r <= 0.10}
    checks["pass"] = all(checks[k] for k in ("tau_ratio_ok", "t50_ok", "t90_ok", "progress_rms_ok"))
    return checks


def grade(arms):
    """arms: {'A': summary, ...} -> verdict dict (the pre-registered rules)."""
    out = {"rules": {}}
    for k in "ABCD":
        if arms.get(k, {}).get("n", 0) < 1:
            out["verdict"] = "INCONCLUSIVE"
            out["reason"] = "arm %s has no gradable roll" % k
            return out
    out["rules"]["0_cadence"] = cadence_rule({k: arms[k]["presents_per_s"] for k in "ABCD"})
    if not out["rules"]["0_cadence"]["pass"]:
        out["verdict"] = "INCONCLUSIVE"
        out["reason"] = "the arms' cadences do not match their configurations: %s" % out["rules"]["0_cadence"]
        return out
    A, B, C, D = (arms[k] for k in "ABCD")
    taus = A["taus"]
    noise = (max(taus) - min(taus)) / min(taus) if len(taus) >= 2 else None
    out["rules"]["1_noise"] = {"a_tau_spread": None if noise is None else round(noise, 4),
                               "pass": noise is not None and noise <= 0.25}
    exp_ratio = C["presents_per_s"] / A["presents_per_s"]
    def_ratio = A["tau_s"] / C["tau_s"]
    out["rules"]["2_defect"] = {"tau_A_over_C": round(def_ratio, 4), "rate_C_over_A": round(exp_ratio, 4),
                                "pass": def_ratio >= 2.5 and abs(def_ratio - exp_ratio) <= 0.35 * exp_ratio}
    out["rules"]["3_fix"] = matches(D, A)
    out["rules"]["3_fix"]["expected_tau_ratio"] = round(A["presents_per_s"] / 30.0, 4)
    out["rules"]["4_control"] = matches(B, A)
    if not out["rules"]["1_noise"]["pass"]:
        out["verdict"], out["reason"] = "INCONCLUSIVE", "the A repeats differ by more than 25 % in tau"
    elif all(out["rules"][k]["pass"] for k in ("2_defect", "3_fix", "4_control")):
        out["verdict"], out["reason"] = "ACCEPT", "defect reproduced at =0; =1 matches 30 Hz; control holds"
    else:
        failed = [k for k in ("2_defect", "3_fix", "4_control") if not out["rules"][k]["pass"]]
        out["verdict"], out["reason"] = "REJECT", "failed: " + ", ".join(failed)
    return out


def plot(per_arm_rolls, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    colors = {"A": "#1f4e79", "B": "#7fa7cf", "C": "#b5482f", "D": "#e09a3e"}
    labels = {"A": "A 30 Hz, ADAPT_DT=0", "B": "B 30 Hz, =1", "C": "C uncapped, =0", "D": "D uncapped, =1"}
    fig, ax = plt.subplots(figsize=(9, 5), dpi=120)
    for arm, rolls in per_arm_rolls.items():
        for i, r in enumerate(rolls):
            if "error" in r or not r["progress"]:
                continue
            xs = [g * GRID_S for g in range(len(r["progress"]))]
            ax.plot(xs, r["progress"], color=colors.get(arm, "grey"), lw=1.4, alpha=0.9,
                    label=labels.get(arm, arm) if i == 0 else None)
    ax.axhline(0, color="#999", lw=0.6)
    ax.set_xlim(0, 1.5)
    ax.set_xlabel("wall time after the peak (s)")
    ax.set_ylabel("normalized progress (L - L_final) / (L_peak - L_final)")
    ax.set_title("Eye adaptation after a dark -> bright cut, presented luminance")
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return True


def plot_seq(per_arm, means, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    colors = {"A": "#1f4e79", "B": "#7fa7cf", "C": "#b5482f", "D": "#e09a3e"}
    labels = {"A": "A 30 Hz, ADAPT_DT=0", "B": "B 30 Hz, =1", "C": "C uncapped, =0", "D": "D uncapped, =1"}
    fig, ax = plt.subplots(figsize=(10, 5), dpi=120)
    xs = [SEQ_GRID[0] + g * GRID_S for g in range(len(next(iter(means.values()), [])))]
    for arm in "ACBD":
        for r in per_arm.get(arm, []):
            if "error" not in r:
                ax.plot([t for t, _l in r["rel"] if t <= SEQ_GRID[1]], [l for t, l in r["rel"] if t <= SEQ_GRID[1]],
                        color=colors[arm], lw=0.6, alpha=0.35)
        if arm in means:
            ax.plot(xs[:len(means[arm])], means[arm], color=colors[arm], lw=1.6, label=labels[arm])
    ax.set_xlabel("wall time after the first title frame (s)")
    ax.set_ylabel("mean presented luma (8-bit code)")
    ax.set_title("Title sequence: presented luminance per arm (thin: rolls, thick: arm mean)")
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return True


# ----------------------------------------------------------------------------------------------- self test
def synth(rate, tau, dur=8.0, t_cut=3.0, dark=20.0, bright=120.0, over=80.0, noise=0.5, seed=1, jitter=0.0):
    """A synthetic roll: a dark scene, a cut at t_cut to a bright one, overexposure decaying with tau."""
    rnd = random.Random(seed)
    rows, t = [], 0.0
    while t < dur:
        l = dark if t < t_cut else bright + over * math.exp(-(t - t_cut) / tau)
        rows.append((t, l + rnd.gauss(0, noise)))
        t += (1.0 / rate) * (1 + rnd.uniform(-jitter, jitter))
    return rows


def selftest():
    fails = []
    def check(cond, what):
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            fails.append(what)
    r = analyze(synth(30, 0.22))
    check("error" not in r and abs(r["tau_s"] - 0.22) / 0.22 < 0.10, "30 Hz tau 0.22 recovered: %s" % r.get("tau_s"))
    check(abs(r["t_step_s"] - 3.0) < 0.05, "the step found at the cut: %s" % r.get("t_step_s"))
    r = analyze(synth(128, 0.048, jitter=0.3))
    check("error" not in r and abs(r["tau_s"] - 0.048) / 0.048 < 0.15, "128 Hz tau 0.048 recovered: %s" % r.get("tau_s"))
    check("error" in analyze(synth(30, 0.22, bright=30.0, over=10.0)), "a rise below 30 levels is no step")
    check("error" in analyze(synth(30, 0.22, dur=5.0)), "a roll ending before peak + 3 s is refused")
    # the rules on synthetic arms (rates as measured on 10-11: 27.8 and 127.9 presents/s)
    def arms(tau_a, tau_b, tau_c, tau_d, ra=27.8, rc=127.9, spread=0.0):
        mk = lambda rate, tau, s: arm_summary([analyze(synth(rate, tau * (1 + s), seed=7)),
                                               analyze(synth(rate, tau * (1 - s), seed=8))])
        return {"A": mk(ra, tau_a, spread), "B": mk(ra, tau_b, 0), "C": mk(rc, tau_c, 0), "D": mk(rc, tau_d, 0)}
    k = 0.8743894696235657 * 0.25 * math.log(2)          # per-update decay exponent
    t30, tu = 1 / (27.8 * k), 1 / (127.9 * k)             # =0 time constants in wall time
    g = grade(arms(t30, t30, tu, 1 / (30 * k)))
    check(g["verdict"] == "ACCEPT", "the expected world (=0 defect, =1 at 30 updates/s) ACCEPTS: %s %s"
          % (g["verdict"], g["reason"]))
    g = grade(arms(t30, t30, tu, tu))
    check(g["verdict"] == "REJECT" and not g["rules"]["3_fix"]["pass"], "=1 doing nothing REJECTS on rule 3")
    g = grade(arms(t30, t30, t30, t30))
    check(g["verdict"] == "REJECT" and not g["rules"]["2_defect"]["pass"], "no defect at =0 REJECTS on rule 2")
    g = grade(arms(t30, t30 / 2, tu, 1 / (30 * k)))
    check(g["verdict"] == "REJECT" and not g["rules"]["4_control"]["pass"], "a broken control REJECTS on rule 4")
    g = grade(arms(t30, t30, tu, 1 / (30 * k), spread=0.2))
    check(g["verdict"] == "INCONCLUSIVE", "A repeats 50 % apart are INCONCLUSIVE")
    # the title-sequence analysis on a synthetic title: black, a cut, a 2 s fade-in, a bright cut at 4 s, a dark cut at
    # 7 s; the game's adaptation law per present (h += (log L - h)(1 - exp2(-0.8744 c))) at 27.8 / 127.9 presents/s
    def seq_roll(rate, scaled, seed, noise=0.4, shift=0.0):
        rnd = random.Random(seed)
        t, h, rows = 0.0, math.log(0.002), []
        while t < 13.0:
            u = t - 3.0 - shift
            s = 0.002 if u < 0 else (0.3 + 0.7 * min(1.0, u / 2.0)) if u < 4.0 else 1.8 if u < 7.0 else 0.4
            c = 0.25 * (30.0 / rate if scaled else 1.0)
            h += (math.log(s) - h) * (1 - 2 ** (-0.8743894696235657 * c))
            e = min(8.0, max(0.25, 0.5 / math.exp(h)))
            x = s * e
            rows.append((t, 4.0 + 251.0 * x / (1.0 + x) + rnd.gauss(0, noise)))
            t += 1.0 / rate
        return seq_from_rows(rows, 2.5 + shift)
    arms = {"A": [seq_roll(27.8, False, 1), seq_roll(27.8, False, 2, shift=0.013)], "B": [seq_roll(27.8, True, 3)],
            "C": [seq_roll(127.9, False, 4), seq_roll(127.9, False, 5)],
            "D": [seq_roll(127.9, True, 6), seq_roll(127.9, True, 7, shift=0.004)]}
    g = grade_seq({k: list(v) for k, v in arms.items()})
    check(g["verdict"] == "ACCEPT", "seq: the expected world ACCEPTS: %s %s %s" % (g["verdict"], g["reason"],
                                                                                 {k: g["rms"][k] for k in g["rms"]}))
    check(g["cut"]["t_s"] is not None and abs(g["cut"]["t_s"] - 4.0) < 0.1, "seq: the settle cut found at 4 s: %s"
          % g["cut"])
    g = grade_seq({"A": arms["A"], "B": arms["B"], "C": arms["C"], "D": arms["C"]})
    check(g["verdict"] == "REJECT" and not g["rules"]["fix"]["pass"], "seq: =1 doing nothing REJECTS on fix")
    g = grade_seq({"A": arms["A"], "B": arms["B"], "C": arms["D"], "D": arms["D"]})
    check(g["verdict"] == "REJECT" and not g["rules"]["defect"]["pass"], "seq: no defect at =0 REJECTS on defect")
    g = grade_seq({"A": [arms["A"][0], seq_roll(27.8, False, 9, noise=8.0)], "B": arms["B"], "C": arms["C"],
                   "D": arms["D"]})
    check(g["verdict"] == "INCONCLUSIVE", "seq: unrepeatable A rolls are INCONCLUSIVE: %s" % g.get("rms"))
    # the freeze-relaxation analysis: holds of a frozen scene, the history relaxing by the game's law per present
    def freeze_roll(rate, scaled, seed, holds=6, noise=0.003):
        rnd = random.Random(seed)
        recs, t, hid, pn = [], 0.0, 0, 0
        for _ in range(holds):
            hid += 1
            cur, h = math.log(rnd.uniform(0.2, 1.5)), math.log(rnd.uniform(0.2, 1.5))   # mismatch at the hold start
            t0 = t
            while t < t0 + 1.5:
                c = 0.25 * (30.0 / rate if scaled else 1.0)
                h += (cur - h) * (1 - 2 ** (-0.8743894696235657 * c))
                x = math.exp(cur) * min(8.0, max(0.25, 0.5 / math.exp(h)))
                pn += 1
                recs.append((t, 4.0 + 251.0 * x / (1.0 + x) + rnd.gauss(0, noise), hid, (t - t0) * 1000.0 + 60.0, None,
                             pn, 0))
                t += 1.0 / rate
            t += 1.5   # unheld time between holds (no records needed)
            pn += int(1.5 * rate)
        return hold_fits(recs)
    fz = {"A": [freeze_roll(29.9, False, 1), freeze_roll(29.9, False, 2)], "B": [freeze_roll(29.9, True, 3)],
          "C": [freeze_roll(115.0, False, 4), freeze_roll(115.0, False, 5)],
          "D": [freeze_roll(115.0, True, 6), freeze_roll(115.0, True, 7)]}
    g = grade_freeze(dict(fz))
    check(g["verdict"] == "ACCEPT", "freeze: the expected world ACCEPTS: %s %s %s" % (
        g["verdict"], g["reason"], {k: g["arms"][k]["tau_s"] for k in "ABCD"}))
    g = grade_freeze({"A": fz["A"], "B": fz["B"], "C": fz["C"], "D": fz["C"]})
    check(g["verdict"] == "REJECT" and not g["rules"]["fix"]["pass"], "freeze: =1 doing nothing REJECTS on fix")
    g = grade_freeze({"A": fz["A"], "B": fz["B"], "C": fz["D"], "D": fz["D"]})
    check(g["verdict"] == "REJECT" and not g["rules"]["defect"]["pass"], "freeze: no defect at =0 REJECTS on defect")
    g = grade_freeze({"A": [freeze_roll(29.9, True, 8, holds=2)], "B": fz["B"], "C": fz["C"], "D": fz["D"]})
    check(g["verdict"] == "INCONCLUSIVE", "freeze: < 4 gradable A holds are INCONCLUSIVE")

    # amendment 3: each hold opens with ~110 ms of still-moving content (the render catching up with the held sim:
    # the x2set3 failure), its stamp advancing; then the stamp stops and only the exposure relaxes
    def freeze_roll_stamped(rate, scaled, seed, holds=6, catchup=0.11, fits=True):
        rnd = random.Random(seed)
        recs, t, hid, stamp, pn = [], 0.0, 0, 0, 0
        for _ in range(holds):
            hid += 1
            cur, h = math.log(rnd.uniform(0.3, 1.2)), math.log(rnd.uniform(0.3, 1.2))
            t0 = t
            while t < t0 + 1.5:
                moving = t - t0 < catchup
                if moving:
                    cur = math.log(rnd.uniform(0.3, 1.2))   # content still changing
                    stamp += 33_333_334   # the shown time still advancing (ns per sim tick)
                c = 0.25 * (30.0 / rate if scaled else 1.0)
                h += (cur - h) * (1 - 2 ** (-0.8743894696235657 * c))
                x = math.exp(cur) * min(8.0, max(0.25, 0.5 / math.exp(h)))
                jit = 0 if moving else rnd.choice((-1_000_000, 0, 1_000_000))   # the 1 ms clock grid (x2set4)
                pn += 1
                recs.append((t, 4.0 + 251.0 * x / (1.0 + x) + rnd.gauss(0, 0.003), hid, (t - t0) * 1000.0, stamp + jit,
                             pn, 0))
                t += 1.0 / rate
            t += 1.5
            pn += int(1.5 * rate)
            stamp += 3_000_000_000
        return hold_fits(recs) if fits else recs
    st = {"A": [freeze_roll_stamped(29.9, False, 11), freeze_roll_stamped(29.9, False, 12)],
          "B": [freeze_roll_stamped(29.9, True, 13)],
          "C": [freeze_roll_stamped(115.0, False, 14), freeze_roll_stamped(115.0, False, 15)],
          "D": [freeze_roll_stamped(115.0, True, 16), freeze_roll_stamped(115.0, True, 17)]}
    g = grade_freeze(dict(st))
    check(g["verdict"] == "ACCEPT", "freeze (stamps): catch-up content before the stamp stops is excluded: %s %s %s" % (
        g["verdict"], g["reason"], {k: g["arms"][k]["tau_s"] for k in "ABCD"}))
    # Astra 2026-10-18 P2 (cadence): the uncorrected 30 Hz A arm fed as D used to ACCEPT on all three graders
    g = grade_freeze({"A": st["A"], "B": st["B"], "C": st["C"], "D": st["A"]})
    check(g["verdict"] == "INCONCLUSIVE" and not g["rules"]["cadence"]["pass"],
          "cadence: freeze with A's 30 Hz rolls as D is INCONCLUSIVE: %s" % g.get("reason"))
    g = grade_freeze({"A": st["A"], "B": st["C"], "C": st["C"], "D": st["D"]})
    check(g["verdict"] == "INCONCLUSIVE", "cadence: freeze with an uncapped control B is INCONCLUSIVE")
    sa = {"A": arm_summary([analyze(synth(27.8, t30, seed=7)), analyze(synth(27.8, t30, seed=8))]),
          "B": arm_summary([analyze(synth(27.8, t30, seed=9))]), "C": arm_summary([analyze(synth(127.9, tu, seed=10))])}
    g = grade(dict(sa, D=sa["A"]))
    check(g["verdict"] == "INCONCLUSIVE" and not g["rules"]["0_cadence"]["pass"],
          "cadence: the step grader with A as D is INCONCLUSIVE: %s" % g.get("reason"))
    g = grade_seq({"A": arms["A"], "B": arms["B"], "C": arms["C"], "D": arms["A"]})
    check(g["verdict"] == "INCONCLUSIVE" and not g["rules"]["cadence"]["pass"],
          "cadence: the sequence grader with A as D is INCONCLUSIVE: %s" % g.get("reason"))
    # Astra 2026-10-18 P2 (continuity): missing presents (pn gaps) or a rising skipped count reject the hold
    raw = freeze_roll_stamped(115.0, True, 16, fits=False)
    gaps = hold_fits([r[:5] + (r[5] * 2, 999999) for r in raw])
    check(gaps and all(f["discontinuous"] and not f["gradable"] for f in gaps),
          "continuity: doubled present ordinals reject every hold: %s" % [f["discontinuous"] for f in gaps][:2])
    skip = hold_fits([r[:6] + (1 if r[3] > 1000.0 else 0,) for r in raw])
    check(skip and all(f["discontinuous"] and not f["gradable"] for f in skip),
          "continuity: a skipped count rising inside the window rejects the hold")
    g = grade_freeze({"A": st["A"], "B": st["B"], "C": st["C"], "D": [gaps, gaps]})
    check(g["verdict"] == "INCONCLUSIVE", "continuity: an arm of discontinuous holds is INCONCLUSIVE")
    check(all(not f["discontinuous"] for f in hold_fits(raw)), "continuity: the intact roll has no discontinuity")
    # Astra 2026-10-18 P3: a black sequence (nothing above SEQ_FIRST) is an ungradable roll, not a NameError
    try:
        r = seq_from_rows([(i / 30.0, 4.0) for i in range(600)], 2.0)
        check("error" in r, "seq: an all-black roll is ungradable: %s" % r.get("error"))
    except Exception as e:   # noqa: BLE001 - the bug being tested
        check(False, "seq: an all-black roll raised %r" % e)
    # Astra 2026-10-18 P2 (anchor): the freeze CLI on rolls written to disk; a missing / malformed report.json or a
    # report.json without the title era is INCONCLUSIVE (exit 3), never every hold of the roll
    import contextlib
    import io
    import shutil
    import tempfile
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    os.makedirs(os.path.join(here, "archive"), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="adapt_selftest_cli_", dir=os.path.join(here, "archive"))
    try:
        good = json.dumps({"eras": [{"era": "title", "t0_s": 4.0, "dur_s": 100.0}]})
        def write_roll(name, recs, report=good):
            d = os.path.join(tmp, name)
            os.makedirs(os.path.join(d, "perf"), exist_ok=True)
            with open(os.path.join(d, "perf", "present_luma.csv"), "w", encoding="utf-8") as f:
                f.write("# synthetic\npn,t_us,luma,w,h,skipped,hold,held_ms,stamp\n")
                for t, l, h, ms, stp, pn, sk in recs:
                    f.write("%d,%d,%.4f,64,40,%d,%d,%.1f,%d\n" % (pn, int(round(t * 1e6)) + 5_000_000, l, sk, h, ms, stp))
            rp = os.path.join(d, "report.json")
            if report is None:
                if os.path.exists(rp):
                    os.remove(rp)
            else:
                with open(rp, "w", encoding="utf-8") as f:
                    f.write(report)
            return d
        spec = {"A": [(29.9, False, 11), (29.9, False, 12)], "B": [(29.9, True, 13)],
                "C": [(115.0, False, 14), (115.0, False, 15)], "D": [(115.0, True, 16), (115.0, True, 17)]}
        recs = {"%s%d" % (k, i + 1): freeze_roll_stamped(r, sc, sd, fits=False)
                for k, v in spec.items() for i, (r, sc, sd) in enumerate(v)}
        for name, rr in recs.items():
            write_roll(name, rr)
        def cli():
            args = ["freeze", "--out", os.path.join(tmp, "out")]
            for k, v in spec.items():
                args += ["--arm", "%s=%s" % (k, ",".join(os.path.join(tmp, "%s%d" % (k, i + 1)) for i in range(len(v))))]
            with contextlib.redirect_stdout(io.StringIO()):
                rc = main(args)
            with open(os.path.join(tmp, "out", "adapt_freeze.json"), encoding="utf-8") as f:
                return rc, json.load(f)["verdict"]
        rc, v = cli()
        check(rc == 0 and v["verdict"] == "ACCEPT", "anchor: the CLI on intact rolls ACCEPTS: %s %s" % (rc, v.get("reason")))
        for what, rep in (("a missing report.json", None), ("a malformed report.json", "{not json"),
                          ("a report.json that is a list", "[]"),
                          ("a report.json without the title era", json.dumps({"eras": [{"era": "boot", "t0_s": 0.0}]}))):
            write_roll("A2", recs["A2"], rep)
            rc, v = cli()
            check(rc == 3 and v["verdict"] == "INCONCLUSIVE" and "anchor" in v["reason"],
                  "anchor: %s in one A roll is INCONCLUSIVE (exit %s)" % (what, rc))
        write_roll("A2", recs["A2"])
        for n in ("D1", "D2"):   # the review's mutation: ordinals doubled, every skip count 999999
            write_roll(n, [r[:5] + (r[5] * 2, 999999) for r in recs[n]])
        rc, v = cli()
        check(rc == 3 and v["verdict"] == "INCONCLUSIVE", "continuity: the CLI on D rolls with present gaps is "
              "INCONCLUSIVE (exit %s): %s" % (rc, v.get("reason")))
        with open(os.path.join(tmp, "nopn.csv"), "w", encoding="utf-8") as f:
            f.write("t_us,luma,hold,held_ms,stamp\n1000,50.0,1,10.0,5\n")
        try:
            load_holds(os.path.join(tmp, "nopn.csv"))
            check(False, "continuity: a CSV without pn is refused")
        except ValueError:
            check(True, "continuity: a CSV without pn is refused")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # the instrument check: synthetic dumps + the CSV the probe would write (pn = dump number + 1)
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image is not None:
        import shutil
        import tempfile
        here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        os.makedirs(os.path.join(here, "archive"), exist_ok=True)
        tmp = tempfile.mkdtemp(prefix="adapt_selftest_", dir=os.path.join(here, "archive"))
        try:
            rnd = random.Random(3)
            roll, dumps = os.path.join(tmp, "roll", "perf"), os.path.join(tmp, "dumps")
            os.makedirs(roll)
            os.makedirs(dumps)
            lines = ["# synthetic", "pn,t_us,luma,w,h,skipped"]
            for p in range(600, 606):
                img = Image.new("RGB", (64, 40))
                img.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(64 * 40)])
                img.save(os.path.join(dumps, "present_UP_%03d_p%06d_64x40.bmp" % (p - 600, p)))
                px = img.load()
                tot = sum(299 * px[x, ((2 * r + 1) * 40) // 80][0] + 587 * px[x, ((2 * r + 1) * 40) // 80][1] +
                          114 * px[x, ((2 * r + 1) * 40) // 80][2] for r in range(40) for x in range(0, 64, 2))
                lines.append("%d,%d,%.4f,64,40,0" % (p + 1, p * 33333, tot / (1000.0 * 40 * 32)))
            csvp = os.path.join(roll, "present_luma.csv")
            with open(csvp, "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")
            ic = instrument_check(os.path.dirname(roll), dumps)
            check(ic["pass"] and ic["best_offset"] == 1 and ic["matched"] == 6,
                  "the instrument check matches 6/6 dumps at one offset: %s"
                  % {k: ic[k] for k in ("matched", "best_offset", "offsets", "worst")})
            # repeated frames (a movie frame on two presents) tie two offsets: the per-offset count still finds +1
            tie = list(lines)
            f4, f3 = tie[4].split(","), tie[3].split(",")   # lines: comment, header, then pn 601, 602, 603 ...
            f4[2] = f3[2]
            tie[4] = ",".join(f4)     # record pn 603 (dump 602's) now equals pn 602 (dump 601's): a repeated frame
            d602 = os.path.join(dumps, "present_UP_002_p000602_64x40.bmp")
            keep = os.path.join(tmp, "orig_602.bmp")
            shutil.move(d602, keep)
            Image.open(os.path.join(dumps, "present_UP_001_p000601_64x40.bmp")).save(d602)
            with open(csvp, "w", encoding="utf-8") as f:
                f.write("\n".join(tie) + "\n")
            ic = instrument_check(os.path.dirname(roll), dumps)
            check(ic["pass"] and ic["best_offset"] == 1, "a repeated frame (two offsets tie) still passes at +1: %s"
                  % {k: ic[k] for k in ("matched", "best_offset", "offsets")})
            os.remove(d602)
            shutil.move(keep, d602)   # the original frame back
            f4 = lines[4].split(",")
            f4[2] = "%.4f" % (float(f4[2]) + 0.5)
            with open(csvp, "w", encoding="utf-8") as f:   # one record off by 0.5 level
                f.write("\n".join(lines[:4] + [",".join(f4)] + lines[5:]) + "\n")
            check(not instrument_check(os.path.dirname(roll), dumps)["pass"], "a record 0.5 level off FAILS the check")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not fails else "FAIL: %d case(s)" % len(fails))
    return 0 if not fails else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("curve")
    c.add_argument("roll")
    g = sub.add_parser("grade")
    g.add_argument("--arm", action="append", required=True, help="A=<roll>,<roll> (A, B, C, D)")
    g.add_argument("--out", required=True)
    pl = sub.add_parser("pilot", help="the pilot roll's instrument check (present dumps vs the CSV) + stimulus check")
    pl.add_argument("roll")
    pl.add_argument("--dumps", required=True, help="the roll's SR_PRESENT_DUMP_DIR")
    pl.add_argument("--json")
    for x in (c, g, pl):
        x.add_argument("--after", type=float, default=0.0, help="seconds after the first record before a step counts")
        x.add_argument("--anchor", default="", help="a perf_report era (e.g. title): the step search starts 1 s before it")
    sq = sub.add_parser("seq", help="the title-sequence analysis (amendment x2set2): --arm A=<roll>,<roll> ...")
    sq.add_argument("--arm", action="append", required=True)
    sq.add_argument("--out", required=True)
    sq.add_argument("--anchor", default="title")
    fz = sub.add_parser("freeze", help="the freeze-relaxation analysis (amendment 2, x2set3): --arm A=<roll>,<roll> ...")
    fz.add_argument("--arm", action="append", required=True)
    fz.add_argument("--out", required=True)
    fz.add_argument("--anchor", default="title", help="holds count at or after this perf_report era's start")
    ins = sub.add_parser("instrument", help="the probe check alone: present dumps vs the roll's CSV")
    ins.add_argument("roll")
    ins.add_argument("--dumps", required=True)
    ins.add_argument("--json")
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    if a.cmd == "instrument":
        ic = instrument_check(a.roll, a.dumps)
        if a.json:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump(ic, f, indent=1)
        print(json.dumps({k: v for k, v in ic.items() if k != "misses"}, indent=1), "misses:", ic["misses"][:5])
        return 0 if ic["pass"] else 1
    if a.cmd == "freeze":
        per_arm, meta, missing = {}, {}, []
        for spec in a.arm:
            name, _, paths = spec.partition("=")
            name = name.strip().upper()
            per_arm[name], meta[name] = [], []
            for p in [x.strip() for x in paths.split(",") if x.strip()]:
                t_from = era_start(p, a.anchor) if a.anchor else None
                if a.anchor and t_from is None:   # Astra 2026-10-18 P2: never fall back to every hold of the roll
                    missing.append(p)
                    meta[name].append({"roll": p, "error": "no '%s' era in report.json (missing or malformed)" % a.anchor})
                    continue
                fits = hold_fits(load_holds(p), t_from)
                per_arm[name].append(fits)
                meta[name].append({"roll": p, "t_from_s": t_from, "holds": len(fits),
                                   "gradable": sum(1 for f in fits if f["gradable"]),
                                   "discontinuous": sum(1 for f in fits if f["discontinuous"])})
        g = grade_freeze(per_arm)
        if missing:
            g = {"verdict": "INCONCLUSIVE", "reason": "the '%s' anchor is unavailable in %s" % (a.anchor, missing),
                 "graded_without_them": g}
        os.makedirs(a.out, exist_ok=True)
        doc = {"schema": "adapt-freeze/1", "verdict": g, "rolls": meta,
               "holds": {k: [[{kk: vv for kk, vv in f.items() if kk != "curve"} for f in roll] for roll in v]
                         for k, v in per_arm.items()}}
        with open(os.path.join(a.out, "adapt_freeze.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        plotted = plot_freeze(per_arm, os.path.join(a.out, "adapt_freeze.png"))
        print(json.dumps(g, indent=1))
        print("wrote", os.path.join(a.out, "adapt_freeze.json"), "(+ adapt_freeze.png)" if plotted else "")
        return {"ACCEPT": 0, "REJECT": 1}.get(g.get("verdict"), 3)
    if a.cmd == "seq":
        per_arm, rows_of = {}, {}
        for spec in a.arm:
            name, _, paths = spec.partition("=")
            name = name.strip().upper()
            per_arm[name] = []
            for p in [x.strip() for x in paths.split(",") if x.strip()]:
                rows, skipped = load(p)
                r = seq_curve(p, rows, a.anchor)
                r["roll"], r["skipped"] = p, skipped
                per_arm[name].append(r)
        g = grade_seq({k: list(v) for k, v in per_arm.items()})
        missing = [r["roll"] for v in per_arm.values() for r in v if r.get("anchor_missing")]
        if missing:   # Astra 2026-10-18 P2: a requested anchor that is unavailable makes the set INCONCLUSIVE
            g = {"verdict": "INCONCLUSIVE", "reason": "the '%s' anchor is unavailable in %s" % (a.anchor, missing)}
        os.makedirs(a.out, exist_ok=True)
        doc = {"schema": "adapt-seq/1", "verdict": {k: v for k, v in g.items() if k != "mean_curves"},
               "rolls": {k: [{kk: vv for kk, vv in r.items() if kk not in ("curve", "rel")} for r in v]
                         for k, v in per_arm.items()},
               "grid": {"from_s": SEQ_GRID[0], "to_s": SEQ_GRID[1], "step_s": GRID_S},
               "mean_curves": {k: [round(x, 3) for x in v] for k, v in g.get("mean_curves", {}).items()}}
        with open(os.path.join(a.out, "adapt_seq.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        plotted = plot_seq(per_arm, g.get("mean_curves", {}), os.path.join(a.out, "adapt_seq.png"))
        print(json.dumps(doc["verdict"], indent=1))
        print("wrote", os.path.join(a.out, "adapt_seq.json"), "(+ adapt_seq.png)" if plotted else "")
        return {"ACCEPT": 0, "REJECT": 1}.get(g.get("verdict"), 3)
    if a.cmd == "selftest":
        return selftest()
    if a.cmd in ("curve", "pilot"):
        rows, skipped = load(a.roll)
        try:
            r = analyze(rows, after_for(a.roll, rows, a.after, a.anchor))
        except ValueError as e:   # the requested anchor is unavailable: bad input, not a measurement
            print(json.dumps({"error": str(e)}))
            return 2
        r.pop("progress", None)
        r["records"], r["skipped"] = len(rows), skipped
        if a.cmd == "curve":
            print(json.dumps(r, indent=1))
            return 0 if "error" not in r else 2
        doc = {"schema": "adapt-pilot/1", "curve": r, "stimulus": stimulus_check(r, rows),
               "instrument": instrument_check(a.roll, a.dumps)}
        doc["pass"] = doc["stimulus"]["pass"] and doc["instrument"]["pass"]
        if a.json:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=1)
        print(json.dumps(doc, indent=1))
        return 0 if doc["pass"] else 1
    per_arm, rolls = {}, {}
    for spec in a.arm:
        name, _, paths = spec.partition("=")
        name = name.strip().upper()
        rolls[name] = []
        for p in [x for x in paths.split(",") if x.strip()]:
            rows, skipped = load(p.strip())
            try:
                r = analyze(rows, after_for(p.strip(), rows, a.after, a.anchor))
            except ValueError as e:   # the anchor is unavailable (Astra 2026-10-18 P2): INCONCLUSIVE below
                r = {"error": str(e), "anchor_missing": True}
            r["roll"], r["records"], r["skipped"] = p.strip(), len(rows), skipped
            rolls[name].append(r)
        per_arm[name] = arm_summary(rolls[name])
    verdict = grade(per_arm)
    missing = [r["roll"] for v in rolls.values() for r in v if r.get("anchor_missing")]
    if missing:
        verdict = {"verdict": "INCONCLUSIVE", "reason": "the '%s' anchor is unavailable in %s" % (a.anchor, missing)}
    os.makedirs(a.out, exist_ok=True)
    doc = {"schema": "adapt-grade/1", "verdict": verdict, "anchor": a.anchor, "after": a.after,
           "arms": {k: {kk: vv for kk, vv in v.items() if kk != "progress"} for k, v in per_arm.items()},
           "rolls": {k: [{kk: vv for kk, vv in r.items() if kk != "progress"} for r in v] for k, v in rolls.items()}}
    with open(os.path.join(a.out, "adapt_grade.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    plotted = plot(rolls, os.path.join(a.out, "adapt_curves.png"))
    print(json.dumps(doc["verdict"], indent=1))
    print("arms:", json.dumps(doc["arms"], indent=1))
    print("wrote", os.path.join(a.out, "adapt_grade.json"), "(+ adapt_curves.png)" if plotted else "")
    return {"ACCEPT": 0, "REJECT": 1}.get(verdict["verdict"], 3)


if __name__ == "__main__":
    sys.exit(main())
