"""step 6 G14b (the APC spin guard): run every mode of difftest --apc-guard-test and check its outcome.

Every mode must PASS (exit 0 and its PASS line). FAIL is only a mode's own assertion (exit 1 with its FAIL line);
anything else is INFRASTRUCTURE, reported apart.

    python tools/difftest/apc_guard_run.py <path to difftest.exe> [--only mode,mode]
Exit 0 = all PASS; 1 = a mode failed; 2 = an infrastructure failure (no verdict).
"""
import os
import subprocess
import sys

MODES = ["uncontended", "guard-waits"]


def run_mode(exe, mode, env=None, timeout=120):
    """'ok' | 'fail' (the mode's assertion) | 'infra', and a detail line."""
    try:
        r = subprocess.run([exe, "--apc-guard-test", mode], capture_output=True, text=True, timeout=timeout,
                           env={**os.environ, **(env or {})})
    except subprocess.TimeoutExpired:
        return "infra", "timeout"
    out = r.stdout + r.stderr
    named = [ln for ln in out.splitlines() if "[APC_GUARD_TEST] PASS" in ln or "[APC_GUARD_TEST] FAIL" in ln]
    detail = f"exit {r.returncode} {(named or out.strip().splitlines()[-1:] or [''])[-1]}"
    if r.returncode == 0 and f"[APC_GUARD_TEST] PASS {mode}" in out:
        return "ok", detail
    if r.returncode == 1 and "[APC_GUARD_TEST] FAIL " in out:
        return "fail", detail
    return "infra", detail


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--only", default="")
    a = ap.parse_args(argv[1:])
    only = [m for m in a.only.split(",") if m]
    if any(m not in MODES for m in only):
        print(f"unknown mode in --only: {only}")
        return 2
    failed = infra = 0
    for mode in (only or MODES):
        status, detail = run_mode(a.exe, mode)
        print(f"{ {'ok': 'ok  ', 'fail': 'FAIL', 'infra': 'INFRA'}[status]} {mode}: {detail}", flush=True)
        failed += status == "fail"
        infra += status == "infra"
    verdict = "PASS" if not failed and not infra else ("INFRA" if infra else "FAIL")
    print(f"apc_guard: {verdict} ({failed} failing, {infra} infrastructure)")
    return 0 if verdict == "PASS" else (2 if infra else 1)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
