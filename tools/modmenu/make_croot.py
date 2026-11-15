#!/usr/bin/env python3
"""make_croot.py - a dedicated CONTENT ROOT for MODS-menu rolls (iter-464 modmenu lane).

The exe finds its content root (the folder holding "Superman Returns (USA)/" and "mods/") next to
the exe or one level up (kernel/file_io.cpp getContentRoot). A roll that must see an exact set of
installed mods - not whatever other lanes junctioned into the repo's mods/ - runs from a root
built here:

    <out>/Superman Returns (USA)   junction -> the real game folder (read-only, never written)
    <out>/bin                      junction -> an exe snapshot folder (archive/exe_snapshots/<id>)
    <out>/mods/<name>              junction -> each --mod NAME=PATH (a mod folder)
    <out>/mods/enabled.txt         --list text ("\\n" separated) - the player's switch file

Launch the roll with -ExePath <out>/bin/SupermanReturns.exe and -Gates "SR_MODS=@enabled.txt" (a
roll ignores enabled.txt unless asked; docs/MODS.md). Nothing is copied: every entry is a
junction, so a croot costs no disk space. Re-running replaces enabled.txt and missing junctions
only; an existing non-junction path is never deleted.

    python tools/modmenu/make_croot.py --out archive/v1_modmenu/croot1 \\
        --exe archive/exe_snapshots/<id>/SupermanReturns.exe \\
        --game "<main checkout>/Superman Returns (USA)" \\
        --mod modmenu_fe=<main checkout>/mods/modmenu_fe \\
        --mod menu_demo=tools/modmenu/menu_demo --list "modmenu_fe"
"""
import argparse
import os
import subprocess
import sys


def junction(link, target):
    target = os.path.abspath(target)
    if not os.path.exists(target):
        raise SystemExit(f"make_croot: target missing: {target}")
    if os.path.lexists(link):
        # an existing junction is kept (same target) or replaced; anything else is refused
        if os.path.isdir(link) and os.path.realpath(link) == os.path.realpath(target):
            return
        if not os.path.islink(link) and not _is_junction(link):
            raise SystemExit(f"make_croot: {link} exists and is not a junction - refusing to replace it")
        os.rmdir(link)
    r = subprocess.run(["cmd", "/c", "mklink", "/J", os.path.normpath(link), os.path.normpath(target)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"make_croot: mklink /J failed: {r.stdout} {r.stderr}")


def _is_junction(p):
    try:
        return bool(os.readlink(p))
    except OSError:
        return False


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", required=True, help="SupermanReturns.exe inside a snapshot folder")
    ap.add_argument("--game", required=True, help='the real "Superman Returns (USA)" folder')
    ap.add_argument("--mod", action="append", default=[], help="NAME=PATH, repeatable")
    ap.add_argument("--list", default="", help='enabled.txt content, lines separated by "\\n"')
    a = ap.parse_args(argv)
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "mods"), exist_ok=True)
    if os.path.basename(os.path.normpath(a.game)) != "Superman Returns (USA)":
        raise SystemExit("make_croot: --game must be the 'Superman Returns (USA)' folder")
    junction(os.path.join(out, "Superman Returns (USA)"), a.game)
    junction(os.path.join(out, "bin"), os.path.dirname(os.path.abspath(a.exe)))
    for m in a.mod:
        name, _, path = m.partition("=")
        if not name or not path:
            raise SystemExit(f"make_croot: --mod wants NAME=PATH, got {m!r}")
        junction(os.path.join(out, "mods", name), path)
    lst = a.list.replace("\\n", "\n")
    with open(os.path.join(out, "mods", "enabled.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# make_croot.py seed (a roll's own player switch file)\n" + lst + ("\n" if lst and not lst.endswith("\n") else ""))
    print(os.path.join(out, "bin", os.path.basename(a.exe)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
