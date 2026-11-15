#!/usr/bin/env python3
"""compare_run.py - run one golden-call script through BOTH sides (the recomp harness exe built by
build_harness.py, and the clean-room interpreter oracle run_script.py) on the same golden blob and
report the cases whose printed results differ.

    python compare_run.py <script> [--mode xenia|recomp|isa] [--jobs 16] [--tag name]
                          [--recomp-out f (reuse an earlier recomp run)] [--mutate va:hexbytes]
Modes (xenon_interp.py): xenia = Xenia's arithmetic (the reference); recomp = XenonRecomp's
arithmetic conventions (bit-exact agreement here proves every OTHER lowering - control flow,
integer, memory, dispatch - matches); isa = exact Power ISA rounding / 64-bit CR0.
--mutate writes bytes into the ORACLE's memory only (mutation-validates the comparison).
Writes <out>/<tag>.recomp.out, <tag>.oracle.<mode>.out, <tag>.<mode>.cov.json, <tag>.<mode>.diff.txt."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
WORK = os.path.join(ROOT, "archive", "golden_call")


def cases(path):
    out = {}
    order = []
    cur = None
    for l in open(path):
        l = l.rstrip("\n")
        if l.startswith("CASE "):
            cur = l[5:]
            order.append(cur)
            out[cur] = []
        elif cur is not None:
            out[cur].append(l)
    return out, order


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--mode", default="xenia")
    ap.add_argument("--tag")
    ap.add_argument("--recomp-out")
    ap.add_argument("--exe", default=os.path.join(WORK, "golden_call_harness.exe"))
    ap.add_argument("--blob", default=os.path.join(WORK, "golden_mem.bin"))
    ap.add_argument("--out-dir", default=os.path.join(WORK, "out"))
    ap.add_argument("--mutate", action="append", default=[], help="va:hexbytes into the ORACLE memory only (mutation test)")
    a = ap.parse_args()
    tag = a.tag or os.path.splitext(os.path.basename(a.script))[0]
    OUT = a.out_dir
    BLOB = a.blob
    rout = a.recomp_out or os.path.join(OUT, tag + ".recomp.out")
    os.makedirs(OUT, exist_ok=True)
    if not a.recomp_out:
        env = dict(os.environ)
        env["PATH"] = os.path.join(ROOT, "build") + os.pathsep + env["PATH"]
        r = subprocess.run([a.exe, BLOB, a.script, rout], env=env, capture_output=True, text=True)
        sys.stderr.write(r.stderr[-400:])
        if r.returncode:
            raise SystemExit("recomp harness failed rc=%d" % r.returncode)
    oout = os.path.join(OUT, "%s.oracle.%s.out" % (tag, a.mode))
    cov = os.path.join(OUT, "%s.%s.cov.json" % (tag, a.mode))
    r = subprocess.run([sys.executable, os.path.join(HERE, "run_script.py"), BLOB, a.script, oout,
                        "--mode", a.mode, "--jobs", str(a.jobs), "--cov", cov] + sum((["--mutate", m] for m in a.mutate), []),
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr[-2000:])
    R, order = cases(rout)
    O, _ = cases(oout)
    div = []
    for c in order:
        if R.get(c) != O.get(c):
            div.append(c)
    with open(os.path.join(OUT, "%s.%s.diff.txt" % (tag, a.mode)), "w") as f:
        for c in div:
            f.write("CASE %s\n" % c)
            ro, oo = R.get(c, []), O.get(c, [])
            for i in range(max(len(ro), len(oo))):
                x = ro[i] if i < len(ro) else "<none>"
                y = oo[i] if i < len(oo) else "<none>"
                if x != y:
                    f.write("  R: %s\n  O: %s\n" % (x, y))
    print("%s mode=%s cases=%d divergent=%d" % (tag, a.mode, len(order), len(div)))
    for c in div[:10]:
        print("  DIVERGENT", c)


if __name__ == "__main__":
    main()
