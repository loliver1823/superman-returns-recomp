#!/usr/bin/env python3
"""hitch_report.py - the stutter census of one perf roll (iter-466 stutter lane, PERFORMANCE.md 7).

  python tools/perf/hitch_report.py <perf_run --out dir> [--log <game log>] [--era ingame]
         [--ratio 1.5] [--json out.json] [--top 8]

Inputs (all written by the game under SR_PERF_STATS=1 / SR_HITCH_LOG=1 / SR_HOST_PROFILE_HZ):
  <dir>/perf/frames.csv          S,<n>,<t_us> per guest VdSwap, P,<n>,<t_us> per host Present
  <dir>/perf/meta.txt            aligns log timestamps with t_us (eras: perf_report.DEFAULT_ERAS)
  the game log                   [HITCH] lines (one per hitch: the interval, the rolling median,
                                 guest swaps inside, every subsystem slot that moved) and
                                 [HITCH_SUM] lines (the same slots over all frames, every 10 s)
  <dir>/perf/hitch_samples.txt   host-profiler stack samples inside each hitch window (optional)

Census (frames.csv, per era): host-present and guest-swap intervals p50/p95/p99/max, hitches =
intervals > ratio x the rolling median of the previous 120 (as the in-game recorder), > 2x and
> 4x, their excess ms, rate per minute and spacing. Attribution ([HITCH] lines in the era): each
hitch gets the slot that explains most of its excess; the table ranks causes by total excess.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import perf_report  # noqa: E402

HITCH = re.compile(r"\[HITCH\] #(\d+) t_us=(\d+) ms=([\d.]+) med=([\d.]+) x[\d.]+ \| guest swaps=(\d+) "
                   r"gmax=([\d.]+) since_last_swap=([\d.]+) \|(.*)$")
SLOT = re.compile(r"(\w+)=(\d+)/([\d.]+)ms/max([\d.]+)(?:/(\d+)KB)?")
SUM = re.compile(r"\[HITCH_SUM\] ([\d.]+)s presents=(\d+) hitches=(\d+) \(>2x (\d+), >4x (\d+)\) "
                 r"excess_ms=([\d.]+) worst=([\d.]+) \|(.*)$")


def rolling_hitches(ts, ratio, win=120, warm=30):
    """[(t_us, ms, med)] for intervals > ratio x the median of the previous `win` intervals."""
    out, ring = [], collections.deque(maxlen=win)
    for a, b in zip(ts, ts[1:]):
        ms = (b - a) / 1000.0
        if len(ring) >= warm:
            med = statistics.median(ring)
            if ms > ratio * med:
                out.append((b, ms, med))
        ring.append(ms)
    return out


def pct(v, q):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def census(ts, t0, t1, ratio):
    seg = [t for t in ts if t0 <= t < t1]
    iv = [(b - a) / 1000.0 for a, b in zip(seg, seg[1:])]
    if len(iv) < 40:
        return None
    h = rolling_hitches(seg, ratio)
    gaps = [(b[0] - a[0]) / 1e6 for a, b in zip(h, h[1:])]
    clustered = sum(1 for i in range(len(h)) if (i and (h[i][0] - h[i - 1][0]) < 1e6) or
                    (i + 1 < len(h) and (h[i + 1][0] - h[i][0]) < 1e6))
    mins = (seg[-1] - seg[0]) / 6e7
    return {
        "frames": len(iv), "p50": pct(iv, .5), "p95": pct(iv, .95), "p99": pct(iv, .99), "max": max(iv),
        "hitches": len(h), "gt2x": sum(1 for _, ms, med in h if ms > 2 * med),
        "gt4x": sum(1 for _, ms, med in h if ms > 4 * med),
        "excess_ms": sum(ms - med for _, ms, med in h), "per_min": len(h) / mins if mins else 0,
        "gap_med_s": statistics.median(gaps) if gaps else 0, "clustered": clustered,
        "hist": collections.Counter(min(8, int(ms // 16.67)) for _, ms, _ in h),
    }


# Which slot explains a hitch. Symptoms first: rtidle = the render thread waited for the CP
# (the frame came late from upstream: guest or CP), present = inside Present (vsync / DWM),
# gpuwait = the frame fence (GPU-bound). Causes on the frame path: pso, res, texload, resolve.
def classify(h):
    ex = max(0.1, h["ms"] - h["med"])
    s = h["slots"]
    ms = lambda k: s.get(k, (0, 0.0, 0.0, 0))[1]  # noqa: E731
    mx = lambda k: s.get(k, (0, 0.0, 0.0, 0))[2]  # noqa: E731
    for k, label in (("pso", "PSO creation"), ("res", "resource creation"), ("xlat", "shader xlat (sync)")):
        if k != "xlat" and ms(k) >= 0.3 * ex:
            return label
    if ms("gpuwait") >= 0.4 * ex:
        return "GPU fence wait"
    if ms("cpwait") >= 0.4 * ex:
        return "CP blocked on render thread"
    if mx("rtidle") >= 0.5 * ex:
        if h["gmax"] >= 1.4 * h["med"]:
            return "late guest frame"
        return "late CP frame"
    if mx("present") >= 0.5 * ex:
        return "Present (vsync/DWM)"
    if ms("texload") + ms("resolve") >= 0.3 * ex:
        return "texture load / resolve"
    return "render thread (other)"


def parse_hitches(log):
    out, sums = [], []
    if not log or not os.path.exists(log):
        return out, sums
    with open(log, "rb") as f:
        for raw in f:
            if b"[HITCH" not in raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            m = HITCH.search(line)
            if m:
                slots = {}
                for sm in SLOT.finditer(m.group(8)):
                    slots[sm.group(1)] = (int(sm.group(2)), float(sm.group(3)), float(sm.group(4)),
                                          int(sm.group(5) or 0))
                out.append({"id": int(m.group(1)), "t_us": int(m.group(2)), "ms": float(m.group(3)),
                            "med": float(m.group(4)), "gswaps": int(m.group(5)), "gmax": float(m.group(6)),
                            "since_swap": float(m.group(7)), "slots": slots})
                continue
            m = SUM.search(line)
            if m:
                slots = {sm.group(1): (int(sm.group(2)), float(sm.group(3)), float(sm.group(4)),
                                       int(sm.group(5) or 0)) for sm in SLOT.finditer(m.group(8))}
                sums.append({"s": float(m.group(1)), "presents": int(m.group(2)), "hitches": int(m.group(3)),
                             "slots": slots})
    return out, sums


def samples(perf):
    """{hitch id: [(tid, kind, [keys])]}, {tid: name}, modules (from any hprof file)."""
    p = os.path.join(perf, "hitch_samples.txt")
    by, names = collections.defaultdict(list), {}
    if os.path.exists(p):
        for line in open(p, encoding="utf-8", errors="replace"):
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "thread":
                names[int(parts[1])] = parts[2] if len(parts) > 2 else "-"
            elif parts[0] == "h" and len(parts) >= 5:
                keys = []
                for k in parts[5:]:
                    m, _, r = k.partition(":")
                    keys.append((int(m), int(r, 16) & 0x7FFFFFFF))
                by[int(parts[1])].append((int(parts[3]), parts[4], keys))
    mods = {}
    for hp in sorted(glob.glob(os.path.join(perf, "hprof_*.txt"))):
        for line in open(hp, encoding="utf-8", errors="replace"):
            if line.startswith("mod "):
                x = line.rstrip("\n").split(" ")
                mods[int(x[1])] = (" ".join(x[4:]), int(x[2], 16), int(x[3], 16))
            elif line.startswith("thread") or line.startswith("self"):
                break
        if mods:
            break
    return by, names, mods


SKIP = re.compile(r"^(ntdll|KERNELBASE|kernel32|win32u)!|^\?|RtlUserThreadStart|BaseThreadInitThunk")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--log")
    ap.add_argument("--era", default="")
    ap.add_argument("--ratio", type=float, default=1.5)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--json")
    a = ap.parse_args()
    perf = os.path.join(a.dir, "perf") if os.path.isdir(os.path.join(a.dir, "perf")) else a.dir
    log = a.log
    if not log:
        logs = glob.glob(os.path.join(a.dir, "*.log")) + glob.glob(os.path.join(a.dir, "drive", "attempt*", "run.log"))
        log = max(logs, key=os.path.getsize) if logs else None
    meta = perf_report.read_meta(perf)
    swaps, presents = perf_report.read_frames(perf)
    marks = perf_report.log_markers(log, perf_report.DEFAULT_ERAS, float(meta.get("epoch_ms_t0", 0)),
                                    swaps[0] if swaps else None)
    end = max(presents[-1] if presents else 0, swaps[-1] if swaps else 0) + 1
    eras = [(e, t, marks[i + 1][1] if i + 1 < len(marks) else end) for i, (e, t) in enumerate(marks)]
    hitches, sums = parse_hitches(log)
    report = {"eras": {}, "causes": {}}
    print("== census (frames.csv; hitch = interval > %.1fx rolling median of 120) ==" % a.ratio)
    print("%-9s %-5s %6s %6s %6s %6s %7s | %5s %4s %4s %8s %6s %6s %5s" % (
        "era", "clock", "frames", "p50", "p95", "p99", "max", "hitch", ">2x", ">4x", "excess", "/min",
        "gap_s", "clust"))
    for era, t0, t1 in eras:
        for clock, ts in (("host", presents), ("guest", swaps)):
            c = census(ts, t0, t1, a.ratio)
            if not c:
                continue
            report["eras"].setdefault(era, {})[clock] = {k: v for k, v in c.items() if k != "hist"}
            print("%-9s %-5s %6d %6.2f %6.2f %6.2f %7.1f | %5d %4d %4d %8.0f %6.1f %6.2f %5d" % (
                era, clock, c["frames"], c["p50"], c["p95"], c["p99"], c["max"], c["hitches"], c["gt2x"],
                c["gt4x"], c["excess_ms"], c["per_min"], c["gap_med_s"], c["clustered"]))
    if not hitches:
        print("(no [HITCH] lines: run with SR_HITCH_LOG=1 for attribution)")
    else:
        sel = hitches
        if a.era:
            rng = [(t0, t1) for e, t0, t1 in eras if e == a.era]
            if rng:
                sel = [h for h in hitches if rng[0][0] <= h["t_us"] < rng[0][1]]
        causes = collections.defaultdict(lambda: {"n": 0, "excess": 0.0, "worst": 0.0, "ids": []})
        for h in sel:
            c = causes[classify(h)]
            c["n"] += 1
            c["excess"] += h["ms"] - h["med"]
            c["worst"] = max(c["worst"], h["ms"])
            c["ids"].append(h["id"])
        print("\n== attribution ([HITCH] lines%s: %d hitches) ==" % (" in era " + a.era if a.era else "", len(sel)))
        print("%-30s %5s %9s %8s" % ("cause", "count", "excess_ms", "worst_ms"))
        for k, c in sorted(causes.items(), key=lambda kv: -kv[1]["excess"]):
            print("%-30s %5d %9.1f %8.1f" % (k, c["n"], c["excess"], c["worst"]))
            report["causes"][k] = {"n": c["n"], "excess_ms": round(c["excess"], 1), "worst_ms": c["worst"]}
        # Slot totals inside hitches vs over all frames ([HITCH_SUM]).
        tot = collections.defaultdict(lambda: [0, 0.0, 0.0])
        for h in sel:
            for k, (n, ms, mx, kb) in h["slots"].items():
                tot[k][0] += n
                tot[k][1] += ms
                tot[k][2] = max(tot[k][2], mx)
        allt = collections.defaultdict(lambda: [0, 0.0, 0.0])
        for s in sums:
            for k, (n, ms, mx, kb) in s["slots"].items():
                allt[k][0] += n
                allt[k][1] += ms
                allt[k][2] = max(allt[k][2], mx)
        print("\n%-9s %18s %18s" % ("slot", "in hitches n/ms/max", "whole run n/ms/max"))
        for k in sorted(set(tot) | set(allt)):
            print("%-9s %7d/%7.1f/%5.1f %7d/%8.1f/%5.1f" % (k, tot[k][0], tot[k][1], tot[k][2], allt[k][0],
                                                          allt[k][1], allt[k][2]))
        by, names, mods = samples(perf)
        if by and mods:
            from symbolize import Symbolizer, short
            sy = Symbolizer(mods)
            keys = {k for v in by.values() for _, _, ks in v for k in ks}
            sy.resolve(keys)
            ids = {h["id"] for h in sel}
            for cause, c in sorted(causes.items(), key=lambda kv: -kv[1]["excess"]):
                cid = set(c["ids"]) & ids
                per = collections.defaultdict(collections.Counter)
                for hid in cid:
                    for tid, kind, ks in by.get(hid, []):
                        frames = [short(sy.sym(*k)) for k in ks]
                        top = next((f for f in frames if not SKIP.match(f)), frames[0] if frames else "?")
                        leaf = frames[0] if frames else "?"
                        per[names.get(tid, str(tid))][(kind, leaf if kind == "R" else top)] += 1
                print("\n-- %s (%d hitches): top samples per thread (R = running leaf, W = waiting in) --"
                      % (cause, len(cid)))
                ranked = sorted(per.items(), key=lambda kv: -sum(kv[1].values()))
                for th, cnt in ranked[:6]:
                    tot_n = sum(cnt.values())
                    items = ", ".join("%s %s %d" % (k[0], k[1][:60], n) for k, n in cnt.most_common(a.top))
                    print("  %-22s %4d: %s" % (th[:22], tot_n, items))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
