#!/usr/bin/env python3
"""zpd_activity.py - the occlusion queries' activity INSIDE a graded window of one perf_run roll (p2.zpd's bake,
native/zpd-bake; Astra round 1 P2-2): lifetime counters do not show that the queries ran where a step grades.

    python tools/perf/zpd_activity.py pacing --roll <roll dir> --trace-summary <p2trace summary.json> --json <out>
    python tools/perf/zpd_activity.py freeze --roll <roll dir> [--ab] [--min-presents 1000] --json <out>

pacing: the window p2_trace graded (its summary's pacing.window_ids [lo, hi], present ids) against the roll's PERIODIC
census - each [GPU_CP] census (every 120 presents: swaps=, the CP's present count, which the trace's present ids
follow) paired with the [NATIVE_ZPD] census line printed right after it (gfx/native_zpd logLine: cumulative issued /
segs / port). Inside [lo, hi]: queries issued, measured (segs) and ported (port) - and the workload SUSTAINED through
the window: each quarter of it (lo..hi split in four) holds issued queries. The samples must reach both ends of the
window (within one census interval). The trace's own kind-7 'query' records are the CP's fence queries, not these.

freeze (Astra round 2 on native/zpd-bake, P2-1 / P2-2): the frozen pass's COMPARED presents, each by ITS OWN frame's
queries. The compared presents are the ones the pixel grade actually compared: freeze_diff.py's `compared | freeze |
a | b | static|unsettled|plain` rows of the stored table (<roll>_freeze.txt; --ab: the static pairs only, the
unsettled ones are excluded from the A/B), in the holds flicker_gate.grade_freeze_table counted (in game, not a movie,
not unsettled; a hold a movie only partly covers is left out) - never a dump line the grade did not use. A present's
queries are its frame stamp on its [SIM_FREEZE_DUMP] line (gfx/native_zpd FrameStamp: `zpd=<BEGINs> rec=<kRkZpd
records> pm4=<literal packets> fswap=<frame>`, the events the CP parsed between that frame's swap packets, carried to
its present): query-bearing = zpd > 0. --ab (the per-present A/B, odd presents the guest body): a query-bearing
present's arm is its form - rec only = the port, pm4 only = the guest body - and each arm needs --min-arm of them.
Every compared present must carry a stamp (stamps_logged: `zpd=-`, or an exe before the stamp, FAILs), and the
query-bearing compared presents must reach --min-presents (the A/B's own minimum, 1,000). The hold edges' zpd census
(`[NATIVE_ZPD] freeze #N begin|end`, kernel/highfps freezeEdgeCensus) is reported per hold as information only (it
brackets the whole hold, not the compared presents).

Exit 0 PASS, 1 FAIL, 2 unusable input (no run.log / no census / no window / no freeze); the JSON is written in every
case but a missing --json directory.
"""
import argparse
import json
import os
import re
import sys

SCHEMA = "zpd-activity/1"
CP = re.compile(r"\[GPU_CP\] census rptr=.* swaps=(\d+) ")
ZC = re.compile(r"\[NATIVE_ZPD\] census mode=(\w+) issued=(\d+) .* segs=(\d+) .* port=(\d+) portDecl=(\d+)\s*$")
EDGE = re.compile(r"\[NATIVE_ZPD\] freeze #(\d+) (begin|end) mode=(\w+) issued=(\d+) .* segs=(\d+) .* port=(\d+) "
                  r"portDecl=(\d+)\s*$")
DUMP = re.compile(r"\[SIM_FREEZE_DUMP\] freeze=(\d+) k=(\d+) present=(\d+)")
# the frame stamp at the dump line's end (kernel/highfps freezeDumpWanted): ` zpd=B rec=R pm4=P fswap=F`, or ` zpd=-`
STAMP = re.compile(r" s zpd=(?:(-)|(\d+) rec=(\d+) pm4=(\d+) fswap=(\d+))\s*$")
COMPARED = re.compile(r"^compared \| (\d+) \| (\d+) \| (\d+) \| (static|unsettled|plain)\s*$")
CENSUS_EVERY = 120  # kernel/gpu_command_processor.cpp: a [GPU_CP] census every 120 presents


def run_log(roll):
    p = os.path.join(roll, "drive", "attempt1", "run.log")
    return p if os.path.exists(p) else None


