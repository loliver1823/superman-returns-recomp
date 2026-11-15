#!/usr/bin/env python3
"""rt_test.py - regression test for the Apt container/movie toolchain (tools/apt/).

Checks, all against the user's own read-only game files:
  1. ui.AST outer TOC ids == FNV-1a-64(UPPER name) for every entry
  2. ui.AST rebuilt by bgfa.Bgfa.rebuild() is byte-identical to the original
  3. every nested per-movie BGFA container rebuilds byte-identical
  4. text.AST rebuilds byte-identical
  5. every movie (.apt/.const) parses with 0 uncovered non-zero bytes (apt_model)
  6. every movie re-serialises byte-exact through apt_write.relayout (identity)
  7. every movie relayed out with 0x40 filler after the magic block (every block and
     pointer moves) re-parses to the same logical movie (apt_asm.logical_view), with
     0 uncovered non-zero bytes and length +0x40
Exit status 0 only when every check passes.

usage (from the repo root):
  python tools/apt/rt_test.py [ui.AST] [text.AST]
"""
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bgfa import Bgfa, DEFAULT_UI, DEFAULT_TEXT, fnv1a64  # noqa: E402
from apt_model import Apt, coverage  # noqa: E402
from apt_asm import logical_view  # noqa: E402
from apt_write import relayout  # noqa: E402

SHIFT = 0x40


def main(argv):
    ui_path = argv[0] if len(argv) > 0 else DEFAULT_UI
    text_path = argv[1] if len(argv) > 1 else DEFAULT_TEXT
    t0 = time.time()
    ok_all = True

    def report(label, good, total, extra=""):
        nonlocal ok_all
        passed = good == total and total > 0
        ok_all &= passed
        print(f"[{'PASS' if passed else 'FAIL'}] {label}: {good}/{total}{extra}")

    ui = open(ui_path, "rb").read()
    outer = Bgfa(ui)

    # 1-2. outer container
    ids = sum(1 for e in outer.entries if int.from_bytes(e["id"], "little") == fnv1a64(e["name"]))
    report("ui.AST TOC ids == FNV-1a-64(UPPER name)", ids, len(outer.entries))
    same = outer.rebuild() == ui
    report(f"ui.AST bgfa rebuild byte-identical ({len(ui)} B)", int(same), 1)

    # 3. nested containers
    nested = [e for e in outer.entries if e["data"][:8] == b"BGFA1.05"]
    n_ok = sum(1 for e in nested if Bgfa(e["data"]).rebuild() == e["data"])
    report("nested BGFA containers rebuild byte-identical", n_ok, len(nested))

    # 4. text.AST
    if os.path.exists(text_path):
        tx = open(text_path, "rb").read()
        report(f"text.AST bgfa rebuild byte-identical ({len(tx)} B)", int(Bgfa(tx).rebuild() == tx), 1)
    else:
        report(f"text.AST present at {text_path}", 0, 1)

    # 5-7. movies
    n = cov_ok = ident = shifted = 0
    mid = collections.Counter()
    fails = []
    for e in nested:
        inner = Bgfa(e["data"])
        apts = [x["data"] for x in inner.entries if x["tag"] == 0x264923D5]
        csts = [x["data"] for x in inner.entries if x["tag"] == 0x264923D6]
        if not apts:
            continue
        name, apt, cst = e["name"], apts[0], csts[0]
        n += 1
        try:
            a = Apt(apt, cst)
            starts = {o for o, s, k in a.blocks}
            for f, t, k in a.relocs:
                if t not in starts:
                    mid[k] += 1
            _, _, _, nz = coverage(a)
            if not nz:
                cov_ok += 1
            else:
                fails.append((name, "coverage", len(nz), nz[:3]))
            b2, c2, _ = relayout(a)
            if b2 == apt and c2 == cst:
                ident += 1
            else:
                d = [i for i in range(min(len(b2), len(apt))) if b2[i] != apt[i]][:5]
                fails.append((name, "identity", len(b2), len(apt), d, c2 == cst))
            b3, c3, _ = relayout(a, insert_after=0, filler=bytes(SHIFT))
            a3 = Apt(b3, c3)
            _, _, _, nz3 = coverage(a3)
            if logical_view(a3) == logical_view(a) and not nz3 and len(b3) == len(apt) + SHIFT:
                shifted += 1
            else:
                fails.append((name, "shifted", len(nz3), len(b3) - len(apt)))
        except Exception as ex:  # a crash is a failure of that movie, keep going
            fails.append((name, "exception", repr(ex)))
    report("movies parsed with 0 uncovered non-zero bytes", cov_ok, n)
    report("movies re-serialised byte-exact (relayout identity, .apt AND .const)", ident, n)
    report(f"movies shifted +{SHIFT:#x}, re-parsed, logically equal", shifted, n)
    print(f"info: pointers into block middles (by kind): {dict(mid) or 0}")
    for f in fails[:10]:
        print("  fail:", f)
    print(f"{'ALL PASS' if ok_all else 'FAILURES'} ({time.time() - t0:.1f} s)")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
