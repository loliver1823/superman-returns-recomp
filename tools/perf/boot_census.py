#!/usr/bin/env python3
"""boot_census.py - the backend-attach timeline of every roll under the given roots (dcb, 2026-10-14; revised after
Astra's round-1 review the same day).

    python tools/perf/boot_census.py --tsv <out.tsv> <root> [<root> ...]   # walk, then summarize
    python tools/perf/boot_census.py --summarize <in.tsv>                  # summarize an earlier walk

Why: boot.device_create's boot_order_gap (docs/native_ledger.json) - the D3D12 backend attaching 20-36 s late in
2 of 3 SR_NATIVE_DEVCREATE rolls (dcc4). The census sets that against every roll on disk. It is OBSERVATIONAL: rolls
differ in exe, preceding rolls, cache history and disk state, so its rates can show that a delay of this kind also
occurs without the gate; they cannot show the gate has no effect.

Every <roll>/drive/attempt*/run.log is one row (deduplicated by path and start time; no roll is dropped for lacking
an attach). Seconds from the log's first line:
  swapchain    [HIGHFPS] swap chain ALLOW_TEARING - d3d12.init reached CreateSwapChainForHwnd (main thread)
  d3dinit / geopso / xlatpso   [D3D12] Backend initialized / Guest geometry pipeline ready / Translated-shader
               pipeline ready (dev profile only: the PLAYER profile drops Info lines)
  shadercache  [SHADER_CACHE] loaded; loadms = the [SHADER_CACHE] boot load done duration (exes from dcb on)
  cpup         [GPU_CP] command processor thread up - where a backend wait starts
  attach       [DISPLAY] PM4 live backend attached (empty when it never attached)
  wait / waitst  the CP's [DISPLAY] CP waited N ms line and its status (attached | TIMED OUT); no line = 0 / none
  revwalk / biggrant / doorbell / renderloop - the guest thread's boot
  prelaunch    the harness's pre-launch interval (steps.log 'saves:' -> 'gates:'): the exe's SHA-256, the repo's git
               identity, the process sweep and a fixed 500 ms sleep, before the game process exists - a proxy for
               the volume's state, not a disk measurement
  priv1/avail1 perf mem.csv's first sample (private MB, available RAM MB)
  arm          devcreate (SR_NATIVE_DEVCREATE=1 and engagement seen: a [NATIVE_DEVCREATE] ring init line, or the first
               [GPU_CP] census with packets=0 - no ME_INIT in the ring) | devcreate-unverified (gate set, neither
               witness) | devcreate-not-engaged (gate set, ME_INIT parsed) | check (SR_NATIVE_FLUSH_CHECK=1: native
               record execution off - devcreate cannot run) | off | off-engaged (anomaly)
  void         the roll is voided by its plan (a *_proof*.json 'voids' entry) or named *void*
  slow         a recorded CP wait > 5 s or a TIMED OUT wait. It is the CP's wait, not the backend's init time: a CP
               that starts late hides a late attach (also reported: initspan = attach - swapchain > 5 s).
Read-only; no game runs.
"""
import argparse
import csv

