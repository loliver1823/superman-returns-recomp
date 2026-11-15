#!/usr/bin/env python3
"""world_floor_validate.py - the world rule's structure test against the archived rolls (tools/world-floor, 2026-10-16).

Why: the flicker gate's world rule (tools/perf/world_floor.py) read a DRAWN dark scene as "no world" - the opening
corridor the cinematic leaves Superman in (s3m_gate: world 6 of 15, 9 dark freezes; qdl1: dark [3, 8]; s3check3's dev
gate: dark [3, 8]), the dusk wall of 2026-10-13 (scm2 / scm2b: world 4-5; dc2 cppm4) - while every true black world
(the startup race, job65's w65_3 / w65_5; the pause menu over a black world, p3a_abp) had to keep failing. The rule
now rescues a frame under the luma floor when its spatially supported central EDGE SHARE reaches STRUCT_EDGE_MIN
(world_floor.structure_scene also requires signal in both uncovered side bands). This script grades every listed
roll under the rule WITHOUT the structure test (threshold 2.0: no
frame qualifies) and WITH it, frame by frame from the archived BMPs (or the sidecar: a roll whose BMPs are gone and
whose sidecar holds no structure measurement must grade EXACTLY as before), and prints the before / after per run
plus the edge-share distribution of every frame the luma floor calls dark - the margin on both sides of the threshold.

    python tools/perf/world_floor_validate.py [--threshold T] [--sweep] [--umbrella DIR] [--only LABEL ...]

Exit 1 on missing/ambiguous/unmeasured coverage, an incomplete tail-40, any world in a black witness's tail,
a failed positive world/drawn expectation, a sidecar-only witness changing, or an unreadable run. Positive world
gates require all selected freezes drawn; drawn roll controls require the existing SPAN_IN_PLAY_MIN share.
The run list is the evidence set of 2026-10-16 (the lane's record: archive/world_floor/NOTES.txt on tools/world-floor,
TOOLKIT 1.8f); paths are under the umbrella (the dev checkout's parent: _worktrees/<lane>/archive/...). Every roll
is graded by world_floor's own inventory (present_index / freeze_world / inventory_rows): the frames the run.log
names, never a glob of what is on disk. The gradient level itself was chosen on the same frames (the lane's
structure_sweep.py: at 8 the darkest drawn corridor, 0.009, sat under a HUD icon over black, 0.035; at 2 they are
0.131 and 0.043).
"""
import argparse
import json
import math
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import world_floor as wf  # noqa: E402

