#!/usr/bin/env python3
"""Whole-run native meter witness; does not establish era, pixel, or gameplay coverage.

Require an exact launch hash/PID, healthy complete log, graceful terminal snapshot,
all eight measured totals, monotonic counters, and seven emulation totals of zero.
The nrec column measures native work and must be positive. Earlier periodic lines
never substitute for terminal evidence. Existing native_meter.py stays informational.

python tools/perf/native_zero.py --log run.log --receipt quit.json \
  --provenance provenance.json --exe-sha256 SHA --json native_zero.json
Exit 0 PASS, 1 observed failure, 2 UNKNOWN/incomplete/unmeasured.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

import native_meter
import terminal_snapshot
try:
    import run_health
except ImportError:
    run_health = None  # A complete tooling checkout is required; never skip health.

COLS = tuple(native_meter.COLS)
ZERO_COLS = tuple(c for c in COLS if c not in native_meter.NATIVE_WORK)
U64_MAX = 2 ** 64 - 1
RUN = re.compile(r"\[NATIVE_METER_RUN\] (\w+) (\d+\.\d+)s frames=(\d+) \| (.*?) \| per_frame (.*?)\s*$")
CHANNEL = re.compile(r"\[QUIT\] channel (Local\\SR_QUIT_\S+?_(\d+)) ready")
BAD = re.compile(r"\[NATIVE_[A-Z0-9_]*BAD\]")


def memory_stale_failure(line):
    """Allow the canonical zero counter in reports, never a stale event.

    The dedicated resource grader checks complete report fields/conservation.
    This meter grader still refuses nonzero, duplicate or malformed stale
    counters and bare STALE diagnostic events anywhere in the complete log.
    """
    marker = "[MEM_RES_CHECK]"
    if marker not in line:
        return False
    body = line.split(marker, 1)[1].strip()
    occurrences = re.findall(r"\bSTALE\b", body)
    if not occurrences:
        return False
    report = re.match(r"(?:consumer_)?terminal=[01] ", body)
    counters = [token for token in body.split() if re.match(r"STALE(?:=|$)", token)]
    return not report or len(occurrences) != 1 or counters != ["STALE=0"]


def pairs(text, averages=False):
    tokens = text.split()
    result = {}
    for token in tokens:
        key, sep, value = token.partition("=")
        if not sep or key not in COLS or key in result:
            raise ValueError("unexpected or duplicate meter column")
        if value == "-":
            raise ValueError("unmeasured meter column: " + key)
        pattern = r"\d+\.\d+" if averages else r"\d+"
        if not re.fullmatch(pattern, value):
            raise ValueError("invalid meter value: " + key)
        if not averages and (len(value) > 20 or int(value) > U64_MAX):
            raise ValueError("meter value exceeds uint64: " + key)
        result[key] = value if averages else int(value)
    if set(result) != set(COLS):
        raise ValueError("missing meter columns: " + ",".join(sorted(set(COLS) - result.keys())))
    return result


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(path):
    with open(path, "rb") as stream:
        before = os.fstat(stream.fileno())
        data = stream.read(1024 * 1024 + 1)
        after = os.fstat(stream.fileno())
        if len(data) > 1024 * 1024 or run_health._identity(before) != run_health._identity(after):
            raise ValueError("changing or over-limit JSON input")
        if run_health._path_identity(after) != run_health._path_identity(os.stat(path)):
            raise ValueError("replaced JSON input")
    value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object)
    if not isinstance(value, dict):
        raise ValueError("JSON input is not an object")
    return value, hashlib.sha256(data).hexdigest()


def grade(log, receipt, provenance, exe_sha256, stranded_policy="forbid"):
    result = dict(schema="native-zero/1", verdict="UNKNOWN", passed=False, reasons=[],
                  log=str(log), exe_sha256=exe_sha256, counters=None, terminal=None,
                  stranded_policy=stranded_policy, scope="whole-run meter only; era/visual proof separate")
    failures, unknown = [], []
    try:
        if run_health is None:
            raise ValueError("required shared run_health scanner unavailable")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", exe_sha256):
            raise ValueError("expected executable SHA256 must be explicit")
        health = run_health.scan_log(str(log))
        result["health"] = health
        if not health["healthy"]:
            (failures if health["verdict"] == "FAIL" else unknown).append(health["reason"])
        if not health["scan_complete"]:
            raise ValueError("health scan incomplete")
        rec, result["receipt_sha256"] = read_json(receipt)
        prov, result["provenance_sha256"] = read_json(provenance)
        pid = prov.get("pid")
        if type(pid) is not int or pid <= 0:
            raise ValueError("launch provenance has no exact process ID")
        if not isinstance(prov.get("exeSha256"), str) or prov["exeSha256"].lower() != exe_sha256.lower():
            failures.append("launch executable hash mismatch")
        scanner = terminal_snapshot.Scanner()
        previous, terminal_rows, seen_channels = None, [], set()
        meter_lines = 0
        digest = hashlib.sha256()
        with open(log, "rb") as stream:
            before = os.fstat(stream.fileno())
            for number in range(1, 2 ** 63):
                raw = stream.readline(run_health.MAX_LINE_BYTES + 1)
                if not raw:
                    break
                if len(raw) > run_health.MAX_LINE_BYTES or stream.tell() > run_health.MAX_LOG_BYTES:
                    raise ValueError("log exceeds scanner limits")
                if not raw.endswith(b"\n"):
                    raise ValueError("unterminated final log line")
                digest.update(raw)
                line = raw.decode("utf-8-sig" if number == 1 else "utf-8", "strict")
                scanner.feed(line)
                for hit in CHANNEL.finditer(line):
                    seen_channels.add((hit.group(1), int(hit.group(2))))
                if BAD.search(line) or memory_stale_failure(line):
                    if len(failures) < 64:
                        failures.append("native invariant failure at line %d" % number)
                if "[NATIVE_METER_RUN]" not in line:
                    continue
                match = RUN.search(line)
                if not match:
                    raise ValueError("malformed meter report at line %d" % number)
                why, _, frames, totals, averages = match.groups()
                values = pairs(totals)
                pairs(averages, averages=True)  # Also reject truncated report tails.
                frame_count = int(frames)
                if frame_count > U64_MAX:
                    raise ValueError("meter frame count exceeds uint64")
                current = dict(frames=frame_count, **values)
                if previous and any(current[c] < previous[c] for c in current):
                    if len(failures) < 64:
                        failures.append("counter/frame reset or decrease at line %d" % number)
                previous = current
                meter_lines += 1
                if meter_lines > 100000:
                    raise ValueError("meter report count exceeds limit")
                if any(values[c] for c in ZERO_COLS):
                    if len(failures) < 64:
                        failures.append("nonzero emulation counters at line %d: %s" %
                                        (number, ",".join(c for c in ZERO_COLS if values[c])))
                if why == "terminal":
                    terminal_rows.append((number, current))
                    if len(terminal_rows) > 64:
                        raise ValueError("terminal meter report count exceeds limit")
            after = os.fstat(stream.fileno())
            if (run_health._identity(before) != run_health._identity(after) or
                    run_health._path_identity(after) != run_health._path_identity(os.stat(log)) or
                    digest.hexdigest() != health["sha256"]):
                raise ValueError("log changed between health and meter scans")
        result["meter_lines"] = meter_lines
        if len(seen_channels) != 1 or next(iter(seen_channels))[1] != pid:
            raise ValueError("launch PID is not uniquely bound to the log's quit channel")
        channel = next(iter(seen_channels))[0]
        ack = channel.replace("Local\\SR_QUIT_", "Local\\SR_QUIT_ACK_", 1)
        if rec.get("request") != channel or rec.get("ack") != ack:
            raise ValueError("quit receipt request/ack does not match the exact log channel")
        snap = scanner.result(str(log), rec)
        result["terminal"] = snap
        if not terminal_snapshot.accept(snap, stranded_policy) or snap.get("stranded") is None:
            raise ValueError("graceful terminal snapshot missing/rejected/unmeasured")
        if type(rec.get("stranded")) is not int or rec["stranded"] < 0 or rec["stranded"] != snap["stranded"]:
            raise ValueError("quit receipt stranded count missing/invalid or disagrees with terminal")
        rows = [(n, row) for n, row in terminal_rows if snap["request_line"] < n < snap["complete_line"]]
        if not rows or len(rows) != len(terminal_rows):
            raise ValueError("meter terminal absent or outside final quit snapshot")
        if any(row != rows[0][1] for _, row in rows):
            raise ValueError("terminal meter reports disagree")
        counters = rows[0][1]
        result["counters"] = counters
        if counters["frames"] <= 0 or counters["frames"] != snap["vdswaps"] - 1:
            raise ValueError("terminal meter/VdSwap frame coverage disagrees or is empty")
        if counters["nrec"] <= 0:
            raise ValueError("no native record work witnessed")
    except (OSError, ValueError, UnicodeError) as error:
        unknown.append(str(error))
    result["reasons"] = list(dict.fromkeys(failures + unknown))
    result["verdict"] = "FAIL" if failures else "UNKNOWN" if unknown else "PASS"
    result["passed"] = result["verdict"] == "PASS"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True)
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--provenance", required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--stranded-policy", choices=terminal_snapshot.STRANDED_POLICIES, default="forbid")
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    result = grade(args.log, args.receipt, args.provenance, args.exe_sha256, args.stranded_policy)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.json:
        Path(args.json).write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return {"PASS": 0, "FAIL": 1, "UNKNOWN": 2}[result["verdict"]]


if __name__ == "__main__":
    raise SystemExit(main())