import json
import os
import re
import statistics
import sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
MARKS = [
    ("swapchain", re.compile(r"\[HIGHFPS\] swap chain ALLOW_TEARING")),
    ("revwalk", re.compile(r"\[REVWALK\] #1 ")),
    ("biggrant", re.compile(r"\[PHYS_LIFO\] big grant")),
    ("doorbell", re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] first doorbell")),
    ("cpup", re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] command processor thread up")),
    ("shadercache", re.compile(r"\[SHADER_CACHE\] loaded")),
    ("loaddone", re.compile(r"\[SHADER_CACHE\] boot load done: ([0-9.]+) ms")),
    ("attach", re.compile(r"\[DISPLAY\] PM4 live backend attached")),
    ("waited", re.compile(r"\[DISPLAY\] CP waited ([0-9.]+) ms for the live backend \(([^)]*)\)")),
    ("renderloop", re.compile(r"\[HIGHFPS\] FGGameRender loop rate")),
    ("d3dinit", re.compile(r"\[D3D12\] Backend initialized")),
    ("geopso", re.compile(r"\[D3D12\] Guest geometry pipeline ready")),
    ("xlatpso", re.compile(r"\[D3D12\] Translated-shader \(XenosRecomp\) pipeline ready")),
    ("dcring", re.compile(r"\[NATIVE_DEVCREATE\] ring init native")),
    ("cpcensus", re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census .* packets=(\d+) ")),
]
PROFILE = re.compile(r"\[LOG_PROFILE\] (\w+)")
STEP_TS = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] (saves: |gates: )")
COLS = ["start", "roll", "arm", "void", "seg", "exe", "prof", "wait", "waitst", "swapchain", "cpup", "revwalk",
        "biggrant", "doorbell", "shadercache", "loadms", "attach", "renderloop", "priv1", "avail1", "prelaunch",
        "d3dinit", "geopso", "xlatpso"]
SKIP_DIRS = ("frames", "content", "perf", "replay")
AFTER_ATTACH = 5000  # the wait line prints right after the attach (gpu_command_processor.cpp): lines to allow for it


