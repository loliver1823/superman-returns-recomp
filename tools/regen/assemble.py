#!/usr/bin/env python3
"""assemble.py - turn the XenonRecomp output into the deployed guest/ppc tree (release lane R1).

Run by tools/regen/regen.py; takes SR_REGEN_IN (XenonRecomp output) and writes SR_REGEN_OUT:

  1. every ppc_recomp.N.cpp as generated, after a CHECK (it edits nothing): no raw guest-memory
     address and no unguarded integer division may be left. Patches 0006 / 0007 make XenonRecomp
     emit `base + PPC_GUEST_OFFSET(X)` (= sr::kernel::aliasGuestVA) for the forms that bypass the
     PPC_LOAD / PPC_STORE macros (lwarx/stwcx, lvx/stvx, dcbz, setjmp/longjmp) and the PowerPC
     division guard. Until iter-467 this script and patch_div_guards.py wrote both into the
     output as text; a XenonRecomp update that adds a new raw form fails here instead of
     silently skipping the alias;
  2. the hand-maintained headers from tools/regen/hand/ (ppc_config.h, ppc_context.h) and the
     (now empty) hand stub file ppc_missing_recomp_stubs.cpp;
  3. ppc_func_mapping.cpp and ppc_recomp_shared.h as generated.

There is no hand-edited recompiled code any more: every behaviour the old spliced bodies added
lives in host code (kernel overrides, tools/regen/superman_regen.toml [[midasm_hook]] entries,
tools/regen/patches) - docs/SPLICE_CONVERSION.md.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
NEW = os.environ.get("SR_REGEN_IN") or os.path.join(ROOT, "tools", "regen", "ppc_out")
OUTD = os.environ.get("SR_REGEN_OUT") or os.path.join(ROOT, "tools", "regen", "assembled")
HAND = os.path.join(HERE, "hand")

# a raw guest address: `(base + X` where X is not PPC_GUEST_OFFSET(...)
RAW_BASE = re.compile(r"\(base \+ (?!PPC_GUEST_OFFSET\()")
# an unguarded division: `ctx.rD.T = ctx.rA.T / ctx.rB.T;` (patch 0006 emits the guarded form)
RAW_DIV = re.compile(r"^\s*ctx\.r\d+\.[us](?:32|64) = ctx\.r\d+\.[us](?:32|64) / ctx\.r\d+\.[us](?:32|64);\s*$")


def check(name, text):
    bad = []
    for i, line in enumerate(text.split("\n"), 1):
        if RAW_BASE.search(line) or RAW_DIV.match(line):
            bad.append(f"{name}:{i}: {line.strip()[:160]}")
    return bad


def main():
    if os.path.isdir(OUTD):
        shutil.rmtree(OUTD)
    os.makedirs(OUTD)
    n, bad = 0, []
    for name in sorted(os.listdir(NEW)):
        if not (name.startswith("ppc_recomp.") and name.endswith(".cpp")):
            continue
        src = os.path.join(NEW, name)
        bad += check(name, open(src, encoding="utf-8").read())
        shutil.copyfile(src, os.path.join(OUTD, name))
        n += 1
    if bad:
        for b in bad[:20]:
            print("assemble: " + b, file=sys.stderr)
        print(f"assemble: {len(bad)} raw guest address(es) / unguarded division(s) in the XenonRecomp "
              f"output - an emitter patches 0006 / 0007 do not cover (tools/regen/patches)", file=sys.stderr)
        return 1
    for name in ("ppc_config.h", "ppc_context.h", "ppc_missing_recomp_stubs.cpp"):
        shutil.copy(os.path.join(HAND, name), os.path.join(OUTD, name))
    for name in ("ppc_func_mapping.cpp", "ppc_recomp_shared.h"):
        shutil.copy(os.path.join(NEW, name), os.path.join(OUTD, name))
    print(f"assemble: {n} cpp files checked (no raw guest address, no unguarded division), "
          f"headers copied -> {OUTD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
