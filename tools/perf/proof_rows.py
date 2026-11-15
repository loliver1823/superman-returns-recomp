#!/usr/bin/env python3
"""proof_rows.py - which native_proof RUN took a set of rolls, read from that run's own proof JSON (native/devcreate-close,
Astra round 1 on 2026-10-14, P1-2). Used by tools/perf/boot_equiv.py, boot_stall.py and boot_attach_eval.py.

A native_proof invocation writes <out>/<prefix>_proof.json (rewritten from scratch by every invocation on that --out):
its `out` (absolute), `exe_id` (the --exe's sha256 prefix), `started` (local ISO time) and one row per step - a roll's
row names the roll by its UMBRELLA-RELATIVE path (Runner.rel) and records the step's verdict (DONE / ABORT / FAIL /
SKIP; a roll the run skipped has a row without a path). find_run() accepts a set of rolls as one run's only when, for
ONE proof JSON beside them:
  - every roll sits directly in that JSON's `out` folder (canonical paths, case-folded);
  - every roll that has a folder is a `roll` row whose umbrella-relative path resolves to the roll's own canonical
    path - never matched by basename (a foreign run's rows with the same basenames are not this run's);
  - the run's identity holds for every roll that ran: its provenance exeSha256 starts with the JSON's exe_id, and its
    log's first line is no earlier than the JSON's `started` (less 2 s of clock rounding).
It returns each roll's row; the callers grade the verdicts (a row that is not DONE is a roll that failed, whatever its
outcome.json says).
"""
import glob
import json
import os
import re
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKOUT = os.path.dirname(os.path.dirname(HERE))
UMBRELLA = os.path.dirname(os.path.dirname(CHECKOUT))  # native_proof.py's UMBRELLA: the rows' paths are relative to it
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
START_SLACK = timedelta(seconds=2)


def key(p):
    return os.path.normcase(os.path.abspath(p))


