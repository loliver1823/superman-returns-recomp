#!/usr/bin/env python3
"""THE recording-verdict gate (iter-439h; feedback_verification_scope 5th call-out).

No better/worse/fixed language about ANY recording until every sheet this
script emits has been LOOKED AT (Read tool, eyes). Metrics can only ADD flag
frames; they can never clear an era — roll G's "movie era" metrics looked
normal while the screen was 50 seconds of solid maroon (mean≈55 IS maroon).

Usage:  python tools/verify_run.py <framesDir> [--out <dir>]

Emits into <out> (default <framesDir>_sheets):
  sheet_NN.png       4x3 tiles, one frame per ~10s bin, timestamped — the
                     WHOLE run, no era skipped
  dense_<era>.png    8 consecutive frames at each detected anomaly window
                     (flicker ping-pong, brightness jumps, capture-rate dips)
  checklist.txt      every sheet + every previously-reported artifact class,
                     to be ticked with what the eyes saw

Frame files: f_<elapsedMs 7 digits>.jpg — produced by tools/record_fullrun.ps1
(PrintWindow PW_RENDERFULLCONTENT loop; gdigrab/CopyFromScreen are MPO-blind).

Dev-tool runs are refused (docs/DEVTOOLS_DESIGN.md 3.2/3.3, iter-463): a frames dir that has
a RUN_CLASS.json sidecar, or whose name starts with devtool_ - itself or any ancestor
directory - came from a dev-tool-assisted run (tools/devtools/skip_run.py). Its frames are
never verdict, milestone or natural-baseline evidence, so the gate exits 3 without writing
sheets. --allow-devtool writes them anyway for debugging, stamped DEV-TOOL-ASSISTED.
Phase 2 (iter-463): the EXE tags its own runs too. --log <superman_returns*.log> (and every
*.log in the frames dir) is checked: a `[RUN_CLASS]` line other than natural+hud
(dev-tool-assisted, devtool-derived-save, save-seeded) or a [GAUGE] line with devtoolActs > 0
refuses the run the same way (exit 3). The exe writes RUN_CLASS.json into every SR_PRESENT_DUMP
directory of a dev-tool-assisted / devtool-derived run, which the sidecar rule above refuses.
Tests: tools/test_verify_run.py.
"""
import argparse
import glob
import os
import sys

from PIL import Image, ImageDraw

KNOWN_ARTIFACT_CLASSES = [
    "movie era shows MOVIES (not solid maroon/red — roll G failure mode)",
    "maroon/solid-color flashes between movie frames",
    "title screen: diagonal slicing / doubled text / half-black quadrants",
    "half-screen seams at x~648 / y~360 (mid-frame splits)",
    "green/yellow/olive bottom rectangles",
    "untextured polygon soup in preload era",
    "chroma strip at movie bottom rows",
    "freeze / capture-rate collapse (compare bin frame counts)",
    "attract demos: geometry+textures intact, no seams",
]


RUN_CLASS_SIDECAR = "RUN_CLASS.json"
DEVTOOL_PREFIX = "devtool_"


def devtool_taint(frames_dir):
    """The reason frames_dir belongs to a dev-tool-assisted run, or None. Checks the directory
    and every ancestor for the RUN_CLASS.json sidecar or the devtool_ name prefix."""
    d = os.path.abspath(frames_dir)
    while True:
        if os.path.isfile(os.path.join(d, RUN_CLASS_SIDECAR)):
            return "%s holds %s (dev-tool-assisted run)" % (d, RUN_CLASS_SIDECAR)
        if os.path.basename(d).startswith(DEVTOOL_PREFIX):
            return "%s is a %s* run directory (dev-tool-assisted run)" % (d, DEVTOOL_PREFIX)
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def log_taint(log_path):
    """The reason a run log belongs to a tagged (non-natural) run, or None (DEVTOOLS_DESIGN 3.2):
    a [RUN_CLASS] tag other than natural+hud, or devtoolActs > 0 on any [GAUGE] line."""
    import re
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as ex:
        return "log %s unreadable (%s)" % (log_path, ex)
    for m in re.finditer(r"\[RUN_CLASS\] (\S+)", text):
        if m.group(1) not in ("natural+hud", "present-dump"):
            return "%s carries [RUN_CLASS] %s" % (log_path, m.group(1))
    for m in re.finditer(r"\[GAUGE\] .*?devtoolActs=(\d+)", text):
        if int(m.group(1)) > 0:
            return "%s carries devtoolActs=%s" % (log_path, m.group(1))
    return None


