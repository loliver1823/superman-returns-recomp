"""roi_freeze.py — freeze the attribution target for the 3D conformance lane.

The director's sequencing ruling (CLAIMS_LEDGER, 2026-09-01) requires the four
visual symptom classes to be frozen in final 1280x720 output space BEFORE any
suppression sweep, and requires the freeze artifact to be a TWO-IMAGE CONTACT
SHEET over the pinned baseline AND the oracle -- "that artifact, not prose about
colour selection, freezes the targets."

    python tools/roi_freeze.py --baseline <frame.bmp> --oracle <frame.png>
                               [--sheet OUT.png] [--json OUT.json]

Rectangles are INCLUSIVE (x0,y0,x1,y1). Read-only with respect to inputs.

The rectangles below are the director's corrected set. My submitted `sky` covered
an oracle building, my `superman_cape` ran into that same building, and my
`spikes` overlapped `facade_left` on columns 150..175 while claiming to be
facade-free. Those are corrections, not preferences: do not "improve" them
without a ruling.
"""
import argparse, hashlib, json, sys
from PIL import Image, ImageStat, ImageDraw

ROIS = {
    "facade_left":   (0,    70,  175, 340),   # ratified unchanged
    "facade_right":  (1020, 80, 1279, 620),   # ratified unchanged
    "sky":           (340,  52,  680,  89),   # corrected: was an oracle building
    "spikes":        (176,  20,  330, 160),   # corrected: x0 176 so it is disjoint
    "superman_cape": (880, 360,  930, 405),   # corrected: was into the building
}

# Regions the ruling requires the ROIs to EXCLUDE. Asserted, never measured.
EXCLUDED = {
    "shield":   (380,  90, 800, 470),
    "wordmark": (330, 430, 970, 520),
    "prompt":   (520, 550, 760, 590),
    "legal":    (460, 600, 820, 660),
}

COLOR = {"facade_left": (0, 255, 255), "facade_right": (0, 255, 255),
         "sky": (255, 255, 0), "spikes": (255, 140, 0),
         "superman_cape": (255, 0, 255)}

def intersects(a, b):
    return max(a[0], b[0]) <= min(a[2], b[2]) and max(a[1], b[1]) <= min(a[3], b[3])

def sha_file(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

def annotate(path, tag):
    im = Image.open(path).convert("RGB")
    d = ImageDraw.Draw(im)
    for k, (x0, y0, x1, y1) in EXCLUDED.items():
        d.rectangle([x0, y0, x1, y1], outline=(255, 60, 60), width=2)
        d.text((x0 + 5, y0 + 4), "excl:" + k, fill=(255, 60, 60))
    for k, (x0, y0, x1, y1) in ROIS.items():
        d.rectangle([x0, y0, x1, y1], outline=COLOR[k], width=3)
        d.text((x0 + 5, max(0, y0 - 12)), k, fill=COLOR[k])
    d.text((8, 6), tag, fill=(255, 255, 255))
    return im

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--sheet", default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    # ── assertions, so a later edit fails loudly instead of contaminating a read
    bad = [(k, e) for k, r in ROIS.items() for e, x in EXCLUDED.items() if intersects(r, x)]
    if bad:
        print("FAIL: ROI intersects an excluded feature:", bad); return 1
    ks = list(ROIS)
    olap = [(ks[i], ks[j]) for i in range(len(ks)) for j in range(i + 1, len(ks))
            if intersects(ROIS[ks[i]], ROIS[ks[j]])]
    if olap:
        print("FAIL: ROIs overlap each other:", olap); return 1

    out = {"rois": {}}
    imgs = {}
    for role, path in (("baseline", a.baseline), ("oracle", a.oracle)):
        im = Image.open(path).convert("RGB")
        if im.size != (1280, 720):
            print(f"FAIL: {role} is {im.size}, expected 1280x720 final output space")
            return 1
        imgs[role] = im
        out[role] = {"path": path, "sha256": sha_file(path)}
        print(f"{role:>9} : {path}\n{'sha256':>9} : {out[role]['sha256']}")

    print(f"\n{'roi':>14} {'rect (inclusive)':>24} {'px':>7} | "
          f"{'baseline sha[:12]':>18} {'meanRGB':>16} | {'oracle sha[:12]':>18} {'meanRGB':>16}")
    for k, (x0, y0, x1, y1) in ROIS.items():
        rec = {"rect": [x0, y0, x1, y1], "pixels": (x1 - x0 + 1) * (y1 - y0 + 1)}
        cells = []
        for role in ("baseline", "oracle"):
            crop = imgs[role].crop((x0, y0, x1 + 1, y1 + 1))   # PIL is half-open
            h = hashlib.sha256(crop.tobytes()).hexdigest()
            m = tuple(int(v) for v in ImageStat.Stat(crop).mean)
            rec[role] = {"sha256": h, "meanRGB": list(m)}
            cells.append(f"{h[:12]:>18} {str(m):>16}")
        print(f"{k:>14} {str((x0,y0,x1,y1)):>24} {rec['pixels']:>7} | {cells[0]} | {cells[1]}")
        out["rois"][k] = rec

    print(f"\nasserted: no ROI intersects {', '.join(EXCLUDED)}; no ROI overlaps another")

    if a.sheet:
        top = annotate(a.baseline, "HLE BASELINE  present 10")
        bot = annotate(a.oracle,   "ORACLE")
        sheet = Image.new("RGB", (1280, 1440))
        sheet.paste(top, (0, 0)); sheet.paste(bot, (0, 720))
        sheet.save(a.sheet)
        print(f"wrote contact sheet {a.sheet}")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=2)
        print(f"wrote {a.json}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