def env_of(roll, attempt):
    env, exe = {}, ""
    try:
        with open(os.path.join(attempt, "provenance.json"), encoding="utf-8-sig") as f:
            j = json.load(f)
        env = j.get("effectiveSrEnv") or {}
        exe = (j.get("exeSha256") or "")[:12].lower()
    except Exception:
        try:
            with open(os.path.join(roll, "perf", "meta.txt"), encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith("env "):
                        k, _, v = line[4:].strip().partition("=")
                        env[k] = v
        except OSError:
            pass
    return env, exe


def mem_first(roll):
    try:
        with open(os.path.join(roll, "perf", "mem.csv"), encoding="utf-8") as f:
            next(f)
            row = next(f).strip().split(",")
            return float(row[1]), float(row[9])
    except Exception:
        return None, None


def prelaunch(attempt):
    t = {}
    try:
        with open(os.path.join(attempt, "steps.log"), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = STEP_TS.match(line)
                if m and m.group(2) not in t:
                    t[m.group(2)] = datetime.strptime(m.group(1), "%H:%M:%S.%f")
    except OSError:
        return None
    if "saves: " in t and "gates: " in t:
        d = (t["gates: "] - t["saves: "]).total_seconds()
        return d + 86400 if d < 0 else d
    return None


def scan(log):
    """Marks from the whole log; it stops early only when nothing later can matter: the attach seen, then either its
    wait line or AFTER_ATTACH more lines, the render-loop line and the engagement witnesses settled, and the
    'boot load done' line seen. render/boot-attach: the shader cache now loads on its own thread (SR_SHADER_LOAD_ASYNC,
    default ON), so its translator stage can end - and its done line print - after all the other markers; without
    that line the scan reads on to EOF (an exe older than dcb has none), so loadms is never lost to an early stop."""
    t0, out, prof = None, {}, ""
    since_attach = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
            if t0 is None:
                t0 = t
                pm = PROFILE.search(line)
                prof = pm.group(1) if pm else ""
            for name, rx in MARKS:
                if name in out:
                    continue
                mm = rx.search(line)
                if mm:
                    out[name] = (t - t0).total_seconds()
                    if name == "waited":
                        out["wait_s"], out["waitst"] = float(mm.group(1)) / 1000.0, mm.group(2)
                    elif name == "loaddone":
                        out["loadms"] = float(mm.group(1))
                    elif name == "cpcensus":
                        out["cppackets"] = int(mm.group(1))
            if "attach" in out:
                since_attach = 0 if since_attach is None else since_attach + 1
                settled = ("loaddone" in out and "renderloop" in out and "cpcensus" in out and
                           ("waited" in out or since_attach > AFTER_ATTACH))
                if settled:
                    break
    return t0, out, prof


def arm_of(env, out):
    on = env.get("SR_NATIVE_DEVCREATE", "") == "1"
    engaged = "dcring" in out or out.get("cppackets") == 0
    parsed_me_init = out.get("cppackets", 0) >= 1 and "dcring" not in out
    if env.get("SR_NATIVE_FLUSH_CHECK", "") == "1":
        return "check"
    if on:
        return "devcreate" if engaged else ("devcreate-not-engaged" if parsed_me_init else "devcreate-unverified")
    return "off-engaged" if "dcring" in out else "off"


def umbrella_of(path):
    """The folder holding _worktrees (proof files name rolls relative to it), else the path's drive root."""
    p = os.path.abspath(path)
    while True:
        parent = os.path.dirname(p)
        if os.path.basename(p) == "_worktrees" or parent == p:
            return parent
        p = parent


def voided_rolls(proof_files):
    """Roll paths (normalized, lower case) the plans' proof files list under 'voids'."""
    out = set()
    for p in proof_files:
        umbrella = umbrella_of(p)
        try:
            j = json.load(open(p, encoding="utf-8"))
        except Exception:
            continue
        for v in (j.get("voids") or []) if isinstance(j, dict) else []:
            r = v.get("roll") if isinstance(v, dict) else None
            if r:
                out.add(os.path.normcase(os.path.normpath(r if os.path.isabs(r) else os.path.join(umbrella, r))))
    return out


def walk(roots, tsv):
    rows, seen, proofs = [], set(), []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            proofs += [os.path.join(dirpath, f) for f in filenames if re.match(r".*_proof.*\.json$", f)]
            if "run.log" not in filenames or not os.path.basename(dirpath).startswith("attempt") or \
                    os.path.basename(os.path.dirname(dirpath)) != "drive":
                continue
            roll = os.path.dirname(os.path.dirname(dirpath))
            env, exe = env_of(roll, dirpath)
            t0, out, prof = scan(os.path.join(dirpath, "run.log"))
            if t0 is None:
                continue
            key = (os.path.normcase(os.path.abspath(dirpath)), t0)
            if key in seen:
                continue
            seen.add(key)
            priv, avail = mem_first(roll)
            rollrel = os.path.relpath(roll, os.path.dirname(os.path.abspath(root)))
            r = {"roll": rollrel, "abs": os.path.normcase(os.path.abspath(roll)), "start": t0.strftime("%m-%d %H:%M:%S"),
                 "arm": arm_of(env, out), "seg": env.get("SR_NATIVE_SEGMENTLESS", "-"), "exe": exe,
                 "prof": prof, "wait": out.get("wait_s", 0.0), "waitst": out.get("waitst", "none"),
                 "loadms": out.get("loadms"), "priv1": priv, "avail1": avail, "prelaunch": prelaunch(dirpath)}
            r.update({k: out.get(k) for k in ("swapchain", "cpup", "revwalk", "biggrant", "doorbell", "shadercache",
                                               "attach", "renderloop", "d3dinit", "geopso", "xlatpso")})
            rows.append(r)
    voids = voided_rolls(proofs)
    for r in rows:
        r["void"] = "void" if ("void" in os.path.basename(r["abs"]) or r["abs"] in voids) else ""
    rows.sort(key=lambda r: (r["start"], r["roll"]))
    with open(tsv, "w", encoding="utf-8") as f:
        f.write("\t".join(COLS) + "\n")
        for r in rows:
            f.write("\t".join("" if r.get(c) is None else
                              (f"{r[c]:.2f}" if isinstance(r.get(c), float) else str(r[c])) for c in COLS) + "\n")
    return tsv


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def summarize(tsv, slow_s=5.0):
    with open(tsv, encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    out = ["BOOT_CENSUS rows=%d (deduplicated by path and start; every roll with a log, attached or not). 'slow' = a "
           "recorded CP wait > %gs or TIMED OUT - the CP's wait, which a late-starting CP can hide; observational: "
           "rows differ in exe, preceding rolls, cache history and disk state" % (len(rows), slow_s)]
    slow = lambda r: (num(r.get("wait")) or 0.0) > slow_s or r.get("waitst") == "TIMED OUT"
    span = lambda r: num(r.get("attach")) is not None and num(r.get("swapchain")) is not None and \
        num(r["attach"]) - num(r["swapchain"]) > slow_s
    arms = sorted(set(r.get("arm", "?") for r in rows))
    for arm in arms:
        for vd in ("", "void"):
            s = [r for r in rows if r.get("arm") == arm and r.get("void", "") == vd]
            if not s:
                continue
            out.append("BOOT_CENSUS arm=%-22s%s rolls=%3d slow=%3d (%.0f%%) timedout=%d noattach=%d initspan>%gs=%d" % (
                arm, " VOID" if vd else "     ", len(s), sum(map(slow, s)), 100.0 * sum(map(slow, s)) / len(s),
                sum(1 for r in s if r.get("waitst") == "TIMED OUT"), sum(1 for r in s if num(r.get("attach")) is None),
                slow_s, sum(map(span, s))))
    for lo, hi in ((0.0, 4.0), (4.0, 1e9)):
        for arm in ("devcreate", "off"):
            s = [r for r in rows if r.get("arm") == arm and not r.get("void") and num(r.get("prelaunch")) is not None
                 and lo < num(r.get("prelaunch")) <= hi]
            out.append("BOOT_CENSUS prelaunch (%g, %s] s arm=%-9s non-void rolls=%3d slow=%3d" % (
                lo, "inf" if hi > 1e8 else "%g" % hi, arm, len(s), sum(map(slow, s))))
    pre_s = [num(r.get("prelaunch")) for r in rows if slow(r) and num(r.get("prelaunch")) is not None]
    pre_f = [num(r.get("prelaunch")) for r in rows if not slow(r) and (num(r.get("wait")) or 0) == 0
             and num(r.get("prelaunch")) is not None]
    if pre_s and pre_f:
        out.append("BOOT_CENSUS prelaunch median: slow rolls %.1f s (n=%d), no-wait rolls %.1f s (n=%d)" % (
            statistics.median(pre_s), len(pre_s), statistics.median(pre_f), len(pre_f)))
    dev = [r for r in rows if r.get("prof") == "dev" and slow(r) and r.get("geopso") and r.get("xlatpso")]
    if dev:
        out.append("BOOT_CENSUS dev-profile slow rolls, s from the log start (the spans hold more than the two loads: "
                   "geo -> loaded also covers createXlatPipeline's start, loaded -> translated pipeline the translator "
                   "plus heap / resource creation): roll | wait | geo done | geo -> loaded | loaded -> translated | attach")
        for r in dev:
            g, s_, x = num(r["geopso"]), num(r["shadercache"]), num(r["xlatpso"])
            out.append("  %s %-60s wait %5.1f | geo %5.2f | %5.1f | %5.1f | attach %5.1f" % (
                r["start"], r["roll"][-60:], num(r["wait"]) or 0.0, g, s_ - g, x - s_, num(r["attach"]) or -1))
        out.append("BOOT_CENSUS dev-profile slow rolls=%d: the geometry pipeline ready by %.2f s at most; their time "
                   "falls between that line and the translated-pipeline line" % (
                       len(dev), max(num(r["geopso"]) for r in dev)))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tsv", help="walk the roots into this TSV, then summarize it")
    ap.add_argument("--summarize", help="summarize an existing TSV")
    ap.add_argument("--out", help="also write the summary here")
    ap.add_argument("roots", nargs="*")
    a = ap.parse_args()
    if a.tsv:
        walk(a.roots, a.tsv)
    tsv = a.tsv or a.summarize
    if not tsv:
        ap.error("--tsv <out> <roots...> or --summarize <tsv>")
    text = summarize(tsv)
    sys.stdout.write(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)


if __name__ == "__main__":
    main()
