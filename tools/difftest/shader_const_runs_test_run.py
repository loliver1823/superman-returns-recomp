"""Run isolated SDK, entry, direct, production-table and oracle comparisons.

Usage: python tools/difftest/shader_const_runs_test_run.py --out archive/<bank>
Each case runs in a fresh process with clean SR_* settings. Receipts are never
overwritten. This proves bounded SDK behavior, not whole-game native acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--label", default="shader_const_sdk")
    parser.add_argument("--exe", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]+", args.label):
        parser.error("label must contain only letters, digits and underscores")
    root = Path(__file__).resolve().parents[2]
    exe = (args.exe or root / "build/difftest.exe").resolve()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / f"{args.label}_unit_matrix.json"
    if receipt.exists() or list(output.glob(f"{args.label}_*_stdout.txt")):
        parser.error("choose an unused label to preserve previous receipts")
    cases = []
    for route in ("entry", "direct", "indirect", "oracle"):
        for mode, settings in (
            ("literal", {"SR_NATIVE_FLUSH_PM4": "1"}),
            ("record", {}),
            ("reconstructed", {"SR_NATIVE_FLUSH_CPPM4": "1"}),
        ):
            cases.append((f"{route}_{mode}", ["--shader-const-runs-test", route], settings))
    for route in ("direct", "indirect"):
        for name, settings in (
            ("shader_off", {"SR_NATIVE_SHADER_CONST_RUNS": "0"}),
            ("flush_off", {"SR_NATIVE_FLUSH": "0"}),
            ("check", {"SR_NATIVE_FLUSH_CHECK": "1"}),
        ):
            cases.append((f"{route}_{name}", ["--shader-const-runs-test", route], settings))
    for feature in ("SR_NATIVE_FLUSH", "SR_NATIVE_SHADER_CONST_RUNS"):
        cases.append((feature.lower() + "_ab_off", ["--shader-const-runs-test", "indirect-off"],
                      {"SR_AB_TOGGLE": feature}))
    cases.extend([
        ("lock_negative", ["--shader-const-runs-test", "lock-negative"], {}),
        ("gate_regression", ["--present-gate-test"], {"SR_NATIVE_GATE": "1", "SR_NATIVE_SEGMENTLESS": "1"}),
        ("callback_regression", ["--callback-test"], {"SR_NATIVE_CALLBACK": "1", "SR_NATIVE_SEGMENTLESS": "1"}),
        ("tile_regression", ["--seg-tile-test"], {}),
    ])
    sources = ["kernel/native_flush.cpp", "kernel/native_flush.hpp", "guest/guest_harness.cpp",
               "guest/guest_harness.hpp", "tools/difftest/shader_const_runs_test.cpp",
               "tools/difftest/difftest_main.cpp", "tools/difftest/shader_const_runs_test_run.py"]
    evidence = dict(exe=str(exe), exe_sha256=digest(exe),
                    source_sha256={name: digest(root / name) for name in sources},
                    scope="Isolated SDK generation and register execution, actual production mapping and tessellated caller; no whole-game or live backpressure claim",
                    rows=[])
    for name, command, settings in cases:
        env = {k: v for k, v in os.environ.items() if not k.startswith("SR_")}
        env.update(SR_NATIVE_FLUSH="1", SR_NATIVE_FLUSH_CHECK="0", SR_NATIVE_SEGMENTLESS="0",
                   SR_NATIVE_SEG_STRESS="0", SR_NATIVE_FLUSH_PM4="0", SR_NATIVE_FLUSH_CPPM4="0",
                   SR_NATIVE_CONST_DEDUP="0", SR_NATIVE_CONST_DEDUP_CHECK="0", SR_AB_TOGGLE="",
                   SR_NATIVE_SHADER_CONST_RUNS="1", SR_NATIVE_GATE="0", SR_NATIVE_CALLBACK="0",
                   SR_ALLOW_FORCING="0", SR_HDR_ADAPT_DT="0", SR_LOG="dev")
        env.update(settings)
        try:
            result = subprocess.run([str(exe), *command], cwd=root, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=3 if name == "lock_negative" else 90)
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout = 124, error.stdout or b""
            stderr = (error.stderr or b"") + b"\nTest reached its bounded process timeout\n"
        paths = [output / f"{args.label}_{name}_{stream}.txt" for stream in ("stdout", "stderr")]
        for path, data in zip(paths, (stdout, stderr)):
            with path.open("xb") as file:
                file.write(data)
        summaries = re.findall(rb"\[([A-Z_]+_TEST)\] (\d+) passed, (\d+) failed", stdout)
        observed = [{"tag": tag.decode(), "passed": int(passed), "failed": int(failed)}
                    for tag, passed, failed in summaries]
        valid = code == 0 and bool(observed) and all(s["passed"] > 0 and s["failed"] == 0 for s in observed)
        if name == "lock_negative":
            valid = code == 124 and b"[SHADER_CONST_LOCK_NEGATIVE] outer Entry acquired; entering unscoped native call" in stdout
            valid &= b"unexpectedly returned" not in stdout
        elif command[0] == "--shader-const-runs-test":
            valid &= stdout.count(b"actual tessellated SDK caller dirty=") == 14
            if command[1].startswith("indirect"):
                valid &= b"production dispatch mapping resolves the strong SDK replacement" in stdout
        row = dict(name=name, command=command, exit=code, valid=valid, summaries=observed,
                   expected_timeout=name == "lock_negative",
                   gates={k: v for k, v in env.items() if k.startswith("SR_")},
                   stdout_sha256=digest(paths[0]), stderr_sha256=digest(paths[1]))
        evidence["rows"].append(row)
        print(json.dumps(row), flush=True)
        receipt.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    evidence["unchanged_exe"] = digest(exe) == evidence["exe_sha256"]
    receipt.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return int(not evidence["unchanged_exe"] or any(not row["valid"] for row in evidence["rows"]))


if __name__ == "__main__":
    sys.exit(main())
