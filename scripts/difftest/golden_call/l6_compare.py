"""L6 old-vs-new: run golden_call scripts (docs/TOOLKIT.md 1.3b) on two harness builds - e.g. the
OLD one (pre-R1 trunk, spliced bodies) and the NEW one (pristine generated code + hooks) - and
compare every CASE's printed output, and the [ICALL_FAIL] stop line of a harness that stops.
Release lane R1 proved its hook ports with it; the scripts are in scripts/difftest/golden_call/r1/
(per-case notes in b*.notes.txt). Build each harness with build_harness.py --work <dir>.

    python scripts/difftest/golden_call/l6_compare.py --old <old dir> --new <new dir>         --blob <golden_mem.bin> script1 [script2 ...] [--out archive/l6/results]

<old dir> / <new dir> hold golden_call_harness.exe and the DLLs it needs. Prints one line per
case (SAME / DIFF / ERROR) and a summary; exit 1 on any DIFF or ERROR."""
import argparse
import os
import re
import subprocess
import sys


def run(harness_dir, blob, script, out):
    exe = os.path.join(harness_dir, "golden_call_harness.exe")
    try:
        r = subprocess.run([exe, blob, script, out], cwd=harness_dir, capture_output=True, text=True,
                           timeout=600)
        rc = r.returncode
        err = (r.stderr or "")[-400:]
    except subprocess.TimeoutExpired:
        rc, err = -1, "TIMEOUT"
    text = open(out, encoding="utf-8", errors="replace").read() if os.path.exists(out) else ""
    return rc, err, text


def split_cases(text):
    cases, cur, name = {}, [], None
    for line in text.splitlines():
        m = re.match(r"^CASE\s+(\S+)", line)
        if m:
            if name is not None:
                cases[name] = cur
            name, cur = m.group(1), [line]
        elif name is not None:
            cur.append(line)
    if name is not None:
        cases[name] = cur
    return cases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--blob", required=True)
    ap.add_argument("--out", default=os.path.join("archive", "l6", "results"))
    ap.add_argument("scripts", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    total = same = 0
    bad = []
    for script in a.scripts:
        tag = os.path.splitext(os.path.basename(script))[0]
        o_rc, o_err, o_txt = run(a.old, a.blob, os.path.abspath(script), os.path.abspath(os.path.join(a.out, tag + ".old.out")))
        n_rc, n_err, n_txt = run(a.new, a.blob, os.path.abspath(script), os.path.abspath(os.path.join(a.out, tag + ".new.out")))
        oc, nc = split_cases(o_txt), split_cases(n_txt)
        want = re.findall(r"(?m)^CASE\s+(\S+)", open(script, encoding="utf-8").read())
        for name in want:
            if name not in oc and name not in nc:
                total += 1
                bad.append((tag, name, "NOT RUN (both)"))
        for name in sorted(set(oc) | set(nc)):
            total += 1
            if name not in oc or name not in nc:
                bad.append((tag, name, "MISSING " + ("old" if name not in oc else "new")))
                continue
            if oc[name] == nc[name]:
                same += 1
            else:
                bad.append((tag, name, "DIFF"))
        if o_rc != n_rc:
            bad.append((tag, "(process)", f"rc old={o_rc} new={n_rc} {o_err!r} {n_err!r}"))
        # a process that dies (ICALL_FAIL -> TerminateProcess) loses its unflushed stdout; the
        # stop line on stderr is then the behaviour to compare
        o_stop = [l for l in o_err.splitlines() if l.startswith("[ICALL_FAIL]")]
        n_stop = [l for l in n_err.splitlines() if l.startswith("[ICALL_FAIL]")]
        if o_stop != n_stop:
            bad.append((tag, "(stop)", f"old={o_stop} new={n_stop}"))
        print(f"  {tag}: rc old={o_rc} new={n_rc}; stop old={o_stop} new={n_stop}")
    for t, n, why in bad:
        print(f"  {why:12s} {t}:{n}")
    print(f"L6: {same}/{total} cases identical; {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
