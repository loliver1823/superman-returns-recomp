#!/usr/bin/env python3
"""meter_delta.py - the native meter per ingame frame, roll against roll (iter-467 native-a P1).

    python tools/perf/meter_delta.py <rollA> <rollB> [--repo <main checkout>]

For each perf_run roll directory: the `[NATIVE_METER] w=` window lines whose timestamp lies in the
ingame era (perf_report's DEFAULT_ERAS markers, as flicker_gate.py uses them) are summed, and the
per-frame pm4 / nrec / edram counts are printed, then B minus A. Windows before the ingame marker
(boot, movies, title, menu, loading) are excluded so that two rolls with different boot lengths
compare the same thing. Quote these numbers, never the whole-run `[NATIVE_METER_RUN]` average.
"""
import argparse
import datetime
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import perf_report  # noqa: E402

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[NATIVE_METER\] w=(\d+) [\d.]+s frames=(\d+) \|.*\| window (.*)$")
KINDS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[NATIVE_METER_KINDS\] w=(\d+) frames=(\d+) \| pm4 per_frame by kind: (.*)$")
KV = re.compile(r"(\w+)=(\d+)")
KVF = re.compile(r"(\w+)=([\d.]+)")
# The columns the summary prints (every column of the window line is parsed and kept in `per`; the
# ledger reads any by name). dispreg = display-register answers (native_meter.hpp, RELEASE_PLAN C1); shm = the
# shared-memory page model's page validations (ledger mem.resources).
SHOW = ("pm4", "nrec", "edram_res", "edram_xfer", "edram_load", "dispreg", "shm", "xma")


def run_log(d):
    p = os.path.join(d, "drive", "attempt1", "run.log")
    return p if os.path.exists(p) else None


def ingame_start_ms(d, log):
    meta = perf_report.read_meta(os.path.join(d, "perf"))
    t0 = float(meta.get("epoch_ms_t0", "0") or 0)
    if not t0:
        return None
    for era, t in perf_report.log_markers(log, perf_report.DEFAULT_ERAS, t0, None):
        if era == "ingame":
            return t0 + t / 1000.0
    return None


def windows(d):
    log = run_log(d)
    if not log:
        return None, []
    start = ingame_start_ms(d, log)
    rows = []
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            m = LINE.match(line)
            k = None if m else KINDS.match(line)
            if not m and not k:
                continue
            mm = m or k
            ts = datetime.datetime.strptime(mm.group(1), "%Y-%m-%d %H:%M:%S.%f")
            ms = ts.timestamp() * 1000.0
            if start is not None and ms < start:
                continue
            frames = int(mm.group(3))
            if m:
                counts = {k2: int(v) for k2, v in KV.findall(m.group(4))}
            else:  # the kinds line carries per-frame averages: back to window totals
                counts = {"kind_" + k2: float(v) * frames for k2, v in KVF.findall(k.group(4))}
                frames = 0  # the window's frames are counted by its [NATIVE_METER] line
            rows.append((int(mm.group(2)), frames, counts))
    return start, rows


def summarize(rows):
    """(frames, {column: per-frame value}) over the selected windows. A column is reported only when EVERY selected
    [NATIVE_METER] window carries it numerically (Astra round 1 on release/mem-meter): a window that lacks a column -
    an exe before the column existed, or an unwired '-' - would otherwise read as 0 over the whole run, and the ledger's
    meter items read this dict (an absent column is 'meter field absent', a FAIL, never a 0). The kind rows
    ([NATIVE_METER_KINDS], frames 0, kind_* keys) are summed on their own."""
    frames = sum(r[1] for r in rows)
    tot, seen = {}, {}
    meter_rows = 0
    for _, _, c in rows:
        is_meter = any(not k.startswith("kind_") for k in c)
        if is_meter:
            meter_rows += 1
        for k, v in c.items():
            tot[k] = tot.get(k, 0) + v
            if is_meter:
                seen[k] = seen.get(k, 0) + 1
    per = {}
    for k, v in tot.items():
        if not k.startswith("kind_") and seen.get(k, 0) != meter_rows:
            continue  # missing from at least one selected window: not a measurement of the run
        per[k] = v / frames if frames else 0.0
    return frames, per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    a = ap.parse_args()
    out = []
    res = {}
    for tag, d in (("A", a.a), ("B", a.b)):
        start, rows = windows(d)
        frames, per = summarize(rows)
        res[tag] = per
        out.append("%s %s: ingame windows=%d frames=%d%s | per_frame %s" % (
            tag, d, len(rows), frames, "" if start is not None else " (NO ingame marker: whole run)",
            " ".join("%s=%.1f" % (k, per[k]) for k in SHOW if k in per)))
    keys = [k for k in SHOW if k in res["A"] and k in res["B"]]
    out.append("B - A per_frame: " + " ".join("%s=%+.1f" % (k, res["B"][k] - res["A"][k]) for k in keys))
    kinds = [k for k in res["A"] if k.startswith("kind_") and k in res["B"]]
    if kinds:
        out.append("by kind (A -> B per_frame): " + " ".join(
            "%s=%.1f->%.1f" % (k[5:], res["A"][k], res["B"][k]) for k in kinds))
    print("\n".join(out))


if __name__ == "__main__":
    main()
