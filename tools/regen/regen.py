#!/usr/bin/env python3
"""regen.py - regenerate guest/ppc from the player's own default.xex (release lane R1).

The repo carries no recompiled game code. guest/ppc (gitignored) is produced from:

  1. default.xex          the player's retail XEX (SR_XEX_PATH, default private/default.xex;
                          never committed - private/ is an allowlist .gitignore)
  2. XenonRecomp          upstream hedge-dev/XenonRecomp at the commit the tools/XenonRecomp
                          submodule pins, plus tools/regen/patches/*.patch, applied to a copy
                          in the build tree (the submodule itself stays pristine) and built
                          there with Ninja + clang-cl
  3. post-processing      assemble.py (the hand headers, and a check that no raw guest address
                          or unguarded division is left: patches 0006 / 0007 emit those), then
                          flag_transform.py (register localization + the committed ABI
                          bridges, tools/regen/abi_bridge.json)

CMake runs this at configure time (guest/CMakeLists.txt, option SR_REGEN): `--ensure` is a
no-op when guest/ppc/.regen_stamp already matches the hash of every input listed in
`stamp_inputs()`, so it only costs the hashing (~1 s) on a normal configure.

    python tools/regen/regen.py --ensure [--xex PATH] [--xr-src DIR] [--work DIR]
    python tools/regen/regen.py --stamp          # print the wanted stamp
    python tools/regen/regen.py --force ...      # regenerate even when the stamp matches
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEST = os.path.join(ROOT, "guest", "ppc")
STAMP = os.path.join(DEST, ".regen_stamp")
# SR_REGEN_TOML: an alternative config (A/B checks, e.g. reproducing an older tree)
TOML = os.environ.get("SR_REGEN_TOML") or os.path.join(HERE, "superman_regen.toml")
SWITCH_TOML = os.path.join(HERE, "superman_switch_tables.toml")
PATCHES = os.path.join(HERE, "patches")
HAND = os.path.join(HERE, "hand")
LLVM = os.environ.get("SR_LLVM_DIR", r"C:/Program Files/LLVM/bin")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def xenonrecomp_pin():
    """The commit the superproject pins tools/XenonRecomp at (the gitlink)."""
    r = subprocess.run(["git", "-C", ROOT, "ls-files", "--stage", "tools/XenonRecomp"],
                       capture_output=True, text=True)
    parts = r.stdout.split()
    if r.returncode != 0 or len(parts) < 2 or parts[0] != "160000":
        sys.exit("regen: cannot read the tools/XenonRecomp gitlink (git ls-files --stage tools/XenonRecomp)")
    return parts[1]


def stamp_inputs():
    """Every file whose content decides guest/ppc (the xex is hashed separately)."""
    files = [TOML, SWITCH_TOML, os.path.abspath(__file__)]
    files += sorted(glob.glob(os.path.join(PATCHES, "*.patch")))
    files += sorted(glob.glob(os.path.join(HAND, "*")))
    for name in ["assemble.py", "flag_transform.py", "abi_bridge.json"]:
        p = os.path.join(HERE, name)
        if os.path.exists(p):
            files.append(p)
    return files


def wanted_stamp(xex_sha):
    h = hashlib.sha256()
    h.update(b"regen v1\n")
    h.update(f"xex {xex_sha}\n".encode())
    h.update(f"xenonrecomp {xenonrecomp_pin()}\n".encode())
    for p in stamp_inputs():
        rel = os.path.relpath(p, ROOT).replace("\\", "/")
        h.update(f"{rel} {sha256_file(p)}\n".encode())
    return h.hexdigest()


def have_stamp():
    try:
        line = open(STAMP, encoding="utf-8").readline().split()
        return line[2] if len(line) >= 3 and line[:2] == ["regen", "v1"] else None
    except OSError:
        return None


def run(cmd, cwd=None, env=None):
    print("regen: $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env)
    if r.returncode != 0:
        sys.exit(f"regen: command failed (rc={r.returncode})")


# ---------------------------------------------------------------- XenonRecomp bootstrap
def bootstrap(xr_src, work):
    """Pinned upstream XenonRecomp + our patches, built in work/xenonrecomp. Cached by
    (pin, patch contents); returns the XenonRecomp executable."""
    pin = xenonrecomp_pin()
    key = hashlib.sha256(pin.encode())
    for p in sorted(glob.glob(os.path.join(PATCHES, "*.patch"))):
        key.update(sha256_file(p).encode())
    key = key.hexdigest()[:16]
    base = os.path.join(work, "xenonrecomp")
    exe = os.path.join(base, "build", "XenonRecomp", "XenonRecomp.exe")
    keyfile = os.path.join(base, "key.txt")
    if os.path.exists(exe) and os.path.exists(keyfile) and open(keyfile).read().strip() == key:
        return exe
    if not os.path.isdir(os.path.join(xr_src, "thirdparty", "simde")) or \
            not os.listdir(os.path.join(xr_src, "thirdparty", "simde")):
        sys.exit(f"regen: XenonRecomp sources not found at {xr_src}. Run\n"
                 f"    git submodule update --init --recursive tools/XenonRecomp\n"
                 f"or point SR_XENONRECOMP_SRC at a checkout that has the pinned commit {pin}.")
    if os.path.isdir(base):
        shutil.rmtree(base)
    src = os.path.join(base, "src")
    # 1) the submodule working tree WITHOUT its git metadata or old build dirs (brings the
    #    thirdparty submodule contents, which `git archive` does not include) ...
    def ignore(d, names):
        drop = [n for n in names if n == ".git"]
        if os.path.normcase(os.path.abspath(d)) == os.path.normcase(os.path.abspath(xr_src)):
            drop += [n for n in names if n.startswith("build") or n == "out"]  # local build trees
        return drop
    shutil.copytree(xr_src, src, ignore=ignore)
    # 2) ... then every tracked file exactly as the PIN has it (undoes any local edit or
    #    local commit in that checkout: the build always starts from pristine upstream)
    tar = os.path.join(base, "pin.tar")
    run(["git", "-C", xr_src, "archive", "--format=tar", "-o", tar, pin])
    with tarfile.open(tar) as tf:
        tf.extractall(src)
    os.remove(tar)
    # 3) our patches. `git apply` resolves paths against the enclosing repository, and the
    #    build tree lives inside ours, so give the copy its own repository first.
    run(["git", "init", "-q"], cwd=src)
    for p in sorted(glob.glob(os.path.join(PATCHES, "*.patch"))):
        run(["git", "apply", "--whitespace=nowarn", p], cwd=src)
    # 4) build only the XenonRecomp target (Release, clang-cl: the recipe in README.md)
    bld = os.path.join(base, "build")
    cmake = os.environ.get("SR_CMAKE", "cmake")
    run([cmake, "-S", src, "-B", bld, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_C_COMPILER={LLVM}/clang-cl.exe", f"-DCMAKE_CXX_COMPILER={LLVM}/clang-cl.exe",
         f"-DCMAKE_LINKER={LLVM}/lld-link.exe", f"-DCMAKE_RC_COMPILER={LLVM}/llvm-rc.exe"])
    run([cmake, "--build", bld, "--target", "XenonRecomp", "-j", os.environ.get("SR_REGEN_JOBS", "8")])
    if not os.path.exists(exe):
        sys.exit(f"regen: XenonRecomp build did not produce {exe}")
    open(keyfile, "w").write(key + "\n")
    return exe


# ---------------------------------------------------------------- the pipeline
def recompile(exe, xex, work):
    """XenonRecomp joins every toml path onto the toml's directory as a string, so the
    build toml lives in `work` and names its inputs by paths relative to it."""
    out = os.path.join(work, "ppc_out")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    text = open(TOML, encoding="utf-8").read()

    def rel(p):
        try:
            return os.path.relpath(p, work).replace("\\", "/")
        except ValueError:  # different drive: copy the input next to the toml
            dst = os.path.join(work, os.path.basename(p))
            shutil.copyfile(p, dst)
            return os.path.basename(p)
    lines = []
    for line in text.splitlines():
        key = line.split("=")[0].strip()
        if key == "file_path":
            line = f'file_path = "{rel(xex)}"'
        elif key == "out_directory_path":
            line = 'out_directory_path = "ppc_out"'
        elif key == "switch_table_file_path":
            line = f'switch_table_file_path = "{rel(SWITCH_TOML)}"'
        lines.append(line)
    btoml = os.path.join(work, "superman_regen.build.toml")
    open(btoml, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    run([exe, btoml, os.path.join(HAND, "ppc_context.h")], cwd=work)
    return out


def deploy(raw, work):
    """assemble.py and flag_transform run on a staging copy
    (work/stage/guest/ppc); only the files whose bytes changed are then copied into guest/ppc,
    so a hook or toml change rebuilds only the guest TUs it touches (a full guest rebuild is
    ~30 min), and a file the new tree no longer has is removed."""
    asm_out = os.path.join(work, "assembled")
    env = dict(os.environ, SR_REGEN_IN=raw, SR_REGEN_OUT=asm_out)
    run([sys.executable, os.path.join(HERE, "assemble.py")], cwd=ROOT, env=env)
    stage_root = os.path.join(work, "stage")
    stage = os.path.join(stage_root, "guest", "ppc")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    for name in os.listdir(asm_out):
        shutil.copyfile(os.path.join(asm_out, name), os.path.join(stage, name))
    run([sys.executable, os.path.join(HERE, "flag_transform.py"), "--deployed", stage], cwd=ROOT)
    os.makedirs(DEST, exist_ok=True)
    if os.path.exists(STAMP):  # an interrupted sync must not look current
        os.remove(STAMP)
    want = set(os.listdir(stage))
    copied = removed = 0
    for name in sorted(os.listdir(DEST)):
        p = os.path.join(DEST, name)
        if os.path.isfile(p) and name not in want:
            os.remove(p)
            removed += 1
    for name in sorted(want):
        src, dst = os.path.join(stage, name), os.path.join(DEST, name)
        if os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src) and \
                open(dst, "rb").read() == open(src, "rb").read():
            continue
        shutil.copyfile(src, dst)
        copied += 1
    print(f"regen: guest/ppc synced: {copied} file(s) written, {removed} removed, "
          f"{len(want) - copied} unchanged", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--xex", default=os.environ.get("SR_XEX_PATH") or os.path.join(ROOT, "private", "default.xex"))
    ap.add_argument("--xr-src", default=os.environ.get("SR_XENONRECOMP_SRC") or os.path.join(ROOT, "tools", "XenonRecomp"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "regen"))
    ap.add_argument("--ensure", action="store_true", help="regenerate only when the stamp differs")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--stamp", action="store_true", help="print the wanted stamp and exit")
    a = ap.parse_args()
    if not os.path.isfile(a.xex):
        if a.ensure and have_stamp():
            print(f"regen: no XEX at {a.xex}; keeping the existing guest/ppc (stamp {have_stamp()[:12]}) "
                  f"- it cannot be checked against the current inputs", flush=True)
            refresh_va_index()
            return 0
        sys.exit(f"regen: no XEX at {a.xex}. Copy your retail default.xex to private/default.xex "
                 f"or configure with -DSR_XEX_PATH=<path>.")
    want = wanted_stamp(sha256_file(a.xex))
    if a.stamp:
        print(want)
        return 0
    if a.ensure and not a.force and have_stamp() == want:
        print(f"regen: guest/ppc is current (stamp {want[:12]})", flush=True)
        refresh_va_index()
        return 0
    os.makedirs(a.work, exist_ok=True)
    exe = bootstrap(os.path.abspath(a.xr_src), os.path.abspath(a.work))
    raw = recompile(exe, os.path.abspath(a.xex), os.path.abspath(a.work))
    deploy(raw, os.path.abspath(a.work))
    with open(STAMP, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"regen v1 {want}\n")
    refresh_va_index()
    print(f"regen: guest/ppc regenerated (stamp {want[:12]})", flush=True)
    return 0


def refresh_va_index():
    """Derived index refresh runs after publication; it never decides regen identity."""
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from va_to_source import ensure_fresh_index
    from pathlib import Path
    try:
        result = ensure_fresh_index(Path(ROOT) / ".cache" / "va_index.db", Path(DEST))
    except Exception as error:
        raise RuntimeError("regen: VA index refresh failed; generated sources remain published and the prior index is preserved: %s" % error) from error
    print("regen: VA index %s (%s)" % (result["status"], result["fingerprint"][:12]), flush=True)


if __name__ == "__main__":
    sys.exit(main())