V1 = "_worktrees/native/archive/v1_native"
# (label, path under the umbrella, kind, expectation)
#   gate   a flicker-gate folder (flicker_gate.json): graded on the freeze middles, as the gate grades them
#   roll   a drive (attempt1): every present the log names, plus the last 40 (the tail the job graded)
#   expectation  world: the dark freezes are drawn scenes (should become world) | drawn: a drawn roll (control) |
#                black: a black world (must gain NO world frame) | menu_black: the pause menu over a black world
#                (must gain NO world frame) | unchanged: BMPs gone, sidecar without structure (must grade as before)
RUNS = [
    ("s3m_gate", V1 + "/s3m_gate/gate", "gate", "world"),
    ("qdl1_rel", V1 + "/qdl1/rel_gate", "gate", "world"),
    ("s3check3_dev", V1 + "/s3check3/dev_gate", "gate", "world"),
    ("s3check3_rel", V1 + "/s3check3/rel_gate", "gate", "world"),
    ("s3pre_rel", V1 + "/s3pre/rel_gate", "gate", "world"),
    ("s3pre_dev", V1 + "/s3pre/dev_gate", "gate", "world"),
    ("s3check2_rel", V1 + "/s3check2/rel_gate", "gate", "world"),
    ("s3check2_dev", V1 + "/s3check2/dev_gate", "gate", "world"),
    ("qdl2_rel", V1 + "/qdl2/rel_gate", "gate", "world"),
    ("qdl3_rel", V1 + "/qdl3/rel_gate", "gate", "world"),
    ("s1pre_a_rel", V1 + "/s1pre_a/rel_gate", "gate", "world"),
    ("bgb1", V1 + "/bgb1/bgb_gate", "gate", "world"),
    # the 2026-10-13 dusk-wall gates (scm2 / scm2b "SCENE-CONFOUNDED", dc2, rel1): their BMPs are gone and their
    # sidecars predate the measurement, so they must grade exactly as before (never a guess)
    ("scm2", V1 + "/scm2/scm_gate", "gate", "unchanged"),
    ("scm2b", V1 + "/scm2b/scm_gate", "gate", "unchanged"),
    ("dc2_gate", V1 + "/dc2/dc_gate", "gate", "unchanged"),
    ("rel1", V1 + "/rel1/gate", "gate", "unchanged"),
    # job65 (defect.black_world, the startup race fixed 41572437): =0 rolls 3 and 5 went BLACK (l65.log: "world:
    # BLACK tail-median 0.0"), the others drew; graded on the tail the job graded
    ("w65_1_w0", "_worktrees/native-b/archive/v1_nativeb/w65_1_w0", "roll", "drawn"),
    ("w65_2_w1", "_worktrees/native-b/archive/v1_nativeb/w65_2_w1", "roll", "drawn"),
    ("w65_3_w0", "_worktrees/native-b/archive/v1_nativeb/w65_3_w0", "roll", "black"),
    ("w65_4_w1", "_worktrees/native-b/archive/v1_nativeb/w65_4_w1", "roll", "drawn"),
    ("w65_5_w0", "_worktrees/native-b/archive/v1_nativeb/w65_5_w0", "roll", "black"),
    ("w65_6_w1", "_worktrees/native-b/archive/v1_nativeb/w65_6_w1", "roll", "drawn"),
    ("fg67_freeze", "_worktrees/native-b/archive/v1_nativeb/fg67/freeze", "roll", "drawn"),
    # the pause menu over a black world (its panel alone clears the luma floor: the context + side bands decide)
    ("p3a_abp", "_worktrees/dev/archive/v1_native/p3a_abp", "roll", "menu_black"),
    # boot H3 (expect fail in the ledger for its hang, not its pixels): the world drew (551 of 631 presents)
    ("p3a_cppm4", "_worktrees/dev/archive/v1_native/p3a_cppm4", "roll", "drawn"),
    # the superseded_black witnesses still on disk as sidecars only
    ("dc2_cppm4", V1 + "/dc2/dc_cppm4", "roll", "unchanged"),
    ("zpd2_freeze", V1 + "/zpd2/zpd_freeze", "roll", "unchanged"),
    ("zpd6_gate", V1 + "/zpd6/zpd_gate", "gate", "unchanged"),
]
WORLD = set(wf.WORLD)


def umbrella_default():
    return os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))


def grade_gate(gate_dir):
    """{class: [freeze ids]} as flicker_gate grades the freeze roll under <gate_dir>/freeze (its own freezes list)."""
    with open(os.path.join(gate_dir, "flicker_gate.json"), encoding="utf-8-sig") as f:
        d = json.load(f)
    roll = os.path.join(gate_dir, "freeze")
    return wf.freeze_world(roll, os.path.join(roll, "drive", "attempt1", "run.log"), d["freeze"]["freezes"])


def grade_roll(roll_dir):
    """[(present, class)] of every present the roll's inventory holds, in present order."""
    att = os.path.join(roll_dir, "drive", "attempt1")
    rows = wf.inventory_rows(os.path.join(att, "frames"), os.path.join(att, "run.log"), None)
    return [(p, cls) for p, ts, play, cls in rows]


def frame_paths_gate(gate_dir):
    """{freeze id: frame path} of the graded freeze middles (for the structure readings)."""
    with open(os.path.join(gate_dir, "flicker_gate.json"), encoding="utf-8-sig") as f:
        d = json.load(f)
    roll = os.path.join(gate_dir, "freeze")
    log = os.path.join(roll, "drive", "attempt1", "run.log")
    txt = wf.read_text(log)
    mids = wf.freeze_middles(txt)
    byp = wf.frames_by_present(os.path.join(roll, "drive", "attempt1", "frames"))
    out = {}
    for r in d["freeze"]["freezes"]:
        if r.get("ingame") and "note" not in r:
            mid = mids.get(r["freeze"])
            if mid and mid[0] in byp:
                out[r["freeze"]] = byp[mid[0]]
    return out


def frame_paths_roll(roll_dir):
    att = os.path.join(roll_dir, "drive", "attempt1")
    return wf.frames_by_present(os.path.join(att, "frames"))


def luma_dark(path):
    c = wf.measure(path, "center")
    return wf.valid_measure(c) and (c[0] < wf.SCENE_LUMA_MIN or c[1] > wf.SCENE_DARK_MAX)


def edge_share(path):
    s = wf.measure(path, "structure")
    return s[1] if wf.valid_measure(s) else None


