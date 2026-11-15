"""Check that a XenosRecomp clone is upstream XenosRecomp + our patch series, nothing else.

The game build makes its translator sources itself (tools/xenosrecomp/prepare.py: the
third_party/XenosRecomp submodule's pinned commit + the patch series). A clone is still what the
offline tool is built from (build_xenos.bat) and what SR_XENOSRECOMP_DIR may point the game build at.
Both must be exactly upstream (the pinned 990d03b) + the patches in prepare.PATCHES (superman.patch,
0002-iter463-setp-and-texture-lod.patch, 0003-iter464-resscale-pixel-position.patch - the
resolution-scaled pixel position; docs/RESOLUTION.md); a local edit to the clone that is not in the
patch series would make the runtime translation and a regenerated offline cache disagree with what
the repo documents.

It extracts XenosRecomp/ at the upstream commit (git archive, from the clone's own history), applies
the patch series with `git apply`, and compares every file with the clone's working tree ignoring
line endings (the clone's working tree is CRLF, the patches LF). Also prints the dxc-bin submodule
commit and the SHA-1 of its DXC DLLs (the game build ships the byte-identical official release;
cmake/dxc_release.cmake). `python tools/xenosrecomp/prepare.py --out DIR --compare <clone>` compares
the build's sources with a clone byte for byte, line endings included.

    python tools/xenosrecomp/verify_clone.py [--clone DIR]   # exit 0 = identical (default tools/XenosRecomp-clone)
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from prepare import PATCHES, pin  # noqa: E402  (one patch list for the build and this check)


def norm(b):
    return b.replace(b"\r\n", b"\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--clone", default=os.path.join(REPO, "tools", "XenosRecomp-clone"))
    CLONE = os.path.abspath(ap.parse_args().clone)
    UPSTREAM = pin()[:7]
    if not os.path.isdir(os.path.join(CLONE, ".git")) and not os.path.isfile(os.path.join(CLONE, ".git")):
        print("no clone at %s (see tools/xenosrecomp/README.md)" % CLONE)
        return 2
    tmp = tempfile.mkdtemp(prefix="xr_verify_")
    try:
        tar = subprocess.run(["git", "-C", CLONE, "archive", UPSTREAM, "XenosRecomp"], capture_output=True)
        if tar.returncode != 0:
            print("git archive %s failed: %s" % (UPSTREAM, tar.stderr.decode(errors="replace")))
            return 2
        tarfile.open(fileobj=io.BytesIO(tar.stdout)).extractall(tmp)
        # Applied as extracted: with this machine's core.autocrlf=true the archive is CRLF and
        # git apply matches the LF patch context against it (an LF-normalised tree does not).
        for pa in PATCHES:
            r = subprocess.run(["git", "apply", "--whitespace=nowarn", os.path.join(HERE, pa)], cwd=tmp,
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("patch %s does not apply to upstream %s: %s" % (pa, UPSTREAM, r.stderr.strip()))
                return 1
        bad = []
        names = set()
        for root, _, files in os.walk(os.path.join(tmp, "XenosRecomp")):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), tmp)
                names.add(rel)
                mine = os.path.join(CLONE, rel)
                if not os.path.exists(mine):
                    bad.append("%s missing in the clone" % rel)
                elif norm(open(mine, "rb").read()) != norm(open(os.path.join(root, f), "rb").read()):
                    bad.append("%s differs from upstream + patches" % rel)
        for root, _, files in os.walk(os.path.join(CLONE, "XenosRecomp")):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), CLONE)
                if rel not in names:
                    bad.append("%s is in the clone but not upstream + patches" % rel)
        sub = subprocess.run(["git", "-C", CLONE, "submodule", "status", "thirdparty/dxc-bin"], capture_output=True,
                             text=True).stdout.strip()
        print("upstream %s + %s" % (UPSTREAM, " + ".join(PATCHES)))
        print("dxc-bin submodule: %s" % sub)
        for dll in ("dxcompiler.dll", "dxil.dll"):
            p = os.path.join(CLONE, "thirdparty", "dxc-bin", "bin", "x64", dll)
            if os.path.exists(p):
                print("  %s sha1 %s" % (dll, hashlib.sha1(open(p, "rb").read()).hexdigest()))
        if bad:
            print("CLONE DIFFERS (%d):" % len(bad))
            for b in bad:
                print("  " + b)
            return 1
        print("clone == upstream + patch series (%d files, line endings ignored)" % len(names))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
