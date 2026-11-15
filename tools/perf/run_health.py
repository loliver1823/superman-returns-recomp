#!/usr/bin/env python3
"""Reject known runtime failures even when the game process exits zero.

    python tools/perf/run_health.py --log run.log --json run_health.json

Exit 0 means a complete, stable, nonempty log scan found no known failure;
it is not gameplay/visual acceptance. Exit 6 means failed or unknown health.
Expected NATIVE_*_BAD diagnostic controls are deliberately not failure markers.
Logs and other diagnostic artifacts are never edited or removed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

UNHEALTHY_EXIT = 6
MAX_LOG_BYTES = 2 * 1024 ** 3
MAX_LINE_BYTES = 1024 ** 2
MARKERS = [
    ("CP stall", re.compile(r"\[CP_WAIT\] stalled")),
    ("GPU hang trigger", re.compile(r"\[GPU_HANG\] ==== trigger")),
    ("thread fatal", re.compile(r"THREAD_FATAL")),
    ("native recording failure", re.compile(r"\[NATIVE_RECORDING\] FAILED\b")),
    ("indirect-call failure", re.compile(r"\[ICALL_FAIL\]")),
    ("orphaned critical section", re.compile(r"dying thread OWNS cs=.*\bORPHANED\b")),
    ("critical-section wedge", re.compile(r"\[CS_WEDGE\] cs=")),
    ("crash", re.compile(r"\[BUGCHECK\]|Guest harness: exception|\[CRASH\]")),
]
# Warnings: counted with their worst case, never a failure (Fable ruling 10, step 4 2k(4)): the GPU-hang watchdog's
# [HOST_WORK_STALL] block - the CP waited for host work on the presenting path (a dump write, a runtime shader or
# pipeline compile), the GPU did not stop. A stall that outlives its host work or reaches 10 s is still a
# [GPU_HANG] trigger, a failure above.
HOST_WORK_STALL = re.compile(r"\[HOST_WORK_STALL\] \(a\) host work (\S+) on host tid \d+ \([^)]*\) for ([\d.]+)s")


def _identity(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns


def _path_identity(stat):
    # Windows Python 3.13.0 reports change time through fstat but birth time
    # through stat in st_ctime_ns. Compare only compatible fields across APIs;
    # the full same-handle identity above still detects changes during reading.
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def scan_log(log):
    """Scan all bytes, retaining the first failure and the exact scanned hash.

    Missing/unreadable, over-limit, or changing inputs cannot be healthy. A
    bounded unterminated final line is still inspected for fatal markers.
    """
    result = dict(schema="run-health/1", log=os.path.abspath(log) if log else None,
                  verdict="UNKNOWN", healthy=False, scan_complete=False,
                  reason=None, line=None, line_number=None, bytes=0, sha256=None,
                  warnings={"host_work_stall": {"count": 0, "max_s": 0.0, "kinds": {}}})
    digest = hashlib.sha256()
    problem = None
    try:
        if not log:
            raise FileNotFoundError("no run.log")
        with open(log, "rb") as stream:
            before = os.fstat(stream.fileno())
            if before.st_size > MAX_LOG_BYTES:
                raise ValueError("run.log exceeds byte limit")
            line_number = 0
            while True:
                raw = stream.readline(MAX_LINE_BYTES + 1)
                if not raw:
                    break
                result["bytes"] += len(raw)
                digest.update(raw)
                line_number += 1
                text = raw.decode("utf-8", "replace")
                hw = HOST_WORK_STALL.search(text)
                if hw:
                    w = result["warnings"]["host_work_stall"]
                    w["count"] += 1
                    w["max_s"] = max(w["max_s"], float(hw.group(2)))
                    w["kinds"][hw.group(1)] = w["kinds"].get(hw.group(1), 0) + 1
                if result["reason"] is None:
                    for reason, regex in MARKERS:
                        if regex.search(text):
                            result.update(reason=reason, line=text.strip()[:1000], line_number=line_number)
                            break
                if len(raw) > MAX_LINE_BYTES or result["bytes"] > MAX_LOG_BYTES:
                    raise ValueError("run.log exceeds line or byte limit")
            after = os.fstat(stream.fileno())
            named = os.stat(log)
            if _identity(before) != _identity(after) or _path_identity(after) != _path_identity(named):
                raise ValueError("run.log changed during scan")
            if result["bytes"] != before.st_size:
                raise ValueError("incomplete run.log read")
            if not result["bytes"]:
                raise ValueError("empty run.log")
            result.update(scan_complete=True, sha256=digest.hexdigest())
    except FileNotFoundError:
        problem = "no run.log"
    except (OSError, ValueError) as error:
        problem = str(error)
    if result["reason"] is not None:
        result["verdict"] = "FAIL"
        if problem:
            result["scan_error"] = problem
    elif problem:
        result["reason"] = problem
    else:
        result.update(verdict="HEALTHY", healthy=True)
    return result


def write_receipt(result, path):
    Path(path).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


def scan_logs(logs):
    """A later successful attempt cannot erase an earlier runtime failure."""
    scans = [scan_log(log) for log in logs] or [scan_log(None)]
    failed = [scan for scan in scans if not scan["healthy"]]
    first = failed[0] if failed else scans[0]
    hws = [scan.get("warnings", {}).get("host_work_stall", {}) for scan in scans]
    warnings = {"host_work_stall": {"count": sum(w.get("count", 0) for w in hws),
                                    "max_s": max([w.get("max_s", 0.0) for w in hws] or [0.0])}}
    return dict(schema="run-health-set/1", healthy=not failed, verdict=first["verdict"],
                reason=first["reason"], line_number=first["line_number"], log=first["log"], scans=scans,
                warnings=warnings)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True)
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    result = scan_log(args.log)
    if args.json:
        write_receipt(result, args.json)
    print(json.dumps(result))
    return 0 if result["healthy"] else UNHEALTHY_EXIT


if __name__ == "__main__":
    raise SystemExit(main())
