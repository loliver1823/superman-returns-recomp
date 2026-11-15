#!/usr/bin/env python3
"""Per-era frame time / fps / per-thread CPU report for one SR_PERF_STATS run.

  python tools/perf/perf_report.py <perf_dir> [--log <game log>] [--eras <spec.json>]
                                   [--json <out.json>] [--top-threads 12]

Inputs (kernel/perf_stats.cpp, docs/PERFORMANCE.md):
  <perf_dir>/frames.csv   S,<n>,<t_us> per guest VdSwap; P,<n>,<t_us> per host Present
  <perf_dir>/threads.csv  cumulative per-thread user/kernel ms, every second
  <perf_dir>/meta.txt     epoch_ms_t0 (aligns log timestamps with t_us)
  --log                   the game log: era boundaries come from milestone lines that are
                          logged in BOTH log profiles (dev and player; app/log.cpp allowlist)

An era spec is a JSON list of {"era": name, "start": regex | "@t0" | "@first_swap",
"after": optional era name}: an era starts at the FIRST log line matching its regex at or
after the start of the era before it, and ends where the next found era starts (the last one
ends at the end of the run). Eras whose marker never appears are reported as absent.

Frame time is the interval between consecutive guest swaps (the game's own frame); fps is
swaps per second of era wall time. p50/p95/p99 are interval percentiles; ">33ms" and ">50ms"
count frames slower than 30 / 20 fps. Presents/s is the host's rate (it may repeat frames).
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime
import json
import os
import re
import sys

# The ingame marker: the END OF MEDIA of a real movie (frames >= 2), see DEFAULT_ERAS. iter-467 hooks: EOM only.
# A `GAP` record is the movie pace logger's 2 s rule (no frame-get for 2 s closes the record), and a stall in the
# opening FMV splits it: the dev gate postmerge_sj's FMV logged `movie#7 GAP wall=12.44s frames=264` (media 8.77 s)
# then `movie#8 EOM wall=22.87s frames=554` (media 21.43 s) - one 30.2 s movie. The GAP matched, the in-game start
# fell 34 s early (on the movie), and the flicker gate graded movie frames as play (postmerge_hooks gate2's span:
# luma 108, "in play 360 of 360", all of it the opening FMV). Over 64 archived gates every opening FMV ends EOM.
INGAME_MOVIE = r"\[MOVIE_PACE\] movie#\d+ EOM wall=[\d.]+s frames=(?:[2-9]|[1-9]\d+)\b"
# iter-467 forcing (owner 2026-10-11: rolls skip the cutscenes, a natural player action): a SKIPPED opening movie
# logs no EOM (a killed movie ends silently; MOVIE_PACE prints a GAP record only when a later movie starts). The
# merge-ready gate (forcing gate/merge_ready, 2026-10-11) showed the new game's opening FMV skipped by pad A WITHOUT
# any `[EV187] ... ACCEPTED-KILL` line (that edge is the title card's at boot), so its in-game start never opened
# ("in play 0 of 360"). The skip's real end is the player getting control: the first `[PC_CONTROLS] context
# gameplay ... mode <a mode>` after the HUD request (the HUD request itself logs `mode -`; an unskipped FMV logs
# its EOM before any mode line: gate final_r2 HUD 17:53:52.6, EOM 17:54:29.9, `mode on-foot camera` 17:54:32.0).
# The EV187 accept stays an alternative (a movie skipped through event 187).
MOVIE_SKIP = (r"\[EV187\] #\d+ slot=\d+ rc=01 ACCEPTED-KILL"
              r"|\[PC_CONTROLS\] context gameplay \(igc type \d+\) config \S+ invX=\d+ invY=\d+ mode [a-z]")
INGAME_START = "(?:%s)|(?:%s)" % (INGAME_MOVIE, MOVIE_SKIP)

DEFAULT_ERAS = [
    {"era": "boot", "start": "@t0"},
    {"era": "intro", "start": "@first_swap"},
    # The title screen: the guest requests its PRESS START screen.
    {"era": "title", "start": r"\[APT_REQ\] #\d+ path='screens/bootflow/ScrPressStart'"},
    # The front end proper: the first FE screen after START (LOAD A GAME? / mode select).
    {"era": "fe_menu", "start": r"\[FE_SETSCREEN\] #1 |\[SR_FSCMD\].*claim LoadCreate"},
    # The custom front end's NEW GAME answer: loading + the opening FMV until the HUD.
    {"era": "loading", "start": r"\[SR_FSCMD\].*answer cppId=\S+ button=0"},
    # Gameplay: the guest requests the in-game HUD package.
    {"era": "gameplay", "start": r"\[APT_REQ\].*path='screens/hud/main_hud'"},
    # In-game proper: the end of the first movie after the HUD request (the opening FMV of a
    # NEW GAME plays right after main_hud is requested). "after" = only searched once that
    # era's marker was seen. A movie has at least 2 frames: a one-frame record (`GAP wall=0.00s
    # frames=1`, a lone frame-get closed by the 2 s gap rule) is not a movie end; it matched 36 s
    # early in the hooks lane's D3 gate, so freezes 5-8 landed on the opening FMV.
    {"era": "ingame", "start": INGAME_START, "after": "gameplay"},
    # Warworld (dev-tool level skip): the plan's arena objective is engaged.
    {"era": "arena", "start": r"\[DEVTOOL\].*WARobj01"},
]

MOVIE_WALL = re.compile(r"\[MOVIE_PACE\] movie#\d+ \S+ wall=([\d.]+)s")
LOG_TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")


def read_meta(d):
    meta = {}
    p = os.path.join(d, "meta.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8", errors="replace"):
            if "=" in line and not line.startswith("env "):
                k, v = line.strip().split("=", 1)
                meta[k] = v
            elif line.startswith("env "):
                meta.setdefault("env", []).append(line[4:].strip())
    return meta


def read_frames(d):
    swaps, presents = [], []
    p = os.path.join(d, "frames.csv")
    with open(p, newline="") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) < 3:
                continue
            (swaps if row[0] == "S" else presents).append(int(row[2]))
    swaps.sort()
    presents.sort()
    return swaps, presents


def read_threads(d):
    """{tid: [(t_us, cpu_ms_total, name)]}"""
    out = {}
    p = os.path.join(d, "threads.csv")
    if not os.path.exists(p):
        return out
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) < 6:
                continue
            t, tid, u, k = int(row[0]), int(row[1]), float(row[2]), float(row[3])
            name = ",".join(row[5:])
            out.setdefault(tid, []).append((t, u + k, name, int(row[4]) if row[4].isdigit() else 0))
    return out


def read_mem(d):
    """[(t_us, private_mb, ws_mb, gpu_local_mb, gpu_nonlocal_mb, other_busy_cores, avail_ram_mb)]
    other_busy_cores = machine-wide busy cores minus this process's (the other lanes' load);
    None when the exe predates those columns or the first sample has no delta yet."""
    out = []
    p = os.path.join(d, "mem.csv")
    if not os.path.exists(p):
        return out
    with open(p, newline="") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) >= 7:
                other = avail = None
                if len(row) >= 10 and float(row[7]) >= 0 and float(row[8]) >= 0:
                    other = max(0.0, float(row[7]) - float(row[8]))
                if len(row) >= 10 and float(row[9]) >= 0:
                    avail = float(row[9])
                out.append((int(row[0]), float(row[1]), float(row[2]), float(row[5]), float(row[6]),
                            other, avail))
    return out


def log_markers(log, eras, t0_epoch_ms, first_swap_us):
    """[(era, t_us)] in run order."""
    found = []
    pats = [(e, re.compile(e["start"])) for e in eras if not e["start"].startswith("@")]
    idx = 0  # eras are ordered: only look for the next ones
    order = [e["era"] for e in eras]
    starts = {}
    for e in eras:
        if e["start"] == "@t0":
            starts[e["era"]] = 0
        elif e["start"] == "@first_swap" and first_swap_us is not None:
            starts[e["era"]] = first_swap_us
    if log and os.path.exists(log):
        pending = [(e["era"], rx, e.get("after")) for e, rx in pats]
        with open(log, "rb") as f:
            for raw in f:
                if not pending:
                    break
                line = raw.decode("utf-8", "replace")
                for i, (era, rx, after) in enumerate(pending):
                    if after and after not in starts:
                        continue
                    if rx.search(line):
                        m = LOG_TS.match(line)
                        if m:
                            ts = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
                            t_us = int((ts.timestamp() * 1000 - t0_epoch_ms) * 1000)
                            # A [MOVIE_PACE] line is logged at the movie's END: for an "after"
                            # era only a movie that STARTED after that era began counts.
                            w = MOVIE_WALL.search(line)
                            if after and w and t_us - float(w.group(1)) * 1e6 < starts[after] - 1e6:
                                continue
                            starts[era] = max(0, t_us)
                        pending.pop(i)
                        break
    for era in order:
        if era in starts:
            found.append((era, starts[era]))
    found.sort(key=lambda x: x[1])
    return found


def pct(v, q):
    if not v:
        return 0.0
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def era_stats(swaps, presents, threads, t0, t1, top, mem=()):
    i0, i1 = bisect.bisect_left(swaps, t0), bisect.bisect_left(swaps, t1)
    s = swaps[i0:i1]
    ints = sorted((b - a) / 1000.0 for a, b in zip(s, s[1:]))
    dur = (t1 - t0) / 1e6
    p0, p1 = bisect.bisect_left(presents, t0), bisect.bisect_left(presents, t1)
    st = {
        "t0_s": round(t0 / 1e6, 1), "t1_s": round(t1 / 1e6, 1), "dur_s": round(dur, 1),
        "swaps": len(s), "fps": round(len(s) / dur, 1) if dur > 0 else 0.0,
        "presents_per_s": round((p1 - p0) / dur, 1) if dur > 0 else 0.0,
        "p50_ms": round(pct(ints, 0.50), 1), "p95_ms": round(pct(ints, 0.95), 1),
        "p99_ms": round(pct(ints, 0.99), 1), "max_ms": round(ints[-1], 1) if ints else 0.0,
        "gt33": sum(1 for x in ints if x > 33.4), "gt50": sum(1 for x in ints if x > 50.0),
    }
    # CPU per thread over the era (cumulative samples: last at/before t1 minus last at/before t0).
    rows = []
    total = 0.0
    for tid, samples in threads.items():
        ts = [x[0] for x in samples]
        # The cumulative sample at/before t0; a thread first seen inside the era starts
        # from its own first sample (its CPU before that is outside the measured span).
        a = max(0, bisect.bisect_right(ts, t0) - 1)
        b = bisect.bisect_right(ts, t1) - 1
        if b < 0:
            continue
        cpu0 = samples[a][1] if a >= 0 else 0.0
        span_s = (samples[b][0] - (samples[a][0] if a >= 0 else samples[0][0])) / 1e6
        if span_s <= 0:
            continue
        d = samples[b][1] - cpu0
        total += d
        csw = (samples[b][3] - (samples[a][3] if a >= 0 else 0)) / span_s
        rows.append((d / (span_s * 1000.0) * 100.0, tid, samples[b][2], csw))
    rows.sort(reverse=True)
    st["cpu_total_cores"] = round(total / (dur * 1000.0), 2) if dur > 0 else 0.0
    ms = [m for m in mem if t0 <= m[0] < t1]
    if ms:
        st["mem_private_mb_max"] = round(max(m[1] for m in ms))
        st["mem_ws_mb_max"] = round(max(m[2] for m in ms))
        st["mem_ws_mb_end"] = round(ms[-1][2])
        st["gpu_local_mb_max"] = round(max(m[3] for m in ms))
        st["gpu_nonlocal_mb_max"] = round(max(m[4] for m in ms))
        oth = [m[5] for m in ms if m[5] is not None]
        av = [m[6] for m in ms if m[6] is not None]
        if oth:
            st["other_busy_cores_avg"] = round(sum(oth) / len(oth), 1)
            st["other_busy_cores_max"] = round(max(oth), 1)
        if av:
            st["avail_ram_mb_min"] = round(min(av))
    st["threads"] = [{"tid": tid, "name": nm, "cpu_pct": round(c, 1), "ctxsw_per_s": round(cs)}
                     for c, tid, nm, cs in rows[:top]]
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("perf_dir")
    ap.add_argument("--log")
    ap.add_argument("--eras")
    ap.add_argument("--json")
    ap.add_argument("--top-threads", type=int, default=10)
    ap.add_argument("--min-era-s", type=float, default=3.0, help="skip eras shorter than this")
    a = ap.parse_args()
    meta = read_meta(a.perf_dir)
    swaps, presents = read_frames(a.perf_dir)
    threads = read_threads(a.perf_dir)
    mem = read_mem(a.perf_dir)
    eras = json.load(open(a.eras)) if a.eras else DEFAULT_ERAS
    t0_epoch = float(meta.get("epoch_ms_t0", "0"))
    marks = log_markers(a.log, eras, t0_epoch, swaps[0] if swaps else None)
    end_us = max([swaps[-1] if swaps else 0, presents[-1] if presents else 0] +
                 [s[-1][0] for s in threads.values() if s])
    result = {"perf_dir": a.perf_dir, "log": a.log, "env": meta.get("env", []), "eras": []}
    for i, (era, t0) in enumerate(marks):
        t1 = marks[i + 1][1] if i + 1 < len(marks) else end_us
        if (t1 - t0) / 1e6 < a.min_era_s:
            continue
        st = era_stats(swaps, presents, threads, t0, t1, a.top_threads, mem)
        st["era"] = era
        result["eras"].append(st)
    # Whole run, steady part (after the first swap).
    if swaps:
        st = era_stats(swaps, presents, threads, swaps[0], end_us, a.top_threads, mem)
        st["era"] = "ALL(after first swap)"
        result["eras"].append(st)
    missing = [e["era"] for e in eras if e["era"] not in [m[0] for m in marks]]
    result["missing_eras"] = missing
    hdr = "%-22s %7s %7s %6s %6s %7s %7s %7s %7s %5s %5s %6s" % (
        "era", "t0_s", "dur_s", "fps", "pres/s", "p50", "p95", "p99", "max", ">33", ">50", "cores")
    print(hdr)
    for st in result["eras"]:
        print("%-22s %7.1f %7.1f %6.1f %6.1f %7.1f %7.1f %7.1f %7.1f %5d %5d %6.2f" % (
            st["era"], st["t0_s"], st["dur_s"], st["fps"], st["presents_per_s"], st["p50_ms"],
            st["p95_ms"], st["p99_ms"], st["max_ms"], st["gt33"], st["gt50"], st["cpu_total_cores"]))
    if missing:
        print("eras not reached (marker absent):", ", ".join(missing))
    if mem:
        print()
        print("%-22s %10s %10s %10s %10s %10s" % ("memory (MB)", "priv max", "ws max", "ws end",
                                                  "vram max", "sysgpu max"))
        for st in result["eras"]:
            if "mem_private_mb_max" in st:
                print("%-22s %10d %10d %10d %10d %10d" % (st["era"], st["mem_private_mb_max"],
                      st["mem_ws_mb_max"], st["mem_ws_mb_end"], st["gpu_local_mb_max"],
                      st["gpu_nonlocal_mb_max"]))
        if any("other_busy_cores_avg" in st for st in result["eras"]):
            print()
            print("%-22s %10s %10s %12s   (machine load outside this process; 32 logical CPUs)" % (
                "contention", "other avg", "other max", "avail RAM min"))
            for st in result["eras"]:
                if "other_busy_cores_avg" in st:
                    print("%-22s %10.1f %10.1f %12s" % (st["era"], st["other_busy_cores_avg"],
                          st["other_busy_cores_max"], st.get("avail_ram_mb_min", "-")))
    for st in result["eras"]:
        print("\n[%s] top threads (%% of one core, context switches/s):" % st["era"])
        for t in st["threads"]:
            print("  %6.1f%%  %8d/s  %6d  %s" % (t["cpu_pct"], t["ctxsw_per_s"], t["tid"], t["name"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
