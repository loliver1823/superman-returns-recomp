"""Render Xenia's trace-dump oracle of the replay traces at a draw resolution scale.

iter-464 resscale (docs/RESOLUTION.md): the oracle for our SR_RES_SCALE=<n> replay is
Xenia's own render of the same .xtr with --draw_resolution_scale_x/_y=<n> (Xenia does
not restore the trace's EDRAM snapshot when scaled - d3d12_render_target_cache.cc:1588 -
and neither does our replay). One PNG per trace, named after the trace, in --out.

    python tools/xenia_scaled_oracles.py --scale 2 --out archive/v1_resscale/xe2
    python tools/xenia_scaled_oracles.py --scale 3 --out archive/v1_resscale/xe3 --traces menu,gp1
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm4_replay_score as score  # noqa: E402  (TRACES / EXTRA / _fixture)

REPO = score.REPO
XE = os.path.join(REPO, "reference", "xenia-canary", "build", "bin", "Windows", "Release",
                  "xenia-gpu-d3d12-trace-dump.exe")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scale", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--traces", default=",".join(list(score.TRACES) + list(score.EXTRA)))
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for short in args.traces.split(","):
        trace = score._fixture({**score.TRACES, **score.EXTRA}[short][0])
        if not os.path.exists(trace):
            print(f"{short}: trace missing ({trace})")
            continue
        png = os.path.join(out, os.path.splitext(os.path.basename(trace))[0] + ".png")
        if os.path.exists(png):
            print(f"{short}: have {png}")
            continue
        cmd = [XE, "--async_shader_compilation=false",
               f"--draw_resolution_scale_x={args.scale}", f"--draw_resolution_scale_y={args.scale}",
               f"--trace_dump_path={out}", trace]
        with open(os.path.join(out, short + ".log"), "w") as log:
            rc = subprocess.run(cmd, cwd=REPO, stdout=log, stderr=subprocess.STDOUT).returncode
        print(f"{short}: rc={rc} {'OK' if os.path.exists(png) else 'NO PNG'} {png}")


if __name__ == "__main__":
    main()
