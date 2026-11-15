"""Run isolated complete-present-gate comparisons against the actual SDK.

Usage: python tools/difftest/present_gate_sdk_test_run.py --out archive/<bank>
The recording-pin fixtures prove SDK emission, return state, production dispatch
and two real segmentless replays. They do not prove live presentation pacing.
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
    parser.add_argument("--label", default="present_gate_sdk")
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
    for route in ("leaf", "direct", "indirect", "oracle"):
        for mode, settings in (
            ("record", {}),
            ("literal", {"SR_NATIVE_FLUSH_PM4": "1"}),
            ("reconstructed", {"SR_NATIVE_FLUSH_CPPM4": "1"}),
        ):
            cases.append((f"{route}_{mode}", ["--present-gate-sdk-test", route], settings))
    for name, settings in (
        ("gate_off", {"SR_NATIVE_GATE": "0"}),
        ("gate_on", {}),
        ("flush_off", {"SR_NATIVE_FLUSH": "0"}),
        ("segmentless_off", {"SR_NATIVE_SEGMENTLESS": "0"}),
        ("dry_check", {"SR_NATIVE_FLUSH": "0", "SR_NATIVE_FLUSH_CHECK": "1"}),
    ):
        cases.append((name, ["--present-gate-test"], settings))
    cases.append(("wrong_return_control", ["--present-gate-sdk-test", "fault-return"], {}))
    evidence = {"exe": str(exe), "exe_sha256": digest(exe), "tool_sha256": digest(Path(__file__)),
                "claim": "offline complete gate and recording-pin contract only", "rows": []}
    for name, command, settings in cases:
        env = {k: v for k, v in os.environ.items() if not k.startswith("SR_")}
        env.update(SR_NATIVE_FLUSH="1", SR_NATIVE_FLUSH_CHECK="0", SR_NATIVE_SEGMENTLESS="1",
                   SR_NATIVE_SEG_STRESS="0", SR_NATIVE_FLUSH_PM4="0", SR_NATIVE_FLUSH_CPPM4="0",
                   SR_NATIVE_CONST_DEDUP="0", SR_NATIVE_CONST_DEDUP_CHECK="0", SR_AB_TOGGLE="",
                   SR_NATIVE_SHADER_CONST_RUNS="0", SR_NATIVE_GATE="1", SR_NATIVE_CALLBACK="1",
                   SR_NATIVE_DEVCREATE="0", SR_ALLOW_FORCING="0", SR_HDR_ADAPT_DT="0", SR_LOG="dev")
        env.update(settings)
        try:
            result = subprocess.run([str(exe), *command], cwd=root, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
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
        if command[0] == "--present-gate-sdk-test":
            valid &= stdout.count(b"SDK route=") == 28
            if command[1] == "indirect":
                valid &= b"production dispatch mapping resolves the strong present-gate replacement" in stdout
        if name == "wrong_return_control":
            valid = code == 1 and len(observed) == 1 and observed[0]["passed"] == 24 and observed[0]["failed"] == 4
            valid &= stdout.count(b"FAIL: SDK route=fault-return") == 4
            valid &= stdout.count(b"FAIL: SDK route=fault-return interval=1 perf=2 writer=00000000 reader=0 threshold=16") == 4
        row = dict(name=name, command=command, exit=code, valid=valid, summaries=observed,
                   expected_failure=name == "wrong_return_control",
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
