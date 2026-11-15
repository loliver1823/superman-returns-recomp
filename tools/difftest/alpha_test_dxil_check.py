#!/usr/bin/env python3
"""alpha_test_dxil_check.py - step 4 lane C round 4 (Astra on 0005, P2): the COMPILED alpha-test epilogue holds no
float compare on the alpha reference.

DXC emits every float compare with the `fast` flag (fcmp fast une / olt / oeq / ogt in the 0005 variants), and
fast-math lets the compiler and the driver assume no NaN - so Xenia's unordered NOT_EQUAL is not guaranteed by a
float `!=`. tools/xenosrecomp/0006 compares in integer arithmetic on the float bits instead; this check proves it on
the bytes the game ships (the CPU model gfx/alpha_test.hpp passesBits() cannot see the compiler).

For every <hash>.ps.at.dxil in the cache dir: `dxc -dumpbin`, find the SharedConstants cbuffer (register b2,
space4: "cb2,space4" in the resource bindings) and its handles, the values read from its row 17 (c17.x =
g_AlphaThreshold = RB_ALPHA_REF, c17.y = g_AlphaTest), taint every value computed from c17.x (to a fixpoint, through
phis), and FAIL a variant where an `fcmp` has a tainted operand - or that does not read both c17.x and c17.y (no
epilogue). The 0005 files are the mutant: run with --expect-fail on them, every variant must fail.

    python tools/difftest/alpha_test_dxil_check.py [--dir data/shader_cache/dxil] [--dxc <dxc.exe>] [--expect-fail]
Exit 0 = every variant passes (or, with --expect-fail, every variant fails)."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

RE_BIND = re.compile(r"^;.*\bcbuffer\s+NA\s+NA\s+CB(\d+)\s+cb2,space4\b")
RE_HANDLE = re.compile(r"^\s*(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+), ")
RE_LOAD = re.compile(r"^\s*(%\d+) = call %dx\.types\.CBufRet\.\w+ @dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, "
                     r"%dx\.types\.Handle (%\d+), i32 17\)")
RE_EXTRACT = re.compile(r"^\s*(%\d+) = extractvalue %dx\.types\.CBufRet\.\w+ (%\d+), (\d+)")
RE_DEF = re.compile(r"^\s*(%\d+) = (.*)$")
RE_SSA = re.compile(r"%\d+")


def find_dxc():
    for p in (os.environ.get("SR_DXC_EXE", ""),
              os.path.join(REPO, "tools", "XenosRecomp-clone", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe"),
              os.path.join(os.environ.get("SR_MAIN_ROOT", ""), "tools", "XenosRecomp-clone", "thirdparty", "dxc-bin",
                           "bin", "x64", "dxc.exe") if os.environ.get("SR_MAIN_ROOT") else "",
              "C:/sr/deps/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe"):
        if p and os.path.isfile(p):
            return p
    return None


def analyse(text):
    """-> (ok, reason) for one disassembled variant."""
    lines = text.splitlines()
    if "@dx.op.storeOutput" not in text and "@dx.op.discard" not in text:
        # A translation whose body writes nothing (DXC reduced it to `ret void`): no colour, so no test to make.
        return True, "trivial (no output, no discard: nothing to test)"
    cbs = {m.group(1) for m in (RE_BIND.match(l) for l in lines) if m}
    if not cbs:
        return False, "no SharedConstants cbuffer (cb2,space4) bound"
    handles = {m.group(1) for m in (RE_HANDLE.match(l) for l in lines) if m and m.group(2) in cbs}
    loads = {m.group(1) for m in (RE_LOAD.match(l) for l in lines) if m and m.group(2) in handles}
    ref, word = set(), set()
    for l in lines:
        m = RE_EXTRACT.match(l)
        if m and m.group(2) in loads:
            (ref if m.group(3) == "0" else word if m.group(3) == "1" else set()).add(m.group(1))
    if not ref or not word:
        return False, "the variant reads %s of c17.x / c17.y (no epilogue?)" % (
            "neither" if not ref and not word else "only c17.y" if not ref else "only c17.x")
    defs = []
    for l in lines:
        m = RE_DEF.match(l)
        if m:
            defs.append((m.group(1), m.group(2), set(RE_SSA.findall(m.group(2)))))
    taint = set(ref)
    changed = True
    while changed:
        changed = False
        for name, _, ops in defs:
            if name not in taint and ops & taint:
                taint.add(name)
                changed = True
    bad = [rhs for name, rhs, ops in defs if rhs.startswith("fcmp ") and ops & taint]
    if bad:
        return False, "%d float compare(s) on the reference, e.g. `%s`" % (len(bad), bad[0][:90])
    return True, "c17.x read by %d value(s), %d derived, no fcmp on them" % (len(ref), len(taint))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", default=os.path.join(REPO, "data", "shader_cache", "dxil"))
    ap.add_argument("--dxc", default=None)
    ap.add_argument("--expect-fail", action="store_true", help="the mutant run: every variant must FAIL")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    dxc = a.dxc or find_dxc()
    if not dxc:
        print("[AT_DXIL_CHECK] no dxc.exe (--dxc, SR_DXC_EXE, or the XenosRecomp clone's thirdparty/dxc-bin)")
        return 2
    files = sorted(f for f in os.listdir(a.dir) if f.endswith(".ps.at.dxil"))
    if not files:
        print("[AT_DXIL_CHECK] no .ps.at.dxil in %s" % a.dir)
        return 2
    n_ok = n_trivial = 0
    fails = []
    for f in files:
        p = subprocess.run([dxc, "-dumpbin", os.path.join(a.dir, f)], capture_output=True, text=True,
                           errors="replace")
        if p.returncode != 0:
            fails.append((f, "dxc -dumpbin failed: %s" % (p.stderr.strip()[:120])))
            continue
        ok, why = analyse(p.stdout)
        if ok and why.startswith("trivial"):
            n_trivial += 1
        elif ok:
            n_ok += 1
            if a.verbose:
                print("  ok   %s: %s" % (f, why))
        else:
            fails.append((f, why))
    for f, why in fails[:12]:
        print("  FAIL %s: %s" % (f, why))
    print("[AT_DXIL_CHECK] %s: %d variants, %d pass, %d trivial (no output: nothing to test), %d fail (dxc %s)"
          % (a.dir, len(files), n_ok, n_trivial, len(fails), dxc))
    if a.expect_fail:
        good = n_ok == 0 and len(fails) > 0
        print("[AT_DXIL_CHECK] mutant run: %s" % ("every variant FAILS - the check sees the float compare (PASS)"
                                               if good else "%d variant(s) passed - the check is blind (FAIL)" % n_ok))
        return 0 if good else 1
    print("[AT_DXIL_CHECK] RESULT %s" % ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
