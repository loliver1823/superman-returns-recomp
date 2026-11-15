#!/usr/bin/env python3
"""route_repeat.py - the pre-registered repeatability test of a canary route (Fable ruling 24(3c);
s4-audit archive/step4_claude_20261018/FLIGHT_ROUTE_DESIGN.md section 5, approved 2026-10-22 ~00:3x).

    python tools/perf/route_repeat.py --route <a2 route file> --route-sha256 <its frozen sha256>
        [--profile k | --profile xenia --xenia-sha256 <the install's sha256>] --runs <run dir> ... --out <result.json>

The runs are canary_compare run folders (run.json, events.jsonl, shots\\, probe\\). The experiment is pinned before its
rolls exist; a run that is not of it makes the input INVALID (exit 2), whatever its numbers:
- the route: the file's sha256 must be --route-sha256 (the frozen one); every run's run.json records that route's
  name, file sha256, tape sha256 (canary_routes.tape_bytes of that file) and scene windows; no --survey run, no
  --extra-gates run;
- the profile: k = the 10-09 exe snapshot 12da45b9c7d5-canary (K_EXE_SHA256), config ours-1x-unc, N = 6, every run
  with the a2 log pin (canary_routes.A2_LOG_PIN, SR_LOG=dev; the coordinator, 2026-10-22: F2 / F3 checkable on every
  K roll; a --no-a2-log-pin run is refused); xenia = stock Canary (the exe sha256 given), config xenia-1x, N = 3, no
  log pin (Xenia's side never carries it); kperf (Fable ruling 42, 2026-10-23) = k's exe, config and N with EVERY
  run unpinned (--no-a2-log-pin: log_pin null, log_pin_off true; a pinned run is refused), graded exactly as k - the
  performance target's block, while the pinned k block stays the route-validity receipt;
- N distinct runs: distinct folders (real paths) and distinct run.json / events.jsonl contents (no copy of a run);
- readable inputs: every successful shot event's luma a finite number, every kept shot a readable image, every fps
  a finite number or absent.
Evidence rules (Astra round 3): a BLANK capture (its 64x36 thumbnail spans <= canary_routes.BLANK_RANGE levels) is
a missing shot; a scene's fps is measured only when the probe lost no record and its presents cover the window
(the first and the last within COVER_EDGE_S of its edges) - else it is unmeasured (a missing fps).

Profile k, everything fixed before the test's rolls (no recalibration after them):
1. Views - every run on view, by BOTH tests, for flight, wall and pause:
   (a) its shot's luma (the harness's own 64x36 mean, events.jsonl) within LUMA_BAND (1.5) of that scene's median;
   (b) its shot's 64x36 grayscale thumbnail distance to the scene's MEDOID shot (the run whose shot has the smallest
       summed distance to the others) <= tau: flight 3.0, wall 1.5, pause 1.5 (canary_routes.TAU, calibrated on the
       existing K shots: audit\\route\\view_distance_calib.json).
2. Per-scene present-fps CV over the runs (statistic: CV_METHOD, compared unrounded):
   - FLIGHT (Fable ruling 36 (3), pre-registered 2026-10-22 before any a2 K roll): the bar is FLIGHT_BAR = 3.0 %; a CV
     above it and below FLIGHT_FABLE_EDGE (6.4 %) gets NO re-baselined bar - the verdict is FABLE (back to Fable with
     the rolls: "flight noisier, or a2 wrong?"); at or above 6.4 % it is a FAIL. a1's 6.4 % (the street-wall mixture's
     CV, Fable 35) is never a fallback bar. At least 5 K rolls behind it (N = 6).
   - wall and pause: each BELOW today's (wall 11.1 %, pause 15.1 %: lane D's 10-21 K rolls), TARGET <= 3.0 % each;
     below today's but above the target = PARTIAL.
3. No regression where nothing changed: street and on-foot CV each <= today's (2.1 %) + 1.0 point.
Verdict: FAIL (a missing shot or fps, a run off view, a street / on-foot regression, wall or pause not below today's,
flight >= 6.4 %), else FABLE (flight in (3.0, 6.4) %), else PASS (flight <= 3.0 and wall / pause at the target), else
PARTIAL (wall / pause below today's, not at the target).
Profile xenia (section 5's "Xenia, before C3 uses it"): every run's flight, wall and pause shot luma within LUMA_BAND
of Xenia's own median (Xenia renders its own luma: its reference is its own); the medoid distances and the CVs are
reported, not graded. Verdict PASS or FAIL.
INFORMATION SCENES (Fable ruling 38, 2026-10-23). A route file's "information_scenes" (canary_routes.
information_scenes; a2-roof-late: ["wall"]) are measured and reported - views, CV, a missing shot or fps - but never
graded. They take no part in "every run on view", in the missing-shot / missing-fps FAIL, or in the wall / pause CV
rule; the R-c guard stays the wall's only regression guard. The flight scene is always graded.
Exit 0 PASS, 3 PARTIAL, 5 FABLE, 1 FAIL, 2 INVALID or unusable input.
"""
import argparse
import bisect
import hashlib
import json
import math
import os
import statistics
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import canary_routes as routes  # noqa: E402

