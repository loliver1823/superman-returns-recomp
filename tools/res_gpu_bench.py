#!/usr/bin/env python3
"""res_gpu_bench.py - deterministic GPU cost per pass of a golden frame at an internal scale
(iter-466 res4k; docs/RESOLUTION.md "Measuring").

Replays a trace N times in one process (SR_GPU_TRACE_REPLAY_LOOPS, caches kept) with
SR_PM4_GPU_TIME=1 and SR_RES_SCALE=<s>, then reports the steady-state [PM4_GPU_TIME] and
[PM4_GPU_PASS] lines (the first window is dropped: it carries the first-pass loads). The frame
is the same every loop, so an A/B of a renderer change is a like-for-like GPU number - unlike a
live roll, where the scene differs between runs. Hold the game slot (tools/game_slot.py).

    python tools/res_gpu_bench.py --tag base --scales 1,3 --traces gp1,lg1
    python tools/res_gpu_bench.py --tag natres --scales 3 --gates "SR_X=1"

Writes archive/res_gpu_bench/<tag>/<trace>_<s>x.txt (the GPU lines) and prints a table.

GPU CLOCKS: the replay loop is light, and the driver may keep the GPU in a low P-state for the
whole run (2026-10-06: P5 at ~500-1400 MHz, every pass ~5x slower than at boost). Each row
therefore carries the median graphics clock and the P-states sampled by nvidia-smi during the
replay. Compare an A/B only between rows at the same clock, or use a live roll's
[PM4_GPU_PASS] (a loaded GPU boosts).
"""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from pm4_replay_score import TRACES, EXTRA, _fixture  # noqa: E402

PASS = re.compile(r"\[PM4_GPU_PASS\] (.*)")
TIME = re.compile(r"\[PM4_GPU_TIME\] avg over 120 frames: gpu busy ([\d.]+) ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--scales", default="1,3")
    ap.add_argument("--traces", default="gp1,lg1")
    ap.add_argument("--loops", type=int, default=480)
    ap.add_argument("--gates", default="")
    ap.add_argument("--exe", default="", help="another exe (e.g. a snapshot) for a before/after A/B")
    a = ap.parse_args()
    out = os.path.join(REPO, "archive", "res_gpu_bench", a.tag)
    os.makedirs(out, exist_ok=True)
    allt = {**TRACES, **EXTRA}
    rows = []
    for name in a.traces.split(","):
        xtr = _fixture(allt[name][0])
        for s in a.scales.split(","):
            rtag = "gb_%s_%s_%sx" % (a.tag, name, s)
            gates = "SR_GPU_TRACE_REPLAY_LOOPS=%d,SR_PM4_GPU_TIME=1,SR_RES_SCALE=%s" % (a.loops, s)
            if a.gates:
                gates += "," + a.gates
            cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   os.path.join(REPO, "tools", "replay_ab.ps1"), "-Tag", rtag, "-Trace", xtr,
                   "-Gates", gates, "-TimeoutSec", "900"] + (["-ExePath", a.exe] if a.exe else [])
            smi = None
            try:  # sample the graphics clock + P-state every 500 ms while the replay runs
                smi = subprocess.Popen(["nvidia-smi", "--query-gpu=clocks.gr,pstate",
                                        "--format=csv,noheader,nounits", "-lms", "500"],
                                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            except OSError:
                smi = None
            subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
            clk = "clock ?"
            if smi:
                smi.terminate()
                samples = [l.split(",") for l in smi.communicate()[0].splitlines() if "," in l]
                mhz = [int(c) for c, _ in samples if c.strip().isdigit()]
                ps = sorted({p.strip() for _, p in samples})
                if mhz:
                    clk = "clock %d MHz (%s)" % (statistics.median(mhz), "/".join(ps))
            log = ""
            for f in ("scratch_replay_%s_err.txt" % rtag, "scratch_replay_%s_out.txt" % rtag,
                      "superman_returns_replay%s.log" % rtag):
                p = os.path.join(REPO, f)
                if os.path.exists(p):
                    log += open(p, encoding="utf-8", errors="replace").read()
            for f in os.listdir(REPO):  # keep the repo root clean (scratch_replay_* -> archive)
                if f.startswith(("scratch_replay_%s" % rtag, "superman_returns_replay%s" % rtag)):
                    shutil.move(os.path.join(REPO, f), os.path.join(out, f))
            times = [float(m.group(1)) for m in TIME.finditer(log)][1:]
            passes = [m.group(1) for m in PASS.finditer(log)][1:]
            with open(os.path.join(out, "%s_%sx.txt" % (name, s)), "w") as f:
                f.write("\n".join(m.group(0) for m in TIME.finditer(log)) + "\n")
                f.write("\n".join(m.group(0) for m in PASS.finditer(log)) + "\n")
            busy = sum(times) / len(times) if times else float("nan")
            rows.append((name, s, busy, clk, passes[-1] if passes else "(no [PM4_GPU_PASS] line)"))
    for name, s, busy, clk, p in rows:
        print("%-5s %sx busy %6.2f ms | %s | %s" % (name, s, busy, clk, p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
