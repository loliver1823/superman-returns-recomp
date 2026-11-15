#!/usr/bin/env python3
"""frame_sheet.py - THE canonical contact-sheet / frame-strip / single-frame maker for presented
frames (iter-466 tools consolidation; folds archive/v1_*/sheet.py, sheet_range.py, strips.py and
frame.py). Input: a SR_PRESENT_DUMP folder (present_<tag>_<n>_p<NNNNNN>_<W>x<H>.bmp) or any
folder of .bmp/.png frames.

    python tools/frame_sheet.py <frames_dir> --out sheet.jpg                      # whole folder, 6 cols
    python tools/frame_sheet.py <frames_dir> --out s.jpg --from 1200 --to 4000 --step 40
    python tools/frame_sheet.py <frames_dir> --out s.jpg --count 24               # 24 evenly spaced
    python tools/frame_sheet.py <frames_dir> --out strip.png --strip --count 8    # one row
    python tools/frame_sheet.py <frames_dir> --out f.png --pick 12345             # one frame (present #)
    python tools/frame_sheet.py <frames_dir> --out f.png --newest                 # the newest frame

--from/--to/--step select by PRESENT NUMBER (_pNNNNNN_) when the names carry one, else by index.
Each tile is labelled p<present> (or the file name). Frames are presented-frame readbacks: the
only capture that supports appearance claims (TOOLKIT; pre-tonemap resolves look dark/flat).
A whole-run review with anomaly sheets is tools/verify_run.py; this tool is for picking ranges.
"""
import argparse
import os
import re
import sys

from PIL import Image, ImageDraw

PNUM = re.compile(r"_p(\d+)_")


def frames(d):
    out = []
    for f in os.listdir(d):
        if f.lower().endswith((".bmp", ".png", ".jpg")):
            m = PNUM.search(f)
            out.append((int(m.group(1)) if m else None, f))
    if all(p is not None for p, _ in out):
        out.sort()
    else:
        out.sort(key=lambda t: os.path.getmtime(os.path.join(d, t[1])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--out", required=True, help=".png/.jpg (never the repo root: archive/ or scratch)")
    ap.add_argument("--from", dest="lo", type=int)
    ap.add_argument("--to", dest="hi", type=int)
    ap.add_argument("--step", type=int, default=1, help="every Nth present number (or Nth file)")
    ap.add_argument("--count", type=int, help="pick this many evenly spaced frames from the selection")
    ap.add_argument("--pick", type=int, help="one frame: this present number (or index)")
    ap.add_argument("--newest", action="store_true", help="one frame: the newest file")
    ap.add_argument("--strip", action="store_true", help="one row (cols = number of frames)")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--thumb", type=int, default=320, help="tile width (height keeps the aspect)")
    ap.add_argument("--scale", type=float, default=1.0, help="single-frame output scale")
    a = ap.parse_args()

    fs = frames(a.dir)
    if not fs:
        sys.exit("frame_sheet: no frames in %s" % a.dir)
    numbered = fs[0][0] is not None
    key = (lambda i, t: t[0]) if numbered else (lambda i, t: i)

    if a.pick is not None or a.newest:
        if a.newest:
            pick = max(fs, key=lambda t: os.path.getmtime(os.path.join(a.dir, t[1])))
        else:
            hit = [t for i, t in enumerate(fs) if key(i, t) == a.pick]
            if not hit:
                sys.exit("frame_sheet: no frame %d (range %s..%s)" % (a.pick, key(0, fs[0]), key(len(fs) - 1, fs[-1])))
            pick = hit[0]
        im = Image.open(os.path.join(a.dir, pick[1])).convert("RGB")
        if a.scale != 1.0:
            im = im.resize((int(im.width * a.scale), int(im.height * a.scale)), Image.LANCZOS)
        im.save(a.out)
        print(a.out, "<-", pick[1])
        return

    sel = []
    for i, t in enumerate(fs):
        k = key(i, t)
        if (a.lo is not None and k < a.lo) or (a.hi is not None and k > a.hi):
            continue
        base = a.lo if a.lo is not None else key(0, fs[0])
        if (k - base) % max(1, a.step) == 0:
            sel.append(t)
    if a.count and len(sel) > a.count:
        n = a.count
        sel = [sel[round(j * (len(sel) - 1) / (n - 1))] for j in range(n)] if n > 1 else sel[:1]
    if not sel:
        sys.exit("frame_sheet: the selection is empty")

    first = Image.open(os.path.join(a.dir, sel[0][1]))
    tw = a.thumb
    th = max(1, tw * first.height // first.width)
    cols = len(sel) if a.strip else max(1, min(a.cols, len(sel)))
    rows = (len(sel) + cols - 1) // cols
    lab = 14
    sheet = Image.new("RGB", (cols * tw, rows * (th + lab)), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    for i, (p, f) in enumerate(sel):
        im = Image.open(os.path.join(a.dir, f)).convert("RGB").resize((tw, th))
        x, y = (i % cols) * tw, (i // cols) * (th + lab)
        sheet.paste(im, (x, y + lab))
        dr.text((x + 3, y + 1), ("p%d" % p) if p is not None else f[:40], fill=(255, 255, 0))
    kw = {"quality": 88} if a.out.lower().endswith((".jpg", ".jpeg")) else {}
    sheet.save(a.out, **kw)
    print(a.out, len(sel), "frames", "(%s..%s)" % (sel[0][0] if numbered else sel[0][1], sel[-1][0] if numbered else sel[-1][1]))


if __name__ == "__main__":
    main()
