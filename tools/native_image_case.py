"""Run a bounded NativeImageStore operation fixture against its production API.

python tools/native_image_case.py --case tools/cases/native_images/submitted.json
  --exe archive/binary/sr_native_image_replay.exe --out archive/image_case_001

Input cases may describe captured API operations or synthetic regressions; the
provenance label is retained, never upgraded to live-game or oracle evidence.
The runner uses WARP and does not boot the game. Success proves this sequence on
the identified executable, not completeness of a producer or a game milestone.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time

from owned_process import OwnedProcess
from set_runner import atomic, digest, no_links, read_json

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "sr-native-image-case/1"
ARITY = {"init":3, "create":8, "acquire":3, "release":2, "drop":1, "block":0,
         "unblock":0, "submit":3, "wait":3, "collect":1, "stats":4,
         "new_generation":2, "shutdown":1}


def normalize(case):
    if not isinstance(case, dict) or set(case) != {"schema", "provenance", "operations"} or case["schema"] != SCHEMA:
        raise ValueError("Expected schema, provenance, operations for sr-native-image-case/1")
    provenance = case["provenance"]
    if (not isinstance(provenance, dict) or provenance.get("kind") not in ("synthetic-regression", "captured-api-operations")
            or not isinstance(provenance.get("source"), str) or not provenance["source"].strip()):
        raise ValueError("Case must identify its origin; captured observations require their source reference")
    operations = case["operations"]
    if not isinstance(operations, list) or not 2 <= len(operations) <= 4096:
        raise ValueError("Expected 2..4096 operations")
    lines = ["SR_IMAGE_CASE 1"]
    for row in operations:
        if not isinstance(row, list) or not row or not isinstance(row[0], str) or row[0] not in ARITY or len(row) != ARITY[row[0]] + 1:
            raise ValueError("Unknown operation or wrong argument count")
        tokens = []
        for value in row:
            if type(value) not in (str, int, float):
                raise ValueError("Operation arguments must be strings or finite numbers")
            token = str(value)
            if len(token) > 64 or not re.fullmatch(r"[A-Za-z0-9_.+-]+", token) or token.lower() in ("nan", "inf", "+inf", "-inf"):
                raise ValueError("Invalid operation token")
            tokens.append(token)
        lines.append(" ".join(tokens))
    if operations[0][0] != "init" or operations[-1][0] != "shutdown":
        raise ValueError("Case must start with init and end with shutdown")
    return "\n".join(lines) + "\n"


def grade(text, operations):
    lines = text.splitlines()
    if not text.endswith("\n") or len(lines) != len(operations) + 1:
        raise ValueError("Incomplete replay observation journal")
    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicate observation field")
            result[key] = value
        return result
    observations = [json.loads(line, object_pairs_hook=unique) for line in lines]
    terminal = observations[-1]
    if (not isinstance(terminal, dict) or terminal != {"complete": True, "steps": len(operations)}
            or terminal.get("complete") is not True or type(terminal.get("steps")) is not int):
        raise ValueError("Missing terminal completion/count")
    for i, (observed, operation) in enumerate(zip(observations, operations), 1):
        if (not isinstance(observed, dict) or set(observed) != {"step","operation","hr","live","pending","colors","depths"}
                or type(observed["step"]) is not int or observed["step"] != i or observed["operation"] != operation[0]
                or any(type(observed[k]) is not int or observed[k] < 0 for k in ("hr","live","pending","colors","depths"))):
            raise ValueError("Replay observations do not cover the exact supplied operations")
    return observations


def run(case_path, exe, out, timeout=30):
    case_path, exe = case_path.resolve(strict=True), exe.resolve(strict=True)
    no_links(out.absolute())
    if out.exists():
        raise ValueError("Output directory must be new")
    case_pin, exe_pin = digest(case_path), digest(exe)
    case = read_json(case_path, max_bytes=1024**2)
    text = normalize(case)
    out.mkdir(parents=True)
    (out / "temp").mkdir()
    operations_path = out / "operations.txt"
    operations_path.write_text(text, encoding="ascii", newline="\n")
    operations_pin = digest(operations_path)
    receipt = {"schema":"sr-native-image-result/1", "status":"unknown", "case":str(case_path),
               "case_pin":case_pin, "operations_pin":operations_pin,
               "executable":str(exe), "executable_pin":exe_pin,
               "provenance":case["provenance"], "steps":len(case["operations"]), "tree_empty":False,
               "limits":{"timeout_seconds":timeout,"memory_mb":2048,"processes":4}}
    child = None
    started = time.monotonic()
    try:
        env = dict(os.environ, TEMP=str(out / "temp"), TMP=str(out / "temp"))
        child = OwnedProcess([str(exe), str(operations_path.resolve())], str(ROOT), env,
                             out / "stdout.jsonl", out / "stderr.txt", memory_limit_mb=2048, process_limit=4)
        while child.active_count():
            if time.monotonic() - started > timeout:
                receipt["timeout"] = True
                child.terminate(124)
                break
            time.sleep(.02)
        receipt["tree_empty"] = child.settle(5)
        receipt["exit_code"] = child.poll()
        child.close()
        child = None
        if not receipt["tree_empty"] or receipt.get("timeout") or receipt["exit_code"] != 0:
            raise ValueError("Native replay failed, timed out, or left unverified descendants")
        if digest(case_path) != case_pin or digest(exe) != exe_pin or digest(operations_path) != operations_pin:
            raise ValueError("Input changed during replay")
        if (out / "stdout.jsonl").stat().st_size > 4 * 1024**2:
            raise ValueError("Observation budget exceeded")
        observations = grade((out / "stdout.jsonl").read_text(encoding="utf-8"), case["operations"])
        atomic(out / "observations.json", observations)
        receipt.update(status="passed", observations=digest(out / "observations.json"))
        return 0
    except BaseException as error:
        receipt["error"] = str(error)
        raise
    finally:
        if child:
            try:
                child.terminate(1)
                receipt["tree_empty"] = child.settle(5)
            finally:
                child.close()
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 4)
        atomic(out / "receipt.json", receipt)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", type=Path, required=True)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    try:
        return run(a.case, a.exe, a.out)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"native_image_case: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
