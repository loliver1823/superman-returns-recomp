#!/usr/bin/env python3
"""costume_preview.py - make a character pack's Fortress COSTUMES pictures (iter-465).

The game's own COSTUMES screen (Fortress of Solitude) shows every suit as a 128x128 thumbnail
and, on A, a 512x512 full-size picture. For a mod suit the exe builds both Apt movies at boot
(kernel/mod_costume_preview.hpp) from two DXT5 .dds files the pack ships. This tool makes those
files from any picture of the suit (a render, a screenshot), the way the retail pictures look:
the character alone on a transparent background, centred, feet near the bottom.

    python tools/srmods/costume_preview.py --pack mods/<mod> --costume <local id> --image render.png
           [--key-background] [--tolerance 18] [--margin 0.04]

--key-background makes a flat backdrop transparent (its colour is taken from the four corners).
--hero-shot takes an opaque in-game screenshot (a pack's preview.png): it finds the suit (the most
saturated region: cape and suit colours against the grey city), crops a square around it and
lifts the brightness so the picture reads like the retail thumbnails.
It writes <pack>/previews/<costume>_thumb.dds and <costume>_full.dds (+ a .png of each to look
at) and sets the costume's "thumbnail_image" / "fullsize_image" in the pack's mod.json. The
files live outside DATA/, so the engine's loose storage never sees them. A suit without them
gets a generic card. Needs Pillow (DXT5 DDS writing).
"""
import argparse
import json
import os
import sys

from PIL import Image

THUMB, FULL = 128, 512


def key_background(im, tol):
    px = im.load()
    w, h = im.size
    corners = [px[0, 0], px[w - 1, 0], px[0, h - 1], px[w - 1, h - 1]]
    bg = tuple(sorted(c[i] for c in corners)[1] for i in range(3))
    out = im.copy()
    o = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            d = max(abs(r - bg[0]), abs(g - bg[1]), abs(b - bg[2]))
            if d <= tol:
                o[x, y] = (r, g, b, 0)
            elif d <= tol * 2:  # soft edge
                o[x, y] = (r, g, b, a * (d - tol) // tol)
    return out


def hero_square(im, center=None, side_frac=None):
    """An opaque screenshot -> a square crop around the suit, brightened (retail-thumb look)."""
    rgb = im.convert("RGB")
    w, h = rgb.size
    small = rgb.resize((w // 4, h // 4))
    sw, sh = small.size
    hsv = small.convert("HSV").load()
    cols = [0.0] * sw
    rows = [0.0] * sh
    for y in range(int(sh * 0.12), sh):          # skip the HUD strip at the top
        for x in range(sw):
            hh, ss, vv = hsv[x, y]
            wgt = (ss / 255.0) ** 2 * (vv / 255.0) if ss > 70 and vv > 35 else 0.0
            cols[x] += wgt
            rows[y] += wgt
    total = sum(cols)
    if total < 250:   # little colour (a dark suit at night): the suit is where the camera looks
        cx, cy, side = w / 2, h * 0.55, h * 0.75
    else:
        cx = sum(i * c for i, c in enumerate(cols)) / total * 4
        cy = sum(i * r for i, r in enumerate(rows)) / total * 4
        acc, lo, hi = 0.0, None, None
        for i, r in enumerate(rows):             # the rows holding the middle 90 % of the weight
            acc += r
            if lo is None and acc >= total * 0.05:
                lo = i
            if hi is None and acc >= total * 0.95:
                hi = i
        side = max((hi - lo) * 4 * 1.6, h * 0.45)
    if center:                    # the author's own framing (fractions of the frame)
        cx, cy = center[0] * w, center[1] * h
    if side_frac:
        side = side_frac * h
    side = min(side, h, w)
    x0 = int(min(max(cx - side / 2, 0), w - side))
    y0 = int(min(max(cy - side * 0.5, 0), h - side))
    crop = rgb.crop((x0, y0, x0 + int(side), y0 + int(side)))
    # brighten: the crop's mean luminance to ~0.42 with a gamma lift, then a little contrast
    lum = [px for px in crop.convert("L").getdata()]
    mean = sum(lum) / (255.0 * len(lum))
    if mean < 0.42:
        import math
        g = max(0.35, math.log(0.42) / math.log(max(mean, 0.02)))
        lut = [min(255, int(255 * (i / 255.0) ** g + 0.5)) for i in range(256)]
        crop = crop.point(lut * 3)
    from PIL import ImageEnhance
    crop = ImageEnhance.Contrast(crop).enhance(1.12)
    crop = ImageEnhance.Color(crop).enhance(1.1)
    return crop.convert("RGBA")


def square(im, margin):
    box = im.getchannel("A").getbbox() or (0, 0, im.width, im.height)
    im = im.crop(box)
    side = int(max(im.width, im.height) * (1 + 2 * margin))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    x = (side - im.width) // 2
    y = side - im.height - int(side * margin)  # feet near the bottom, like the retail art
    canvas.paste(im, (x, max(0, y)), im)
    return canvas


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--costume", required=True, help="the costume's local id in mod.json")
    ap.add_argument("--image", required=True)
    ap.add_argument("--key-background", action="store_true")
    ap.add_argument("--hero-shot", action="store_true", help="opaque in-game screenshot: crop around the suit + brighten")
    ap.add_argument("--center", help="--hero-shot framing override: X,Y of the suit as fractions of the frame")
    ap.add_argument("--side", type=float, help="--hero-shot crop side as a fraction of the frame height")
    ap.add_argument("--tolerance", type=int, default=18)
    ap.add_argument("--margin", type=float, default=0.04)
    a = ap.parse_args(argv)

    manifest = os.path.join(a.pack, "mod.json")
    with open(manifest, encoding="utf-8") as f:
        mod = json.load(f)
    costume = next((c for c in mod.get("costumes", []) if c.get("id", "").lower() == a.costume.lower()), None)
    if costume is None:
        sys.exit("no costume %r in %s" % (a.costume, manifest))

    im = Image.open(a.image).convert("RGBA")
    if a.key_background:
        im = key_background(im, a.tolerance)
    center = tuple(float(v) for v in a.center.split(",")) if a.center else None
    sq = hero_square(im, center, a.side) if a.hero_shot else square(im, a.margin)
    os.makedirs(os.path.join(a.pack, "previews"), exist_ok=True)
    rel = {}
    for kind, dim in (("thumb", THUMB), ("full", FULL)):
        pic = sq.resize((dim, dim), Image.LANCZOS)
        stem = "previews/%s_%s" % (costume["id"], kind)
        pic.save(os.path.join(a.pack, stem + ".png"))
        pic.save(os.path.join(a.pack, stem + ".dds"), pixel_format="DXT5")
        rel[kind] = stem + ".dds"
    costume["thumbnail_image"] = rel["thumb"]
    costume["fullsize_image"] = rel["full"]
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        json.dump(mod, f, indent=2)
        f.write("\n")
    print("wrote %s and %s; mod.json updated" % (rel["thumb"], rel["full"]))


if __name__ == "__main__":
    main(sys.argv[1:])
