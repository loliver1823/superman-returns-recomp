#!/usr/bin/env python3
"""Report a HOST profile (SR_HOST_PROFILE_HZ, kernel/host_profile.cpp): top-N functions per
thread, per era.

  python tools/perf/host_profile.py <perf_dir> [--eras-json <perf_report --json out>]
                                    [--top 25] [--threads 8] [--exe <exe with its PDB>]
                                    [--folded <out.txt>] [--era NAME]

Each hprof_<n>.txt covers one interval (t0_us..t1_us on the perf_stats clock). With
--eras-json (tools/perf/perf_report.py --json), intervals are grouped by the era holding
most of their time; without it the whole run is one group.

Samples: R = the thread consumed CPU since the previous tick (a CPU sample); W = an idle
thread's once-a-second wait-site sample. "self" is the innermost frame, "incl" counts each
function once per sample. A CPU sample whose innermost frame is a system call stub
(ntdll!Nt*/Zw*, win32u!Nt*) is a thread entering/leaving the kernel (a wait, a yield, an I/O
call): reported as "syscall" share, and its caller chain names who asked.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symbolize  # noqa: E402

HDR = re.compile(r"# host_profile v1 pid=(\d+) n=(\d+) why=(\S+) hz=(\d+) t0_us=(\d+) t1_us=(\d+)")


def parse_key(s):
    """'<module>:<hex>' -> (module, rva). Bit 31 marks a raw rva (a leaf function without
    unwind data, keyed by address rather than by function start): the symbolizer merges it."""
    m, _, r = s.partition(":")
    return int(m), int(r, 16) & 0x7FFFFFFF


def load(perf_dir):
    files = []
    for p in sorted(glob.glob(os.path.join(perf_dir, "hprof_*.txt"))):
        rec = {"path": p, "mods": {}, "threads": {}, "self": collections.defaultdict(collections.Counter),
               "incl": collections.defaultdict(collections.Counter), "stk": collections.defaultdict(list)}
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("# host_profile"):
                    m = HDR.match(line)
                    if m:
                        rec["hz"] = int(m.group(4))
                        rec["t0"], rec["t1"] = int(m.group(5)), int(m.group(6))
                    continue
                parts = line.rstrip("\n").split(" ")
                if parts[0] == "mod":
                    rec["mods"][int(parts[1])] = (" ".join(parts[4:]), int(parts[2], 16), int(parts[3], 16))
                elif parts[0] == "thread":
                    tid = int(parts[1])
                    kv = dict(x.split("=", 1) for x in parts[2:] if "=" in x)
                    rec["threads"][tid] = {"name": kv.get("name", "-"), "run": int(kv.get("run", 0)),
                                           "wait": int(kv.get("wait", 0)), "cycles": int(kv.get("cycles", 0))}
                elif parts[0] in ("self", "incl"):
                    tid, cnt = int(parts[1]), int(parts[2])
                    rec[parts[0]][tid][parse_key(parts[3])] += cnt
                elif parts[0] == "stk":
                    tid, kind, cnt = int(parts[1]), parts[2], int(parts[3])
                    keys = [parse_key(k) for k in parts[4].split(";")] if len(parts) > 4 and parts[4] else []
                    rec["stk"][tid].append((kind, cnt, keys))
        files.append(rec)
    return files


SYSCALL = re.compile(r"^(ntdll|win32u)!(Nt|Zw)[A-Z]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("perf_dir")
    ap.add_argument("--eras-json")
    ap.add_argument("--era", help="report only this era")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--exe", help="exe whose PDB names the frames (default: the path in the profile)")
    ap.add_argument("--folded", help="write folded stacks (flamegraph.pl format) for the selected era(s)")
    ap.add_argument("--stacks", type=int, default=0, help="also print the top N CPU stacks per thread")
    a = ap.parse_args()
    files = load(a.perf_dir)
    if not files:
        print("no hprof_*.txt in", a.perf_dir)
        return 1
    eras = [("ALL", 0, 1 << 62)]
    if a.eras_json:
        ej = json.load(open(a.eras_json))
        eras = [(e["era"], int(e["t0_s"] * 1e6), int(e["t1_s"] * 1e6)) for e in ej["eras"]
                if not e["era"].startswith("ALL")]
    groups = collections.OrderedDict((e[0], []) for e in eras)
    for rec in files:
        if "t0" not in rec:
            continue
        best, bov = None, 0
        for name, t0, t1 in eras:
            ov = min(t1, rec["t1"]) - max(t0, rec["t0"])
            if ov > bov:
                best, bov = name, ov
        if best:
            groups[best].append(rec)
    # One symbolizer over the union of module tables (module indices are stable per run).
    mods = {}
    for rec in files:
        mods.update(rec["mods"])
    sym = symbolize.Symbolizer(mods, a.exe)
    allkeys = set()
    for rec in files:
        for d in (rec["self"], rec["incl"]):
            for c in d.values():
                allkeys.update(c.keys())
        for lst in rec["stk"].values():
            for _, _, ks in lst:
                allkeys.update(ks)
    sym.resolve(allkeys)
    S = lambda k: symbolize.short(sym.sym(*k))  # noqa: E731
    folded = collections.Counter()
    for era, recs in groups.items():
        if a.era and era != a.era:
            continue
        if not recs:
            continue
        span = sum(r["t1"] - r["t0"] for r in recs) / 1e6
        hz = recs[0].get("hz", 0)
        # Merge threads across intervals by (tid).
        th = collections.defaultdict(lambda: {"name": "-", "run": 0, "wait": 0, "cycles": 0,
                                              "self": collections.Counter(), "incl": collections.Counter(),
                                              "stk": collections.Counter()})
        for r in recs:
            for tid, t in r["threads"].items():
                x = th[tid]
                if t["name"] != "-":
                    x["name"] = t["name"]
                x["run"] += t["run"]
                x["wait"] += t["wait"]
                x["cycles"] += t["cycles"]
            for tid, c in r["self"].items():
                for k, n in c.items():
                    th[tid]["self"][S(k)] += n
            for tid, c in r["incl"].items():
                for k, n in c.items():
                    th[tid]["incl"][S(k)] += n
            for tid, lst in r["stk"].items():
                for kind, cnt, ks in lst:
                    names = [S(k) for k in ks]
                    th[tid]["stk"][(kind, tuple(names))] += cnt
        order = sorted(th.items(), key=lambda kv: -kv[1]["cycles"])
        print("=" * 100)
        print("ERA %s: %d interval(s), %.0f s, %d Hz" % (era, len(recs), span, hz))
        print("thread CPU (cycles over the era; ~GHz-s): " + ", ".join(
            "%s/%d %.1f" % (t["name"], tid, t["cycles"] / 1e9) for tid, t in order[:12]))
        for tid, t in order[:a.threads]:
            if not t["run"]:
                continue
            root = "-"
            # Name an unnamed host thread by the deepest SupermanReturns frame of its top stack.
            if t["name"] in ("-", ""):
                for (kind, names), _ in t["stk"].most_common(5):
                    exe_frames = [n for n in names if "!" not in n]
                    if exe_frames:
                        root = exe_frames[-1]
                        break
            sc = sum(n for f, n in t["self"].items() if SYSCALL.match(f))
            print("-" * 100)
            print("thread %d %s%s: %d CPU samples (%.0f%% of wall at %d Hz), %d wait samples, "
                  "%.1f Gcycles; syscall-leaf %.0f%%" % (
                      tid, t["name"], (" root=" + root) if root != "-" else "", t["run"],
                      100.0 * t["run"] / max(1.0, span * hz), hz, t["wait"], t["cycles"] / 1e9,
                      100.0 * sc / max(1, t["run"])))
            print("  %-58s %6s   %-58s %6s" % ("SELF", "%", "INCLUSIVE", "%"))
            selfs = t["self"].most_common(a.top)
            incls = t["incl"].most_common(a.top)
            for i in range(max(len(selfs), len(incls))):
                l = selfs[i] if i < len(selfs) else ("", 0)
                r = incls[i] if i < len(incls) else ("", 0)
                print("  %-58s %5.1f%%   %-58s %5.1f%%" % (
                    l[0][:58], 100.0 * l[1] / t["run"] if l[1] else 0.0,
                    r[0][:58], 100.0 * r[1] / t["run"] if r[1] else 0.0))
            if a.stacks:
                print("  top CPU stacks (leaf first):")
                for (kind, names), n in [x for x in t["stk"].most_common() if x[0][0] == "R"][:a.stacks]:
                    print("   %5.1f%% %s" % (100.0 * n / t["run"], " < ".join(names[:14])))
            if a.folded:
                for (kind, names), n in t["stk"].items():
                    if kind == "R":
                        folded[";".join([t["name"]] + list(reversed(names)))] += n
    if a.folded:
        with open(a.folded, "w") as f:
            for k, n in folded.most_common():
                f.write("%s %d\n" % (k, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
