#!/usr/bin/env python3
"""log_stall_eval.py - grade live rolls for the asynchronous log writer (app/log-async; docs/TOOLKIT.md 3.2).

    python tools/perf/log_stall_eval.py --proof <native_proof --out folder> [--prefix lsa] [--json <out.json>]
    python tools/perf/log_stall_eval.py <run.log> [--census] [--inject-ms 5000] [--json <out.json>]
    python tools/perf/test_log_stall_eval.py                 # the rule's tests

--proof grades native_proof's log_async plan: <prefix>_census0 (SR_NATIVE_BRACKETS=1: the CP logs a [PM4_COV] F line
per frame, the line that held the CP ~4.85 s in tcmp1 census0), <prefix>_plain, and <prefix>_inject (the census roll
with SR_LOG_INJECT_STALL=5000@120: the writer sleeps 5 s inside one write in gameplay). Each roll's log is
<roll>/drive/attempt1/run.log.

THE RULE (registered with the plan before any roll). Per roll, each check PASS / FAIL with its evidence:
  witness   at least one [LOG_STALL] census line, the first within 25 s of the log's first stamp and consecutive ones
            at most 25 s apart (the writer's period is 10 s): the writer ran the whole roll. A log without them came
            from an exe without the writer; a log with only the final census did not show the writer running.
  complete  every census line's bytes= equals the raw line bytes before it (the file bytes before the line's start
            minus one per '\\n': the text-mode '\\r'), so no line before it was lost, torn or written twice; every
            census says lost=0 (no write failed for good, no emergency line went without room); the last census
            line within 25 s of the log's last stamp; census rolls: the [PM4_COV] F frame= numbers rise by exactly 1
            from line to line (none lost, none reordered).
  waits     no [LOG_STALL] producer wait with ms > --max-wait-ms (100), and the census counters agree: every census's
            max_wait_ms <= the bound (it covers the waits whose witness line found no room too), and its waits= =
            the producer-wait lines before it + its unwitnessed= (a wait the file does not show is a FAIL, not a
            pass). Every wait is listed.
  cp        census rolls: after +60 s (boot excluded, as in the tcmp diagnosis), the largest interval between two
            consecutive F lines' stamps <= --max-cp-gap-ms (1000). The CP logs one F line per frame at frameEnd
            (gfx/pm4_coverage.cpp) and the stamp is taken at the call, so the interval is the CP's frame interval
            plus whatever the log call itself cost. The old logger's stall signature was 0.7-4.9 s. Top 5 listed.
  pace      after +60 s, the largest [HIGHFPS_PACE] interval max= <= --max-pace-ms (1000). Top 5 listed.
  inject    --inject-ms N (the injected roll): exactly one [LOG_STALL] write ... injected=1 line with ms >= N (the
            stall really happened), at least one F line stamped inside its window [stamp - ms, stamp] (the CP kept
            logging frames while the writer slept), and no producer wait at all - no wait line and waits=0 in the
            last census (16 MB of queue holds 5 s of log).
The bounds are the brief's; they say nothing about other hitches, which the listed top intervals let a reader see.
Exit 0 = every check of every roll PASS, 1 = a FAIL, 2 = an input could not be read.
"""
import argparse
import datetime
import json
import os
import re
import sys

