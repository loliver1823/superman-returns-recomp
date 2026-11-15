"""Shared roll-health check: recovered dump stalls are VOID, never passing proof.

The narrow exception requires swap-phase H1, a completed present dump inside
[trigger - flat, max(trigger, diagnostic end) + 10s], and later swap progress.
Other phases, relogs, malformed diagnostics and other abort markers fail.
"""
import datetime
import math
import re
from run_health import MARKERS

LOG_TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
HANG_TRIGGER = re.compile(
    r"\[GPU_HANG\] ==== trigger (\w+)\s*\| episode key=(\w+) relog=(\d+) \|"
    r".*? flat ([\d.]+)s \| presents=.*XE_SWAP #(\d+)\)")
HANG_PHASE = re.compile(r"\[GPU_HANG\] \(e\) CP phase=(\S+) for ([\d.]+)s")
PERF_SWAPS = re.compile(r"\[PERF\] t=\d+s .*\(swaps=(\d+) ")
DUMP_WRITE = re.compile(r"\[PRESENT_DUMP\] (?!WRITE FAILED ).+\.bmp(?:\s+nonBlackPx=|\s*$)")
PHASE_WITHIN_S = 40.0
ABORT = [(why, regex) for why, regex in MARKERS if why != "GPU hang trigger"]


def timestamp(line):
    match = LOG_TS.match(line)
    try:
        return datetime.datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f") if match else None
    except ValueError:
        return None


def review_gpu_hangs(log):
    """Stream one exact log path; return status, trigger count and evidence."""
    episodes, dumps, perfs, failures = [], [], [], []
    current = None
    triggers = 0
    try:
        with open(log, encoding="utf-8", errors="replace") as source:
            for line in source:
                for why, regex in ABORT:
                    if regex.search(line):
                        failures.append("%s: %s" % (why, line.strip()[:200]))
                if "[GPU_HANG] ==== trigger" in line:
                    triggers += 1
                    match = HANG_TRIGGER.search(line)
                    current = None
                    if not match or not timestamp(line):
                        failures.append("malformed GPU_HANG trigger: " + line.strip()[:220])
                        continue
                    family, key, relog, flat, swaps = match.groups()
                    if int(relog):
                        failures.append("episode %s re-logged (not recovered)" % key)
                        continue
                    try:
                        flat = float(flat)
                        if not math.isfinite(flat):
                            raise ValueError("nonfinite stall duration")
                        timestamp(line) - datetime.timedelta(seconds=flat)
                    except (ValueError, OverflowError):
                        failures.append("invalid stall duration: " + line.strip()[:220])
                        continue
                    current = {"t": timestamp(line), "family": family, "flat": flat,
                               "xe": int(swaps), "phase": None, "end": None, "line": line.strip()[:220]}
                    episodes.append(current)
                elif "[GPU_HANG]" in line and current:
                    when = timestamp(line)
                    if when and 0 <= (when - current["t"]).total_seconds() <= PHASE_WITHIN_S:
                        phase = HANG_PHASE.search(line)
                        if phase and current["phase"] is None:
                            current["phase"] = phase.group(1)
                        if "[GPU_HANG] ==== end" in line:
                            current["end"] = when
                            current = None
                elif DUMP_WRITE.search(line):
                    dumps.append((timestamp(line), line.strip()[:200]))
                elif "[PERF]" in line:
                    match = PERF_SWAPS.search(line)
                    if match:
                        perfs.append((timestamp(line), int(match.group(1))))
    except (OSError, TypeError) as error:
        return {"status": "FAIL", "triggers": triggers, "evidence": ["unreadable run.log: " + str(error)]}
    if failures:
        return {"status": "FAIL", "triggers": triggers, "evidence": failures}
    if not episodes:
        return {"status": "PASS", "triggers": 0, "evidence": ["no GPU_HANG episode"]}
    evidence = []
    for episode in episodes:
        why = None
        low = episode["t"] - datetime.timedelta(seconds=episode["flat"])
        high = max(episode["t"], episode["end"] or episode["t"]) + datetime.timedelta(seconds=10)
        hits = [item for item in dumps if item[0] and low <= item[0] <= high]
        recovered = [item for item in perfs if item[0] and item[0] > episode["t"] and item[1] > episode["xe"]]
        if episode["family"] != "H1":
            why = "%s is not a dump-stall H1" % episode["family"]
        elif episode["phase"] != "swap":
            why = "CP phase %s, not swap" % episode["phase"]
        elif not hits:
            why = "no [PRESENT_DUMP] write in the stall window"
        elif not recovered:
            why = "not recovered (no later [PERF] past XE_SWAP #%d)" % episode["xe"]
        if why:
            return {"status": "FAIL", "triggers": triggers, "evidence": [why + ": " + episode["line"]]}
        evidence.extend(["hang: " + episode["line"], "dump in the window: " + hits[0][1],
                         "recovered: [PERF] swaps=%d at %s > XE_SWAP #%d" %
                         (recovered[0][1], recovered[0][0].strftime("%H:%M:%S"), episode["xe"])])
    return {"status": "VOID", "triggers": triggers, "evidence": evidence}


def dump_stall_void(log):
    health = review_gpu_hangs(log)
    return health["status"] == "VOID", health["evidence"]
