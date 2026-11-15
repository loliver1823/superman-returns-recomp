#!/usr/bin/env python3
"""relink_exe.py - link SupermanReturns.exe against an ALTERNATE guest library, with the
host objects of the current build untouched (iter-464 codegen lane).

Why: a guest-codegen A/B (register localization, compiler flags, macro changes) needs two
exes that differ ONLY in the guest code. A full rebuild of the other variant costs 20+ min
of guest compile; the link alone costs ~1 min. Keep the other variant's
build/guest/SupermanReturnsGuest.lib (archive/...) and relink.

    python tools/regen/relink_exe.py --lib archive/v1_codegen/base/SupermanReturnsGuest.lib \
        --out archive/exe_snapshots/codegen-base-relink
Prints the exe path. The output dir gets the exe, its pdb and the runtime DLLs next to it
(like tools/snapshot_exe.ps1), so run_recomp_x.ps1 / sr_drive.ps1 -ExePath can use it.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# SDL3.dll (RELEASE_PLAN step 3); SDL2.dll only in a pre-SDL3 build tree. Each is copied when present.
DLLS = ("SDL3.dll", "SDL2.dll", "dxcompiler.dll", "dxil.dll", "ffmpeg_xenia.dll")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True, help="the guest static library to link instead of build/guest/SupermanReturnsGuest.lib")
    ap.add_argument("--out", required=True, help="output directory (created)")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    lib = os.path.abspath(a.lib).replace("\\", "/")
    lines = open(os.path.join(build, "build.ninja"), encoding="utf-8").read().split("\n")
    i = next(k for k, l in enumerate(lines) if l.startswith("build SupermanReturns.exe:"))
    objs = [o for o in lines[i].split(":", 1)[1].split("|")[0].split()[1:]]
    flags = {}
    for l in lines[i + 1:i + 14]:
        m = re.match(r"\s+(\w+) = (.*)", l)
        if m:
            flags[m.group(1)] = m.group(2)
    # the link rule (CMakeFiles/rules.ninja): "<driver> -nostartfiles -nostdlib $FLAGS
    # $LINK_FLAGS $in -o $TARGET_FILE -Xlinker /implib:.. -Xlinker /pdb:.. $LINK_PATH $LINK_LIBRARIES"
    rules = open(os.path.join(build, "CMakeFiles", "rules.ninja"), encoding="utf-8").read().split("\n")
    j = next(k for k, l in enumerate(rules) if l.startswith("rule CXX_EXECUTABLE_LINKER__SupermanReturns_"))
    rule = rules[j + 1]
    m = re.search(r"&& (\S+) -nostartfiles -nostdlib", rule)
    if not m:
        raise SystemExit("unexpected link rule: " + rule[:200])
    driver = m.group(1)
    libs = flags["LINK_LIBRARIES"].replace("guest/SupermanReturnsGuest.lib", '"%s"' % lib)
    exe = os.path.join(out, "SupermanReturns.exe").replace("\\", "/")
    pdb = os.path.join(out, "SupermanReturns.pdb").replace("\\", "/")
    implib = os.path.join(out, "SupermanReturns.lib").replace("\\", "/")
    cmd = "%s -nostartfiles -nostdlib %s %s %s -o \"%s\" -Xlinker /MANIFEST:EMBED -Xlinker /implib:\"%s\" -Xlinker /pdb:\"%s\" -Xlinker /version:0.0 %s %s" % (
        driver, flags["FLAGS"], flags["LINK_FLAGS"], " ".join(objs), exe, implib, pdb,
        flags.get("LINK_PATH", ""), libs)
    cmdf = os.path.join(out, "relink.cmd")
    open(cmdf, "w").write(cmd + "\n")
    r = subprocess.run(["cmd", "/c", cmdf], cwd=build, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write((r.stdout + r.stderr)[-3000:] + "\n")
        raise SystemExit("link failed rc=%d" % r.returncode)
    for d in DLLS:
        src = os.path.join(build, d)
        if os.path.exists(src):
            shutil.copy2(src, out)
    print(exe)


if __name__ == "__main__":
    main()
