"""step 6 I3-B (bindless, Fable ruling 44): run every mode of difftest --native-bindless-test and check its outcome.

The lifetime modes must PASS (exit 0 and their PASS line). The selection modes must end in the production refusal:
exit 3 with "[NATIVE_RECORDING] FAILED stage=<their stage>". Each mode is its own process (a refusal ends it).
FAIL is only the mode's own verdict: its assertion (exit 1 with the fixture's FAIL line). Everything else is
INFRASTRUCTURE, reported apart: a crash, a hang, an API / device failure (the fixture's exit 2), a production refusal
inside a lifetime mode, or a selection mode refused at another stage. A mutant is killed only by a FAIL.

    python tools/difftest/native_bindless_run.py <path to difftest.exe> [--only mode,mode]
Exit 0 = all as expected; 1 = a mode failed; 2 = an infrastructure failure (no verdict).
"""
import subprocess
import sys

# mode: (expected exit, the refusal stage an exit-3 mode must name)
MODES = {
    "rewrite-before-submit": (0, None),   # the submission check: a slot rewritten between bind and submit is BAD
    "rewrite-in-flight": (0, None),       # the write check: a rewrite of a slot an in-flight submission reads is BAD
    "cow-gpu": (0, None),                 # copy-on-write: the in-flight draw reads the OLD view, the next the new one
    "inplace-gpu-control": (0, None),     # the control: an in-place rewrite reaches the in-flight draw (and is BAD)
    "raw-ring-gpu": (0, None),            # a full ring wrap: the wrapping write waits; the in-flight draw read its view
    "heap-change-gpu": (0, None),         # a foreign heap set between draws: the next bind restores the heap
    "raw-ring-same-recording": (0, None),  # a repeated range reuses its ring slot within one recording only
    "exhaust-victim": (0, None),          # slot exhaustion reuses only a 2-frame-old slot, after a counted wait
    "single-submission": (0, None),       # Fable 44 (1) (a): a native recording executes once
    "selection-dimension": (3, "texture-binding-dimension"),  # a selection of the wrong view type is refused
    "selection-range": (3, "texture-binding-index"),          # a selection past its table: refused by bounds alone
}


def run_mode(exe, mode, timeout=600):
    """'ok' | 'fail' (the mode's assertion) | 'infra' (anything else: no verdict), and a detail line."""
    expected, stage = MODES[mode]
    try:
        r = subprocess.run([exe, "--native-bindless-test", mode], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "infra", "timeout"
    out = r.stdout + r.stderr
    lines = out.strip().splitlines()
    named = [ln for ln in lines if "[NATIVE_BINDLESS_TEST] FAIL" in ln or "[NATIVE_BINDLESS_TEST] INFRA" in ln
             or "[NATIVE_RECORDING] FAILED stage=" in ln]
    detail = f"exit {r.returncode} {(named or lines[-1:] or [''])[-1]}"
    if expected == 0 and r.returncode == 0 and f"[NATIVE_BINDLESS_TEST] PASS {mode}" in out:
        return "ok", detail
    if expected == 3 and r.returncode == 3 and f"FAILED stage={stage} " in out:
        return "ok", detail
    if r.returncode == 1 and "[NATIVE_BINDLESS_TEST] FAIL " in out:
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
    exe = a.exe
    results = {}
    for mode in (only or MODES):
        status, detail = run_mode(exe, mode)
        results[mode] = status
        print(f"{ {'ok': 'ok  ', 'fail': 'FAIL', 'infra': 'INFRA'}[status]} {mode}: {detail}", flush=True)
    failed = sum(s == "fail" for s in results.values())
    infra = sum(s == "infra" for s in results.values())
    verdict = "PASS" if not failed and not infra else ("INFRA" if infra else "FAIL")
    print(f"native_bindless: {verdict} ({failed} failing, {infra} infrastructure, of {len(results)})")
    return 0 if verdict == "PASS" else (2 if infra else 1)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
