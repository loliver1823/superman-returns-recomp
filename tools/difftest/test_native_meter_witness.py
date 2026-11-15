#!/usr/bin/env python3
"""Run the standalone release witness fixture and retain real process receipts.

These are unit-test processes with isolated device/guest dependency doubles, not
game acceptance or graceful-quit receipts. Run on the scheduled build VM only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

RUN = re.compile(r"\[NATIVE_METER_RUN\] (\w+) [\d.]+s frames=(\d+) \| (.*?) \| per_frame")
EXPECTED = dict(pm4=1, edram_res=2, edram_xfer=3, edram_load=4, nrec=5, dispreg=6, shm=7, xma=17)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    exe = args.exe.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    exe_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    cases = []
    for name in ("positive", "reset", "omit-terminal", "unwired", "array-init-failure"):
        start = time.monotonic()
        command = [str(exe)] + ([] if name == "positive" else ["--control=" + name])
        try:
            process = subprocess.run(command, capture_output=True, timeout=15)
            stdout, stderr, code = process.stdout, process.stderr, process.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b"", error.stderr or b"", None
        (args.out / (name + ".stdout.txt")).write_bytes(stdout)
        (args.out / (name + ".stderr.txt")).write_bytes(stderr)
        text = stdout.decode("utf-8", "replace")
        rows = [(why, int(frames), dict(re.findall(r"(\w+)=(\d+|-)", totals)))
                for why, frames, totals in RUN.findall(text)]
        terminal = [row for row in rows if row[0] == "terminal"]
        periodic = [row for row in rows if row[0] == "periodic"]
        expected = {key: str(value) for key, value in EXPECTED.items()}
        checks = dict(exit_code=(code == (3 if name == "array-init-failure" else 0)),
                      no_stderr=not stderr, no_failed_assertion="FAIL " not in text)
        if name == "array-init-failure":
            checks.update(refusal="[NATIVE_XMA_BAD]" in text,
                          counted_before_refusal="[NATIVE_METER_REFUSAL] boundary=1 delegated=0" in text,
                          no_false_completion=not terminal and "snapshot complete" not in text)
        else:
            checks["actual_release_fixture"] = "[NATIVE_METER_WITNESS_TEST] failures=0" in text
            if name == "omit-terminal":
                checks["omitted_terminal"] = len(periodic) == 1 and not terminal
            elif name == "unwired":
                checks["unwired_not_zero"] = len(terminal) == 2 and all(
                    row[2].get(key) == "-" for row in terminal for key in ("edram_res", "edram_xfer", "edram_load"))
            elif name == "reset":
                checks["visible_reset"] = len(periodic) == 1 and periodic[0][2].get("pm4") == "1" and len(terminal) == 2 and all(
                    row[2].get("pm4") == "0" for row in terminal)
            else:
                checks["all_exact_terminal_increments"] = len(terminal) == 2 and all(
                    row[1] == 1 and row[2] == expected for row in terminal)
        cases.append(dict(name=name, command=command, exit_code=code, elapsed_s=time.monotonic() - start,
                          checks=checks, passed=all(checks.values()), stdout_sha256=hashlib.sha256(stdout).hexdigest()))
    result = dict(schema="native-meter-witness-test/1", scope="standalone release unit fixture, not gameplay acceptance",
                  exe=str(exe), exe_sha256=exe_hash, cases=cases, passed=all(case["passed"] for case in cases))
    if hashlib.sha256(exe.read_bytes()).hexdigest() != exe_hash:
        result.update(passed=False, error="fixture executable changed during tests")
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