TS = re.compile(rb"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
F_LINE = re.compile(rb"\] \[PM4_COV\] F frame=(\d+) ")
PACE = re.compile(rb"\] \[HIGHFPS_PACE\] .*?interval_ms .*?max=([\d.]+)")
CENSUS = re.compile(rb"\] \[LOG_STALL\] census (\w+) .*? bytes=(\d+) ")
CFIELD = {k: re.compile(rb" " + k.encode() + rb"=([\d.]+)") for k in ("waits", "unwitnessed", "max_wait_ms", "lost")}
PWAIT = re.compile(rb"\] \[LOG_STALL\] producer wait .*? ms=([\d.]+)")
WRITE = re.compile(rb"\] \[LOG_STALL\] write .*? ms=([\d.]+)")
CENSUS_GAP_S = 25.0
LATE_S = 60.0


def stamp(line):
    m = TS.match(line)
    if not m:
        return None
    try:
        return datetime.datetime.strptime(m.group(1).decode(), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None


def grade(path, census=False, inject_ms=0, max_wait_ms=100.0, max_cp_gap_ms=1000.0, max_pace_ms=1000.0):
    """One run.log -> {"path", "checks": {name: {"verdict", "evidence"}}, "verdict"}; None if unreadable."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    lines = []  # (byte offset, raw '\n' count before it, stamp, line bytes without the newline)
    off, nl = 0, 0
    for raw in data.split(b"\n"):
        if off >= len(data):
            break
        lines.append((off, nl, stamp(raw), raw.rstrip(b"\r")))
        off += len(raw) + 1
        nl += 1
    stamps = [s for _o, _n, s, _l in lines if s]
    t0 = stamps[0] if stamps else None
    t_end = max(stamps) if stamps else None
    checks = {}

    # witness + complete (census bytes=, lost=) + the census's wait counters (for "waits")
    cens, bad_bytes, lost, recon = [], [], [], []
    wait_lines = 0
    for o, n, s, l in lines:
        if PWAIT.search(l):
            wait_lines += 1
        m = CENSUS.search(l)
        if m:
            f = {k: (float(r.search(l).group(1)) if r.search(l) else None) for k, r in CFIELD.items()}
            cens.append((s, m.group(1).decode(), f, wait_lines))
            want = int(m.group(2))
            if want != o - n:
                bad_bytes.append("census at offset %d: bytes=%d but %d raw bytes precede it" % (o, want, o - n))
            if f["lost"] is None or f["lost"] > 0:
                lost.append("census at %s: lost=%s" % (s, "?" if f["lost"] is None else int(f["lost"])))
    gaps = [(b[0] - a[0]).total_seconds() for a, b in zip(cens, cens[1:]) if a[0] and b[0]]
    worst = max(gaps) if gaps else 0.0
    first_s = (cens[0][0] - t0).total_seconds() if cens and cens[0][0] and t0 else None
    ok = bool(cens) and worst <= CENSUS_GAP_S and first_s is not None and first_s <= CENSUS_GAP_S
    checks["witness"] = {"verdict": "PASS" if ok else "FAIL", "evidence": "%d census lines, the first %s s after the "
                         "log's start, largest gap %.1f s (both <= %.0f)%s" % (
        len(cens), "-" if first_s is None else "%.1f" % first_s, worst, CENSUS_GAP_S,
        "" if cens else " - no [LOG_STALL] census line: not the async writer's exe")}
    tail_s = (t_end - cens[-1][0]).total_seconds() if cens and cens[-1][0] and t_end else None
    frames = [(int(F_LINE.search(l).group(1)), s) for _o, _n, s, l in lines if F_LINE.search(l)]
    frame_bad = [(a[0], b[0]) for a, b in zip(frames, frames[1:]) if b[0] != a[0] + 1]
    why = []
    if bad_bytes:
        why += bad_bytes[:5]
    if lost:
        why += lost[:5]
    if tail_s is None or tail_s > CENSUS_GAP_S:
        why.append("last census %s s before the log's end (<= %.0f)" % (tail_s, CENSUS_GAP_S))
    if census and not frames:
        why.append("no [PM4_COV] F line (not a census roll?)")
    if frame_bad:
        why.append("%d F frame= steps that are not +1, first %s" % (len(frame_bad), frame_bad[:3]))
    checks["complete"] = {"verdict": "FAIL" if why else "PASS", "evidence": "; ".join(why) or (
        "%d census bytes= exact, lost=0, last %.1f s before the end%s" % (
            len(cens), tail_s, ", F frames %d..%d consecutive" % (frames[0][0], frames[-1][0]) if frames else ""))}

    # waits: the wait lines, and every census's counters (a wait whose witness found no room is only counted)
    waits = [(s, float(PWAIT.search(l).group(1))) for _o, _n, s, l in lines if PWAIT.search(l)]
    over = [w for w in waits if w[1] > max_wait_ms]
    for s, _kind, f, before in cens:
        if f["waits"] is None or f["unwitnessed"] is None or f["max_wait_ms"] is None:
            recon.append("census at %s lacks waits= / unwitnessed= / max_wait_ms=" % s)
            continue
        if f["max_wait_ms"] > max_wait_ms:
            recon.append("census at %s: max_wait_ms=%.1f > %.0f" % (s, f["max_wait_ms"], max_wait_ms))
        if int(f["waits"]) != before + int(f["unwitnessed"]):
            recon.append("census at %s: waits=%d but %d wait lines before it + unwitnessed=%d" % (
                s, int(f["waits"]), before, int(f["unwitnessed"])))
    last = cens[-1][2] if cens else {}
    checks["waits"] = {"verdict": "FAIL" if over or recon else "PASS",
                       "evidence": "%d producer wait lines%s, census waits=%s unwitnessed=%s max_wait_ms=%s; over %.0f "
                                   "ms: %s%s" % (
        len(waits), (", max %.1f ms" % max(w[1] for w in waits)) if waits else "",
        *("-" if last.get(k) is None else ("%.1f" % last[k] if k == "max_wait_ms" else "%d" % last[k])
          for k in ("waits", "unwitnessed", "max_wait_ms")), max_wait_ms,
        ", ".join("%.1f ms at %s" % (w[1], w[0]) for w in over[:5]) or "none",
        ("; " + "; ".join(recon[:5])) if recon else "")}

    # cp (census rolls)
    if census:
        late = [(f, s) for f, s in frames if s and t0 and (s - t0).total_seconds() >= LATE_S]
        ivs = sorted((((b[1] - a[1]).total_seconds() * 1000.0, a[0], b[0]) for a, b in zip(late, late[1:])), reverse=True)
        top = ivs[:5]
        ok = bool(ivs) and top[0][0] <= max_cp_gap_ms
        checks["cp"] = {"verdict": "PASS" if ok else "FAIL", "evidence": "%d F intervals after +%.0f s, max %s ms (<= %.0f); top %s" % (
            len(ivs), LATE_S, ("%.1f" % top[0][0]) if top else "-", max_cp_gap_ms,
            ", ".join("%.1f ms (frame %d->%d)" % t for t in top))}

    # pace
    paces = [(float(PACE.search(l).group(1)), s) for _o, _n, s, l in lines
             if PACE.search(l) and s and t0 and (s - t0).total_seconds() >= LATE_S]
    paces.sort(key=lambda p: p[0], reverse=True)
    ok = bool(paces) and paces[0][0] <= max_pace_ms
    checks["pace"] = {"verdict": "PASS" if ok else "FAIL", "evidence": "%d [HIGHFPS_PACE] windows after +%.0f s, max %s ms (<= %.0f); top %s" % (
        len(paces), LATE_S, ("%.1f" % paces[0][0]) if paces else "-", max_pace_ms,
        ", ".join("%.1f ms at %s" % (p[0], p[1].strftime("%H:%M:%S")) for p in paces[:5]))}

    # inject
    if inject_ms:
        inj = [(s, float(WRITE.search(l).group(1))) for _o, _n, s, l in lines
               if WRITE.search(l) and b"injected=1" in l]
        why = []
        during = 0
        if len(inj) != 1:
            why.append("%d injected write lines (want exactly 1)" % len(inj))
        else:
            s, ms = inj[0]
            if ms < inject_ms:
                why.append("the injected write took %.1f ms (< %d)" % (ms, inject_ms))
            if s:
                lo = s - datetime.timedelta(milliseconds=ms)
                during = sum(1 for _f, fs in frames if fs and lo <= fs <= s)
            if not during:
                why.append("no F line stamped inside the stalled write's window (the CP stopped logging frames)")
        if waits:
            why.append("%d producer waits (want 0)" % len(waits))
        cw = last.get("waits")
        if cw is None or cw > 0:
            why.append("the last census says waits=%s (want 0)" % ("?" if cw is None else int(cw)))
        checks["inject"] = {"verdict": "FAIL" if why else "PASS", "evidence": "; ".join(why) or (
            "one injected write %.1f ms; %d F lines logged by the CP while the writer slept; no producer wait "
            "(no line, census waits=0)" % (inj[0][1], during))}
    verdict = "PASS" if all(c["verdict"] == "PASS" for c in checks.values()) else "FAIL"
    return {"path": path, "census": census, "inject_ms": inject_ms, "checks": checks, "verdict": verdict}


def report(r):
    out = ["%s %s" % (r["verdict"], r["path"])]
    for name, c in r["checks"].items():
        out.append("  %-8s %s  %s" % (name, c["verdict"], c["evidence"]))
    return "\n".join(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("log", nargs="?")
    p.add_argument("--proof")
    p.add_argument("--prefix", default="lsa")
    p.add_argument("--census", action="store_true")
    p.add_argument("--inject-ms", type=int, default=0)
    p.add_argument("--max-wait-ms", type=float, default=100.0)
    p.add_argument("--max-cp-gap-ms", type=float, default=1000.0)
    p.add_argument("--max-pace-ms", type=float, default=1000.0)
    p.add_argument("--json")
    a = p.parse_args(argv)
    if bool(a.log) == bool(a.proof):
        p.error("give one run.log or --proof <folder>")
    bounds = dict(max_wait_ms=a.max_wait_ms, max_cp_gap_ms=a.max_cp_gap_ms, max_pace_ms=a.max_pace_ms)
    if a.proof:
        rolls = [("census0", True, 0), ("plain", False, 0), ("inject", True, 5000)]
        jobs = [(os.path.join(a.proof, "%s_%s" % (a.prefix, tag), "drive", "attempt1", "run.log"), c, i)
                for tag, c, i in rolls]
    else:
        jobs = [(a.log, a.census, a.inject_ms)]
    results, rc = [], 0
    for path, census, inject in jobs:
        r = grade(path, census=census, inject_ms=inject, **bounds)
        if r is None:
            print("UNREADABLE %s" % path)
            results.append({"path": path, "verdict": "UNREADABLE"})
            rc = 2
            continue
        print(report(r))
        results.append(r)
        if r["verdict"] != "PASS" and rc == 0:
            rc = 1
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"rolls": results, "verdict": "PASS" if rc == 0 else ("UNREADABLE" if rc == 2 else "FAIL")}, f,
                      indent=1, default=str)
    print("log_stall_eval: %s" % ("PASS" if rc == 0 else ("UNREADABLE" if rc == 2 else "FAIL")))
    return rc


if __name__ == "__main__":
    sys.exit(main())
