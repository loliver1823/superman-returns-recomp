#!/usr/bin/env python3
"""apt_unload.py - the Apt runtime's UNLOAD renumbering contract, simulated on the bytes.

What the runtime does (IDA, iter-463 quit investigation + skeptic, re-read twice):
  load    AptRegistrar_826ADD40 -> per char of type 5/9 AptSpriteFramesRelocate_sub_826A39A0
          -> AptActionBytecodeRelocate_sub_8269F3A0(bc, blob, const, &counter), a3 != 0:
          ops 0x88 ConstantPool / 0x96 Push: every array slot is read as a .const item
          INDEX (0x8269F5BC: 8*idx + *(const+28)) and overwritten IN PLACE with the live AS
          value built from that item.
  unload  AptMovieUnrelocate_sub_826A3180 (only code caller AptMovieHolderDestroy_sub_826A3880)
          resets the counter (0x826A31A4), then for every char in index order, types 5/9
          only, skipping a NULL slot or any index an import record names (the skip is by
          import-table membership, +8 localCharId: at runtime an imported slot is NOT NULL
          during this loop - the first loop releases it, a later loop at 0x826A36F0 nulls
          it) -> AptSpriteFramesUnrelocate_sub_826A0758:
          frames in order -> items in order (type 1 Action bytecode +4, type 3 PlaceObject
          clip-action records +8 each, type 8 InitAction bytecode +8) -> sub_8269F3A0 with
          a3 == 0: a LINEAR scan to the first End; at 0x88/0x96 it releases each value and
          writes `slot = counter++` (0x8269F6BC). The ORIGINAL index is not restored.
  reload  a later load of the same movie reuses the resident payload (only the ui.AST
          header and TOC are read again) and relocates the counter-written indices.
So a movie survives unload + reload only if every slot's stored index == its position in
this walk and #slots == #items (one item per slot). All 820 retail movies satisfy it; the
iter-463 custom_frontend build broke it in its 5 patched movies (items appended at the end),
and the reloaded ScrPressStart's registerClass pool resolved to [reg2, reg2, reg2]: class
never bound, no onLoad, no InitGui - the QUIT GAME soft-lock.

This module is the INDEPENDENT checker: it reads only bytes (apt_model's operand-size table,
the same switch as the runtime's), never apt_graph, which does the renumbering it checks.
check() asserts, per movie:
  1. index == counter for every slot, and #slots == #items;
  2. every slot is walked: the unload walk's slot fields == apt_model's pointer walker's
     Push/ConstantPool fields (no reachable array the runtime would leave un-renumbered);
  3. with `before` (the resolved constants of the same movie before a renumbering), every
     slot resolves to the same constant as before;
  4. a reload after a simulated unload (every slot := its walk position, exactly what the
     runtime writes) resolves the same constants as the first load (and as `before`).
Constants compare by value: (type, string bytes) for strings, (type, raw word) otherwise
(the load path interns strings by content; string offsets inside the .const may move).

usage (repo root):
  python tools/apt/apt_unload.py [UI.AST ...] [--movie NAME] [--show N]
    default: the user's retail ui.AST and, when present, mods/custom_frontend/DATA/ui.AST;
    prints per-container totals and every failing movie; exit 1 when any movie fails.
"""
import os
import struct
import sys

try:
    from .apt_model import Apt, align4, insn_end
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from apt_model import Apt, align4, insn_end


def _u(b, o):
    return struct.unpack_from(">I", b, o)[0]


def _scan(b, pc, out, owner):
    """sub_8269F3A0's linear scan (a3 == 0): every 0x88/0x96 index slot up to the first End."""
    while True:
        op = b[pc]
        if op == 0:
            return
        if op in (0x88, 0x96):
            a = align4(pc + 1)
            n, arr = _u(b, a), _u(b, a + 4)
            for k in range(n):
                out.append((arr + 4 * k, _u(b, arr + 4 * k), op, owner))
        pc = insn_end(op, pc)


def unload_slots(apt, cst=None, a=None):
    """[(fieldOffset, storedIndex, op, owner)] in the runtime's unload order."""
    a = a or Apt(apt, cst)
    b = a.b
    mv = a.movie
    nchars, tab = _u(b, mv + 0x1C), _u(b, mv + 0x20)
    imported = {imp[2] for imp in a.imports}
    out = []
    for idx in range(nchars):
        c = _u(b, tab + 4 * idx)
        if not c or idx in imported:
            continue
        t = 9 if c == mv else _u(b, c)
        if t not in (5, 9):
            continue
        nfr, fr = _u(b, c + 0x10), _u(b, c + 0x14)
        for f in range(nfr):
            n, arr = _u(b, fr + 8 * f), _u(b, fr + 8 * f + 4)
            for k in range(n):
                it = _u(b, arr + 4 * k)
                ty = _u(b, it)
                if ty == 1:
                    _scan(b, _u(b, it + 4), out, f"char{idx}/fr{f}/action")
                elif ty == 3:
                    ca = _u(b, it + 0x3C)
                    if ca:
                        cnt, recs = _u(b, ca), _u(b, ca + 4)
                        for r in range(cnt):
                            _scan(b, _u(b, recs + 12 * r + 8), out, f"char{idx}/fr{f}/clipevent{r}")
                elif ty == 8:
                    _scan(b, _u(b, it + 8), out, f"char{idx}/fr{f}/initaction(sprite {_u(b, it + 4)})")
    return out