def scan(log, stamps=None):
    """(samples, edges, dumps): samples [(swaps, issued, segs, port, portDecl)] - each [GPU_CP] census paired with the
    [NATIVE_ZPD] census line after it; edges {freeze: {begin|end: (issued, segs, port, portDecl)}}; dumps {freeze:
    [present, ...]}. stamps (a dict, filled when given): {present: (zpd, rec, pm4, fswap)} for a dump line with a
    frame stamp, {present: None} for one with `zpd=-` or none at all (an exe before the stamp)."""
    samples, edges, dumps, pend = [], {}, {}, None
    with open(log, "rb") as f:
        for raw in f:
            ln = raw.decode("utf-8", "replace").rstrip("\r\n")
            m = CP.search(ln)
            if m:
                pend = int(m.group(1))
                continue
            z = ZC.search(ln)
            if z and pend is not None:
                samples.append((pend,) + tuple(int(z.group(i)) for i in (2, 3, 4, 5)))
                pend = None
                continue
            e = EDGE.search(ln)
            if e:
                edges.setdefault(int(e.group(1)), {})[e.group(2)] = tuple(int(e.group(i)) for i in (4, 5, 6, 7))
                continue
            d = DUMP.search(ln)
            if d:
                dumps.setdefault(int(d.group(1)), []).append(int(d.group(3)))
                if stamps is not None:
                    s = STAMP.search(ln)
                    stamps[int(d.group(3))] = (tuple(int(s.group(i)) for i in (2, 3, 4, 5))
                                               if s and not s.group(1) else None)
    return samples, edges, dumps


def pacing(a):
    out = {"schema": SCHEMA, "mode": "pacing_window", "roll": a.roll, "trace_summary": a.trace_summary}
    log = run_log(a.roll)
    try:
        summ = json.load(open(a.trace_summary, encoding="utf-8"))
        lo, hi = [int(x) for x in summ["pacing"]["window_ids"]]
    except (OSError, ValueError, KeyError, TypeError) as e:
        out.update(verdict="UNUSABLE", why="no graded window in the trace summary: %s" % e)
        return out, 2
    out["window"] = [lo, hi]
    if not log:
        out.update(verdict="UNUSABLE", why="no run.log in %s" % a.roll)
        return out, 2
    samples, _edges, _dumps = scan(log)
    inside = [s for s in samples if lo <= s[0] <= hi]
    out["samples"], out["samples_in_window"] = len(samples), len(inside)
    if len(inside) < 2:
        out.update(verdict="UNUSABLE", why="fewer than two census samples inside the window")
        return out, 2
    first, last = inside[0], inside[-1]
    out["first_sample"], out["last_sample"] = first[0], last[0]
    out["issued"], out["segs"], out["port"] = last[1] - first[1], last[2] - first[2], last[3] - first[3]
    span = (hi - lo) / 4.0
    quarters = []
    for q in range(4):
        qlo, qhi = lo + q * span, lo + (q + 1) * span
        qs = [s for s in inside if qlo <= s[0] <= qhi]
        quarters.append({"lo": int(qlo), "hi": int(qhi), "samples": len(qs),
                         "issued": (qs[-1][1] - qs[0][1]) if len(qs) >= 2 else None})
    out["quarters"] = quarters
    idle, best, start = 0, 0, None
    for x, y in zip(inside, inside[1:]):
        if y[1] == x[1]:
            idle += y[0] - x[0]
            best = max(best, idle)
        else:
            idle = 0
    out["longest_idle_presents"] = best
    out["active_intervals"] = sum(1 for x, y in zip(inside, inside[1:]) if y[1] > x[1])
    out["intervals"] = len(inside) - 1
    checks = {
        "samples_cover_window": first[0] - lo <= CENSUS_EVERY and hi - last[0] <= CENSUS_EVERY,
        "issued_in_window": out["issued"] > 0,
        "segs_in_window": out["segs"] > 0,
        "port_in_window": out["port"] > 0,
        "every_quarter_active": all((q["issued"] or 0) > 0 for q in quarters),
    }
    out["checks"] = checks
    ok = all(checks.values())
    out["verdict"] = "PASS" if ok else "FAIL"
    return out, 0 if ok else 1


def arm_of(stamp):
    """A query-bearing present's A/B arm by its frame's form: 'port' (kRkZpd records only), 'guest' (literal packets
    only), else None (no query, or both forms in one frame - no single arm)."""
    if not stamp or stamp[0] <= 0:
        return None
    if stamp[1] > 0 and stamp[2] == 0:
        return "port"
    if stamp[2] > 0 and stamp[1] == 0:
        return "guest"
    return None


