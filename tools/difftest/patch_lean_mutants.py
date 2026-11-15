"""step 6 G8 (SR_PATCH_LEAN): source mutants of gfx/resource_memory.cpp's notePatch lean path, each killed by ITS named
check of sr_resource_memory_test (tools/difftest/resource_memory_test.cpp).

For each mutant: apply it (exactly one match, else unusable), build sr_resource_memory_test, run it, and look for the
mutant's named check as a FAIL line. KILLED = that check FAILS; anything else (a crash, a hang, a pass) is no kill. The
source is restored byte for byte after each mutant, and the unmutated test must pass at the end. The lean path's fence
is not unit-observable on x86 (a missing fence races only rarely): it is the review's, not a claimed kill.

    python tools/difftest/patch_lean_mutants.py --build-dir build [--only L1,L3]

Exit 0 = every mutant killed and the final run PASS; 1 = a survivor or the final run failed; 2 = unusable.
"""
import argparse
import os
import pathlib
import subprocess
import sys

SRC = pathlib.Path("gfx/resource_memory.cpp")
TARGET = "sr_resource_memory_test"
# name: (the source text, its mutant, the named check that must FAIL)
MUTANTS = {
    # the lean path skips on the page's pending bit alone (no dword check)
    "L1": ("        if ((patchPending_[page >> 6].load(std::memory_order_acquire) >> (page & 63u) & 1u) &&\n"
           "            (block->dwords[set][d >> 6].load(std::memory_order_acquire) >> (d & 63u) & 1u)) {",
           "        if ((patchPending_[page >> 6].load(std::memory_order_acquire) >> (page & 63u) & 1u)) {",
           "G8: a pending page with another dword set takes the full path for the new dword"),
    # the lean path reads the 0x8 window's bit whatever the note's window
    "L2": ("            (block->dwords[set][d >> 6].load(std::memory_order_acquire) >> (d & 63u) & 1u)) {",
           "            (block->dwords[0][d >> 6].load(std::memory_order_acquire) >> (d & 63u) & 1u)) {",
           "G8: a dword pending through one window takes the full path when noted through the other"),
    # no lean path at all
    "L3": ("  if (patchLean_ && len == 4u && !(va & 3u)) {", "  if (false && len == 4u && !(va & 3u)) {",
           "G8: a second note of a still-pending dword takes the lean path"),
    # the switch ignored: the lean path also when SR_PATCH_LEAN=0
    "L4": ("  if (patchLean_ && len == 4u && !(va & 3u)) {", "  if (len == 4u && !(va & 3u)) {",
           "G8: SR_PATCH_LEAN=0 takes the full path every time (the control)"),
    # the launch variable ignored: lean whatever SR_PATCH_LEAN says (run with SR_PATCH_LEAN=0)
    "L5": ("    const bool v = !e || e[0] != '0';", "    const bool v = !e || e[0] != '0' || true;",
           "G8: an arena at its default follows SR_PATCH_LEAN"),
    # step 6 G8c: the fast-path statistic's relaxed load + store loses its increment
    "L6": ("          shard.fast.store(shard.fast.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);",
           "          shard.fast.store(shard.fast.load(std::memory_order_relaxed), std::memory_order_relaxed);",
           "G8: a second note of a still-pending dword takes the lean path, is counted, and is taken once"),
    # step 6 G8c: the leaf split drops the call into the full path (a note the lean check does not answer is lost)
    "L7": ("  notePatchSlow(va, len, set, window, shard);\n", "",
           "G8: a note after the take takes the full path and re-arms the page"),
}
# a mutant's run environment, over the inherited one
MUTANT_ENV = {"L5": {"SR_PATCH_LEAN": "0"}}


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def run_test(exe, env=None):
    """(exit code, stdout+stderr), or (None, reason) on a timeout."""
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=300,
                           env={**os.environ, **env} if env else None)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    exe = str(pathlib.Path(a.build_dir) / (TARGET + ".exe"))
    original = SRC.read_bytes()
    crlf = b"\r\n" in original

    def text(t):
        return (t.replace("\n", "\r\n") if crlf else t).encode()

    only = [m for m in a.only.split(",") if m]
    build = ["cmake", "--build", a.build_dir, "-j", "12", "--target", TARGET]
    chosen = [(n, m) for n, m in MUTANTS.items() if not only or n in only]
    for name, (old, new, check) in chosen:
        if original.count(text(old)) != 1:
            print(f"UNUSABLE {name}: the target text occurs {original.count(text(old))} times")
            return 2
    survivors, unusable = [], []
    try:
        for name, (old, new, check) in chosen:
            SRC.write_bytes(original.replace(text(old), text(new)))
            if run(build):
                print(f"UNUSABLE {name}: the mutant does not build")
                return 2
            rc, out = run_test(exe, MUTANT_ENV.get(name))
            if rc is None:
                verdict = "UNUSABLE"
            elif any(ln.startswith("FAIL ") and check in ln for ln in out.splitlines()):
                verdict = "killed"
            else:
                verdict = "SURVIVED"
            fails = [ln for ln in (out or "").splitlines() if ln.startswith("FAIL ")]
            print(f"{verdict} {name} by '{check}' (exit {rc}; {len(fails)} FAIL lines: {fails[:3]})", flush=True)
            if verdict == "SURVIVED":
                survivors.append(name)
            elif verdict == "UNUSABLE":
                unusable.append(name)
    finally:
        SRC.write_bytes(original)
    if run(build):
        print("patch_lean_mutants: FAIL (the restored source does not build)")
        return 1
    rc, out = run_test(exe)
    if rc != 0:
        print(f"patch_lean_mutants: FAIL (the unmutated test does not pass: exit {rc})")
        return 1
    if unusable:
        print(f"patch_lean_mutants: UNUSABLE {unusable} (no verdict) survivors={survivors}")
        return 2
    print(f"patch_lean_mutants: {'PASS' if not survivors else 'FAIL'} survivors={survivors}")
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
