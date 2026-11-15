#!/usr/bin/env python3
"""NATIVE N0 zero-impact check: replay the golden Xenia traces through two exes
(or one exe under two gate sets) and compare the produced frames byte for byte.

    python tools/native_replay_check.py --exe-a <baseline.exe> --exe-b build/SupermanReturns.exe \
        --tag n0_off [--gates-b "SR_PM4_COVERAGE=1"] [--traces menu,eahd,t16,t21,t25]

Each run executes with cwd = this tree (the exe reads data/... relative to it) and its
artifacts (replay_frame_1.bmp, the instance log) are moved to
archive/v1_native/replay/<tag>/<side>_<trace>/. SR_SHADER_CACHE_DIR points at a per-tag
copy of data/shader_cache/dxil (one per side), so a runtime-translated miss never dirties
the repo and both sides start from the same cache state. Prints the MD5 of
each frame and the [GPU:REPLAY] SUMMARY line, and the [PM4_COV] CUM line when present.
Exit 1 if any trace differs between A and B or produced no frame.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRACES = {
    "menu": "menu_pressstart_454107ED_9415.xtr",
    "eahd": "fmv_eahd_454107ED_1124.xtr",
    "t16": "fmv_t16.xtr",
    "t21": "fmv_t21.xtr",
    "t25": "fmv_t25.xtr",
}
# Extra gameplay fixtures (pm4_replay_score.py EXTRA): absolute paths in the main tree (SR_MAIN_ROOT, else
# the main checkout git knows).
def _main_root():
    """The main checkout (lane worktrees share its archive/, disc and build): SR_MAIN_ROOT, else the
    parent of git's common directory, else this repo."""
    if os.environ.get("SR_MAIN_ROOT"):
        return os.environ["SR_MAIN_ROOT"]
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        common = subprocess.run(["git", "-C", here, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return os.path.abspath(os.path.join(here, "..", "..") if os.path.basename(here) != "tools" else os.path.join(here, ".."))


_MAIN = Path(_main_root())
_XE = _MAIN / "archive/xenia_oracle_gameplay_20260925/xtr"  # archived copies; Xenia's scratch/gpu is not durable
EXTRA = {
    "gp1": _XE / "454107ED_25023.xtr",
    "gp2": _XE / "454107ED_28050.xtr",
    "lg1": _MAIN / "archive/p3_gptrace3/live_454107ED_3400.xtr",
    "lg2": _MAIN / "archive/p3_gptrace3/live_454107ED_3700.xtr",
}


def trace_path(t):
    return str(EXTRA[t]) if t in EXTRA else str(ROOT / "data" / "gpu_traces" / TRACES[t])


def run(exe, trace, gates, wd, timeout):
    """One replay with cwd = this tree (the exe reads data/... relative to cwd);
    every artifact it leaves in the root is moved into `wd`."""
    if wd.exists():
        shutil.rmtree(wd)
    wd.mkdir(parents=True)
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("SR_"):
            del env[k]
    inst = "nrc_" + wd.parent.name + "_" + wd.name
    env.update({
        "SR_DEVTOOLS": "0", "SR_HLE_FRAME_LIST": "0", "SR_INSTANCE_ID": inst,
        "SR_GPU_TRACE_REPLAY": trace_path(trace),
        "SR_SHADER_CACHE_DIR": str(wd.parent / ("_shader_cache_" + wd.name[0]) / "dxil"),
    })
    for pair in [g for g in gates.split(",") if "=" in g]:
        k, v = pair.strip().split("=", 1)
        env[k] = v
    for stale in ROOT.glob("replay_frame_*.bmp"):
        stale.unlink()
    out = open(wd / "out.txt", "w")
    err = open(wd / "err.txt", "w")
    try:
        p = subprocess.run([str(exe)], cwd=ROOT, env=env, stdout=out, stderr=err, timeout=timeout)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        rc = "TIMEOUT"
    out.close()
    err.close()
    for f in list(ROOT.glob("replay_frame_*.bmp")) + list(ROOT.glob("superman_returns_%s*" % inst)):
        shutil.move(str(f), str(wd / f.name))
    bmp = wd / "replay_frame_1.bmp"
    md5 = hashlib.md5(bmp.read_bytes()).hexdigest() if bmp.exists() else None
    text = (wd / "err.txt").read_text(errors="replace")
    summ = [l.split("[GPU:REPLAY] ", 1)[1] for l in text.splitlines() if "[GPU:REPLAY] SUMMARY" in l]
    cov = [l.split("[PM4_COV] ", 1)[1] for l in text.splitlines() if "[PM4_COV] CUM" in l]
    return {"rc": rc, "md5": md5, "summary": summ[-1] if summ else None, "cov": cov[-1] if cov else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-a", required=True)
    ap.add_argument("--exe-b", required=True)
    ap.add_argument("--gates-a", default="")
    ap.add_argument("--gates-b", default="")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--traces", default=",".join(TRACES))
    ap.add_argument("--timeout", type=int, default=240)
    a = ap.parse_args()
    base = ROOT / "archive" / "v1_native" / "replay" / a.tag
    base.mkdir(parents=True, exist_ok=True)
    for side in "AB":  # one fresh cache copy per side: both start from the same state
        cache = base / ("_shader_cache_" + side) / "dxil"
        if cache.exists():
            shutil.rmtree(cache)
        shutil.copytree(ROOT / "data" / "shader_cache" / "dxil", cache)
    bad = 0
    for t in [x for x in a.traces.split(",") if x]:
        ra = run(Path(a.exe_a).resolve(), t, a.gates_a, base / ("A_" + t), a.timeout)
        rb = run(Path(a.exe_b).resolve(), t, a.gates_b, base / ("B_" + t), a.timeout)
        same = ra["md5"] is not None and ra["md5"] == rb["md5"]
        bad += 0 if same else 1
        print("%-5s %s A=%s B=%s" % (t, "SAME" if same else "DIFF", ra["md5"], rb["md5"]))
        print("      A: rc=%s %s" % (ra["rc"], ra["summary"]))
        print("      B: rc=%s %s" % (rb["rc"], rb["summary"]))
        if rb["cov"]:
            print("      B cov: %s" % rb["cov"][:400])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