def freeze(a):
    out = {"schema": SCHEMA, "mode": "freeze_window", "roll": a.roll, "ab": a.ab, "min_presents": a.min_presents,
           "min_arm": a.min_arm if a.ab else None}
    log = run_log(a.roll)
    if not log:
        out.update(verdict="UNUSABLE", why="no run.log in %s" % a.roll)
        return out, 2
    stamps = {}
    _samples, edges, dumps = scan(log, stamps)
    if not dumps:
        out.update(verdict="UNUSABLE", why="no [SIM_FREEZE_DUMP] line: no compared frozen present")
        return out, 2
    table_path = a.freeze_table or (a.roll.rstrip("/\\") + "_freeze.txt")
    out["freeze_table"] = table_path
    try:
        with open(table_path, encoding="utf-8", errors="replace") as f:
            table = f.read()
    except OSError as e:
        out.update(verdict="UNUSABLE", why="no freeze table (the pixel grade's): %s" % e)
        return out, 2
    compared = {}
    for ln in table.splitlines():
        m = COMPARED.match(ln.strip())
        if m:
            compared.setdefault(int(m.group(1)), []).append((int(m.group(2)), int(m.group(3)), m.group(4)))
    # the holds the pixel grade counted: flicker_gate's own rule (in game, not a movie, settled), on the same table
    import flicker_gate as fg
    g = fg.grade_freeze_table(a.roll, log, table, quiet=True)
    counted = {r["freeze"] for r in g.get("freezes", []) if "differing_pairs" in r and "movie_partial" not in r}
    want = "static" if a.ab else "plain"
    rows, cmp_all, missing, qb, arms = [], set(), set(), set(), {"port": 0, "guest": 0}
    for n in sorted(set(dumps) | set(compared)):
        e = edges.get(n, {})
        row = {"freeze": n, "dumped": len(dumps.get(n, [])), "counted": n in counted}
        if "begin" in e and "end" in e:  # information: the hold's edges bracket all of it, not the compared presents
            row["hold_edges"] = {"issued": e["end"][0] - e["begin"][0], "port": e["end"][2] - e["begin"][2],
                                 "portDecl": e["end"][3] - e["begin"][3]}
        pairs = [(pa, pb) for pa, pb, kind in compared.get(n, []) if kind == want] if n in counted else []
        pres = sorted({p for pair in pairs for p in pair})
        miss = [p for p in pres if stamps.get(p) is None]
        good = []
        for p in pres:
            st = stamps.get(p)
            if not st or st[0] <= 0:
                continue
            if a.ab:
                arm = arm_of(st)
                if arm is None:
                    continue
                arms[arm] += 1
            good.append(p)
        cmp_all.update(pres)
        missing.update(miss)
        qb.update(good)
        row.update(compared_pairs=len(pairs), compared_presents=len(pres), unstamped=len(miss),
                   query_bearing_presents=len(good))
        rows.append(row)
    out["freezes"] = rows
    out["holds"], out["counted_holds"] = len(rows), sum(1 for r in rows if r["counted"])
    out["compared_presents"], out["unstamped_presents"] = len(cmp_all), len(missing)
    out["query_bearing_presents"] = len(qb)
    out["query_bearing_holds"] = sum(1 for r in rows if r["query_bearing_presents"])
    if a.ab:
        out["arms"] = arms
    checks = {
        "compared_listed": bool(cmp_all),
        "stamps_logged": bool(cmp_all) and not missing,
        "query_bearing_presents": len(qb) >= a.min_presents,
    }
    if a.ab:
        checks["both_arms"] = arms["port"] >= a.min_arm and arms["guest"] >= a.min_arm
    out["checks"] = checks
    ok = all(checks.values())
    out["verdict"] = "PASS" if ok else "FAIL"
    if not checks["compared_listed"]:
        out["why"] = ("the freeze table lists no compared pair in a counted hold (freeze_diff.py before Astra round 2's "
                      "compared rows, or no hold the grade counted)")
    elif not checks["stamps_logged"]:
        out["why"] = ("%d of %d compared presents without a frame stamp (zpd=- or an exe before the stamp): their "
                      "queries cannot be read" % (len(missing), len(cmp_all)))
    elif not checks["query_bearing_presents"]:
        out["why"] = "query-bearing compared presents %d < %d" % (len(qb), a.min_presents)
    elif not ok:
        out["why"] = "an A/B arm below %d query-bearing compared presents (port %d, guest %d)" % (
            a.min_arm, arms["port"], arms["guest"])
    return out, 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("pacing")
    p.add_argument("--roll", required=True)
    p.add_argument("--trace-summary", required=True)
    p.add_argument("--json", required=True)
    f = sub.add_parser("freeze")
    f.add_argument("--roll", required=True)
    f.add_argument("--ab", action="store_true",
                   help="the per-present A/B: a query-bearing present's arm by its frame's form; each arm >= --min-arm")
    f.add_argument("--min-presents", type=int, default=1000)
    f.add_argument("--min-arm", type=int, default=250, help="--ab: query-bearing compared presents each arm needs")
    f.add_argument("--freeze-table", default="", help="the pixel grade's freeze_diff table (default <roll>_freeze.txt)")
    f.add_argument("--json", required=True)
    a = ap.parse_args(argv)
    out, rc = pacing(a) if a.mode == "pacing" else freeze(a)
    with open(a.json, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    keys = ("verdict", "why", "window", "issued", "segs", "port", "longest_idle_presents", "holds", "counted_holds",
            "compared_presents", "unstamped_presents", "query_bearing_holds", "query_bearing_presents", "arms")
    print("%s: %s" % (a.mode, ", ".join("%s=%s" % (k, out[k]) for k in keys if k in out)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
