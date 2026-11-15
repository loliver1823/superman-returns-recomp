#!/usr/bin/env python3
"""mem_res_eras.py - per-era deltas of a cumulative counter line in one game log (the shared helper of lane D's
mem.resources graders: tools/perf/mem_res_texid.py, tools/perf/mem_res_reader_census.py).

The eras are tools/perf/pm4_census_eras.py's ERAS (the one era list of the per-era censuses): boot from the process
start, intro from the first rendered PM4 frame (the first `[PM4_BIND_CENSUS] last frame` line), then title, fe_menu,
mods, loading, gameplay, ingame (after gameplay), arena - each the first match of its marker. An era's numbers are the
difference between the last window at or before its start and the first window at or after its end (a window that
straddles a boundary is charged to BOTH eras, so nothing near a boundary lands in neither, as tools/perf/
mem_res_census.py does); the last era ends at the log's last window. With --label ERA (a chapter-save roll) the eras
before the gameplay HUD keep their names and ERA runs from the HUD (or --entry REGEX, or --entry start) to the end.
A window is any line the caller's regex matches; its integer key=value fields are the counters.
"""
import datetime as dt
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pm4_census_eras import ERAS  # noqa: E402

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
KV = re.compile(r"\b([a-z_][a-z0-9_.]*)=(\d+)\b")
FIRST_FRAME = re.compile(r"\[PM4_BIND_CENSUS\] last frame")
QUIT_FAILED = re.compile(r"\[QUIT\] terminal snapshot FAILED(.*)")


def ts(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def parse(path, window_rx, extra=None, line_hook=None):
    """{t0, end, windows: [{t, why, kv, line}], marks: {era: t}, first_frame, found: {name: t}, quit_failed}.
    window_rx: a compiled regex whose group 1 is the window's `why` (periodic / terminal / ...)."""
    windows, marks, found = [], {}, {}
    first_t = last_t = first_frame = quit_failed = None
    pending = [(e, re.compile(rx), after) for e, rx, after in ERAS if rx and not rx.startswith("@")]
    extra = {k: re.compile(v) for k, v in (extra or {}).items() if v}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = ts(line)
            if t is None:
                continue
            if first_t is None:
                first_t = t
            last_t = t
            if first_frame is None and FIRST_FRAME.search(line):
                first_frame = t
            for k, rx in extra.items():
                if k not in found and rx.search(line):
                    found[k] = t
            m = window_rx.search(line)
            if m:
                windows.append({"t": t, "why": m.group(1), "kv": {k: int(v) for k, v in KV.findall(line[m.end(1):])},
                                "line": line.rstrip()})
            if line_hook:
                line_hook(t, line)
            if "[QUIT]" in line:
                q = QUIT_FAILED.search(line)
                if q:
                    quit_failed = q.group(1).strip()
            for i, (era, rx, after) in enumerate(pending):
                if after and after not in marks:
                    continue
                if rx.search(line):
                    marks[era] = t
                    pending.pop(i)
                    break
    return dict(path=path, t0=first_t, end=last_t, windows=windows, marks=marks, first_frame=first_frame,
                found=found, quit_failed=quit_failed)


def spans(run, label=None, entry=None):
    """[(era, t_start, t_end or None, problem)] in time order."""
    marks = {"boot": run["t0"]}
    if run["first_frame"] is not None:
        marks["intro"] = run["first_frame"]
    for e, t in run["marks"].items():
        if e != "intro":
            marks[e] = t
    order = sorted(marks.items(), key=lambda kv: kv[1])
    problem = None
    if label:
        te = run["t0"] if entry == "start" else run["found"].get("entry") if entry else run["marks"].get("gameplay")
        if te is None:
            problem = "no entry evidence for %s" % label
            te = run["end"]
        order = [(e, t) for e, t in order if t < te] + [(label, te)]
    out = []
    for i, (e, t) in enumerate(order):
        out.append((e, t, order[i + 1][1] if i + 1 < len(order) else None, problem if (label and e == label) else None))
    return out


def era_delta(run, t0, t1):
    """(delta kv, windows inside, terminal reached, problem). Delta = first window at/after t1 (or the last window when
    the era runs to the end) minus the last window at/before t0 (zeros when none: the process start)."""
    W = run["windows"]
    if not W:
        return {}, 0, False, "no window"
    before = [w for w in W if w["t"] <= t0]
    base = before[-1]["kv"] if before else {}
    if t1 is None:
        top = W[-1]
        terminal = top["why"] == "terminal"
    else:
        after = [w for w in W if w["t"] >= t1]
        top = after[0] if after else W[-1]
        terminal = bool(after)
    inside = sum(1 for w in W if (w["t"] > t0) and (t1 is None or w["t"] < t1))
    if top["t"] < t0:
        return {}, inside, terminal, "no window at or after the era's start"
    delta = {}
    for k, v in top["kv"].items():
        d = v - base.get(k, 0)
        if d < 0:
            return {}, inside, terminal, "counter %s went backwards (two processes in one log?)" % k
        delta[k] = d
    return delta, inside, terminal, None
