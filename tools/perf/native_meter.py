#!/usr/bin/env python3
"""native_meter.py <roll dir | run.log> [...] - the NATIVE METER over the gameplay window.

Sums the [NATIVE_METER] 2 s window lines (kernel/native_meter.hpp) that fall in the ingame era
(report.json, the same tail window tools/perf/ab_tick.py uses for the tick) and prints, per counter,
the per-frame average and the worst single frame, plus the last whole-run [NATIVE_METER_RUN] line.
The goal is all zeros; "-" = no feeder wired for that counter yet (never read as 0).
A bare run.log (no report.json) uses every window line.  (native lane, iter-467)

Columns (COLS) mirror kernel/native_meter.cpp kName: pm4, the three EDRAM counters, nrec (native work,
shown but never part of the verdict), dispreg (display-register answers: RELEASE_PLAN C1's fourth class),
shm (the shared-memory page model's page validations: ledger mem.resources, docs/RELEASE_COMPOSITION.md section 7)
and xma. A column the kernel prints that is missing here would be silently left out of the verdict, so
tools/perf/test_native_meter.py checks the two lists agree.
"""
import json
import os
import re
import sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
WIN = re.compile(r"\[NATIVE_METER\] w=(\d+) ([\d.]+)s frames=(\d+) \| per_frame avg/max(.*?) \| window(.*)$")
PAIR = re.compile(r" (\w+)=([\d.]+)/(\d+)| (\w+)=-")
VAL = re.compile(r" (\w+)=(\d+|-)")
COLS = ["pm4", "edram_res", "edram_xfer", "edram_load", "nrec", "dispreg", "shm", "xma"]
NATIVE_WORK = {"nrec"}  # native work counters: shown, never part of the verdict
KINDS = re.compile(r"\[NATIVE_METER_KINDS\] w=(\d+) frames=(\d+) \| pm4 per_frame by kind:(.*)$")
KV = re.compile(r" (\w+)=([\d.]+)")


def load(arg):
    """A roll dir (report.json beside drive/) -> (log path, the ingame era dict); a bare log -> (log, None)."""
    if os.path.isdir(arg):
        rep = json.load(open(os.path.join(arg, "report.json")))
        ing = None
        for e in rep["eras"]:
            if e.get("name") == "ingame" or e.get("era") == "ingame":
                ing = e
        if ing is None:
            ing = rep["eras"][-2]
        return rep["log"], ing
    return arg, None


def parse_log(log):
    """Every window line of a run.log -> (rows, run_line, kind_rows, kind_run_line).
    rows: [(ts, frames, {col: max or None}, {col: total or None})]; kind_rows: [(ts, frames, {kind: per_frame})]."""
    rows, run, krows, krun = [], None, [], None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[NATIVE_METER_RUN]" in line:
                run = line.rstrip()
                continue
            if "[NATIVE_METER_KINDS] run" in line:
                krun = line.rstrip()
                continue
            km = KINDS.search(line)
            if km:
                t = TS.match(line)
                kts = datetime.strptime(t.group(1), "%Y-%m-%d %H:%M:%S.%f") if t else None
                krows.append((kts, int(km.group(2)), {k: float(v) for k, v in KV.findall(km.group(3))}))
                continue
            m = WIN.search(line)
            if not m:
                continue
            t = TS.match(line)
            ts = datetime.strptime(t.group(1), "%Y-%m-%d %H:%M:%S.%f") if t else None
            frames = int(m.group(3))
            mx, tot = {}, {}
            for p in PAIR.finditer(m.group(4)):
                if p.group(1):
                    mx[p.group(1)] = int(p.group(3))
                else:
                    mx[p.group(4)] = None
            for v in VAL.finditer(m.group(5)):
                tot[v.group(1)] = None if v.group(2) == "-" else int(v.group(2))
            rows.append((ts, frames, mx, tot))
    return rows, run, krows, krun


def tail_window(rows, ing, ts_index=0):
    """The rows inside the ingame era: the trailing `dur_s - 2` seconds of the run (ab_tick's rule)."""
    if ing is None or not rows or rows[-1][ts_index] is None:
        return rows
    end = rows[-1][ts_index]
    return [r for r in rows if r[ts_index] is not None and (end - r[ts_index]).total_seconds() <= ing["dur_s"] - 2]


def summarize(rows, ing=None, krows=()):
    """The meter over `rows` (parse_log's) -> dict: window (label), frames, per {col: (per_frame, max) or None
    when unwired / no rows}, nonzero (emulation columns > 0), unwired, verdict, kinds {kind: per_frame}."""
    used = tail_window(rows, ing)
    win = "all window lines"
    if ing is not None and rows and rows[-1][0] is not None:
        win = f"ingame era dur={ing['dur_s']}s fps={ing.get('fps', '?')}"
        if used:
            win += f" lines {used[0][0].strftime('%H:%M:%S')}-{used[-1][0].strftime('%H:%M:%S')}"
    frames = sum(r[1] for r in used)
    per, nonzero, unwired = {}, [], []
    for c in COLS:
        tots = [r[3].get(c) for r in used]
        if not used or any(v is None for v in tots):
            per[c] = None
            unwired.append(c)
            continue
        s = sum(tots)
        m = max((r[2].get(c) or 0) for r in used)
        per[c] = (s / frames if frames else 0.0, m)
        if s and c not in NATIVE_WORK:
            nonzero.append(c)
    if not used:
        verdict = "NO [NATIVE_METER] WINDOW LINES (meter off, or the log profile dropped them)"
    else:
        verdict = "NATIVE (all wired counters 0)" if not nonzero else "NOT NATIVE: " + ", ".join(nonzero)
        if unwired:
            verdict += "  | unwired: " + ", ".join(unwired)
    kinds = {}
    kused = tail_window(list(krows), ing)
    kf = sum(r[1] for r in kused)
    if kf:
        names = list(kused[0][2].keys())
        kinds = {n: sum(r[2].get(n, 0.0) * r[1] for r in kused) / kf for n in names}
    return {"window": win, "lines": len(used), "frames": frames, "per": per, "nonzero": nonzero,
            "unwired": unwired, "verdict": verdict, "kinds": kinds}


def report_lines(arg):
    """The printed report of one roll dir / log, as a list of lines."""
    log, ing = load(arg)
    rows, run, krows, krun = parse_log(log)
    s = summarize(rows, ing, krows)
    out = [f"{os.path.basename(arg.rstrip('/')):16s} window={s['window']} lines={s['lines']} frames={s['frames']}"]
    parts = []
    for c in COLS:
        v = s["per"][c]
        parts.append(f"{c}=-" if v is None else f"{c}={v[0]:.1f}/frame (max {v[1]})")
    out.append("   per_frame: " + "  ".join(parts))
    out.append("   verdict: " + s["verdict"])
    if s["kinds"]:
        out.append("   pm4 by kind (per frame): " + " ".join(f"{n}={v:.1f}" for n, v in s["kinds"].items()))
    if run:
        out.append("   run: " + run[run.find("[NATIVE_METER_RUN]"):][:400])
    if krun:
        out.append("   run kinds: " + krun[krun.find("[NATIVE_METER_KINDS]"):][:400])
    return out


def main(argv):
    for a in argv:
        print("\n".join(report_lines(a)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
