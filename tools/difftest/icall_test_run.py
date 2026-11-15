#!/usr/bin/env python3
"""iter-463 burndown item 10: run both halves of `difftest --icall-test`.

  continue  (run with SR_ALLOW_FORCING=1): every unresolvable target
            reaches kernel/indirect_call.cpp and gets the legacy r3=0.
  fail      (default exe): after a mapped call works, an unresolvable call must
            STOP the process - exit code 0xC1CA11FA and an [ICALL_FAIL] line.
            A zero / 1 exit (the call returned) is a failure.

Run from the repo root after `cmake --build build --target difftest`:
    python tools/difftest/icall_test_run.py [--exe build/difftest.exe]
Prints "N passed, 0 failed".
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import crash_markers  # noqa: E402

EXIT_ICALL_FAIL = 0xC1CA11FA


def run(exe, mode):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SR_")}
    if mode == "continue":
        env["SR_ALLOW_FORCING"] = "1"   # read once at process start (forcingMaster)
    p = subprocess.run([exe, "--icall-test", mode], cwd=ROOT, env=env,
                       capture_output=True, text=True, errors="replace", timeout=120)
    return p.returncode & 0xFFFFFFFF, p.stdout + p.stderr


def main():
    exe = os.path.join(ROOT, "build", "difftest.exe")
    if "--exe" in sys.argv:
        exe = sys.argv[sys.argv.index("--exe") + 1]
    ok = bad = 0

    def check(c, what, detail=""):
        nonlocal ok, bad
        print(f"  {'ok  ' if c else 'FAIL'} {what}" + (f"  ({detail})" if detail and not c else ""))
        if c:
            ok += 1
        else:
            bad += 1

    rc, out = run(exe, "continue")
    check(rc == 0 and "[ICALL_TEST]" in out and ", 0 failed" in out,
          "continue mode: dispatch + every unresolvable target returns r3=0 under SR_ALLOW_FORCING",
          f"rc={rc:#x}")
    check("[ICALL_UNRESOLVED]" in out and "kind=null" in out and "kind=out-of-range" in out
          and "kind=unmapped" in out, "continue mode: each kind is logged with call_site/target/object")
    check("[ICALL_FAIL]" not in out, "continue mode: no [ICALL_FAIL]")

    rc, out = run(exe, "fail")
    check(rc == EXIT_ICALL_FAIL, "fail mode: the default exe STOPS on an unresolvable call",
          f"rc={rc:#x} want {EXIT_ICALL_FAIL:#x}")
    check("returned on the default path" not in out.lower() and "RETURNED on the default path" not in out,
          "fail mode: the unresolvable call never returned")
    check(any(crash_markers.scan_fatal(l) == "icall_fail" for l in out.splitlines()),
          "fail mode: the [ICALL_FAIL] line is a crash_markers.py fatal marker")
    check("call_site=82722E30" in out and "target=01010000" in out,
          "fail mode: the log names the call site and the target")
    print(f"[ICALL_TEST_RUN] {ok} passed, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
