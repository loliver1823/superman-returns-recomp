#!/usr/bin/env python3
"""apt2txt.py - human-readable dump of one ui.AST movie (structure + AS disassembly).

usage (from the repo root):
  python tools/apt/apt2txt.py <TOCNAME> [ui.AST] > archive/.../dump_<TOCNAME>.txt
  python tools/apt/apt2txt.py --list [ui.AST]        # TOC names of all movies
<TOCNAME> is the ui.AST TOC name (e.g. SCRPRESSSTART, MAIN), case-insensitive -
never a data/ui_ast_extracted/ carve name (carve n holds TOC record n+2).
The default container is <repo>/Superman Returns (USA)/DATA/ui.AST (read-only).
Promoted from archive/v1_frontend/apt_format/apt2txt.py (2026-09-30).
"""
import os
import struct
import sys

try:
    from .bgfa import movies, DEFAULT_UI
    from .apt_model import Apt, CHAR_NAMES, ITEM_NAMES
    from .apt_asm import disasm, fmt, listing
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bgfa import movies, DEFAULT_UI
    from apt_model import Apt, CHAR_NAMES, ITEM_NAMES
    from apt_asm import disasm, fmt, listing

PO_FLAGS = [(0x01, "move"), (0x02, "char"), (0x04, "matrix"), (0x08, "cxform"),
            (0x10, "ratio"), (0x20, "name"), (0x40, "clipdepth"), (0x80, "clipactions")]


def f32(a, o):
    return struct.unpack_from(">f", a.b, o)[0]


def s_at(a, o):
    v = a.u(o)
    return a.b[v:a.b.index(b"\0", v)].decode("latin1") if v else None


def dump(a, out):
    w = out.write
    w(f"Movie @{a.movie:#x} {a.width}x{a.height} {a.ms_per_frame}ms/frame chars={len(a.chars)} "
      f"frames={a.u(a.movie + 0x10)}\n")
    for i, (n, cid) in enumerate(a.exports):
        w(f"  export[{i}] {n!r} = char {cid}\n")
    for e in a.imports:
        w(f"  import url={e[0]!r} symbol={e[1]!r} -> local char {e[2]}\n")
    for idx, (c, t) in sorted(a.chars.items()):
        if not c:
            w(f"  char[{idx}] null\n"); continue
        nm = CHAR_NAMES.get(t, f"type{t}")
        extra = ""
        if t == 1:
            extra = f" bounds=({f32(a,c+16):.1f},{f32(a,c+20):.1f},{f32(a,c+24):.1f},{f32(a,c+28):.1f}) geomId={a.u(c+0x20)}"
        elif t == 7:
            extra = f" imageId={a.u(c+0x10)}"
        elif t == 2:
            extra = (f" bounds=({f32(a,c+16):.1f},{f32(a,c+20):.1f},{f32(a,c+24):.1f},{f32(a,c+28):.1f}) font={a.u(c+0x20)} "
                     f"align={a.u(c+0x24)} color={a.u(c+0x28):08x} size={f32(a,c+0x2c)} flags={a.u(c+0x30):x}/{a.u(c+0x34):x}/{a.u(c+0x38):x} "
                     f"text={s_at(a,c+0x3c)!r} var={s_at(a,c+0x40)!r}")
        elif t == 3:
            extra = f" name={s_at(a,c+0x10)!r} glyphs={a.u(c+0x14)}"
        elif t in (5, 9):
            extra = f" frames={a.u(c+0x10)}"
        w(f"  char[{idx}] @{c:#x} {nm}{extra}\n")
    for idx, (c, t) in sorted(a.chars.items()):
        if t not in (5, 9) or not c:
            continue
        nfr = a.u(c + 0x10); fr = a.u(c + 0x14)
        w(f"\n== timeline of char[{idx}] ({CHAR_NAMES.get(t)}) {nfr} frames ==\n")
        for f in range(nfr):
            n = a.u(fr + 8 * f); arr = a.u(fr + 8 * f + 4)
            for k in range(n):
                it = a.u(arr + 4 * k); ty = a.u(it)
                nm = ITEM_NAMES.get(ty, f"type{ty}")
                if ty == 3:
                    fl = a.u(it + 4)
                    fls = "|".join(n for b, n in PO_FLAGS if fl & b)
                    m = [f32(a, it + 0x10 + 4 * q) for q in range(6)]
                    w(f" f{f} PlaceObject[{fls}] depth={a.i(it+8)} char={a.i(it+12)} "
                      f"m=[{m[0]:.3f} {m[1]:.3f} {m[2]:.3f} {m[3]:.3f} | {m[4]:.1f},{m[5]:.1f}] "
                      f"cxmul={a.u(it+0x28):08x} cxadd={a.u(it+0x2c):08x} ratio={a.u(it+0x30):x} "
                      f"name={s_at(a,it+0x34)!r} clipdepth={a.i(it+0x38)}\n")
                    ca = a.u(it + 0x3c)
                    if ca:
                        for q in range(a.u(ca)):
                            r = a.u(ca + 4) + 12 * q
                            w(f"    clipevent flags={a.u(r):08x} key={a.u(r+4):x}\n")
                            for ln in listing(a, a.u(r + 8)):
                                w("      " + ln + "\n")
                elif ty in (1, 8):
                    bc = a.u(it + 4) if ty == 1 else a.u(it + 8)
                    w(f" f{f} {nm}" + (f" sprite={a.u(it+4)}" if ty == 8 else "") + "\n")
                    for ln in listing(a, bc):
                        w("      " + ln + "\n")
                elif ty == 2:
                    w(f" f{f} FrameLabel {s_at(a,it+4)!r} flags={a.u(it+8):x} frame={a.u(it+12)}\n")
                elif ty == 5:
                    w(f" f{f} BackgroundColor {a.u(it+4):08x}\n")
                elif ty == 4:
                    w(f" f{f} RemoveObject depth={a.i(it+4)}\n")
                else:
                    w(f" f{f} {nm} raw={a.b[it:it+8].hex()}\n")


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Dump one ui.AST movie (selected by TOC name).")
    ap.add_argument("movie", nargs="?", help="TOC name, e.g. SCRPRESSSTART")
    ap.add_argument("ui", nargs="?", default=DEFAULT_UI, help="container (default: the game's ui.AST)")
    ap.add_argument("--list", action="store_true", help="list the TOC names of all movies")
    args = ap.parse_args(argv)
    if args.list:
        # with --list the single positional (if any) is the container
        ui = args.movie if args.movie else args.ui
        for name, inner, apt, cst in movies(ui):
            if apt is not None:
                print(name)
        return 0
    if not args.movie:
        ap.error("movie name required (or --list)")
    want = args.movie.upper()
    for name, inner, apt, cst in movies(args.ui):
        if name.upper() == want and apt is not None:
            dump(Apt(apt, cst), sys.stdout)
            return 0
    sys.exit(f"no movie {want} in {args.ui}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
