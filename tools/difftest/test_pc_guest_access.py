#!/usr/bin/env python3
"""Retain actual standalone release-reader fixture results (scheduled VM only)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

REQUIRED = (
    "actual game reader reports gameplay with MMIO closed",
    "partial inaccessible slot cannot become zero-valued gameplay",
    "derived slot offset cannot wrap into low memory",
    "actual camera hook writes yaw/pitch/reference and delegates exactly once",
    "camera crossing protected span refuses writes and still delegates once",
    "actual action publishes completed button writes",
    "actual action publishes BE axis halfword",
    "actual action restores BE axis halfword",
    "actual move calls once under lease and restores registers",
    "protected and underflowed move scratch refuse guest invocation",
    "actual Apt invocation writes bounded scratch and restores known context",
    "Apt executor derived offset refuses protected read",
    "Apt stack offset cannot wrap and known published context is restored after refusal",
    "every feature-initiated guest call retains the operation lease",
    "no feature operation reopens hardware protection",
    "retiring failed Apt operation restores through its original lease",
    "retired arena refuses new feature operation",
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    executable = args.exe.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    rows = []
    for name in ("positive", "omit-protection", "whole-arena-refusal"):
        command = [str(executable)] + ([] if name == "positive" else ["--control=" + name])
        start = time.monotonic()
        try:
            result = subprocess.run(command, capture_output=True, timeout=30)
            stdout, stderr, code = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b"", error.stderr or b"", None
        (args.out / (name + ".stdout.txt")).write_bytes(stdout)
        (args.out / (name + ".stderr.txt")).write_bytes(stderr)
        text = stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        terminal = re.findall(r"^\[PC_GUEST_ACCESS_TEST\] passed=(\d+) failed=(\d+)$", text, re.M)
        checks = dict(exit_code=code == (0 if name == "positive" else 1),
                      no_stderr=not stderr, one_terminal=len(terminal) == 1)
        if name == "positive":
            checks.update(no_failed_checks="FAIL " not in text,
                          zero_failures=len(terminal) == 1 and terminal[0][1] == "0",
                          required_paths=all("PASS " + label + "\n" in text.replace("\r\n", "\n")
                                             for label in REQUIRED))
        else:
            checks["real_rejection"] = len(terminal) == 1 and int(terminal[0][1]) > 0
            marker = ("actual hardware aperture stays PAGE_NOACCESS" if name == "omit-protection"
                      else "actual game reader reports gameplay with MMIO closed")
            checks["named_defect"] = "FAIL " + marker in text
        rows.append(dict(name=name, command=command, elapsed_s=time.monotonic() - start,
                         exit_code=code, checks=checks, passed=all(checks.values()),
                         stdout_sha256=hashlib.sha256(stdout).hexdigest(),
                         stderr_sha256=hashlib.sha256(stderr).hexdigest()))
    stable = hashlib.sha256(executable.read_bytes()).hexdigest() == digest
    receipt = dict(schema="pc-guest-access-test/1", exe=str(executable), exe_sha256=digest,
                   scope="production host feature paths with guest boundary doubles; not gameplay acceptance",
                   cases=rows, stable_executable=stable, passed=stable and all(row["passed"] for row in rows))
    (args.out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
