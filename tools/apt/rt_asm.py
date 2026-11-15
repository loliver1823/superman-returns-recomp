#!/usr/bin/env python3
"""rt_asm.py - regression test for the text assembler (aptasm.py) and Graph.gc().

For every bytecode block of every movie in the user's own read-only ui.AST:
  1. canonical: disassemble (aptasm.render) -> parse -> the parsed values equal the
     block's values, and bind() -> Graph.emit() of the whole movie is BYTE-EXACT (.apt
     AND .const). bind() writes every assembled operand (strings, .const items, index
     arrays, params, branch targets, function ends, immediates) back into the block's
     own storage, so a wrong value anywhere is a byte difference.
  2. apt2txt syntax: the apt_asm.listing() lines apt2txt prints (address column,
     numeric branch offsets, DefineFunction w=[...] sizes) assemble the same way,
     byte-exact.
  3. fresh: every block re-assembled with materialize_new() (fresh strings, arrays and
     .const items, the path new mod code takes), then gc() drops the old storage; the
     emitted movie re-parses with 0 uncovered non-zero bytes and every block
     re-disassembles to the same canonical text.
  4. gc() on an unedited movie removes nothing and the emit stays byte-exact.
  5. sensitivity: single-token edits of one real block's text (a register, function
     flags, a Push register, a branch target, a pool string, an immediate) each change
     the bytes or are rejected by the assembler.
Exit status 0 only when every check passes.

usage (from the repo root):
  python tools/apt/rt_asm.py [ui.AST]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bgfa import movies, movie, DEFAULT_UI  # noqa: E402
from apt_model import Apt, coverage  # noqa: E402
from apt_asm import listing  # noqa: E402
from apt_graph import from_apt  # noqa: E402
import aptasm  # noqa: E402


def tail_of(a):
    return len(a.b) - max(o + s for o, s, k in a.blocks)


def same_values(x, y):
    return len(x) == len(y) and all(p.key() == q.key() for p, q in zip(x, y))


def code_keys(g):
    return [k for k in g.order if g.blocks[k].code is not None]


def check_canonical(a, apt, cst, stats):
    g = from_apt(a)
    ok = True
    for k in code_keys(g):
        stats["blocks"] += 1
        ref = aptasm.lower(g, k)
        P = aptasm.parse(aptasm.render(ref))
        if same_values(P.ins, ref):
            stats["values"] += 1
        else:
            ok = False
        g.blocks[k].code = aptasm.bind(P.ins, g, k)
    b, c, _ = g.emit(orig_tail=tail_of(a))
    return ok and b == apt and c == cst


def check_apt2txt(a, apt, cst, stats):
    g = from_apt(a)
    ref_g = from_apt(a)
    ok = True
    for k in code_keys(g):
        text = "\n".join(listing(a, k))
        P = aptasm.parse(text)
        if same_values(P.ins, aptasm.lower(ref_g, k)):
            stats["values2"] += 1
        else:
            ok = False
        g.blocks[k].code = aptasm.bind(P.ins, g, k)
    b, c, _ = g.emit(orig_tail=tail_of(a))
    return ok and b == apt and c == cst


def check_fresh(a, apt, cst, stats):
    g = from_apt(a)
    keys = code_keys(g)
    texts = [aptasm.render(aptasm.lower(g, k)) for k in keys]
    nitems = len(g.items)
    for k, t in zip(keys, texts):
        g.blocks[k].code = aptasm.materialize_new(aptasm.parse(t).ins, g, k)
    removed, dropped = g.gc()
    stats["gc_blocks"] += len(removed)
    stats["gc_items"] += dropped
    b, c, _ = g.emit(orig_tail=tail_of(a))
    a2 = Apt(b, c)
    nz = coverage(a2)[3]
    g2 = from_apt(a2)
    texts2 = [aptasm.render(aptasm.lower(g2, k)) for k in code_keys(g2)]
    if c == cst:
        stats["fresh_const_identical"] += 1
    return not nz and texts2 == texts and len(g2.items) == nitems


def check_gc_identity(a, apt, cst):
    g = from_apt(a)
    removed, dropped = g.gc()
    b, c, _ = g.emit(orig_tail=tail_of(a))
    return not removed and not dropped and b == apt and c == cst


def sensitivity(ui_path):
    """mutate one token of SCRPRESSSTART's ScrPressStart class block (bytecode 0x41e4)."""
    _, apt, cst = movie("SCRPRESSSTART", ui_path)
    a = Apt(apt, cst)
    key = 0x41E4
    base = aptasm.render(aptasm.lower(from_apt(a), key))
    muts = [("StoreRegister 1", "StoreRegister 2"),
            ("flags=0x30129", "flags=0x30128"),
            ("Push [reg1]", "Push [reg3]"),
            ("If L12", "If L11"),
            ("'ScrPressStart::InitGui()'", "'ScrPressStart::InitGuI()'"),   # every occurrence
            ("EA_PushByte 2", "EA_PushByte 3"),
            ("pool[18]='mSlots'", "pool[17]='_visible'"),
            ("op_74", "op_73")]
    caught = 0
    for old, new in muts:
        assert old in base, old
        text = base.replace(old, new) if old.startswith("'") else base.replace(old, new, 1)
        g = from_apt(a)
        try:
            g.blocks[key].code = aptasm.bind(aptasm.parse(text).ins, g, key)
            b, c, _ = g.emit(orig_tail=tail_of(a))
            caught += (b != apt or c != cst)
        except aptasm.AsmError:
            caught += 1
    # the assembler must reject these
    bad = [("undefined label", "    Jump L9\n    End\n"),
           ("End mid-block", "    End\n    Pop\n    End\n"),
           ("no End", "    Pop\n"),
           ("pool check", "    ConstantPool ['a']\n    EA_PushConstByte? pool[0]='b'\n    End\n"),
           ("raw offset mid-instruction", "    Jump 1\n    Push [reg1]\n    End\n"),
           (".func2 without flags", "    .func2 ''()\n    .end\n    End\n"),
           ("unclosed .func", "    .func ''()\n    End\n"),
           ("byte pool index > 255", "    ConstantPool [" + ", ".join(f"'s{i}'" for i in range(300))
            + "]\n    EA_GetNamedMember pool[299]\n    End\n")]
    rejected = 0
    for name, text in bad:
        try:
            aptasm.parse(text)
        except aptasm.AsmError:
            rejected += 1
    return caught, len(muts), rejected, len(bad)


