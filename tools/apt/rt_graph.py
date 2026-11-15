#!/usr/bin/env python3
"""rt_graph.py - regression test for the editable graph (apt_graph.py), the writer
every edit goes through.

Checks, for every movie in the user's own read-only ui.AST:
  1. identity: from_apt() -> Graph.emit() is byte-exact (.apt AND .const) - every
     bytecode block is re-assembled from its symbolic instruction list and the .const
     is re-emitted from its item list, so this proves the assembler, not a memcpy
  2. shifted: a 0x40-byte filler block inserted after the magic block (every later
     block, pointer and bytecode operand moves) emits a file that re-parses to the
     same logical movie (apt_asm.logical_view), with 0 uncovered non-zero bytes and
     length +0x40
  3. walk order: Graph.renumber_walk_order() (the runtime's unload order, one .const item
     per index slot) moves nothing on a retail movie and the emit stays byte-exact, and the
     independent byte-level checker (apt_unload.check) passes the retail movie
Exit status 0 only when every check passes.

usage (from the repo root):
  python tools/apt/rt_graph.py [ui.AST]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bgfa import movies, DEFAULT_UI  # noqa: E402
from apt_model import Apt, coverage  # noqa: E402
from apt_asm import logical_view  # noqa: E402
from apt_graph import from_apt, Block  # noqa: E402
import apt_unload  # noqa: E402

SHIFT = 0x40


def main(argv):
    ui_path = argv[0] if argv else DEFAULT_UI
    t0 = time.time()
    n = ident = shifted = tries = walk = 0
    fails = []
    for name, inner, apt, cst in movies(ui_path):
        if apt is None:
            continue
        n += 1
        try:
            a = Apt(apt, cst)
            lastend = max(o + s for o, s, k in a.blocks)
            tail = len(apt) - lastend
            g = from_apt(a)
            tries += sum(1 for k in g.order if g.blocks[k].code for i in g.blocks[k].code if i.op == 0x8F)
            b, c, _ = g.emit(orig_tail=tail)
            if b == apt and c == cst:
                ident += 1
            else:
                d = [i for i in range(min(len(b), len(apt))) if b[i] != apt[i]][:4]
                fails.append((name, "identity", len(b), len(apt), d, c == cst))
            g2 = from_apt(a)
            g2.add_block(Block("filler", bytes(SHIFT)), after=g2.order[0])
            b2, c2, _ = g2.emit(orig_tail=tail)
            a2 = Apt(b2, c2)
            _, _, _, nz = coverage(a2)
            if logical_view(a2) == logical_view(a) and not nz and len(b2) == len(apt) + SHIFT:
                shifted += 1
            else:
                fails.append((name, "shifted", len(nz), len(b2) - len(apt)))
            g3 = from_apt(a)
            rn = g3.renumber_walk_order()
            b3, c3, _ = g3.emit(orig_tail=tail)
            ok_u, problems, _ = apt_unload.check(apt, cst)
            if (b3, c3) == (apt, cst) and not rn["moved"] and not rn["duplicated"] and not rn["arrays_copied"] and ok_u:
                walk += 1
            else:
                fails.append((name, "walk order", rn, problems[:2]))
        except Exception as ex:  # a crash is a failure of that movie, keep going
            fails.append((name, "exception", repr(ex)))
    ok = n > 0 and ident == n and shifted == n and walk == n
    print(f"[{'PASS' if ident == n and n else 'FAIL'}] graph identity byte-exact (.apt AND .const): {ident}/{n}")
    print(f"[{'PASS' if shifted == n and n else 'FAIL'}] graph shifted +{SHIFT:#x}, re-parsed, logically equal: {shifted}/{n}")
    print(f"[{'PASS' if walk == n and n else 'FAIL'}] retail is in unload-walk order: renumber_walk_order moves "
          f"nothing and emits byte-exact, apt_unload.check passes: {walk}/{n}")
    print(f"info: Try (0x8F) ops in corpus: {tries}")
    for f in fails[:10]:
        print("  fail:", f)
    print(f"{'ALL PASS' if ok else 'FAILURES'} ({time.time() - t0:.1f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
