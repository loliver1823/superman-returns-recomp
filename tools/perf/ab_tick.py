#!/usr/bin/env python3
"""ab_tick.py <roll dir> [...] - ingame fps (report.json), median guest render tick
([HIGHFPS_PACE] tick_cost_ms render over the ingame window), ICALL counts, last [NATIVE_UPLOAD] /
[NATIVE_LEAF] / [NATIVE_REC] lines.  (native lane, iter-467 N1 phase 1)"""
import json, os, re, statistics, sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
TICK = re.compile(r"\[HIGHFPS_PACE\].*tick_cost_ms render=([\d.]+)")

for d in sys.argv[1:]:
    rep = json.load(open(os.path.join(d, "report.json")))
    era = {e.get("name", ""): e for e in rep["eras"]} if isinstance(rep["eras"], list) else rep["eras"]
    ing = None
    for e in rep["eras"]:
        if e.get("name") == "ingame" or e.get("era") == "ingame":
            ing = e
    if ing is None:  # eras without names: ingame is the one before ALL
        ing = rep["eras"][-2]
    log = rep["log"]
    ticks = []
    last = {}
    icall = 0
    rows = []
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[ICALL_FAIL]" in line or "[ICALL_UNRESOLVED]" in line:
                icall += 1
            for tag in ("[NATIVE_UPLOAD]", "[NATIVE_LEAF] setShaderConstantF", "[NATIVE_REC]"):
                if tag in line:
                    last.setdefault(tag, [])
                    last[tag] = (last[tag] + [line.rstrip()])[-3:]
            m = TICK.search(line)
            if m:
                t = TS.match(line)
                if t:
                    rows.append((datetime.strptime(t.group(1), "%Y-%m-%d %H:%M:%S.%f"), float(m.group(1))))
    if rows:
        end = rows[-1][0]
        ticks = [v for (t, v) in rows if (end - t).total_seconds() <= ing["dur_s"] - 2]
    med = statistics.median(ticks) if ticks else float("nan")
    win = ""
    if rows and ticks:
        used = [t for (t, v) in rows if (end - t).total_seconds() <= ing["dur_s"] - 2]
        win = f" window=ingame era t0={ing.get('t0_s', '?')}s dur={ing['dur_s']}s ticks {used[0].strftime('%H:%M:%S')}-{used[-1].strftime('%H:%M:%S')}"
    print(f"{os.path.basename(d):14s} ingame fps={ing['fps']:6.1f} p50={ing['p50_ms']:5.1f}ms "
          f"tick={med:5.2f}ms (n={len(ticks)}) icall={icall}{win}")
    for tag, ls in last.items():
        for l in ls[-1:]:
            print("   ", l[l.find(tag):][:400])
