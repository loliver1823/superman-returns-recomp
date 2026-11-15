#!/usr/bin/env python3
"""oracle_objcmp.py - does a change leave the RELEASE configuration's code of the shipping units it touched unchanged?
(C1, docs/RELEASE_COMPOSITION.md; written for mem.resources step 2a, whose census hooks must compile to nothing under
SR_ORACLE=0.) Each translation unit is compiled twice with its own dev-build command (<build>/compile_commands.json, the
SupermanReturns object) plus -DSR_ORACLE=0 - the release configuration's switch - once with the tree's sources, once with
the base commit's versions of every touched file put in place (restored byte for byte afterwards, checked); the two
objects' disassembly (llvm-objdump -d, addresses dropped) is compared per unit. IDENTICAL = the release code of that unit
is the base's; DIFFERENT lists the functions whose code moved. A cheap stand-in for a release build when only a few
units changed (no release tree, no disk); it does not replace the release audit (tools/player/composition_audit.py).

    python tools/player/oracle_objcmp.py --tree <checkout with a configured dev build> --base <commit>
           --tus gfx/a.cpp,kernel/b.cpp [--swap gfx/x.inc,...] [--build-dir build] [--out <dir>]
           [--objdump <llvm-objdump>]

--tus: the shipping units to compare (each needs a SupermanReturns entry in compile_commands.json); --swap: other touched
files those units include (headers, .inc), put back to the base too. Run where the tree is built (the cloud worker).
Exit code = the units that differ or failed + files not restored.
"""
import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys

SYM = re.compile(r"<(\?[^+>]+|[A-Za-z_][^+>]*)")


def compile_cmd(entry, obj):
    args = entry.get("arguments") or shlex.split(entry["command"], posix=False)
    args = [x.strip('"') for x in args]
    out, skip = [], False
    for x in args:
        if skip:
            skip = False
            continue
        if x == "-o":
            skip = True
            continue
        out.append(x)
    return out + ["-DSR_ORACLE=0", "-o", obj]


def disasm(objdump, obj):
    d = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--no-leading-addr", obj], capture_output=True,
                       text=True).stdout
    return [re.sub(r"\b[0-9a-f]{6,16}\b", "ADDR", ln) for ln in d.splitlines()[2:]]


def functions_of(lines):
    """{symbol: [instruction lines]} - a symbol label line '<sym>:' opens a function."""
    cur, out = None, collections.OrderedDict()
    for ln in lines:
        m = re.match(r"^<([^>]+)>:$", ln.strip())
        if m:
            cur = m.group(1)
            out.setdefault(cur, [])
            continue
        if cur is not None:
            out[cur].append(re.sub(r"\+0x[0-9a-f]+>", "+OFF>", ln))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=".")
    ap.add_argument("--base", required=True)
    ap.add_argument("--tus", required=True)
    ap.add_argument("--swap", default="")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--out", default="")
    ap.add_argument("--objdump", default=r"C:\Program Files\LLVM\bin\llvm-objdump.exe" if os.name == "nt" else "llvm-objdump")
    a = ap.parse_args(argv)
    tree = os.path.abspath(a.tree)
    tus = [t for t in a.tus.split(",") if t]
    swap = tus + [s for s in a.swap.split(",") if s and s not in tus]
    out_dir = a.out or os.path.join(tree, "archive", "oracle_objcmp")
    cc = json.load(open(os.path.join(tree, a.build_dir, "compile_commands.json"), encoding="utf-8"))
    cmds = {}
    for e in cc:
        f = os.path.normcase(os.path.normpath(e["file"]))
        for tu in tus:
            if f.endswith(os.path.normcase(os.path.normpath(tu))) and "SupermanReturns.dir" in e.get("output", e["command"]):
                cmds[tu] = e
    missing = [t for t in tus if t not in cmds]
    if missing:
        print("no SupermanReturns compile command for %s" % missing)
        return 2
    saved, base = {}, {}
    for f in swap:
        with open(os.path.join(tree, f), "rb") as fh:
            saved[f] = fh.read()
        base[f] = subprocess.run(["git", "-C", tree, "show", "%s:%s" % (a.base, f)], capture_output=True,
                                 check=True).stdout

    def compile_all(mode):
        od = os.path.join(out_dir, mode)
        os.makedirs(od, exist_ok=True)
        res = {}
        for tu in tus:
            obj = os.path.join(od, tu.replace("/", "__") + ".obj")
            p = subprocess.run(compile_cmd(cmds[tu], obj), cwd=cmds[tu].get("directory", tree), capture_output=True,
                               text=True)
            if p.returncode:
                print("%s %s: compile failed (%d): %s" % (mode, tu, p.returncode, (p.stderr or p.stdout)[-600:]))
                res[tu] = None
                continue
            lines = disasm(a.objdump, obj)
            with open(obj + ".dis", "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines))
            res[tu] = lines
        return res

    lane = compile_all("lane")
    try:
        for f in swap:
            with open(os.path.join(tree, f), "wb") as fh:
                fh.write(base[f])
        based = compile_all("base")
    finally:
        for f in swap:
            with open(os.path.join(tree, f), "wb") as fh:
                fh.write(saved[f])
    bad = 0
    for f in swap:
        with open(os.path.join(tree, f), "rb") as fh:
            if fh.read() != saved[f]:
                print("NOT RESTORED: %s" % f)
                bad += 1
    for tu in tus:
        x, y = lane.get(tu), based.get(tu)
        if x is None or y is None:
            print("%-36s ERROR (a compile failed)" % tu)
            bad += 1
            continue
        if x == y:
            print("%-36s IDENTICAL machine code under SR_ORACLE=0 (%d lines)" % (tu, len(x)))
            continue
        fx, fy = functions_of(x), functions_of(y)
        moved = [s for s in set(fx) | set(fy) if fx.get(s) != fy.get(s)]
        print("%-36s DIFFERENT under SR_ORACLE=0 (%d vs %d lines): %d function(s) - %s" % (
            tu, len(y), len(x), len(moved), ", ".join(sorted(moved)[:6])))
        bad += 1
    print("oracle_objcmp: %s" % ("every unit compiles to the base's code under SR_ORACLE=0" if not bad else
                                 "%d unit(s) differ or failed" % bad))
    return bad


if __name__ == "__main__":
    sys.exit(main())
