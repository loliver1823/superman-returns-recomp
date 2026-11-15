"""iter-465 window: draws the exe's own app icon (app/app_icon.ico) - an ORIGINAL design.

The icon Explorer shows for SupermanReturns.exe, and the window's icon until the game's own title
icon (read at run time from the player's default.xex, app/window_icon.cpp) replaces it. It uses no
game art and no DC trademark (no shield, no "S"): a midnight-blue rounded tile with a red
double chevron rising over a gold horizon line - "flight" + "recompiled" in the port's red/gold
accent. Our own work (CREDITS.md, Assets).

Usage: python tools/player/make_app_icon.py   (writes app/app_icon.ico; needs Pillow)
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "app", "app_icon.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
S = 1024  # drawn large, downsampled per size

NAVY = (14, 24, 54, 255)
NAVY_EDGE = (40, 64, 130, 255)
RED = (214, 40, 40, 255)
GOLD = (244, 190, 52, 255)


def chevron(d, cx, tip_y, half_w, height, thick, fill):
    # an upward chevron: outer V minus inner V, as one polygon
    d.polygon([
        (cx, tip_y),
        (cx + half_w, tip_y + height),
        (cx + half_w - thick, tip_y + height + thick * 0.55),
        (cx, tip_y + thick * 1.15),
        (cx - half_w + thick, tip_y + height + thick * 0.55),
        (cx - half_w, tip_y + height),
    ], fill=fill)


def draw():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = S * 0.04
    r = S * 0.2
    d.rounded_rectangle([m, m, S - m, S - m], radius=r, fill=NAVY_EDGE)
    e = S * 0.07
    d.rounded_rectangle([e, e, S - e, S - e], radius=r * 0.8, fill=NAVY)
    cx = S / 2
    chevron(d, cx, S * 0.17, S * 0.31, S * 0.27, S * 0.115, RED)
    chevron(d, cx, S * 0.40, S * 0.31, S * 0.27, S * 0.115, RED)
    d.rounded_rectangle([S * 0.2, S * 0.76, S * 0.8, S * 0.82], radius=S * 0.03, fill=GOLD)
    return im


def main():
    big = draw()
    imgs = [big.resize((n, n), Image.LANCZOS) for n in SIZES]
    imgs[-1].save(OUT, format="ICO", sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