def value(c, i):
    """the constant item i resolves to (None when i is out of range)."""
    if not 0 <= i < c.count:
        return None
    t, v = c.items[i]
    if t == 1 and v:
        return (1, c.strings[v])
    return (t, v)


def resolved(apt, cst):
    """the constant every slot resolves to on a load, in walk order."""
    a = Apt(apt, cst)
    return [value(a.c, s[1]) for s in unload_slots(None, a=a)]


def simulate_unload(apt, cst):
    """the .apt bytes after the runtime's unload: slot k := k, in walk order."""
    b = bytearray(apt)
    for pos, s in enumerate(unload_slots(apt, cst)):
        struct.pack_into(">I", b, s[0], pos)
    return bytes(b)


def check(apt, cst, before=None, show=3):
    """-> (ok, problems[str], stats{slots, items, mismatch}). See the module docstring."""
    a = Apt(apt, cst)
    slots = unload_slots(None, a=a)
    problems = []
    mism = [(pos, s) for pos, s in enumerate(slots) if s[1] != pos]
    if mism:
        problems.append(f"{len(mism)} slot(s) whose index != unload counter, first "
                        + "; ".join(f"pos {p} {s[3]} stored {s[1]} ({value(a.c, s[1])!r:.40})" for p, s in mism[:show]))
    if len(slots) != a.c.count:
        problems.append(f"{len(slots)} slots but {a.c.count} .const items (the counter needs one item per slot)")
    walked = [s[0] for s in slots]
    if len(set(walked)) != len(walked):
        problems.append(f"{len(walked) - len(set(walked))} slot field(s) reached twice by the unload walk")
    refs = {f for f, _ in a.const_refs}
    if set(walked) != refs:
        problems.append(f"unload walk covers {len(set(walked) & refs)}/{len(refs)} Push/ConstantPool slots "
                        f"(+{len(set(walked) - refs)} the pointer walker does not see)")
    first = [value(a.c, s[1]) for s in slots]
    if before is not None:
        diff = [k for k in range(max(len(first), len(before)))
                if k >= len(first) or k >= len(before) or first[k] != before[k]]
        if diff:
            k = diff[0]
            problems.append(f"{len(diff)} slot(s) resolve differently than before the renumbering, first pos {k}: "
                            f"{before[k] if k < len(before) else None!r:.40} -> {first[k] if k < len(first) else None!r:.40}")
    ref = before if before is not None else first
    a2 = Apt(simulate_unload(apt, cst), cst)
    again = [value(a2.c, s[1]) for s in unload_slots(None, a=a2)]
    bad = [k for k in range(max(len(again), len(ref))) if k >= len(again) or k >= len(ref) or again[k] != ref[k]]
    if bad:
        k = bad[0]
        problems.append(f"a reload after unload resolves {len(bad)} slot(s) differently, first pos {k} "
                        f"({slots[k][3] if k < len(slots) else '?'}): {ref[k] if k < len(ref) else None!r:.40} -> "
                        f"{again[k] if k < len(again) else None!r:.40}")
    return not problems, problems, {"slots": len(slots), "items": a.c.count, "mismatch": len(mism)}


def check_container(ui_bytes_or_path, names=None):
    """-> (nMovies, [(name, problems, stats)] failures)."""
    try:
        from .bgfa import Bgfa, TAG_APT, TAG_CONST
    except ImportError:
        from bgfa import Bgfa, TAG_APT, TAG_CONST
    data = ui_bytes_or_path
    if not isinstance(data, (bytes, bytearray)):
        data = open(data, "rb").read()
    n, fails = 0, []
    for e in Bgfa(data).entries:
        if e["data"][:8] != b"BGFA1.05":
            continue
        name = e["name"]
        inner = Bgfa(e["data"]).entries
        apt = next((x["data"] for x in inner if x["tag"] == TAG_APT), None)
        cst = next((x["data"] for x in inner if x["tag"] == TAG_CONST), None)
        if apt is None or (names and name.upper() not in names):
            continue
        n += 1
        ok, problems, st = check(apt, cst)
        if not ok:
            fails.append((name, problems, st))
    return n, fails


def main(argv):
    import argparse
    try:
        from .bgfa import DEFAULT_UI, REPO
    except ImportError:
        from bgfa import DEFAULT_UI, REPO
    ap = argparse.ArgumentParser(description="Check the Apt unload-renumbering invariant per movie.")
    ap.add_argument("ui", nargs="*")
    ap.add_argument("--movie", action="append", help="only this movie (repeatable)")
    args = ap.parse_args(argv)
    uis = args.ui or [DEFAULT_UI] + [p for p in [os.path.join(REPO, "mods", "custom_frontend", "DATA", "ui.AST")]
                                     if os.path.isfile(p)]
    names = {m.upper() for m in args.movie} if args.movie else None
    rc = 0
    for p in uis:
        n, fails = check_container(p, names)
        print(f"[{'PASS' if not fails and n else 'FAIL'}] {p}: unload walk == stored indices, slots == items, "
              f"reload resolves the first load's constants: {n - len(fails)}/{n} movies")
        for name, problems, st in fails:
            print(f"   {name}: slots {st['slots']} items {st['items']} index!=counter {st['mismatch']}")
            for pr in problems:
                print(f"      - {pr}")
        rc |= bool(fails) or not n
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