def read_json(p):
    try:
        with open(p, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def log_span(roll):
    """(first, last) timestamps of a roll's run.log (datetime), or (None, None)."""
    first = last = None
    try:
        with open(os.path.join(roll, "drive", "attempt1", "run.log"), "rb") as f:
            for raw in f:
                m = TS.match(raw.decode("utf-8", "replace"))
                if m:
                    t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
                    first = first or t
                    last = t
    except OSError:
        pass
    return first, last


GATE_ROLLS = ("span", "freeze")   # flicker_gate.py's two rolls, in the order it runs them
GATE_JSON = "flicker_gate.json"


def gate_boots(gate_dir, rel_gate, row):
    """The BOOTS a flicker-gate step took, as roll-shaped rows (native/devcreate-fix, Astra round 1: "every ON boot"
    omitted the gate's two): {canonical path: row} for the gate's current `span` and `freeze` rolls - verdict DONE when
    the gate's flicker_gate.json records perf_run exit 0 for that roll, FAIL otherwise (a gate that wrote no JSON, or
    whose row is not PASS / FAIL, failed its boots too) - and for every RETAINED attempt beside them (flicker_gate's
    `span_try1`, a span it re-placed and rolled again; any other folder of the gate holding a drive/attempt1), marked
    `retained`: its verdict from its own outcome.json (SUCCESS -> DONE, else FAIL). Each row carries `gate` (the gate
    step's tag) and `paths.roll`; the callers grade the rest from the roll's log, as for any roll."""
    out = {}
    if not os.path.isdir(gate_dir):
        return out
    fg = read_json(os.path.join(gate_dir, GATE_JSON))
    fg = fg if isinstance(fg, dict) else None
    gate_ok = row.get("verdict") in ("PASS", "FAIL")
    for name in sorted(os.listdir(gate_dir)):
        d = os.path.join(gate_dir, name)
        if not os.path.isdir(os.path.join(d, "drive", "attempt1")):
            continue
        r = {"step": "%s/%s" % (row.get("step"), name), "kind": "roll", "gate": row.get("step"),
             "paths": {"roll": rel_gate.rstrip("/") + "/" + name}, "retained": name not in GATE_ROLLS}
        if name in GATE_ROLLS:
            ec = (fg or {}).get(name + "_exit")
            if fg is None:
                r["verdict"], r["detail"] = "FAIL", "the gate wrote no %s" % GATE_JSON
            elif not gate_ok:
                r["verdict"], r["detail"] = "FAIL", "the gate's row says %s" % row.get("verdict")
            elif ec == 0:
                r["verdict"], r["detail"] = "DONE", "perf_run exit 0 (flicker_gate %s_exit)" % name
            else:
                r["verdict"], r["detail"] = "FAIL", "perf_run exit %s (flicker_gate %s_exit)" % (ec, name)
        else:
            res = (read_json(os.path.join(d, "drive", "attempt1", "outcome.json")) or {}).get("result")
            r["verdict"] = "DONE" if res == "SUCCESS" else "FAIL"
            r["detail"] = "a retained attempt of the gate's %s (outcome %s)" % (name, res or "missing")
        out[key(d)] = r
    return out


def run_rows(js, umbrella=None):
    """(doc, {canonical roll path: row}, problem or '') of one proof JSON: its `roll` rows keyed by the canonical path
    their umbrella-relative path resolves to, plus the boots of every `gate` row (gate_boots: its span / freeze rolls
    and retained attempts, roll-shaped); a JSON whose `out` is not the folder it sits in is refused."""
    umbrella = umbrella or UMBRELLA
    doc = read_json(js)
    if not isinstance(doc, dict):
        return None, {}, "%s unreadable" % js
    if key(doc.get("out") or "") != os.path.dirname(key(js)):
        return doc, {}, "%s: its out %r is not the folder it sits in" % (js, doc.get("out"))
    rows = {}
    for row in doc.get("steps") or []:
        if isinstance(row, dict) and row.get("kind") == "roll":
            rel = str((row.get("paths") or {}).get("roll") or "")
            if rel:
                rows[key(os.path.join(umbrella, rel.replace("/", os.sep)))] = row
        elif isinstance(row, dict) and row.get("kind") == "gate":
            rel = str((row.get("paths") or {}).get("gate") or "")
            if rel:
                rows.update(gate_boots(os.path.join(umbrella, rel.replace("/", os.sep)), rel, row))
    return doc, rows, ""


def identity_problems(doc, rows, rolls):
    """The run's identity for every roll it records as run: its provenance exeSha256 starts with the run's exe_id and
    its log's first line is no earlier than the run's `started`."""
    probs = []
    exe_id = str(doc.get("exe_id") or "").lower()
    try:
        started = datetime.fromisoformat(str(doc.get("started")))
    except ValueError:
        started = None
    for r in rolls:
        row = rows.get(key(r))
        if row is None or row.get("verdict") == "SKIP" or not os.path.isdir(r):
            continue
        prov = read_json(os.path.join(r, "drive", "attempt1", "provenance.json")) or {}
        sha = str(prov.get("exeSha256") or "").lower()
        if not exe_id or not sha.startswith(exe_id):
            probs.append("%s: exe %s is not the run's %s" % (r, sha[:12] or "unknown", exe_id or "unknown"))
        first, _last = log_span(r)
        if started is None or first is None or first < started - START_SLACK:
            probs.append("%s: its log starts %s, before the run started %s" % (r, first, doc.get("started")))
    return probs


def find_run(rolls, umbrella=None):
    """-> {"proof": path or None, "doc": dict or None, "rows": {roll key: row}, "problems": [...]} for the rolls taken
    by ONE native_proof run (the module docstring's rule). problems empty = the run is identified and records every roll.
    The run is the proof JSON beside the rolls whose rows resolve to the most of them (a tie is ambiguous, refused), so
    a run that ended early still yields the rows it has - a roll it never ran has no folder and a SKIP row without a
    path, and the caller names it as not run; a roll FOLDER the run does not record is a problem."""
    rolls = [r for r in rolls if r]
    dirs = {os.path.dirname(key(r)) for r in rolls}
    if len(dirs) != 1:
        return {"proof": None, "doc": None, "rows": {}, "problems": ["the rolls are not in one native_proof --out folder"]}
    tried, best = [], []
    for js in sorted(glob.glob(os.path.join(dirs.pop(), "*_proof.json"))):
        doc, rows, bad = run_rows(js, umbrella)
        if bad:
            tried.append(bad)
            continue
        hit = [r for r in rolls if key(r) in rows]
        if not hit:
            tried.append("%s: no row resolves to any of them" % os.path.basename(js))
            continue
        best.append((len(hit), js, doc, rows))
    best.sort(key=lambda x: -x[0])
    if not best:
        return {"proof": None, "doc": None, "rows": {},
                "problems": ["no native_proof run beside the rolls records them (%s)" % ("; ".join(tried) or
                                                                                    "no *_proof.json")]}
    if len(best) > 1 and best[1][0] == best[0][0]:
        return {"proof": None, "doc": None, "rows": {},
                "problems": ["two runs beside the rolls record as many of them (%s, %s): ambiguous" % (
                    os.path.basename(best[0][1]), os.path.basename(best[1][1]))]}
    _n, js, doc, rows = best[0]
    probs = ["%s: a roll folder the run %s does not record" % (r, os.path.basename(js))
             for r in rolls if key(r) not in rows and os.path.isdir(r)]
    return {"proof": js, "doc": doc, "rows": {key(r): rows[key(r)] for r in rolls if key(r) in rows},
            "problems": probs + identity_problems(doc, rows, rolls)}


def row_verdict(run, roll):
    """The run's verdict for a roll ('DONE', 'ABORT', 'FAIL', 'SKIP', ...) or None when the run does not record it."""
    row = (run.get("rows") or {}).get(key(roll))
    return None if row is None else row.get("verdict")
