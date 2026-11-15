#!/usr/bin/env python3
"""glyph_map.py - the identities of the game's button-prompt icons (iter-464 input lane).

The renderer recognises the game's prompt icons by content: FNV-1a-64 of the first `len`
bytes of the guest texture (gfx/glyph_subst.cpp identify()). The texture bytes are the image
payload of the Apt image character in ui.AST, loaded verbatim: a 128-byte header, then DXT5
blocks, 8in16-swapped, linear block order (docs/INPUT_GLYPHS.md "how the game draws a prompt").
`len` is what the renderer hashes: rows-of-blocks x pitch, pitch = width rounded up to 32
texels (gfx/d3d12_backend.cpp getOrCreateTexture2D srcBytes).

    python tools/glyphs/glyph_map.py --print            # the `tex` lines for the icon list
    python tools/glyphs/glyph_map.py --check            # data/glyphs/glyph_map.txt vs ui.AST + PNGs
    python tools/glyphs/glyph_map.py --extract OUTDIR   # decode the originals to PNG (for previews)

Reads the user's own `Superman Returns (USA)/DATA/ui.AST` (read-only); nothing game-derived is
written into the repo except the hashes, which identify, not reproduce, the art.
"""
import argparse
import os
import struct
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "apt"))
import bgfa  # noqa: E402

TAG_IMAGE = 0x264923D3
FMT_DXT4_5 = 20  # Xenos k_DXT4_5 (the fetch constant's format field)
MANIFEST = os.path.join(REPO, "data", "glyphs", "glyph_map.txt")

# (identity, ui.AST movie, image character) - every prompt icon the game draws for a
# controller button (docs/INPUT_GLYPHS.md inventory). Directional arrows (HINTUP..),
# the question mark and the d-pad highlight overlay are device-neutral and not listed.
ICONS = [
    ("hint_a", "HINTA", 1), ("hint_b", "HINTB", 1), ("hint_x", "HINTX", 1), ("hint_y", "HINTY", 1),
    ("hint_lb", "HINTSHOULDER1", 1), ("hint_rb", "HINTSHOULDER2", 1),
    ("hint_lt", "HINTLT", 1), ("hint_rt", "HINTRT", 1),          # = HINTSHOULDER3 / 4 (same bytes)
    ("hint_start", "HINTSTART", 1), ("hint_back", "HINTSELECT", 1),
    ("hint_dpad", "HINTDPAD", 1),                                 # = every HINTDPAD_* base
    ("hint_ls_label", "HINTLSTICK", 1), ("hint_rs_label", "HINTRSTICK", 1),
    ("hint_stick", "HINTSTICK", 1),                               # = HINTLSTICK/HINTRSTICK base
    ("big_a", "A", 4), ("big_b", "B", 4), ("big_x", "X", 4), ("big_y", "Y", 4),
    ("powers_dpad", "DPADPOWERS", 5),                             # the HUD powers selector
]


def ui_path():
    """The repo's game folder, else SR_GAME_DIR (a worktree has no game folder)."""
    if os.path.isfile(bgfa.DEFAULT_UI):
        return bgfa.DEFAULT_UI
    g = os.environ.get("SR_GAME_DIR")
    return os.path.join(g, "DATA", "ui.AST") if g else bgfa.DEFAULT_UI


def images(ui):
    """{(movie, char): (w, h, payload)} for the icon movies."""
    want = {m for _, m, _ in ICONS}
    out = {}
    for name, inner, _apt, _cst in bgfa.movies(ui):
        if name not in want:
            continue
        for e in inner.entries:
            if e["tag"] != TAG_IMAGE:
                continue
            d = e["data"]
            w, h = struct.unpack_from(">HH", d, 0)
            ch = int.from_bytes(e["id"][:4], "little")
            out[(name, ch)] = (w, h, d[128:])
    return out


def hashed_len(w, h):
    pitch = (w + 31) // 32 * 32
    return ((h + 3) // 4) * (pitch // 4) * 16


def fnv1a64(b):
    x = 0xCBF29CE484222325
    for c in b:
        x ^= c
        x = (x * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return x


def tex_lines(ui):
    imgs = images(ui)
    lines = []
    for ident, movie, ch in ICONS:
        if (movie, ch) not in imgs:
            raise SystemExit(f"{movie} char {ch}: no image in {ui}")
        w, h, pl = imgs[(movie, ch)]
        n = hashed_len(w, h)
        if n > len(pl):
            raise SystemExit(f"{movie} char {ch}: payload {len(pl)} < hashed length {n}")
        lines.append(f"tex {ident} {FMT_DXT4_5} {w} {h} {n} {fnv1a64(pl[:n]):016x}")
    return lines


def check(ui, manifest):
    want = {l.split()[1]: l for l in tex_lines(ui)}
    have = {}
    problems = 0
    root = os.path.dirname(manifest)
    for raw in open(manifest, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == "tex":
            have[tok[1]] = " ".join(tok)
        elif tok[0] in ("use", "keyglyph", "keydefault"):
            for t in tok:
                p = t[4:] if t.startswith("png:") else t if t.endswith(".png") else None
                if p and not os.path.isfile(os.path.join(root, p)):
                    print(f"MISSING png {p}")
                    problems += 1
    for k, v in want.items():
        if have.get(k) != v:
            print(f"MISMATCH {k}: manifest {have.get(k)!r} vs ui.AST {v!r}")
            problems += 1
    print(f"{len(want)} identities checked, {problems} problem(s)")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ui", default=ui_path())
    ap.add_argument("--print", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--extract")
    ap.add_argument("--payloads", help="write each icon's raw guest bytes (the hashed span) to DIR/<id>.bin "
                                       "(input of build/difftest.exe --glyph-test)")
    a = ap.parse_args()
    if a.payloads:
        os.makedirs(a.payloads, exist_ok=True)
        imgs = images(a.ui)
        for ident, movie, ch in ICONS:
            w, h, pl = imgs[(movie, ch)]
            open(os.path.join(a.payloads, ident + ".bin"), "wb").write(pl[:hashed_len(w, h)])
        print(f"{len(ICONS)} payloads -> {a.payloads}")
    if a.print:
        print("\n".join(tex_lines(a.ui)))
    if a.check:
        sys.exit(1 if check(a.ui, a.manifest) else 0)
    if a.extract:
        sys.path.insert(0, os.path.join(REPO, "tools", "glyphs"))
        from dxt5 import decode_dxt5  # noqa: E402
        from PIL import Image  # noqa: E402
        os.makedirs(a.extract, exist_ok=True)
        imgs = images(a.ui)
        for ident, movie, ch in ICONS:
            w, h, pl = imgs[(movie, ch)]
            Image.frombytes("RGBA", (w, h), decode_dxt5(pl, w, h)).save(os.path.join(a.extract, ident + ".png"))
        print(f"{len(ICONS)} originals -> {a.extract}")


if __name__ == "__main__":
    main()