K_EXE_SHA256 = "12da45b9c7d58d75adcd90b55cabe46395c1b9d86d40cd992b491fb4b6539b5b"  # exe_snapshots\12da45b9c7d5-canary
# log_pin / log_pin_off: what every run of the profile must record (canary_compare's run.json). k = the pinned K block
# (the route-validity receipt, Fable 42 (1)); kperf = the UNPINNED K block, every run --no-a2-log-pin (the performance
# target, Fable 42 (2)); xenia = Xenia's side, which never carries the pin.
PROFILES = {"k": {"n": 6, "config": "ours-1x-unc", "exe_sha256": K_EXE_SHA256, "log_pin": routes.A2_LOG_PIN,
                  "log_pin_off": False},
            "kperf": {"n": 6, "config": "ours-1x-unc", "exe_sha256": K_EXE_SHA256, "log_pin": None, "log_pin_off": True},
            "xenia": {"n": 3, "config": "xenia-1x", "exe_sha256": None, "log_pin": None, "log_pin_off": None}}
K_PROFILES = ("k", "kperf")   # graded alike; they differ only in the log pin
NEEDED_N = PROFILES["k"]["n"]
TODAY_CV = {"flight": 6.4, "wall": 11.1, "pause": 15.1, "street": 2.1, "on_foot": 2.1}  # lane D, 10-21 K rolls
TARGET_CV = 3.0
FLIGHT_BAR = 3.0          # Fable 36 (3): flight's CV bar, pre-registered before any a2 K roll
FLIGHT_FABLE_EDGE = 6.4   # Fable 36 (6): (FLIGHT_BAR, this) goes back to Fable; never a fallback bar (36 (7))
REGRESSION_POINTS = 1.0
COVER_EDGE_S = 1.0  # a graded scene's presents must start and end within 1 s of its window's edges
CV_METHOD = "sample"  # statistics.stdev (n - 1) / mean x 100 - lane D's method, confirmed before the test


class Unusable(ValueError):
    pass


def cv(values):
    """The raw (unrounded) CV in %; None for fewer than two values or a zero mean."""
    if len(values) < 2 or not statistics.mean(values):
        return None
    sd = statistics.stdev(values) if CV_METHOD == "sample" else statistics.pstdev(values)
    return 100.0 * sd / statistics.mean(values)


def _r(x, nd=3):
    return None if x is None else round(x, nd)