def main(argv):
    ui_path = argv[0] if argv else DEFAULT_UI
    t0 = time.time()
    stats = {"blocks": 0, "values": 0, "values2": 0, "gc_blocks": 0, "gc_items": 0, "fresh_const_identical": 0}
    n = canon = a2t = fresh = gcid = with_code = 0
    fails = []
    for name, inner, apt, cst in movies(ui_path):
        if apt is None:
            continue
        n += 1
        a = Apt(apt, cst)
        with_code += bool(a.bytecode)
        for label, fn in (("canonical", check_canonical), ("apt2txt", check_apt2txt), ("fresh", check_fresh)):
            try:
                good = fn(a, apt, cst, stats)
            except Exception as ex:  # a crash is a failure of that movie, keep going
                good = False
                fails.append((name, label, repr(ex)[:200]))
            else:
                if not good:
                    fails.append((name, label))
            if good:
                if label == "canonical":
                    canon += 1
                elif label == "apt2txt":
                    a2t += 1
                else:
                    fresh += 1
        try:
            gcid += check_gc_identity(a, apt, cst)
        except Exception as ex:
            fails.append((name, "gc", repr(ex)[:200]))
    caught, nm, rejected, nb = sensitivity(ui_path)
    nb_ = stats["blocks"]
    res = [
        (canon == n, f"canonical .aptasm -> assemble -> movie byte-exact (.apt AND .const): {canon}/{n} movies"),
        (stats["values"] == nb_, f"canonical text re-parses to the block's exact values: {stats['values']}/{nb_} blocks "
                                 f"(the bytecode of {with_code} movies; {n - with_code} movies have none)"),
        (a2t == n, f"apt2txt listing syntax -> assemble -> movie byte-exact: {a2t}/{n} movies"),
        (stats["values2"] == nb_, f"apt2txt listing re-parses to the block's exact values: {stats['values2']}/{nb_} blocks"),
        (fresh == n, f"fresh re-assembly of every block + gc(), re-parsed, same text, 0 uncovered: {fresh}/{n} movies"),
        (gcid == n, f"gc() on an unedited movie removes nothing, emit byte-exact: {gcid}/{n} movies"),
        (caught == nm, f"single-token mutations of a real block change the bytes or are rejected: {caught}/{nm}"),
        (rejected == nb, f"malformed sources rejected by the assembler: {rejected}/{nb}"),
    ]
    ok = n > 0 and all(r for r, _ in res)
    for r, text in res:
        print(f"[{'PASS' if r else 'FAIL'}] {text}")
    print(f"info: fresh path dropped {stats['gc_blocks']} old blocks + {stats['gc_items']} old .const items via gc(); "
          f".const byte-identical after fresh re-assembly: {stats['fresh_const_identical']}/{n}")
    for f in fails[:10]:
        print("  fail:", f)
    print(f"{'ALL PASS' if ok else 'FAILURES'} ({time.time() - t0:.1f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
