#!/usr/bin/env python3
"""flush_split_snapshot.py - p1.flush_residue's counters from boot THROUGH THE LAST SNAPSHOT of a roll's run.log (Astra
rounds 1-3 on native/flush-residue).

    python tools/perf/flush_split_snapshot.py <run.log> --json <out.json>
    python tools/perf/flush_split_snapshot.py --self-test

Under SR_NATIVE_FLUSH_SPLIT the game prints `[NATIVE_FLUSH] split snapshot #<n> period=<ms>ms ...` on the first draw at
least <period> after the previous line (kernel/native_flush.cpp splitSnapshot; the clock read on every draw): the S1
flush's declines the gate must leave at 0 and row 9's per-entry record counters, all cumulative from boot. The line is
NOT terminal: the rolls end by Stop-Process (sr_drive has no quit hook; a graceful-quit hook is acceptance.zero's own
infra item), so the counters are known only up to the LAST line. What this tool grades, exactly:
  - the counters from boot through the last snapshot (`last`, cumulative: zero there means zero from boot to last_ts);
  - freshness relative to the LAST LOGGED TIMESTAMP, not to the process's termination: `tail_s` = the log's last
    timestamped line minus the last snapshot, and `fresh` = tail_s <= fresh_bound_s = period_s + FRESH_SLACK_S
    (1.0 s at the game's 0.5 s period; the self-test pins it: the bound passes, the bound + 1 ms fails);
  - the remaining execution tail - after the last snapshot, and after the last logged line - is unmeasured: nothing
    here grades it, so nothing here is "zero for the whole run".
Every earlier snapshot is checked too (`ever_nonzero`) and the counters must never fall (`monotone`).

The json (dot-free keys, for the ledger's json checks):
    snapshots, period_s, fresh_bound_s, first_ts, last_ts, log_end_ts, tail_s, fresh, monotone, ever_nonzero
    (+ ever_nonzero_n)
    last.{draws, count_ffff, ipp0, nosplit, convertible_declined, fallbacks, aborts, after_planning, big_draws,
          big_parts, sec_const, sec_reg, sec_fetch}           (the last snapshot's counters: boot .. last_ts)
    last.<entry>.{calls, taken, open, pre, live, kickT, kickTStop, waitT, calls_eq_taken}   (entry: resolve, clearF,
                  gpr; waitT = taken through a modelled GPU wait - information, not graded; a line without it reads None)
    strict_ok  - every strict condition (boot through the last snapshot, fresh as above) at once (a convenience; the
                 ledger checks the fields)
Exit 0 with a json written (also when strict_ok is false); 2 when the log is missing or holds no snapshot.
"""
import argparse
import datetime
import json
import re
import sys

SNAP = re.compile(r"\[NATIVE_FLUSH\] split snapshot #(\d+) period=(\d+)ms (.*)$")
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
NUM = re.compile(r"([A-Za-z>0-9]+)=(\d+)")
ENTRIES = ("resolve", "clearF", "gpr")
HEAD_KEYS = {"draws": "draws", "count>0xFFFF": "count_ffff", "ipp0": "ipp0", "noSplit": "nosplit",
             "sectionFallbacks": "fallbacks", "sectionAborts": "aborts", "afterPlanning": "after_planning"}
# The freshness bound beyond the reporting period (log / scheduling slack): fresh = tail_s <= period_s + FRESH_SLACK_S.
FRESH_SLACK_S = 0.5


def ts_of(line):
    m = TS.match(line)
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def parse_snapshot(rest):
    """The fields after `period=`: head counters, `big draws= parts=`, `sections const= reg= fetch=`, three entries."""
    parts = [p.strip() for p in rest.split("|")]
    out = {}
    for k, v in NUM.findall(parts[0]):
        if k in HEAD_KEYS:
            out[HEAD_KEYS[k]] = int(v)
    for p in parts[1:]:
        words = p.split()
        if not words:
            continue
        fields = {k: int(v) for k, v in NUM.findall(p)}
        if words[0] == "big":
            out["big_draws"], out["big_parts"] = fields.get("draws"), fields.get("parts")
        elif words[0] == "sections":
            out["sec_const"], out["sec_reg"], out["sec_fetch"] = fields.get("const"), fields.get("reg"), fields.get("fetch")
        elif words[0] in ENTRIES:
            e = {k: fields.get(k) for k in ("calls", "taken", "open", "pre", "live", "kickT", "kickTStop", "waitT")}
            e["calls_eq_taken"] = e["calls"] is not None and e["calls"] == e["taken"]
            out[words[0]] = e
    if all(k in out for k in ("count_ffff", "ipp0", "nosplit")):
        out["convertible_declined"] = out["count_ffff"] - out["ipp0"] - out["nosplit"]
    return out


def flat(d, prefix=""):
    """{dotted key: number} of the numeric leaves (the monotone check)."""
    r = {}
    for k, v in d.items():
        if isinstance(v, dict):
            r.update(flat(v, prefix + k + "."))
        elif isinstance(v, int) and not isinstance(v, bool):
            r[prefix + k] = v
    return r


def residue_nonzero(s):
    bad = [k for k in ("convertible_declined", "fallbacks", "aborts") if s.get(k)]
    for e in ENTRIES:
        x = s.get(e) or {}
        bad += ["%s.%s" % (e, k) for k in ("open", "pre", "live", "kickTStop") if x.get(k)]
        if x and not x.get("calls_eq_taken"):
            bad.append(e + ".calls_ne_taken")
    return bad