def _finite(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


def _file_sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def _scenes_key(scenes):
    return json.dumps(scenes, sort_keys=True)


def pin_of(route_path, route_sha256, profile, xenia_sha256=None):
    """The experiment's frozen identity, from the route file (refused unless it is the frozen one) and the profile."""
    if profile not in PROFILES:
        raise Unusable(f"unknown profile {profile!r}")
    route = routes.load_route(route_path)
    if route["_sha256"] != route_sha256:
        raise Unusable(f"{route_path} is not the frozen route file (sha256 {route['_sha256']}, frozen {route_sha256})")
    exe = PROFILES[profile]["exe_sha256"] if profile in K_PROFILES else xenia_sha256
    if not exe:
        raise Unusable("the xenia profile needs --xenia-sha256 (the read-only install's sha256)")
    log_pin = PROFILES[profile]["log_pin"]
    return {"profile": profile, "n": PROFILES[profile]["n"], "config": PROFILES[profile]["config"], "exe_sha256": exe,
            "log_pin": dict(log_pin) if log_pin else None, "log_pin_off": PROFILES[profile]["log_pin_off"],
            "route_name": routes.name_of(route),
            "route_file_sha256": route["_sha256"],
            "tape_sha256": routes.sha256_bytes(routes.tape_bytes(route)), "scenes": routes.scenes_of(route),
            "information_scenes": list(routes.information_scenes(route))}


def load_run(run_dir):
    """One run's identity, fps and shots (thumbnails read here: an unreadable input raises Unusable)."""
    import canary_compare as cc
    meta_path, ev_path = os.path.join(run_dir, "run.json"), os.path.join(run_dir, "events.jsonl")
    try:
        with open(meta_path, encoding="utf-8") as f:
            meta = json.load(f)
        res = cc.analyse_run(run_dir)
        with open(ev_path, encoding="utf-8") as f:
            evs = [json.loads(line) for line in f if line.strip()]
        pres, _guest, pinfo = cc.load_probe(run_dir)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        raise Unusable(f"{run_dir}: {error}") from error
    shots, blank = {}, []
    for e in evs:
        if e.get("ev") == "shot" and e.get("ok") and e.get("name") in routes.POST_SCENES:
            lu, p = e.get("luma"), os.path.join(run_dir, "shots", e["name"] + ".png")
            if not _finite(lu):
                raise Unusable(f"{run_dir}: shot {e['name']} has no finite luma ({lu!r})")
            if os.path.isfile(p):
                try:
                    th = routes.thumb(p)
                except Exception as error:  # noqa: BLE001 (PIL raises several types for a corrupt image)
                    raise Unusable(f"{p}: not a readable image ({error})") from error
                if routes.is_blank(th):  # a blank capture is no evidence: the shot counts as missing
                    blank.append(e["name"])
                    shots.pop(e["name"], None)
                    continue
                if e["name"] in blank:
                    blank.remove(e["name"])
                shots[e["name"]] = {"luma": float(lu), "path": p, "thumb": th}
    # A scene's fps is measured only when the probe lost nothing and its presents cover the window: the first and the
    # last present within COVER_EDGE_S of the window's edges (a probe that stopped early leaves it unmeasured).
    lost = pinfo.get("lost")
    probe_ok = isinstance(lost, int) and not isinstance(lost, bool) and lost == 0
    fps, coverage = {}, {}
    for name, st in res["scenes"].items():
        p = st.get("present") or {}
        v = p.get("fps_raw", p.get("fps"))   # unrounded when the analysis has it (Astra pin r1 #4)
        if v is not None and not _finite(v):
            raise Unusable(f"{run_dir}: scene {name} fps is not a finite number ({v!r})")
        t0, t1 = st["t0"], st["t1"]
        i, j = bisect.bisect_left(pres, t0), bisect.bisect_right(pres, t1)
        first = pres[i] - t0 if i < j else None
        last = t1 - pres[j - 1] if i < j else None
        covered = first is not None and first <= COVER_EDGE_S and last <= COVER_EDGE_S
        coverage[name] = {"first_gap_s": _r(first), "last_gap_s": _r(last), "covered": covered, "probe_lost": lost}
        fps[name] = v if (covered and probe_ok) else None
    return {"run": run_dir, "realpath": os.path.normcase(os.path.realpath(run_dir)),
            "content": (_file_sha(meta_path), _file_sha(ev_path)),
            "exe_sha256": meta.get("exe_sha256") or meta.get("xenia_sha256"), "config": meta.get("config"),
            "route": meta.get("route_name", routes.A1), "route_file_sha256": meta.get("route_file_sha256"),
            "tape_sha256": meta.get("tape_sha256"), "scenes": meta.get("scenes"), "survey": meta.get("survey"),
            "extra_gates": meta.get("extra_gates"), "log_pin": meta.get("log_pin"),
            "information_scenes": (meta["route"].get("information_scenes", []) if isinstance(meta.get("route"), dict)
                                   else []),
            "log_pin_off": meta.get("log_pin_off"), "fps": fps, "shots": shots, "blank_shots": blank,
            "coverage": coverage, "probe_lost": lost,
            "windows": {name: (st["t0"], st["t1"]) for name, st in res["scenes"].items()}}


def identity_problems(runs, pin):
    """Why these runs are not the pinned experiment ([] = they are)."""
    out = []
    if len(runs) != pin["n"]:
        out.append(f"{len(runs)} runs (the {pin['profile']} test is N = {pin['n']})")
    if len({r["realpath"] for r in runs}) != len(runs):
        out.append("a run folder is given more than once")
    if len({r["content"] for r in runs}) != len(runs):
        out.append("two run folders hold the same run (identical run.json and events.jsonl)")
    want = {"exe_sha256": pin["exe_sha256"], "config": pin["config"], "route": pin["route_name"],
            "route_file_sha256": pin["route_file_sha256"], "tape_sha256": pin["tape_sha256"]}
    for r in runs:
        name = os.path.basename(r["run"])
        for k, v in want.items():
            if r.get(k) != v:
                out.append(f"{name}: {k} {r.get(k)!r}, the experiment's {v!r}")
        if r.get("scenes") is None or _scenes_key(r["scenes"]) != _scenes_key(pin["scenes"]):
            out.append(f"{name}: its scene windows are not the frozen route's")
        if r.get("survey"):
            out.append(f"{name}: a --survey run")
        if r.get("extra_gates"):
            out.append(f"{name}: an --extra-gates run ({r['extra_gates']})")
        # log_pin_off by identity (True / False / None), so a numeric 1 / 0 is no opt-out record (Astra kperf r1)
        if r.get("log_pin") != pin["log_pin"] or r.get("log_pin_off") is not pin.get("log_pin_off"):
            out.append(f"{name}: log pin {r.get('log_pin')!r} (off: {r.get('log_pin_off')!r}), "
                       f"the experiment's {pin['log_pin']!r} (off: {pin.get('log_pin_off')!r})")
    return out


def _views(runs, scene):
    """(rows, median luma, medoid run) of one scene's shots over the runs; None when a shot is missing."""
    have = [r for r in runs if scene in r["shots"]]
    if len(have) != len(runs):
        return None
    lumas = [r["shots"][scene]["luma"] for r in have]
    med = statistics.median(lumas)
    thumbs = [r["shots"][scene]["thumb"] for r in have]
    sums = [sum(routes.distance(t, u) for u in thumbs) for t in thumbs]
    medoid = sums.index(min(sums))
    rows = []
    for r, lu, t in zip(have, lumas, thumbs):
        d = routes.distance(t, thumbs[medoid])
        rows.append({"run": os.path.basename(r["run"]), "luma": lu, "dluma_raw": abs(lu - med), "distance_raw": d})
    return rows, med, os.path.basename(have[medoid]["run"])


def grade(runs, pin):
    """The verdict of runs that ARE the pinned experiment (identity_problems(runs, pin) == [])."""
    profile = pin["profile"]
    info = set(pin.get("information_scenes") or ())   # Fable 38: reported, never graded
    problems, information, views = [], [], {}
    for scene in routes.POST_SCENES:
        v = _views(runs, scene)
        if v is None:
            (information if scene in info else problems).append(
                f"{scene}: a shot is missing in {sum(1 for r in runs if scene not in r['shots'])} run(s)")
            continue
        rows, med, medoid = v
        for x in rows:
            luma_ok = x["dluma_raw"] <= routes.LUMA_BAND
            dist_ok = x["distance_raw"] <= routes.TAU[scene]
            x["on_view"] = luma_ok and (dist_ok or profile == "xenia")
            x["luma_ok"], x["distance_ok"] = luma_ok, dist_ok
            x["dluma"], x["distance"] = _r(x["dluma_raw"]), _r(x["distance_raw"])
        views[scene] = {"median_luma": med, "medoid": medoid, "tau": routes.TAU[scene], "runs": rows,
                        "graded": ("information only (Fable 38)" if scene in info else
                                   "luma and distance" if profile in K_PROFILES else "luma (distance reported)")}
    graded_views = [s for s in routes.POST_SCENES if s not in info]
    views_ok = all(s in views for s in graded_views) and all(all(x["on_view"] for x in views[s]["runs"])
                                                            for s in graded_views)
    cvs = {}
    for scene in TODAY_CV:
        vals = [r["fps"].get(scene) for r in runs]
        raw = cv(vals) if all(v is not None and v > 0 for v in vals) else None
        cvs[scene] = {"fps": vals, "cv_raw": raw, "cv": _r(raw, 2), "today": TODAY_CV[scene],
                      "graded": scene not in info}
        if raw is None and profile in K_PROFILES:
            (information if scene in info else problems).append(
                f"{scene}: no fps in {sum(1 for v in vals if not v)} run(s)")
    others = tuple(s for s in ("wall", "pause") if s not in info)
    below_today = all(cvs[s]["cv_raw"] is not None and cvs[s]["cv_raw"] < TODAY_CV[s] for s in others)
    at_target = all(cvs[s]["cv_raw"] is not None and cvs[s]["cv_raw"] <= TARGET_CV for s in others)
    fl = cvs["flight"]["cv_raw"]
    flight_class = ("missing" if fl is None else "pass" if fl <= FLIGHT_BAR else
                    "fable" if fl < FLIGHT_FABLE_EDGE else "fail")
    no_regress = all(cvs[s]["cv_raw"] is not None and cvs[s]["cv_raw"] <= TODAY_CV[s] + REGRESSION_POINTS
                     for s in ("street", "on_foot"))
    if profile == "xenia":
        verdict = "PASS" if views_ok and not problems else "FAIL"
    elif problems or not (views_ok and below_today and no_regress) or flight_class in ("missing", "fail"):
        verdict = "FAIL"
    elif flight_class == "fable":
        verdict = "FABLE"
    elif at_target:
        verdict = "PASS"
    else:
        verdict = "PARTIAL"
    return {"verdict": verdict, "problems": problems, "information_scenes": sorted(info),
            "information": information, "views_ok": views_ok, "views": views, "cv": cvs,
            "cv_method": CV_METHOD, "target_cv": TARGET_CV, "below_today": below_today, "at_target": at_target,
            "flight_bar": FLIGHT_BAR, "flight_fable_edge": FLIGHT_FABLE_EDGE, "flight_class": flight_class,
            "street_foot_no_regression": no_regress, "cv_graded": profile in K_PROFILES,
            "frozen": {"tau": routes.TAU, "luma_band": routes.LUMA_BAND, "today_cv": TODAY_CV, "n": pin["n"]}}


def _pin_record(pin):
    return {k: v for k, v in pin.items() if k != "scenes"} | {"scenes_sha256": hashlib.sha256(
        _scenes_key(pin["scenes"]).encode("utf-8")).hexdigest()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--route", required=True, help="the frozen a2 route file")
    ap.add_argument("--route-sha256", required=True, help="its frozen sha256 (FLIGHT_ROUTE_DESIGN.md)")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="k")
    ap.add_argument("--xenia-sha256", default=None)
    ap.add_argument("--runs", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        pin = pin_of(a.route, a.route_sha256, a.profile, a.xenia_sha256)
        runs = [load_run(d) for d in a.runs]
    except (Unusable, OSError, ValueError, KeyError) as error:
        print(f"route_repeat: unusable input: {error}", file=sys.stderr)
        return 2
    bad = identity_problems(runs, pin)
    if bad:
        result = {"verdict": "INVALID", "problems": bad}
    else:
        result = grade(runs, pin)
    result["experiment"] = _pin_record(pin)
    result["runs"] = [os.path.abspath(d) for d in a.runs]
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    for scene, v in result.get("views", {}).items():
        print(f"{scene}: median luma {v['median_luma']}, medoid {v['medoid']}, "
              + ", ".join(f"{x['run']} {'ok' if x['on_view'] else 'OFF'} (d {x['distance']}, dl {x['dluma']})"
                          for x in v["runs"]))
    for scene, v in result.get("cv", {}).items():
        print(f"{scene}: CV {v['cv']} % (today {v['today']} %)")
    print(f"route_repeat: {result['verdict']} {result['problems']}")
    return {"PASS": 0, "PARTIAL": 3, "FABLE": 5, "FAIL": 1, "INVALID": 2}[result["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
