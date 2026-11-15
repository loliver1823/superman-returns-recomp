#!/usr/bin/env python3
"""prepare.py - the XenosRecomp sources the game build compiles: the pinned upstream plus our patches.

The game links XenosRecomp's shader recompiler for runtime shader translation (cmake/xenos_xlat.cmake).
Its sources are upstream hedge-dev/XenosRecomp at the commit the third_party/XenosRecomp submodule pins
(990d03b) with tools/xenosrecomp/*.patch applied in PATCHES order. CMake runs this at configure time:

  1. `git archive` of the pinned commit's XenosRecomp/ directory and LICENSE.md, taken from the submodule's
     object store (whatever its working tree holds; the submodule is never modified), with line endings
     as committed (LF);
  2. the patch series, applied with `git apply` in a scratch repository (patch files are read with their
     line endings normalized to LF, so a core.autocrlf checkout of this repository applies them too);
  3. the files written into OUT with the line endings of the clone that the offline shader cache
     (data/shader_cache) and every game build so far were made from (tools/XenosRecomp-clone, a
     core.autocrlf=true checkout, then patched): CRLF, except the two files in LF_FILES. The bytes are part
     of the output: cmake/xenos_embed.cmake embeds shader_common.h byte for byte (the runtime translator
     prepends it to every shader, as the offline tool does), shader_recompiler.cpp holds a multi-line raw
     string of HLSL (its newlines reach the generated shaders), and the runtime cache is keyed by hashes
     of the raw files. `--compare <clone>` checks the result against such a clone byte for byte.

OUT/key.txt holds a hash of (pin, patch contents, this script); a matching key makes the run a no-op.

    python tools/xenosrecomp/prepare.py --out <build>/xenosrecomp [--src third_party/XenosRecomp]
    python tools/xenosrecomp/prepare.py --out DIR --compare <clone>   # also byte-compare with a clone
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
ROOT = os.path.dirname(os.path.dirname(HERE))
SUBMODULE = "third_party/XenosRecomp"
UPSTREAM = "https://github.com/hedge-dev/XenosRecomp"
PATCHES = ["superman.patch", "0002-iter463-setp-and-texture-lod.patch",
           "0003-iter464-resscale-pixel-position.patch",
           "0004-step4-const-relative-per-operand.patch",
           "0005-step4-alpha-test-variant.patch",
           "0006-step4-alpha-test-strict-compare.patch"]
EXPORT = ["XenosRecomp", "LICENSE.md"]
# The two files the reference clone holds with LF endings (the patch series' tooling rewrote them); every
# other file there is CRLF. See step 3 above.
LF_FILES = {"XenosRecomp/main.cpp", "XenosRecomp/shader_recompiler.cpp"}
VERSION = "prepare v1"


def git(*args, cwd=None, binary=False):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True)
    if r.returncode != 0:
        sys.exit("prepare: git %s failed: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout if binary else r.stdout.decode().strip()


def pin(root=ROOT):
    """The commit the superproject pins third_party/XenosRecomp at (the gitlink)."""
    parts = git("-C", root, "ls-files", "--stage", SUBMODULE).split()
    if len(parts) < 2 or parts[0] != "160000":
        sys.exit("prepare: no %s gitlink in %s (git ls-files --stage %s)" % (SUBMODULE, root, SUBMODULE))
    return parts[1]


def patch_bytes(name):
    with open(os.path.join(HERE, name), "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def key(commit):
    h = hashlib.sha256(("%s\n%s\n" % (VERSION, commit)).encode())
    for name in PATCHES:
        h.update(("%s %s\n" % (name, hashlib.sha256(patch_bytes(name)).hexdigest())).encode())
    with open(os.path.abspath(__file__), "rb") as f:
        h.update(hashlib.sha256(f.read().replace(b"\r\n", b"\n")).hexdigest().encode())
    return h.hexdigest()


def to_crlf(data):
    """What a core.autocrlf=true checkout writes for a text blob: every LF becomes CRLF."""
    if b"\0" in data:
        return data
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def build_tree(src, commit, out):
    tmp = tempfile.mkdtemp(prefix="xr_prepare_")
    pdir = tempfile.mkdtemp(prefix="xr_patches_")
    try:
        tar = git("-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", src, "archive", "--format=tar",
                  commit, *EXPORT, binary=True)
        with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
            tf.extractall(tmp)
        # `git apply` resolves paths against the enclosing repository: give the export its own.
        git("-c", "init.defaultBranch=main", "init", "-q", cwd=tmp)
        for name in PATCHES:
            p = os.path.join(pdir, name)
            with open(p, "wb") as f:
                f.write(patch_bytes(name))
            git("-c", "core.autocrlf=false", "apply", "--whitespace=nowarn", p, cwd=tmp)
        shutil.rmtree(os.path.join(tmp, ".git"))
        stage = out + ".tmp"
        shutil.rmtree(stage, ignore_errors=True)
        n = 0
        for dp, _, files in os.walk(tmp):
            for f in files:
                rel = os.path.relpath(os.path.join(dp, f), tmp)
                dst = os.path.join(stage, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(os.path.join(dp, f), "rb") as fi, open(dst, "wb") as fo:
                    data = fi.read()
                    fo.write(data if rel.replace(os.sep, "/") in LF_FILES else to_crlf(data))
                n += 1
        return stage, n
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(pdir, ignore_errors=True)


def compare(out, clone):
    """Byte-compare OUT's XenosRecomp/ with a clone's (exit status 1 on any difference)."""
    bad, n = [], 0
    for dp, _, files in os.walk(os.path.join(out, "XenosRecomp")):
        for f in files:
            rel = os.path.relpath(os.path.join(dp, f), out)
            n += 1
            other = os.path.join(clone, rel)
            if not os.path.isfile(other):
                bad.append("%s: missing in %s" % (rel, clone))
            elif open(other, "rb").read() != open(os.path.join(dp, f), "rb").read():
                bad.append("%s: differs" % rel)
    for dp, _, files in os.walk(os.path.join(clone, "XenosRecomp")):
        for f in files:
            rel = os.path.relpath(os.path.join(dp, f), clone)
            if not os.path.isfile(os.path.join(out, rel)):
                bad.append("%s: only in %s" % (rel, clone))
    for b in bad:
        print("prepare: COMPARE " + b)
    print("prepare: %d XenosRecomp/ files byte-compared with %s: %s" % (n, clone, "IDENTICAL" if not bad else
                                                                         "%d DIFFER" % len(bad)))
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="output directory (gets XenosRecomp/, LICENSE.md, key.txt)")
    ap.add_argument("--src", default=os.path.join(ROOT, *SUBMODULE.split("/")),
                    help="a XenosRecomp checkout holding the pinned commit (default: the submodule)")
    ap.add_argument("--pin", help="commit to export (default: the superproject's gitlink)")
    ap.add_argument("--compare", help="byte-compare the result with this clone's XenosRecomp/")
    a = ap.parse_args()
    src, out = os.path.abspath(a.src), os.path.abspath(a.out)
    commit = a.pin or pin()
    if subprocess.run(["git", "-C", src, "cat-file", "-e", commit + "^{commit}"], capture_output=True).returncode:
        sys.exit("prepare: %s does not have the pinned XenosRecomp commit %s. Run\n"
                 "    git submodule update --init --recursive %s\n"
                 "or configure with -DSR_XENOSRECOMP_DIR=<a patched clone> (tools/xenosrecomp/README.md)."
                 % (src, commit, SUBMODULE))
    want = key(commit)
    kf = os.path.join(out, "key.txt")
    if os.path.isfile(kf) and open(kf).read().strip() == want and \
            os.path.isfile(os.path.join(out, "XenosRecomp", "shader_recompiler.cpp")):
        print("prepare: %s is current (XenosRecomp %s + %d patches)" % (out, commit[:10], len(PATCHES)))
    else:
        stage, n = build_tree(src, commit, out)
        shutil.rmtree(out, ignore_errors=True)
        os.replace(stage, out)
        with open(kf, "w", encoding="utf-8", newline="\n") as f:
            f.write(want + "\n")
        print("prepare: XenosRecomp %s + %s -> %s (%d files)" % (commit[:10], " + ".join(PATCHES), out, n))
    if a.compare and not compare(out, os.path.abspath(a.compare)):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
