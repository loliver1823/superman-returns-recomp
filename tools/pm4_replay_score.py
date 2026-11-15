"""Score the PM4 replay path against Xenia's own renders of the same traces.

Runs tools/replay_ab.ps1 for each golden trace, moves its artifacts out of the
repo root into archive/pm4_score/<tag>/, and compares our frame with the Xenia
trace-dump oracle (data/gpu_traces/*_oracle.png, rendered by
xenia-gpu-d3d12-trace-dump.exe from the same .xtr, so the scene is identical).

Reported per trace: mean abs diff (0-255), distinct colours and non-black
coverage for BOTH images (a black frame scores well against a dark target -
TOOLKIT 1.6d - so never read MAD alone), and a side-by-side PNG to look at.

    python tools/pm4_replay_score.py --tag base
    python tools/pm4_replay_score.py --tag rtc --gates "SR_PM4_RTCACHE=1"
    python tools/pm4_replay_score.py --tag x --traces menu,eahd

iter-464 resscale: --scale N replays with SR_RES_SCALE=N and scores against Xenia's
render at the same scale (tools/xenia_scaled_oracles.py --scale N --out <dir>; default
dir archive/v1_resscale/xe<N>). The side-by-side is downsized to 1280 wide.
"""
import argparse
import collections
import json
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACES = {
    "menu": ("menu_pressstart_454107ED_9415.xtr", "menu_pressstart_oracle.png"),
    "eahd": ("fmv_eahd_454107ED_1124.xtr", "fmv_eahd_oracle.png"),
    "t16": ("fmv_t16.xtr", "fmv_t16_oracle.png"),
    "t21": ("fmv_t21.xtr", "fmv_t21_oracle.png"),
    "t25": ("fmv_t25.xtr", "fmv_t25_oracle.png"),
}
# Opt-in traces referenced where they were captured (not part of the default
# regression set): Xenia's own F4 captures of GAMEPLAY (Superman in the
# Metropolis streets, HUD). The oracle PNG is Xenia's trace-dump render of the
# same .xtr; render it once with
#   xenia-gpu-d3d12-trace-dump.exe --async_shader_compilation=false
#     --trace_dump_path=archive/xtr_extra <xtr>
# The captures are read from their archived copy (the 2026-09-25 oracle set), not the Xenia
# build's scratch/gpu: a Xenia rebuild or re-clone empties that folder (2026-10-09 incident).
_XE_TRACES = "archive/xenia_oracle_gameplay_20260925/xtr"
EXTRA = {
    "gp1": (f"{_XE_TRACES}/454107ED_25023.xtr", "archive/xtr_extra/454107ED_25023.png"),
    "gp2": (f"{_XE_TRACES}/454107ED_28050.xtr", "archive/xtr_extra/454107ED_28050.png"),
    # iter-460: OUR live gameplay traces (sr_drive frontend_navigate_pm4.json +
    # SR_PM4_TRACE_SWAP=3400/EVERY=300), oracle = Xenia's render of the same trace.
    "lg1": ("archive/p3_gptrace3/live_454107ED_3400.xtr", "archive/p3_gptrace3/xenia/live_454107ED_3400.png"),
    "lg2": ("archive/p3_gptrace3/live_454107ED_3700.xtr", "archive/p3_gptrace3/xenia/live_454107ED_3700.png"),
}


def _fixture(path):
    # A bare file name lives in data/gpu_traces; anything with a slash is repo-relative.
    return os.path.join(REPO, *path.split("/")) if "/" in path else os.path.join(REPO, "data", "gpu_traces", path)


def image_stats(img):
    counts = collections.Counter(img.getdata())
    n = img.size[0] * img.size[1]
    return len(counts), 100.0 * (n - counts[(0, 0, 0)]) / n


EXE = None  # iter-466 gfx: --exe, another exe of this tree (a saved baseline build)
ORACLE_DIR = None  # iter-464: --oracle-dir / --scale (Xenia renders named after the trace)