def analyse(lines):
    snaps, last_ts = [], None
    for ln in lines:
        t = ts_of(ln)
        if t:
            last_ts = t
        m = SNAP.search(ln)
        if m:
            snaps.append((t, int(m.group(2)), parse_snapshot(m.group(3))))
    if not snaps:
        return {"error": "no [NATIVE_FLUSH] split snapshot line"}
    period = snaps[-1][1] / 1000.0
    first_ts, snap_ts, last = snaps[0][0], snaps[-1][0], snaps[-1][2]
    tail = (last_ts - snap_ts).total_seconds() if (last_ts and snap_ts) else None
    monotone = True
    prev = None
    for _, _, s in snaps:
        f = flat(s)
        if prev is not None and any(k in prev and v < prev[k] for k, v in f.items()):
            monotone = False
        prev = f
    ever = sorted({b for _, _, s in snaps for b in residue_nonzero(s)})
    bound = period + FRESH_SLACK_S  # relative to the last LOGGED timestamp; the execution tail after it is unmeasured
    fresh = tail is not None and tail <= bound
    strict = (fresh and monotone and not ever and not residue_nonzero(last) and
              all((last.get(e) or {}).get("calls", 0) > 0 for e in ENTRIES))
    fmt = lambda t: t.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] if t else None
    return {"snapshots": len(snaps), "period_s": period, "fresh_bound_s": bound, "first_ts": fmt(first_ts),
            "last_ts": fmt(snap_ts), "log_end_ts": fmt(last_ts), "tail_s": tail, "fresh": fresh, "monotone": monotone,
            "ever_nonzero": ever, "ever_nonzero_n": len(ever), "last": last, "strict_ok": strict}


SAMPLE = ("[2026-10-14 12:00:{s:02d}.{ms:03d}] [INFO ] [BOOT] [NATIVE_FLUSH] split snapshot #{n} period=500ms draws={d} "
          "count>0xFFFF={c} ipp0=0 noSplit={o} sectionFallbacks={f} sectionAborts=0 afterPlanning=0 | big draws=3 "
          "parts=7 | sections const=5 reg=1 fetch=1 | resolve calls={rc} taken={rt} open=0 pre=0 live=0 kickT=2 "
          "kickTStop=0 waitT=0 | clearF calls=4 taken=4 open=0 pre=0 live=0 kickT=1 kickTStop=0 waitT=1 | gpr calls=3 "
          "taken=3 open=0 pre=0 live=0 kickT=1 kickTStop=0 waitT=0")


def self_test():
    def log(rows, end_s, end_ms=0):
        out = [SAMPLE.format(**r) for r in rows]
        out.append("[2026-10-14 12:00:%02d.%03d] [INFO ] [BOOT] [PM4_COV] F frame=1" % (end_s, end_ms))
        return out
    good = [dict(s=1, ms=0, n=1, d=10, c=0, o=0, f=0, rc=5, rt=5), dict(s=1, ms=500, n=2, d=20, c=1, o=1, f=0, rc=6, rt=6)]
    # (name, lines, strict_ok, fresh) - the last snapshot is at 12:00:01.500 with period 500 ms: the bound is 1.0 s
    cases = [
        ("clean, fresh", log(good, 2), True, True),
        ("the tail exactly at the bound (1.000 s after the last snapshot)", log(good, 2, 500), True, True),
        ("the tail 1 ms past the bound (1.001 s)", log(good, 2, 501), False, False),
        ("stale: the last snapshot 3.5 s before the last logged line", log(good, 5), False, False),
        ("a section fallback", log(good[:1] + [dict(good[1], f=1)], 2), False, True),
        ("a convertible >0xFFFF decline", log(good[:1] + [dict(good[1], c=2)], 2), False, True),
        ("a row-9 call not taken", log(good[:1] + [dict(good[1], rc=7)], 2), False, True),
        ("a counter going down", log([dict(good[0], d=30)] + good[1:], 2), False, True),
        ("an earlier nonzero snapshot", log([dict(good[0], f=1)] + good[1:], 2), False, True),
    ]
    bad = 0
    for name, lines, want, want_fresh in cases:
        d = analyse(lines)
        ok = d.get("strict_ok") == want and d.get("fresh") == want_fresh and d.get("fresh_bound_s") == 1.0
        bad += not ok
        print("%s %s (strict_ok %s, fresh %s, tail_s %s, bound %s)" % ("ok  " if ok else "FAIL", name, d.get("strict_ok"),
                                                                       d.get("fresh"), d.get("tail_s"),
                                                                       d.get("fresh_bound_s")))
    if analyse(["[2026-10-14 12:00:01.000] [INFO ] nothing"]).get("error") is None:
        bad += 1
        print("FAIL no snapshot must be an error")
    last = analyse(log(good, 2)).get("last") or {}
    w = [(last.get(e) or {}).get("waitT") for e in ENTRIES]
    if w != [0, 1, 0]:
        bad += 1
        print("FAIL waitT per entry parsed as %s (want [0, 1, 0])" % w)
    print("[FLUSH_SPLIT_SNAPSHOT_SELFTEST] %d cases, %d failed" % (len(cases) + 2, bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?")
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    try:
        with open(a.log, "rb") as f:
            lines = f.read().decode("utf-8", "replace").splitlines()
    except (OSError, TypeError) as e:
        print("flush_split_snapshot: %s" % e)
        return 2
    d = analyse(lines)
    d["log"] = a.log
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=1)
    print(json.dumps({k: d.get(k) for k in ("snapshots", "tail_s", "fresh", "monotone", "ever_nonzero", "strict_ok",
                                            "error")}))
    return 2 if "error" in d else 0


if __name__ == "__main__":
    sys.exit(main())