def dhash(img):
    px = img.load()
    h = 0
    for y in range(8):
        for x in range(16):
            h = (h << 1) | (1 if px[x, y] > px[x + 1, y] else 0)
    return h


def ham(a, b):
    return bin(a ^ b).count("1")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("frames_dir")
    ap.add_argument("--out", default=None)
    ap.add_argument("--allow-devtool", action="store_true",
                    help="write sheets for a dev-tool-assisted run anyway (debugging only; stamped)")
    ap.add_argument("--log", action="append", default=[],
                    help="the run's superman_returns*.log (repeatable): its [RUN_CLASS] / devtoolActs tags are checked")
    args = ap.parse_args(argv)

    taint = devtool_taint(args.frames_dir)
    logs = list(args.log) + sorted(glob.glob(os.path.join(args.frames_dir, "*.log")))
    for lp in logs:
        if not taint:
            taint = log_taint(lp)
    if taint and not args.allow_devtool:
        print(f"REFUSED: {taint}. Dev-tool-assisted frames are never verdict evidence "
              "(DEVTOOLS_DESIGN 3.2/3.3); pass --allow-devtool for debugging sheets.")
        return 3
    if taint:
        print(f"WARNING: DEV-TOOL-ASSISTED RUN ({taint}) - sheets are for debugging only")

    # iter-445 C7 (audit blocker 3): this globbed f_*.jpg ONLY - the PrintWindow
    # capture, which is recorded as returning BLACK for our flip-model swapchain
    # (feedback_capture_method_authority). The one trustworthy oracle is the
    # in-engine readback SR_PRESENT_DUMP, which writes
    # present_cl_%03u_p%06u_%ux%u.bmp - a different prefix, extension and sort
    # key. So the project's own verdict tool could not read the only evidence
    # its verdict rule accepts. Both are supported now, present_*.bmp preferred.
    files = sorted(glob.glob(os.path.join(args.frames_dir, "present_*.bmp")))
    key_kind = "present"
    if not files:
        files = sorted(glob.glob(os.path.join(args.frames_dir, "f_*.jpg")))
        key_kind = "printwindow"
        if files:
            print("WARNING: falling back to f_*.jpg (PrintWindow) - these are "
                  "BLACK for our flip-model swapchain; prefer SR_PRESENT_DUMP")
    if not files:
        print(f"ERROR: no present_*.bmp or f_*.jpg frames in {args.frames_dir}")
        return 1
    out = args.out or (args.frames_dir.rstrip("\\/") + "_sheets")
    os.makedirs(out, exist_ok=True)

    def _key(f):
        b = os.path.basename(f)
        if key_kind == "present":
            # ...._p%06u_...  -> the present index, which is the only monotonic
            # field in the name (the _cl_%03u counter restarts per category).
            import re
            m = re.search(r"_p(\d{6})_", b)
            # The key is used downstream as ELAPSED MILLISECONDS (binned at
            # //10000, labelled /1000 as seconds). A present index is not that,
            # and feeding one in collapses every frame into one 10 s bin - the
            # sheet then shows a single tile and reads as "the run produced one
            # frame". Convert to a pseudo-timestamp at the guest's own ~60 Hz
            # swap cadence so the existing binning works unchanged.
            return int(m.group(1)) * 1000 // 60 if m else 0
        return int(b[2:9])
    frames = [(_key(f), f) for f in files]
    t_end = frames[-1][0]

    # ---- per-frame quick metrics (flag-adders only) --------------------------
    hashes = []
    means = []
    for ts, fp in frames:
        im = Image.open(fp).convert("L").resize((17, 8))
        hashes.append(dhash(im))
        px = im.resize((8, 4)).load()
        means.append(sum(px[x, y] for y in range(4) for x in range(8)) / 32.0)

    anomalies = []  # (index, reason)
    for i in range(2, len(frames)):
        if ham(hashes[i], hashes[i - 1]) > 40:
            anomalies.append((i, "big-change"))
        if abs(means[i] - means[i - 1]) > 45:
            anomalies.append((i, "brightness-jump"))
    # capture-rate dips: bins with < 1/4 the median frame count
    bin_counts = {}
    for ts, _ in frames:
        bin_counts.setdefault(ts // 10000, 0)
        bin_counts[ts // 10000] += 1
    med = sorted(bin_counts.values())[len(bin_counts) // 2]
    slow_bins = sorted(b for b, n in bin_counts.items() if n < max(2, med // 4))

    # ---- full-run contact sheets (EVERY ~10s bin, 12 tiles/sheet) ------------
    tile_w, tile_h = 426, 240
    ticks = list(range(0, t_end + 1, 10000))
    sheets = []
    for si in range(0, len(ticks), 12):
        chunk = ticks[si:si + 12]
        sheet = Image.new("RGB", (tile_w * 4, tile_h * 3), (24, 24, 24))
        for k, target in enumerate(chunk):
            ts, fp = min(frames, key=lambda x: abs(x[0] - target))
            im = Image.open(fp).convert("RGB").resize((tile_w, tile_h))
            d = ImageDraw.Draw(im)
            d.rectangle([0, 0, 96, 18], fill=(0, 0, 0))
            d.text((4, 3), f"t={ts / 1000:.0f}s", fill=(255, 255, 0))
            sheet.paste(im, ((k % 4) * tile_w, (k // 4) * tile_h))
        name = f"sheet_{si // 12:02d}.png"
        sheet.save(os.path.join(out, name))
        sheets.append(name)

    # ---- dense strips at anomaly windows (8 consecutive frames) --------------
    dense = []
    seen_windows = set()
    for idx, reason in anomalies[:24]:
        w = frames[idx][0] // 5000  # 5s window key
        if w in seen_windows:
            continue
        seen_windows.add(w)
        lo = max(0, idx - 3)
        strip = Image.new("RGB", (tile_w * 4, tile_h * 2), (24, 24, 24))
        for k in range(8):
            j = min(len(frames) - 1, lo + k)
            im = Image.open(frames[j][1]).convert("RGB").resize((tile_w, tile_h))
            d = ImageDraw.Draw(im)
            d.rectangle([0, 0, 130, 18], fill=(0, 0, 0))
            d.text((4, 3), f"t={frames[j][0] / 1000:.1f}s", fill=(0, 255, 255))
            strip.paste(im, ((k % 4) * tile_w, (k // 4) * tile_h))
        name = f"dense_{frames[idx][0] // 1000:03d}s_{reason}.png"
        strip.save(os.path.join(out, name))
        dense.append(name)

    # ---- checklist -----------------------------------------------------------
    cl = os.path.join(out, "checklist.txt")
    with open(cl, "w") as f:
        if taint:
            f.write(f"DEV-TOOL-ASSISTED RUN - NOT verdict, milestone or baseline evidence: {taint}\n")
        f.write("VERDICT GATE — every line needs eyes + a written observation.\n")
        f.write(f"run: {args.frames_dir}  frames={len(frames)}  dur={t_end / 1000:.0f}s\n\n")
        f.write("SHEETS (view every one):\n")
        for s in sheets:
            f.write(f"  [ ] {s}\n")
        for s in dense:
            f.write(f"  [ ] {s}\n")
        if slow_bins:
            f.write(f"\nSLOW BINS (<1/4 median rate): {[b * 10 for b in slow_bins]}s\n")
        f.write("\nARTIFACT CLASSES (state seen/not-seen for each):\n")
        for a in KNOWN_ARTIFACT_CLASSES:
            f.write(f"  [ ] {a}\n")
    print(f"sheets={len(sheets)} dense={len(dense)} -> {out}")
    print(f"checklist: {cl}")
    print("VERDICT GATE: view every sheet with eyes before any verdict wording.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
