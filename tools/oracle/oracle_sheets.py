#!/usr/bin/env python3
"""oracle_sheets.py - step-4 lane A (xenia-oracle): side-by-side sheets of the chapter matrix.

    py -3.11 tools/oracle/oracle_sheets.py sheets --xenia <xenia pass dir> --ours <ours pass dir> --out <dir>
        [--seeds a,b] [--offsets 10,30,60,120]
    py -3.11 tools/oracle/oracle_sheets.py pair --left X.png --right O.png --out pair.png [--crop x,y,w,h] [--scale 2]
        [--labels "Xenia,ours"]
    py -3.11 tools/oracle/oracle_sheets.py xsheet --xenia <xenia pass dir> --out <dir> [--seeds ...]   # Xenia only

sheets: per seed one JPEG <out>/<seed>.jpg - row 1 Xenia (tools/oracle/xenia_seed_pass.py shots/hud_+NNN.png, a
PrintWindow of the 1280x720 client), row 2 ours (tools/oracle/ours_seed_pass.py matched/hud_+NNN.png, the GPU-readback
present nearest to the same offset), the columns the offsets after each side's own anchor (the guest's main_hud
request). Plus <out>/stats.json: per image mean RGB / luma, near-black share, mean saturation and a 160x90 edge share
(cheap flags only: the two sides show different MOMENTS of a non-deterministic game - traffic, AI, the idle camera -
so the eyes judge, the numbers only add flags; AGENTS.md "metric traps").
pair: two images side by side at 1:1 (or --scale), optionally the same crop of each, for close-ups.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont, ImageStat


def font(sz):
    for f in ("arialbd.ttf", "arial.ttf", "segoeui.ttf"):
        try:
            return ImageFont.truetype(f, sz)
        except OSError:
            continue
    return ImageFont.load_default()


def latest_ok(pass_dir, seed, which):
    """The newest run folder of a seed whose result passed (else the newest at all)."""
    cands = sorted(glob.glob(os.path.join(pass_dir, seed)) + glob.glob(os.path.join(pass_dir, seed + "_r*")),
                   key=os.path.getmtime)
    best = None
    for c in cands:
        j = os.path.join(c, "run.json" if which == "xenia" else "ours_run.json")
        try:
            d = json.load(open(j))
        except (OSError, ValueError):
            continue
        v = d.get("result", d).get("verdict")
        if v == "PASS" or best is None:
            best = (c, d)
    return best


def stats(img):
    im = img.convert("RGB")
    st = ImageStat.Stat(im)
    small = im.resize((160, 90))
    g = small.convert("L")
    px = list(g.getdata())
    near_black = sum(1 for v in px if v < 16) / len(px)
    hsv = small.convert("HSV")
    sat = ImageStat.Stat(hsv).mean[1]
    edges = 0
    for y in range(89):
        for x in range(159):
            v = px[y * 160 + x]
            if abs(v - px[y * 160 + x + 1]) >= 8 or abs(v - px[(y + 1) * 160 + x]) >= 8:
                edges += 1
    return {"mean_rgb": [round(c, 1) for c in st.mean], "luma": round(ImageStat.Stat(g).mean[0], 1),
            "near_black": round(near_black, 3), "sat": round(sat, 1), "edge_share": round(edges / (159 * 89), 3)}


def cell(path, w, h):
    if path and os.path.exists(path):
        im = Image.open(path).convert("RGB")
        return im.resize((w, h), Image.LANCZOS), stats(im)
    im = Image.new("RGB", (w, h), (60, 0, 60))
    ImageDraw.Draw(im).text((10, h // 2), "missing", fill=(255, 255, 255), font=font(24))
    return im, None


def sheet_for(seed, rows, offsets, out, cw=640, ch=360):
    """rows = [(label, {offset: path}, note)]"""
    pad, top, left = 6, 54, 170
    W = left + len(offsets) * (cw + pad)
    H = top + len(rows) * (ch + pad + 22)
    sh = Image.new("RGB", (W, H), (24, 24, 28))
    d = ImageDraw.Draw(sh)
    d.text((10, 8), seed, fill=(255, 230, 120), font=font(26))
    st = {}
    for ci, o in enumerate(offsets):
        d.text((left + ci * (cw + pad) + 8, 34), "anchor %+d s" % o, fill=(200, 200, 200), font=font(16))
    for ri, (label, paths, note) in enumerate(rows):
        y = top + ri * (ch + pad + 22)
        d.text((8, y + 8), label, fill=(255, 255, 255), font=font(18))
        for li, line in enumerate((note or "").split("\n")[:6]):
            d.text((8, y + 34 + li * 18), line, fill=(170, 170, 170), font=font(13))
        for ci, o in enumerate(offsets):
            im, s = cell(paths.get(o), cw, ch)
            sh.paste(im, (left + ci * (cw + pad), y))
            st["%s %+d" % (label, o)] = s
            if s:
                d.text((left + ci * (cw + pad) + 4, y + ch + 2), "luma %.0f sat %.0f black %.2f edges %.2f" % (
                    s["luma"], s["sat"], s["near_black"], s["edge_share"]), fill=(150, 150, 150), font=font(13))
    sh.save(out, quality=90)
    return st


def cmd_sheets(a):
    offsets = [int(x) for x in a.offsets.split(",")]
    os.makedirs(a.out, exist_ok=True)
    seeds = a.seeds.split(",") if a.seeds else sorted({re.sub(r"_r\d+$", "", os.path.basename(p))
                                                     for p in glob.glob(os.path.join(a.xenia, "*"))
                                                     if os.path.isfile(os.path.join(p, "run.json"))})
    allst = {}
    for seed in seeds:
        xr = latest_ok(a.xenia, seed, "xenia")
        rows = []
        if xr:
            res = xr[1].get("result", {})
            rows.append(("Xenia (retail)", {o: os.path.join(xr[0], "shots", "hud_%+04d.png" % o) for o in offsets},
                         "%s\nlanding %s\nanchor %s s\n%s" % (os.path.basename(xr[0]), res.get("landing"),
                                                             res.get("anchor_s"), res.get("verdict"))))
        if a.ours:
            orr = latest_ok(a.ours, seed, "ours")
            if orr:
                m = orr[1].get("matched") or {}
                rows.append(("ours", {o: os.path.join(orr[0], "matched", "hud_%+04d.png" % o) for o in offsets},
                             "%s\nlanding %s\n%s\nat %s" % (os.path.basename(orr[0]), orr[1].get("landing"),
                                                           orr[1].get("verdict"),
                                                           " ".join(str(m.get("hud_%+04d" % o, {}).get("at_s")) for o in offsets))))
        if not rows:
            continue
        allst[seed] = sheet_for(seed, rows, offsets, os.path.join(a.out, seed + ".jpg"))
        print("sheet", os.path.join(a.out, seed + ".jpg"))
    with open(os.path.join(a.out, "stats.json"), "w") as f:
        json.dump(allst, f, indent=1)
    return 0


def cmd_pair(a):
    L, R = Image.open(a.left).convert("RGB"), Image.open(a.right).convert("RGB")
    if R.size != L.size:
        R = R.resize(L.size, Image.LANCZOS)
    if a.crop:
        x, y, w, h = [int(v) for v in a.crop.split(",")]
        L, R = L.crop((x, y, x + w, y + h)), R.crop((x, y, x + w, y + h))
    if a.scale != 1:
        L = L.resize((int(L.width * a.scale), int(L.height * a.scale)), Image.NEAREST)
        R = R.resize((int(R.width * a.scale), int(R.height * a.scale)), Image.NEAREST)
    labels = (a.labels or "left,right").split(",")
    out = Image.new("RGB", (L.width + R.width + 8, L.height + 30), (24, 24, 28))
    out.paste(L, (0, 30))
    out.paste(R, (L.width + 8, 30))
    d = ImageDraw.Draw(out)
    d.text((6, 4), labels[0], fill=(255, 255, 255), font=font(20))
    d.text((L.width + 14, 4), labels[1] if len(labels) > 1 else "", fill=(255, 255, 255), font=font(20))
    out.save(a.out)
    print(a.out, json.dumps({"left": stats(L), "right": stats(R)}))
    return 0


def cmd_spawn(a):
    """spawn: the Midtown spawn hover (every Metropolis seed loads at the same plaza; Superman at the same screen
    place from ~+5 to ~+15 s) - one crop strip per side and seed, from the survey frames (Xenia every 5 s, ours every
    2 s), for the cape / suit at a matched pose phase (X5)."""
    box = tuple(int(v) for v in a.crop.split(","))
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for seed in a.seeds.split(","):
        xr = latest_ok(a.xenia, seed, "xenia")
        orr = latest_ok(a.ours, seed, "ours")
        cells = []
        if xr:
            for t in (5, 10, 15):
                p = os.path.join(xr[0], "survey", "s_%+04d.jpg" % t)
                if os.path.exists(p):
                    cells.append(("X %+d" % t, p))
        if orr:
            for p in sorted(glob.glob(os.path.join(orr[0], "survey", "s_+00*.jpg"))):
                t = float(os.path.basename(p)[2:9])
                if 4.0 <= t <= 15.5:
                    cells.append(("O %+.1f" % t, p))
        rows.append((seed, cells))
    w, h = box[2] - box[0], box[3] - box[1]
    ncol = max(len(c) for _s, c in rows) if rows else 1
    sh = Image.new("RGB", (ncol * (w + 4) + 4, len(rows) * (h + 24)), (20, 20, 22))
    d = ImageDraw.Draw(sh)
    for ri, (seed, cells) in enumerate(rows):
        y = ri * (h + 24)
        d.text((4, y + 2), seed, fill=(255, 230, 120), font=font(14))
        for ci, (lab, p) in enumerate(cells):
            im = Image.open(p).convert("RGB")
            if im.width != 1280:
                im = im.resize((1280, int(im.height * 1280 / im.width)), Image.LANCZOS)
            x = 4 + ci * (w + 4)
            sh.paste(im.crop(box), (x, y + 20))
            d.text((x + 160, y + 2), lab, fill=(200, 255, 200) if lab.startswith("O") else (200, 200, 255), font=font(14))
    out = os.path.join(a.out, "spawn_capes.jpg")
    sh.save(out, quality=90)
    print(out)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("spawn")
    sp.add_argument("--xenia", required=True)
    sp.add_argument("--ours", required=True)
    sp.add_argument("--out", required=True)
    sp.add_argument("--seeds", required=True)
    sp.add_argument("--crop", default="420,330,660,690")
    s = sub.add_parser("sheets")
    s.add_argument("--xenia", required=True)
    s.add_argument("--ours", default="")
    s.add_argument("--out", required=True)
    s.add_argument("--seeds", default="")
    s.add_argument("--offsets", default="10,30,60,120")
    x = sub.add_parser("xsheet")
    x.add_argument("--xenia", required=True)
    x.add_argument("--out", required=True)
    x.add_argument("--seeds", default="")
    x.add_argument("--offsets", default="10,30,60,120")
    p = sub.add_parser("pair")
    p.add_argument("--left", required=True)
    p.add_argument("--right", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--crop", default="")
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--labels", default="")
    a = ap.parse_args()
    if a.cmd == "xsheet":
        a.ours = ""
        return cmd_sheets(a)
    return {"sheets": cmd_sheets, "pair": cmd_pair, "spawn": cmd_spawn}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