def dist(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "-"
    return "n=%d min %.3f med %.3f max %.3f" % (len(vals), min(vals), statistics.median(vals), max(vals))


def counts(classes):
    c = {}
    for k in classes:
        c[k] = c.get(k, 0) + 1
    return c


def fmt_counts(c):
    order = ["gameplay", "menu_world", "dark", "menu_black", "unmeasured"]
    return " ".join("%s %d" % (k, c[k]) for k in order if c.get(k))


def with_threshold(t, fn):
    old = wf.STRUCT_EDGE_MIN
    wf.STRUCT_EDGE_MIN = t
    try:
        return fn()
    finally:
        wf.STRUCT_EDGE_MIN = old


def expectation_problems(before, after, expect, kind):
    """Assert measured coverage and a positive/negative expectation, never just lack of regression."""
    problems = []
    known = set(WORLD) | {"dark", "menu_black", "unmeasured"}
    for label, rows in (("before", before), ("after", after)):
        ids = [p for p, _ in rows]
        if not rows or len(set(ids)) != len(ids):
            problems.append(label + " inventory empty or ambiguous")
        if any(c not in known or c == "unmeasured" for _, c in rows):
            problems.append(label + " contains missing/unknown measurements")
    if {p for p, _ in before} != {p for p, _ in after}:
        problems.append("before/after inventory differs")
    if problems:
        return problems
    selected = after if kind == "gate" else after[-40:]
    if kind != "gate" and len(selected) != 40:
        problems.append("tail-40 coverage incomplete")
    drawn = sum(c in WORLD for _, c in selected)
    if expect == "world" and drawn != len(selected):
        problems.append("positive world witness did not classify every selected frame as drawn")
    elif expect == "drawn" and drawn / len(selected) < wf.SPAN_IN_PLAY_MIN:
        problems.append("drawn control is below the world-share floor")
    elif expect in ("black", "menu_black") and drawn:
        problems.append("negative witness contains drawn world frames")
    elif expect == "unchanged" and after != before:
        problems.append("sidecar-only witness changed")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--threshold", type=float, default=wf.STRUCT_EDGE_MIN, help="STRUCT_EDGE_MIN under test")
    ap.add_argument("--sweep", action="store_true", help="also grade at 0.04 / 0.06 / 0.12 / 0.16")
    ap.add_argument("--umbrella", default=umbrella_default())
    ap.add_argument("--cache", help="a measurement cache of this run's own (SR_WORLD_FLOOR_CACHE), not the shared one")
    ap.add_argument("--only", nargs="*", help="labels to grade (default: every run)")
    a = ap.parse_args(argv)
    if not math.isfinite(a.threshold) or not 0 <= a.threshold <= 1:
        ap.error("--threshold must be finite and between 0 and 1")
    if a.only is not None and (not a.only or set(a.only) - {r[0] for r in RUNS}):
        ap.error("--only must select existing run labels")
    if a.cache:
        os.environ["SR_WORLD_FLOOR_CACHE"] = a.cache
    thresholds = [a.threshold] + ([t for t in (0.04, 0.06, 0.12, 0.16) if t != a.threshold] if a.sweep else [])
    print("world rule: luma floor %s / %s, structure: grad >= %d, edge share >= %s (testing %s)" % (
        wf.SCENE_LUMA_MIN, wf.SCENE_DARK_MAX, wf.STRUCT_GRAD, wf.STRUCT_EDGE_MIN, thresholds))
    bad = 0
    rows = []
    for label, rel, kind, expect in RUNS:
        if a.only and label not in a.only:
            continue
        path = os.path.join(a.umbrella, rel.replace("/", os.sep))
        if not os.path.isdir(path):
            print("%-14s MISSING %s" % (label, path))
            bad += 1
            continue
        try:
            if kind == "gate":
                before = with_threshold(2.0, lambda: grade_gate(path))
                after = {t: with_threshold(t, lambda: grade_gate(path)) for t in thresholds}
                paths = frame_paths_gate(path)
                dark_ids = sorted(before["dark"] + before["menu_black"])
                shares = {fid: edge_share(paths[fid]) for fid in paths}
                lit = [shares[f] for f in shares if f not in dark_ids]
                drk = [shares[f] for f in dark_ids if f in shares]
                b = "world %d dark %s menu_black %s unmeasured %s" % (
                    wf.world_count(before), before["dark"], before["menu_black"], before["unmeasured"])
                for t in thresholds:
                    w = after[t]
                    rows.append((label, kind, expect, t, wf.world_count(before), wf.world_count(w),
                                 "world %d dark %s menu_black %s unmeasured %s" % (
                                     wf.world_count(w), w["dark"], w["menu_black"], w["unmeasured"])))
                flatten = lambda classes: sorted((p, c) for c, ids in classes.items() for p in ids)
                for issue in expectation_problems(flatten(before), flatten(after[a.threshold]), expect, kind):
                    print("%-14s FAIL: %s" % (label, issue))
                    bad += 1
                print("%-14s %-10s before: %s" % (label, expect, b))
                for t in thresholds:
                    print("%-14s %-10s   at %.2f: %s" % ("", "", t, rows[-len(thresholds) + thresholds.index(t)][6]))
                print("%-14s %-10s   edge share: lit freezes %s | dark-by-luma freezes %s" % ("", "", dist(lit), dist(drk)))
                if drk:
                    print("%-14s %-10s   dark-by-luma per freeze: %s" % ("", "", " ".join(
                        "%d:%s" % (f, "%.3f" % shares[f] if shares.get(f) is not None else "-") for f in dark_ids if f in shares)))
                if expect == "unchanged":
                    for t in thresholds:
                        if after[t] != before:
                            print("%-14s %s: a sidecar-only witness changed at %.2f" % (
                                label, "FAIL" if t == a.threshold else "note (sweep)", t))
                            bad += t == a.threshold
            else:
                before = with_threshold(2.0, lambda: grade_roll(path))
                after = {t: with_threshold(t, lambda: grade_roll(path)) for t in thresholds}
                for issue in expectation_problems(before, after[a.threshold], expect, kind):
                    print("%-14s FAIL: %s" % (label, issue))
                    bad += 1
                paths = frame_paths_roll(path)
                tail = [p for p, _ in before][-40:]
                cb = counts([c for _, c in before])
                tb = counts([c for p, c in before if p in tail])
                print("%-14s %-10s before: all %s | tail-40 %s" % (label, expect, fmt_counts(cb), fmt_counts(tb)))
                darkp = [p for p, c in before if c in ("dark", "menu_black")]
                shares_dark = [edge_share(paths[p]) for p in darkp if p in paths]
                shares_lit = [edge_share(paths[p]) for p, c in before if c in WORLD and p in paths]
                for t in thresholds:
                    ca = counts([c for _, c in after[t]])
                    ta = counts([c for p, c in after[t] if p in tail])
                    wa = sum(ca.get(k, 0) for k in WORLD)
                    wb = sum(cb.get(k, 0) for k in WORLD)
                    twa = sum(ta.get(k, 0) for k in WORLD)
                    twb = sum(tb.get(k, 0) for k in WORLD)
                    print("%-14s %-10s   at %.2f: all %s | tail-40 %s" % ("", "", t, fmt_counts(ca), fmt_counts(ta)))
                    rows.append((label, kind, expect, t, twb, twa, "tail-40 %s | all %s" % (fmt_counts(ta), fmt_counts(ca))))
                    # a black witness is black in its TAIL (the in-game world the job graded; its intro / title /
                    # menu presents are drawn 2D and may rescue): the tail must gain no world frame. Only the
                    # threshold under test counts toward the exit code; a sweep value that fails is the margin, noted
                    if expect in ("black", "menu_black") and twa > twb:
                        print("%-14s %s: a %s witness's tail gained %d world frame(s) at %.2f" % (
                            label, "FAIL" if t == a.threshold else "note (sweep)", expect, twa - twb, t))
                        bad += t == a.threshold
                    if expect == "unchanged" and after[t] != before:
                        print("%-14s %s: a sidecar-only witness changed at %.2f" % (
                            label, "FAIL" if t == a.threshold else "note (sweep)", t))
                        bad += t == a.threshold
                shares_tail = [edge_share(paths[p]) for p in tail if p in paths]
                print("%-14s %-10s   edge share: world-by-luma frames %s | dark-by-luma frames %s | tail-40 %s" % (
                    "", "", dist(shares_lit), dist(shares_dark), dist(shares_tail)))
        except Exception as x:
            print("%-14s ERROR %s: %s" % (label, type(x).__name__, x))
            bad += 1
    wf.save_cache()
    if not rows:
        print("FAIL: no witness rows were validated")
        bad += 1
    print()
    print("%-14s %-5s %-10s %-5s %6s %6s  %s" % ("run", "kind", "expect", "thr", "before", "after", "after (classes)"))
    for r in rows:
        print("%-14s %-5s %-10s %-5.2f %6d %6d  %s" % r)
    print("\n%s" % ("VALIDATE FAIL (%d)" % bad if bad else "VALIDATE OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