def run_one(short, tag, gates, out_dir):
    trace, oracle = {**TRACES, **EXTRA}[short]
    if ORACLE_DIR:
        oracle = os.path.join(ORACLE_DIR, os.path.splitext(os.path.basename(trace))[0] + ".png")
    run_tag = f"{tag}_{short}"
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           os.path.join("tools", "replay_ab.ps1"), "-Tag", run_tag,
           "-Trace", _fixture(trace)]
    if gates:
        cmd += ["-Gates", gates]
    if EXE:
        cmd += ["-ExePath", EXE]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    summary = [l.strip() for l in proc.stdout.splitlines() if "SUMMARY" in l or "frames=" in l]
    for suffix in (".png", ".bmp", "_out.txt", "_err.txt"):
        src = os.path.join(REPO, f"scratch_replay_{run_tag}{suffix}")
        if os.path.exists(src):
            shutil.move(src, os.path.join(out_dir, f"{short}{suffix}"))
    stray = os.path.join(REPO, "replay_frame_1.bmp")
    if os.path.exists(stray):
        os.remove(stray)
    ours_path = os.path.join(out_dir, f"{short}.png")
    row = {"trace": short, "summary": summary[0] if summary else None}
    if not os.path.exists(ours_path):
        row["error"] = "no frame produced"
        return row
    ours = Image.open(ours_path).convert("RGB")
    ref = Image.open(_fixture(oracle)).convert("RGB")
    if ref.size != ours.size:
        ref = ref.resize(ours.size, Image.BILINEAR)
    row["mad"] = sum(ImageStat.Stat(ImageChops.difference(ours, ref)).mean) / 3.0
    row["ours_colours"], row["ours_nonblack"] = image_stats(ours)
    row["ref_colours"], row["ref_nonblack"] = image_stats(ref)
    if ours.size[0] > 1280:  # iter-464: scaled frames - a viewable sheet
        ours = ours.resize((1280, ours.size[1] * 1280 // ours.size[0]), Image.LANCZOS)
        ref = ref.resize(ours.size, Image.LANCZOS)
    w, h = ours.size
    sheet = Image.new("RGB", (w * 2, h + 30), (40, 40, 40))
    sheet.paste(ref, (0, 30))
    sheet.paste(ours, (w, 30))
    draw = ImageDraw.Draw(sheet)
    draw.text((10, 8), f"XENIA oracle: {oracle}", fill=(255, 255, 0))
    draw.text((w + 10, 8), f"OURS PM4 replay [{tag}] {gates}  MAD={row['mad']:.2f}", fill=(255, 255, 0))
    sheet.save(os.path.join(out_dir, f"{short}_vs_oracle.png"))
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--gates", default="")
    ap.add_argument("--traces", default=",".join(TRACES))
    # An ad-hoc trace (e.g. a live capture, gfx/pm4_live_trace.hpp) with the
    # Xenia trace-dump render of it as the oracle: --xtr X --oracle Y [--name N].
    ap.add_argument("--xtr")
    ap.add_argument("--oracle")
    ap.add_argument("--name", default="adhoc")
    ap.add_argument("--scale", type=int, default=0, help="iter-464: SR_RES_SCALE for the replay")
    ap.add_argument("--oracle-dir", help="iter-464: Xenia renders <trace>.png (default with --scale: "
                                         "archive/v1_resscale/xe<N>)")
    ap.add_argument("--exe", help="iter-466 gfx: replay with this exe (default build/SupermanReturns.exe)")
    args = ap.parse_args()
    global ORACLE_DIR, EXE
    EXE = args.exe
    if args.scale:
        args.gates = ",".join(g for g in (args.gates, f"SR_RES_SCALE={args.scale}") if g)
        if not args.oracle_dir:
            args.oracle_dir = os.path.join(REPO, "archive", "v1_resscale", f"xe{args.scale}")
    if args.oracle_dir:
        ORACLE_DIR = os.path.abspath(args.oracle_dir)
    if args.xtr:
        if not args.oracle:
            sys.exit("--xtr needs --oracle (render it with xenia-gpu-d3d12-trace-dump.exe)")
        rel = lambda q: os.path.relpath(os.path.abspath(q), REPO).replace(os.sep, "/")
        EXTRA[args.name] = (rel(args.xtr), rel(args.oracle))
        args.traces = args.name
    out_dir = os.path.join(REPO, "archive", "pm4_score", args.tag)
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for short in [t.strip() for t in args.traces.split(",") if t.strip()]:
        if short not in TRACES and short not in EXTRA:
            sys.exit(f"unknown trace '{short}' (have: {', '.join({**TRACES, **EXTRA})})")
        rows.append(run_one(short, args.tag, args.gates, out_dir))
    print(f"{'trace':6} {'MAD':>7} {'ours col':>9} {'ref col':>8} {'ours nb%':>9} {'ref nb%':>8}  summary")
    for r in rows:
        if "error" in r:
            print(f"{r['trace']:6} {'-':>7}  {r['error']}")
            continue
        print(f"{r['trace']:6} {r['mad']:7.2f} {r['ours_colours']:9d} {r['ref_colours']:8d} "
              f"{r['ours_nonblack']:9.1f} {r['ref_nonblack']:8.1f}  {(r['summary'] or '')[:90]}")
    with open(os.path.join(out_dir, "score.json"), "w") as f:
        json.dump({"tag": args.tag, "gates": args.gates, "rows": rows}, f, indent=2)
    print("out:", out_dir)


if __name__ == "__main__":
    main()
