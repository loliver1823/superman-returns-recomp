#!/usr/bin/env python3
"""make_test_mods.py - a sample MOD LIBRARY for look rolls (iter-465 modlib lane).

Writes ten small mods into <out> (a test content root's mods/ folder) so the in-game mod library
(docs/MODS.md "The mod library") has a realistic catalogue to show: every category, previews
and screenshots, one mod without a preview (generated card art), one that is off by default,
two that conflict over the same asset stem, a character pack with four suits, and one with a
refused manifest (shown as a PROBLEM card).

The mods are inert for the game: their only content is a uniquely named loose .txt under
DATA/_workinprogress/<id>/ that nothing requests (the conflict pair share one stem on purpose).

Images: with --frames <png/jpg/bmp files or folders>, previews and screenshots are cropped and
colour-graded from those frames (use your OWN captures of this game; the output is local test
content, never committed). Without frames, designed placeholders (gradients, shapes, type) are
drawn instead. No title is burnt into the art: the library prints the name under every card
(iter-465 review: a title on the art and under it read twice). Needs Pillow.

  python tools/modlib/make_test_mods.py --out archive/v1_modlib/croot/mods \\
      --frames archive/v1_audio/oracle
"""
import argparse
import glob
import json
import os
import random
import shutil

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps

W, H = 1280, 720


def load_frames(specs):
    files = []
    for s in specs or []:
        if os.path.isdir(s):
            for ext in ("png", "jpg", "jpeg", "bmp"):
                files += glob.glob(os.path.join(s, "**", "*." + ext), recursive=True)
        else:
            files += glob.glob(s)
    out = []
    for f in sorted(set(files)):
        try:
            im = Image.open(f).convert("RGB")
        except Exception:
            continue
        if im.width < 320:
            continue
        # drop the bottom 7% (some desktop captures carry a taskbar there)
        im = im.crop((0, 0, im.width, int(im.height * 0.93)))
        out.append((os.path.splitext(os.path.basename(f))[0], im))
    return out


def pick(frames, keys, k, fallback_index):
    """The k-th picked frame (by file-stem substring), else a deterministic fallback."""
    if keys:
        key = keys[k % len(keys)]
        for stem, im in frames:
            if key == stem:
                return im
        for stem, im in frames:
            if key in stem:
                return im
    return frames[fallback_index % len(frames)][1]


def grade(im, tint, sat=1.0, bright=1.0, contrast=1.05):
    im = ImageEnhance.Color(im).enhance(sat)
    im = ImageEnhance.Brightness(im).enhance(bright)
    im = ImageEnhance.Contrast(im).enhance(contrast)
    if tint:
        layer = Image.new("RGB", im.size, tint)
        im = Image.blend(im, layer, 0.22)
    return im


def crop_zoom(im, zoom, cx, cy):
    w, h = im.size
    cw, ch = w / zoom, h / zoom
    x0 = max(0, min(w - cw, cx * w - cw / 2))
    y0 = max(0, min(h - ch, cy * h - ch / 2))
    return im.crop((int(x0), int(y0), int(x0 + cw), int(y0 + ch))).resize((W, H), Image.LANCZOS)


def placeholder(seed, c0, c1, shape):
    rnd = random.Random(seed)
    im = Image.new("RGB", (W, H), c0)
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=tuple(int(c0[i] * (1 - t) + c1[i] * t) for i in range(3)))
    for _ in range(14):
        x = rnd.randint(-200, W)
        d.polygon([(x, H), (x + 420, 0), (x + 470, 0), (x + 50, H)], fill=tuple(min(255, v + 18) for v in c0))
    if shape == "rings":
        for r in range(60, 600, 70):
            d.ellipse([W * 0.66 - r, H * 0.5 - r, W * 0.66 + r, H * 0.5 + r], outline=(255, 255, 255), width=2)
    elif shape == "bars":
        for i in range(24):
            h = rnd.randint(60, 420)
            d.rectangle([80 + i * 46, H - 80 - h, 108 + i * 46, H - 80], fill=(255, 220, 140))
    return im.filter(ImageFilter.GaussianBlur(0.6))


