#!/usr/bin/env python3
"""build_harness.py - build golden_call_harness.exe (the RECOMP side of the golden-call
differential) against the recompiled guest lib, WITHOUT touching the main exe target.

Compile flags come from the `difftest` target in build/compile_commands.json; the link line
reuses the difftest target's kernel/app objects from build/build.ninja (build them once with
`cmake --build build --target difftest`). The guest lib is COPIED into the work dir first
(--pin, default on) so a concurrent rebuild of build/guest/SupermanReturnsGuest.lib cannot
change the code under test mid-run.

    python scripts/difftest/golden_call/build_harness.py [--work archive/golden_call] [--no-pin]
Prints the exe path. Needs the Ninja + clang toolchain the project build uses.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Strong defs the harness itself replaces (see the link step); any other duplicate is an error.
ALLOWED_DUPLICATES = ("sub_8246CA80",)
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(ROOT, "archive", "golden_call"))
    ap.add_argument("--no-pin", action="store_true")
    a = ap.parse_args()
    work = os.path.abspath(a.work).replace("\\", "/")
    os.makedirs(work, exist_ok=True)
    build = os.path.join(ROOT, "build")
    lib = os.path.join(build, "guest", "SupermanReturnsGuest.lib")
    if not a.no_pin:
        pinned = os.path.join(work, "SupermanReturnsGuest.lib")
        if not os.path.exists(pinned) or os.path.getmtime(pinned) < os.path.getmtime(lib):
            shutil.copy2(lib, pinned)
        lib = pinned
    lib = lib.replace("\\", "/")
    cc = json.load(open(os.path.join(build, "compile_commands.json")))
    comp = next(e["command"] for e in cc if e["file"].replace("\\", "/").endswith("tools/difftest/kpcr_test.cpp"))
    src = os.path.join(HERE, "golden_call_harness.cpp").replace("\\", "/")
    obj = work + "/golden_call_harness.obj"
    exe = work + "/golden_call_harness.exe"
    comp = re.sub(r"-o \S+", "-o " + obj, comp)
    comp = re.sub(r"-c \S+", "-c " + src, comp).replace("\\", "/")
    lines = open(os.path.join(build, "build.ninja"), encoding="utf-8").read().split("\n")
    i = next(k for k, l in enumerate(lines) if l.startswith("build difftest.exe:"))
    objs = [o for o in lines[i].split(":", 1)[1].split("|")[0].split()[1:] if "tools/difftest/" not in o]
    flags = {}
    for l in lines[i + 1:i + 12]:
        m = re.match(r"\s+(\w+) = (.*)", l)
        if m:
            flags[m.group(1)] = m.group(2)
    libs = flags["LINK_LIBRARIES"].replace("guest/SupermanReturnsGuest.lib", lib)
    # iter-467: the harness's HEAP command strong-defines RtlAllocateHeap (sub_8246CA80), which
    # guest/ppc_import_glue.cpp strong-defines too (outside forcing, its body is the game's
    # allocator, which the harness's definition runs whenever no HEAP is active). /force:multiple
    # keeps the FIRST definition - the harness object comes first on the link line - and every
    # other duplicate still fails the build (ALLOWED_DUPLICATES below).
    link = "%s %s %s -Xlinker /force:multiple %s %s -o %s %s %s" % (
        comp.split()[0], flags["FLAGS"], flags["LINK_FLAGS"], obj, " ".join(objs), exe,
        flags.get("LINK_PATH", ""), libs)
    for cmd in (comp, link):
        cmdf = os.path.join(work, "build_step.cmd")
        open(cmdf, "w").write(cmd + "\n")
        r = subprocess.run(["cmd", "/c", cmdf], cwd=build, capture_output=True, text=True)
        out = (r.stdout + r.stderr).splitlines()
        errs = [l for l in out if " error" in l.lower() or "error:" in l.lower()]
        errs += ["unexpected " + l for l in out
                 if "duplicate symbol" in l and not any(s in l for s in ALLOWED_DUPLICATES)]
        if r.returncode or errs:
            sys.stderr.write("\n".join(errs[:30]) + "\n")
            raise SystemExit("build failed rc=%d" % r.returncode)
    print(exe)


if __name__ == "__main__":
    main()
