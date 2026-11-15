#!/usr/bin/env python3
"""preview.py - contact sheet: each game prompt icon next to its replacement per glyph set.

    build/difftest.exe --glyph-preview archive/v1_input/preview/rgba
    python tools/glyphs/preview.py archive/v1_input/preview/rgba archive/v1_input/preview/sheet.png

Rows = icons, columns = the game's own icon (xbox), ps4, ps5, switch, keyboard; drawn at the
size the game shows them (32 px hint icons are drawn at 3x so details are visible) on the two
backgrounds prompts appear over (the dark menu panel and a light sky).
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import glyph_map  # noqa: E402
from dxt5 import decode_dxt5  # noqa: E402

SETS = ["xbox", "ps4", "ps5", "switch", "keyboard"]


def main(rgba_dir, out):
    imgs = glyph_map.images(glyph_map.ui_path())
    cell, lab = 104, 110
    rows = glyph_map.ICONS
    bgs = [(28, 36, 60), (170, 190, 215)]
    W = lab + cell * len(SETS) * len(bgs) + 20
    H = 24 + cell * len(rows)
    sheet = Image.new("RGBA", (W, H), (20, 20, 20, 255))
    d = ImageDraw.Draw(sheet)
    for bi, bg in enumerate(bgs):
        for si, s in enumerate(SETS):
            x = lab + (bi * len(SETS) + si) * cell + bi * 20
            d.text((x + 4, 6), s, fill=(255, 255, 0, 255))
    for ri, (ident, movie, ch) in enumerate(rows):
        y = 24 + ri * cell
        d.text((4, y + cell // 2 - 6), ident, fill=(255, 255, 255, 255))
        w, h, pl = imgs[(movie, ch)]
        orig = Image.frombytes("RGBA", (w, h), decode_dxt5(pl, w, h))
        disp = 96  # every icon shown at the same on-screen size
        for bi, bg in enumerate(bgs):
            for si, s in enumerate(SETS):
                x = lab + (bi * len(SETS) + si) * cell + bi * 20
                tile = Image.new("RGBA", (cell - 4, cell - 4), bg + (255,))
                path = os.path.join(rgba_dir, f"{ident}__{s}.rgba")
                if s != "xbox" and os.path.isfile(path):
                    raw = open(path, "rb").read()
                    n = int((len(raw) // 4) ** 0.5)
                    img = Image.frombytes("RGBA", (n, n), raw)
                else:
                    img = orig
                tile.alpha_composite(img.resize((disp, disp), Image.LANCZOS), (4, 4))
                sheet.paste(tile, (x, y))
    sheet.save(out)
    print(out, f"{len(rows)} icons x {len(SETS)} sets")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