MODS = [
    dict(id="sample_mission", name="Sample Mission", author="Lachlan Oliver", version="0.2.0", category="missions",
         desc="A sample mission mod for the mod library tests: a long description with two paragraphs, three "
              "screenshots and a link, to exercise wrapping and the details page.\n\nRequires a new game or a "
              "save from chapter 1.",
         tint=(40, 200, 90), zoom=1.15, focus=(0.5, 0.5), shots=3, nexus=True,
         pick=["s04", "s07", "r2_s01", "s17"]),
    dict(id="suit_collection", name="Classic Suit Collection", author="CapeWorks", version="1.4", category=None,
         desc="Four extra suits for the Fortress costume screen: Kingdom Come, Black Recovery, Electric Blue and "
              "the 1978 Flight suit. Each suit is its own entry - tick the ones you want in your costume list.",
         tint=(160, 30, 40), zoom=1.7, focus=(0.5, 0.62), shots=4,
         pick=["s13", "s08", "s11", "s10", "s09"],
         suits=[("kingdom", "Kingdom Come"), ("recovery", "Black Recovery Suit"), ("electric", "Electric Blue"),
                ("flight78", "1978 Flight Suit")]),
    dict(id="metropolis_daylight", name="Metropolis Daylight", author="SunDial", version="2.1", category="visuals",
         desc="Brighter skies and warmer light over Metropolis at every time of day.", tint=(255, 190, 90),
         zoom=1.15, focus=(0.5, 0.45), shots=2, bright=1.5, conflict=True, pick=["s09", "s17", "s11"]),
    dict(id="hd_city_textures", name="HD City Textures", author="TexForge", version="3.0", category="visuals",
         desc="Sharper building facades and street surfaces for the whole city.", tint=(120, 170, 255),
         zoom=3.0, focus=(0.10, 0.30), shots=2, conflict=True, pick=["shot_title", "s08", "s13"]),
    dict(id="orchestral_score", name="Orchestral Score Remaster", author="Maestro_K", version="1.0",
         category="audio", desc="Re-balanced flight and combat music cues with a fuller orchestral mix.",
         noimage=True),
    dict(id="clean_hud", name="Clean HUD", author="Lachlan Oliver", version="0.9", category="interface",
         desc="A minimal heads-up display: the health ring hides when you are at full strength.",
         tint=(70, 110, 220), zoom=1.0, focus=(0.5, 0.5), shots=1, menu=True, pick=["s14", "s16"]),
    dict(id="speed_flight", name="Speed Force Flight", author="Velocity", version="1.2", category="gameplay",
         desc="Faster top speed, quicker take-off and a longer super-speed burst.", tint=(255, 120, 60),
         zoom=1.1, focus=(0.5, 0.5), shots=2, streak=True, pick=["s06", "r2_s01", "s07"]),
    dict(id="fortress_expanded", name="Fortress Expanded", author="IceArchitect", version="0.5", category="worlds",
         desc="Opens the Fortress of Solitude's north wing with two new crystal chambers to explore.",
         tint=(150, 220, 255), zoom=1.3, focus=(0.5, 0.5), shots=3, bright=1.35, sat=0.35,
         pick=["r2_s01", "s04", "s05"]),
    dict(id="lex_tower", name="Lex Tower Challenge", author="Daily Planet Modding", version="1.1",
         category="missions", desc="A timed rooftop challenge around LexCorp tower.", tint=(90, 220, 120),
         zoom=1.35, focus=(0.55, 0.42), shots=2, enabled=False, pick=["s16", "s12", "s15_gameplay_at_dump"]),
    dict(id="broken_link", name="Broken Link Pack", author="Unknown", version="0.1", category="other",
         desc="Its mod.json points at a website that is not Nexus Mods, so the manager refuses it.",
         noimage=True, badnexus=True),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="the test content root's mods/ folder")
    ap.add_argument("--frames", action="append", default=[])
    ap.add_argument("--clean", action="store_true", help="remove the generated mods first")
    a = ap.parse_args()
    frames = load_frames(a.frames)
    print(f"{len(frames)} usable frame(s)")
    os.makedirs(a.out, exist_ok=True)
    rnd = random.Random(465)
    for i, m in enumerate(MODS):
        d = os.path.join(a.out, m["id"])
        if a.clean and os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(os.path.join(d, "DATA", "_workinprogress", m["id"]), exist_ok=True)
        stem = "MODLIB_SHARED_SKY" if m.get("conflict") else "MODLIB_TEST_" + m["id"].upper()
        with open(os.path.join(d, "DATA", "_workinprogress", m["id"], stem + ".txt"), "w") as f:
            f.write("mod library test content (" + m["id"] + ")\n")
        man = {"format": 1, "id": m["id"], "name": m["name"], "author": m["author"], "version": m["version"],
               "description": m["desc"]}
        if m.get("category"):
            man["category"] = m["category"]
        # the default load order (higher = nearer the top): the showcase mods first
        man["priority"] = len(MODS) - i
        if m.get("enabled") is False:
            man["enabled"] = False
        if m.get("nexus"):
            man["nexus"] = "https://www.nexusmods.com/"
        if m.get("badnexus"):
            man["nexus"] = "http://example.com/not-nexus"
        if m.get("suits"):
            # the manager's scoped stem prefix (kernel/mod_manager.cpp modAssetPrefix):
            # SRM_<FNV-1a-64 of the upper-case id, 16 lower-case hex>_
            h = 0xCBF29CE484222325
            for c in m["id"].upper().encode():
                h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
            pre = "SRM_%016x_" % h
            man["costumes"] = [{"id": sid, "name": sname, "entity": pre + sid.upper() + "_ENT"}
                               for sid, sname in m["suits"]]
        if not m.get("noimage"):
            imgs = []
            n = 1 + m.get("shots", 1)
            for k in range(n):
                if frames:
                    src = pick(frames, m.get("pick"), k, i * 3 + k * 2)
                    fx, fy = m["focus"]
                    im = crop_zoom(src, m["zoom"] + 0.15 * k, fx + 0.05 * k * rnd.choice([-1, 1]), fy)
                    im = grade(im, m.get("tint"), sat=m.get("sat", 1.1), bright=m.get("bright", 1.05))
                else:
                    c0 = tuple(int(v * 0.6) for v in m.get("tint", (60, 90, 160)))
                    im = placeholder(i * 10 + k, c0, (4, 10, 30), "rings" if k % 2 else "bars")
                imgs.append(im)
            imgs[0].save(os.path.join(d, "preview.jpg"), quality=90)
            man["preview"] = "preview.jpg"
            if len(imgs) > 1:
                os.makedirs(os.path.join(d, "screenshots"), exist_ok=True)
                man["screenshots"] = []
                for k, im in enumerate(imgs[1:], 1):
                    p = "screenshots/shot%d.png" % k
                    im.save(os.path.join(d, p.replace("/", os.sep)))
                    man["screenshots"].append(p)
        with open(os.path.join(d, "mod.json"), "w", encoding="utf-8") as f:
            json.dump(man, f, indent=2)
        print("wrote", m["id"])


if __name__ == "__main__":
    main()
