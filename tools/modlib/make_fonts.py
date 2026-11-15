#!/usr/bin/env python3
"""make_fonts.py - build the mod library's static Saira faces (data/fonts/saira/).

The mod library (docs/MODS.md "The mod library") draws its text with Saira, a SIL OFL 1.1
family by Omnibus-Type (Copyright 2020 The Saira Project Authors, no Reserved Font Name).
Its wide, flat-terminal italic is the closest open match to the game's own extended menu type.
Dear ImGui's stb_truetype rasteriser does not read variable-font axes, so this script makes
static instances (fontTools varLib.instancer), subsets them to Latin + Latin-1 + a few marks,
and renames the family "Saira SR" (a modified version must not use the original name).

Sources (google/fonts, ofl/saira, downloaded 2026-10-03):
  https://raw.githubusercontent.com/google/fonts/main/ofl/saira/Saira%5Bwdth,wght%5D.ttf
      sha256 9d050fc5a01c85f74c4257c207d50b55d1e40c37308c642f974a2c5003231dde
  https://raw.githubusercontent.com/google/fonts/main/ofl/saira/Saira-Italic%5Bwdth,wght%5D.ttf
      sha256 965972c2543b72948aedb55fae36e23175661d0f9d2d8d7f14a1009e466912f8
  https://raw.githubusercontent.com/google/fonts/main/ofl/saira/OFL.txt  (-> data/fonts/saira/OFL.txt)

Usage:
  python tools/modlib/make_fonts.py --src <dir with the two VF files> --out data/fonts/saira
"""
import argparse
import os

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

FACES = [
    # output file, source, wdth, wght
    ("SairaSR-ExpandedBlackItalic.ttf", "Saira-Italic-VF.ttf", 125, 850),  # titles
    ("SairaSR-ExpandedBoldItalic.ttf", "Saira-Italic-VF.ttf", 125, 700),   # tabs, card titles, buttons
    ("SairaSR-ExpandedSemiBold.ttf", "Saira-VF.ttf", 125, 600),            # labels, badges
    ("SairaSR-Medium.ttf", "Saira-VF.ttf", 100, 500),                      # body text
]

UNICODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192,
    0x2193, 0x2122, 0x00A9, 0x00AE, 0x2715, 0x2713]


def make(src_dir, out_dir, name, src, wdth, wght):
    f = TTFont(os.path.join(src_dir, src))
    inst = instancer.instantiateVariableFont(f, {"wdth": wdth, "wght": wght})
    opts = subset.Options()
    opts.layout_features = ["kern", "liga", "calt"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.hinting = False
    ss = subset.Subsetter(opts)
    ss.populate(unicodes=UNICODES)
    ss.subset(inst)
    fam = "Saira SR"
    sub = name.split("-")[1].replace(".ttf", "")
    for rec in inst["name"].names:
        if rec.nameID in (1, 16):
            rec.string = fam
        elif rec.nameID in (2, 17):
            rec.string = sub
        elif rec.nameID == 4:
            rec.string = fam + " " + sub
        elif rec.nameID == 6:
            rec.string = "SairaSR-" + sub
    inst.save(os.path.join(out_dir, name))
    print("wrote", name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for face in FACES:
        make(a.src, a.out, *face)


if __name__ == "__main__":
    main()
