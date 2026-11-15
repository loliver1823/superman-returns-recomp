#!/usr/bin/env python3
"""owner_census.py - exact packet-key counts from the [PM4_COVMAP] PRODUCER census (SR_NATIVE_BRACKETS=1), and the
base / target comparison boot.device_create's census steps grade (dcb, 2026-10-14; ported to the merged census of
native/residue-census, dev 6528ffef / 4f6c7972).

    python tools/perf/owner_census.py <roll> [--producer 00000000] [--name UNSTAMPED]   # that producer's last keys
    python tools/perf/owner_census.py --base <census0 roll> --target <census1 roll> --json <out.json>

A roll is a perf_run folder (drive/attempt1/run.log) or a run.log. The census prints cumulative lines per producer,
`[PM4_COVMAP] <why> inner=<va> name=<n> pk=.. dw=.. keys=<key>:<count>,...` (gfx/pm4_coverage: Stamp::inner, the
innermost table scope that wrote the bytes; XX = type-3 opcode, rXX = type-0 register page); each producer's LAST line
is its count OVER THE CENSUS-COVERED INTERVAL - not the whole run: the census reports periodically (every 256 frames or
20 s, gfx/pm4_coverage.cpp frameEnd) with no final report (the harness ends the game with Stop-Process -Force), so the
frames after the last report are outside it (Astra round 3: rcen1's logs end 155 and 207 frames after their last
census). The covered interval is the frames= of the [PM4_COV] CUM line printed with those lines; the uncovered TAIL is
reported in frames (the [PM4_COV] F lines after it - one per completed frame, every producer together, unattributed)
and seconds (that report to the log's last line). The `owner=` lines beside them (the outermost call's) are not read.
inner=00000000
name=UNSTAMPED = packets no stamp claims; RING_UNSTAMPED shares the VA. A log with no `inner=` line (a census before
native/residue-census, e.g. dc5c) reads as no census: its counts are not comparable with these.

The comparison (--base/--target), the type-0 pair the kind meter cannot isolate. In the merged census 820F0048's
packets sit on two lines: its per-frame call under its caller Swap (inner=82112050; 820F0048 is no table scope) and its
boot call UNSTAMPED (D3DDeviceCreate 820F34C0's bracket opens before the device exists, so nothing it writes is
stamped). rcen1 census0 (exe 6705f9e06683): opcode 63 only there.
  * opcode 63 (SET_BIN_SELECT_HI): 820F0048's, one per call;
  * page r0F (RB_BC_CONTROL 0x0F01): 820F0048's (one per call) AND 82103238's (called from 821058B0 at device
    creation, UNSTAMPED; Astra 2026-10-14 after dc5c's census0 first line r0F:2 with 63:1). So the base's residual r0F
    (not 820F0048's) = base r0F - base 63, over the whole census and on the UNSTAMPED line alone, and the target's r0F
    is required to equal it on both.
That r0F check is an AGGREGATE RESIDUAL CONSISTENCY CHECK, not a producer witness: equal counts are consistent with
820F0048's r0F gone and the residual unchanged, conditional on the residual being equal in both runs. They do not
say which producer wrote the target's r0F - 82103238's write lost while one devcreate r0F stays unstamped gives the
same counts (Astra round 2, 2026-10-14). A table row for 82103238 would not stamp it: its write falls inside the
device-less CreateDevice bracket, before the first point the bracket can read the device (the flush's ring write).
The equal-residual condition has static support only (IDA, read-only): 82103238's one code caller is 821058B0, whose
callers 82106010 / 82106280 are reached only from D3DDeviceCreate 820F34C0 - the title device (RenderDeviceInit
82621258) and the dead flags-2 recorder 82304C38; no reset path re-runs it. Once per title-device creation, either arm.
Every count claim is OVER THE CENSUS-COVERED INTERVAL; the checks also require each roll's last report within one
reporting period of its run's end (tail < 256 frames and <= 21 s, the F lines reaching the report), the F records
contiguous and valid from frame 1 through the last (Astra round 4: a gap anywhere, the tail included, is INCOMPLETE and
fails), every comparable key reconciled between the F lines and the census over the covered frames, and the target's
tail frames (unattributed F lines) to hold no opcode 63 and no RB_BC_CONTROL (the residual is a boot write, frame 1).
JSON fields: base_roll, target_roll (as passed), base_census / target_census (any inner= line), base.total.<key> and
base.unstamped.<key> (0 when absent; total = every producer's last line summed), base.holders_63 (producers with
opcode 63), the same under target, base_coverage / target_coverage (census_frames, census_ts, last_frame, end_ts,
tail_frames, tail_seconds, tail.<key> from the F lines, within_period), r0F_other = base total r0F - base total 63,
r0F_other_unstamped (the same on UNSTAMPED), and the checks base_63_exercised (> 0), target_63_zero (every producer
line), target_r0F_equals_other, target_unstamped_r0F_equals_other, base_census_covers_run, target_census_covers_run,
frames_match_census (both rolls: complete F records summing to the census totals for EVERY comparable opcode /
page key - keys_compared, keys_mismatched; the census's other-class oN keys have no F counterpart: keys_not_comparable),
frames_complete (both rolls: exactly one valid F record for every frame 1..last_frame, else INCOMPLETE -
frames_incomplete names the gaps),
target_tail_clean. Exit 0 = PASS, 1 = FAIL (the JSON still written). Read-only apart from --json.

--require-terminal (tools/graceful-quit, docs/TOOLKIT.md 1.8m): grade the WHOLE run. A roll ended by the graceful quit
(sr_drive -GracefulQuit) prints the census once more at its end with the why token `terminal`, after the CP drained:
its last lines are then that terminal report, the tail is 0 frames and the counts cover frames 1..N (plus the partial
frame N+1 the drain ended in, printed as the last F line). Read without the option, such a log grades as any other (the
last report simply covers everything). With it, the checks base_census_terminal / target_census_terminal are added and
the producers graded are the TERMINAL rows only: inner= lines with the why token `terminal` strictly between the
snapshot's request and complete lines of a complete snapshot ([QUIT] terminal snapshot complete AND the quit receipt of
a clean exit 0, tools/perf/terminal_snapshot.py), with no frame, census row or CUM line after the complete line, no
terminal row outside the snapshot, and no producer whose rows are all periodic; a roll without that (every roll ended
by Stop-Process) FAILS. coverage.census_why, coverage.terminal (the snapshot's status / frames / tail / waiting /
families / exit_reason), coverage.terminal_rows, coverage.rows_outside_snapshot,
coverage.producers_without_terminal_row and coverage.census_is_terminal are reported for every roll. Without the
option nothing else changes.
"""
import argparse
import datetime
import json
import os
import re
import sys

import terminal_snapshot as ts  # tools/graceful-quit: the [QUIT] terminal snapshot reader (same folder)

LINE = re.compile(r"\[PM4_COVMAP\] \S+ inner=([0-9A-Fa-f]{8}) name=(\S+) pk=(\d+) dw=(\d+) keys=(\S*)")
LINE_WHY = re.compile(r"\[PM4_COVMAP\] (\S+) inner=")  # the row's why token (periodic / terminal)
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
CUM = re.compile(r"\[PM4_COV\] CUM \S+ frames=(\d+) ")
CUM_WHY = re.compile(r"\[PM4_COV\] CUM (\S+) frames=")  # the report's why token (periodic / terminal)
FLINE = re.compile(r"\[PM4_COV\] F frame=(\d+) .* ops=(\S*) regs=(\S*)")
FRAME_ANY = re.compile(r"\[PM4_COV\] F frame=(\d+)")  # any F record, parsed or not
OP_KV = re.compile(r"^([0-9A-F]{2}):(\d+)$")
REG_KV = re.compile(r"^([0-9A-F]{2}):(\d+)/\d+$")
OTHER_KEY = re.compile(r"^o\d+$")  # the census's other-class keys (no F-line counterpart)
KEYS = ("r0F", "63", "61", "62", "60", "r0E", "r0C", "r05")
UNSTAMPED = ("00000000", "UNSTAMPED")
# gfx/pm4_coverage.cpp frameEnd: report("periodic") when frames % 256 == 0 or 20 s after the last report
REPORT_FRAMES, REPORT_SECONDS, SLACK_S = 256, 20.0, 1.0


def short(ns):
    """A list of frame numbers as ranges, at most 10."""
    out, i = [], 0
    while i < len(ns) and len(out) < 10:
        j = i
        while j + 1 < len(ns) and ns[j + 1] == ns[j] + 1:
            j += 1
        out.append(str(ns[i]) if i == j else "%d-%d" % (ns[i], ns[j]))
        i = j + 1
    return ",".join(out) + (",..." if i < len(ns) else "")


def parse_ts(s):
    return datetime.datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f")


def run_log(roll):
    if os.path.isfile(roll):
        return roll
    return os.path.join(roll, "drive", "attempt1", "run.log")


def parse_keys(text):
    out = {}
    for kv in text.split(","):
        if ":" in kv:
            k, _, v = kv.partition(":")
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def parse_frame_keys(ops, regs):
    """A [PM4_COV] F line's keys in the census's spelling: ops XX:n as XX, regs XX:pk/dw as rXX (packets); None when
    any entry is malformed (the record is then not valid - INCOMPLETE)."""
    out = {}
    for field, page in ((ops, False), (regs, True)):
        for kv in field.split(",") if field else []:
            m = (REG_KV if page else OP_KV).match(kv)
            if not m:
                return None
            out[("r" if page else "") + m.group(1)] = int(m.group(2))
    return out


def read_census(roll, require_terminal=False, stranded_policy="allow"):
    """One pass over the run.log -> {"producers": {(va, name): {key: count}} from each producer's LAST inner= line,
    "coverage": the interval those counts cover and the tail after it}. The census is CUMULATIVE but periodic
    (gfx/pm4_coverage.cpp frameEnd: every 256 frames or 20 s, no final report - the harness kills the game, sr_drive
    Stop-Process -Force), so the last lines cover the run up to their report only: census_frames = the frames= of the
    [PM4_COV] CUM line printed with them; the tail = the [PM4_COV] F lines (one per completed frame, every producer
    together) after it, and the seconds from that report to the log's last line.
    require_terminal (tools/graceful-quit; Astra round 1, P2-4): the producers are the TERMINAL rows only - inner= lines
    with the why token `terminal` strictly between the snapshot's request and complete lines - and census_is_terminal
    also needs at least one such row, no terminal row outside the snapshot, no census row or CUM line after the
    complete line, and no producer whose rows are all periodic.
    stranded_policy (native/quit-drain round 2; terminal_snapshot.STRANDED_POLICIES): "allow" - a snapshot with stranded
    threads is a terminal one (the counts close at the ring cut; the stranded thread's dropped work is after it); "forbid"
    - it is not. Either way coverage["terminal"] carries stranded / stranded_threads / stranded_mismatch / the policy, and
    a count that differs from its records is never terminal."""
    if stranded_policy not in ts.STRANDED_POLICIES:
        raise ValueError("stranded_policy must be one of %s" % (ts.STRANDED_POLICIES,))
    last, cum, census = {}, None, None
    last_frame, end_ts = 0, None
    frames = {}  # F-line keys per frame number
    bad_frames = []  # F records that do not parse, and frame numbers seen twice
    rows, cums = [], []  # (line number, why, producer, keys text) / (line number, why) - bound to the snapshot below
    snap = ts.Scanner()  # tools/graceful-quit: the [QUIT] lines, read in the same pass
    with open(run_log(roll), encoding="utf-8", errors="replace") as f:
        for line in f:
            snap.feed(line)
            t = TS.match(line)
            if t:
                end_ts = t.group(1)
            c = CUM.search(line)
            if c:
                w = CUM_WHY.search(line)
                cum = (t.group(1) if t else None, int(c.group(1)), w.group(1) if w else None, snap.n)
                cums.append((snap.n, cum[2]))
                continue
            m = LINE.search(line)
            if m:
                last[(m.group(1).upper(), m.group(2))] = m.group(5)
                w = LINE_WHY.search(line)
                rows.append((snap.n, w.group(1) if w else None, (m.group(1).upper(), m.group(2)), m.group(5)))
                census = cum  # the CUM line of the report these lines belong to
                continue
            loose = FRAME_ANY.search(line)
            if loose:
                n = int(loose.group(1))
                last_frame = max(last_frame, n)
                fl = FLINE.search(line)
                keys = parse_frame_keys(fl.group(2), fl.group(3)) if fl and int(fl.group(1)) == n else None
                if keys is None:
                    bad_frames.append("frame %d unparsed" % n)
                elif n in frames:
                    bad_frames.append("frame %d twice" % n)
                else:
                    frames[n] = keys
    # tools/graceful-quit: the snapshot (with the quit receipt beside the log: Astra round 1, P1-3) and every census
    # row bound to it (P2-4)
    log = run_log(roll)
    s = snap.result(log, ts.read_receipt(ts.find_receipt(log)))
    req, comp = s["request_line"], s["complete_line"]

    def inside(n):
        return s["status"] == "complete" and req < n < comp

    term, outside = {}, []
    for n, why, p, keys in rows:
        if comp is not None and n > comp:
            outside.append("row after the complete line: %s %s (%s)" % (p[0], p[1], why))
        elif why == "terminal":
            if inside(n):
                term[p] = keys
            else:
                outside.append("terminal row outside a complete snapshot: %s %s" % p)
    outside += ["CUM line after the complete line (%s)" % why for n, why in cums if comp is not None and n > comp]
    periodic_only = sorted("%s %s" % p for p in last if p not in term)
    if require_terminal:
        last = term  # the producers ARE the terminal report's rows
        census = cum if (cum and cum[2] == "terminal" and inside(cum[3])) else None
    cov = {"census_frames": census[1] if census else None, "census_ts": census[0] if census else None,
           "last_frame": last_frame, "end_ts": end_ts}
    P = {p: parse_keys(k) for p, k in last.items()}
    # Astra round 4: the F records must be CONTIGUOUS and valid from frame 1 through the last one - a frame missing
    # anywhere (the tail included), unparsed or repeated means INCOMPLETE: no claim about every completed frame
    missing = [n for n in range(1, last_frame + 1) if n not in frames]
    cov["frames_complete"] = bool(last_frame) and not missing and not bad_frames
    cov["frames_incomplete"] = (["frames missing: %s" % short(missing)] if missing else []) + bad_frames[:20]
    if census and census[0] and end_ts and last_frame >= census[1]:  # the F lines reach the report (else unseen)
        cov["tail_frames"] = last_frame - census[1]
        cov["tail_seconds"] = round((parse_ts(end_ts) - parse_ts(census[0])).total_seconds(), 3)
        cov["tail"] = {k: sum(v.get(k, 0) for n, v in frames.items() if n > census[1]) for k in KEYS}
        cov["within_period"] = cov["tail_frames"] < REPORT_FRAMES and cov["tail_seconds"] <= REPORT_SECONDS + SLACK_S
        # the F lines are the same count: frames 1..census_frames sum to the census's totals for EVERY comparable key
        # (Astra round 4: all 43 opcode / page keys of rcen1 reconcile), so their tail is the census's own measure, only
        # unattributed. The census's other-class keys (oN) have no F-line counterpart: reported as not comparable.
        covered, total = {}, {}
        for n, v in frames.items():
            if n <= census[1]:
                for k, x in v.items():
                    covered[k] = covered.get(k, 0) + x
        for keys in P.values():
            for k, x in keys.items():
                total[k] = total.get(k, 0) + x
        comparable = sorted(k for k in set(covered) | set(total) if not OTHER_KEY.match(k))
        cov["keys_compared"] = len(comparable)
        cov["keys_not_comparable"] = sorted(k for k in total if OTHER_KEY.match(k))
        cov["keys_mismatched"] = ["%s frames %d census %d" % (k, covered.get(k, 0), total.get(k, 0))
                                  for k in comparable if covered.get(k, 0) != total.get(k, 0)]
        cov["frames_match_census"] = cov["frames_complete"] and not cov["keys_mismatched"]
    else:
        cov.update(tail_frames=None, tail_seconds=None, tail={k: None for k in KEYS}, within_period=False,
                   keys_compared=0, keys_not_comparable=[], keys_mismatched=[], frames_match_census=False)
    # tools/graceful-quit: was the report these counts come from the terminal one of a complete snapshot (exit
    # evidence included), every producer row of it inside the snapshot, nothing after it? (Graded only under
    # --require-terminal.)
    cov["census_why"] = census[2] if census else None
    cov["terminal"] = {k: s[k] for k in ("status", "frames", "tail", "waiting", "families", "exit_reason")}
    # native/quit-drain round 2: the stranded threads ride along (count, every record, the policy) - never dropped
    cov["terminal"].update(ts.stranded_view(s, stranded_policy))
    cov["terminal_rows"] = len(term)
    cov["rows_outside_snapshot"] = outside[:20]
    cov["producers_without_terminal_row"] = periodic_only[:20]
    cov["census_is_terminal"] = bool(census and census[2] == "terminal" and ts.accept(s, stranded_policy)
                                     and inside(census[3]) and cov["tail_frames"] == 0 and term and not outside
                                     and not periodic_only)
    return {"producers": P, "coverage": cov}


def producers(roll, require_terminal=False, stranded_policy="allow"):
    """{(va, name): {key: count}} from each producer's LAST inner= line (cumulative over the census-covered interval);
    require_terminal: its terminal row inside a complete snapshot only."""
    return read_census(roll, require_terminal, stranded_policy)["producers"]


def producer_keys(roll, producer="00000000", name=None, require_terminal=False, stranded_policy="allow"):
    """{key: count} of one producer's last line, None without one; 00000000 with no name = UNSTAMPED (RING_UNSTAMPED
    shares the VA: name it)."""
    P = producers(roll, require_terminal, stranded_policy)
    va = producer.upper()
    if name:
        return P.get((va, name))
    if va == UNSTAMPED[0]:
        return P.get(UNSTAMPED)
    return next((keys for (v, _n), keys in P.items() if v == va), None)


def side(P):
    total = {k: sum(p.get(k, 0) for p in P.values()) for k in KEYS}
    unst = {k: P.get(UNSTAMPED, {}).get(k, 0) for k in KEYS}
    return {"total": total, "unstamped": unst,
            "holders_63": sorted("%s %s" % p for p, keys in P.items() if keys.get("63"))}


def compare(base_roll, target_roll, require_terminal=False, stranded_policy="allow"):
    B, T = (read_census(base_roll, require_terminal, stranded_policy),
            read_census(target_roll, require_terminal, stranded_policy))
    b, t = B["producers"], T["producers"]
    d = {"base_roll": base_roll, "target_roll": target_roll, "base_census": bool(b), "target_census": bool(t),
         "base": side(b), "target": side(t), "base_coverage": B["coverage"], "target_coverage": T["coverage"]}
    bt, bu = d["base"]["total"], d["base"]["unstamped"]
    tt, tu = d["target"]["total"], d["target"]["unstamped"]
    d["r0F_other"] = bt["r0F"] - bt["63"]
    d["r0F_other_unstamped"] = bu["r0F"] - bu["63"]
    both = d["base_census"] and d["target_census"]
    ttail = T["coverage"]["tail"]
    d["checks"] = {
        # over the census-covered interval (each roll's last census report)
        "base_63_exercised": d["base_census"] and bt["63"] > 0,
        "target_63_zero": d["target_census"] and tt["63"] == 0,
        "target_r0F_equals_other": both and d["r0F_other"] >= 0 and tt["r0F"] == d["r0F_other"],
        "target_unstamped_r0F_equals_other": both and d["r0F_other_unstamped"] >= 0
                                             and tu["r0F"] == d["r0F_other_unstamped"],
        # that interval reaches within one reporting period of each run's end (< 256 frames, <= 20 s + 1 s)
        "base_census_covers_run": bool(d["base_census"] and B["coverage"]["within_period"]),
        "target_census_covers_run": bool(d["target_census"] and T["coverage"]["within_period"]),
        # the F lines measure what the census measures (frames 1..census_frames sum to its totals), so the tail is read
        # in the census's own counts
        "frames_match_census": bool(B["coverage"]["frames_match_census"] and T["coverage"]["frames_match_census"]),
        # Astra round 4: every frame 1..last_frame has exactly one valid F record in both rolls (else INCOMPLETE: a gap
        # in the tail would hide its packets from target_tail_clean and the per-frame items)
        "frames_complete": bool(B["coverage"]["frames_complete"] and T["coverage"]["frames_complete"]),
        # the target's tail frames ([PM4_COV] F lines after its last report, every producer together): no opcode 63
        # and no RB_BC_CONTROL (the residual r0F is a boot write, in frame 1, never in a tail frame)
        "target_tail_clean": bool(d["target_census"] and ttail["63"] == 0 and ttail["r0F"] == 0),
    }
    if require_terminal:  # tools/graceful-quit: the WHOLE run - each roll's counts are its terminal report
        d["require_terminal"] = True
        d["stranded_policy"] = stranded_policy  # native/quit-drain round 2: explicit, and the counts carried
        d["base_stranded"] = B["coverage"]["terminal"]["stranded"]
        d["target_stranded"] = T["coverage"]["terminal"]["stranded"]
        d["checks"]["base_census_terminal"] = bool(B["coverage"]["census_is_terminal"])
        d["checks"]["target_census_terminal"] = bool(T["coverage"]["census_is_terminal"])
    d["ok"] = all(d["checks"].values())
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("roll", nargs="?")
    ap.add_argument("--producer", default="00000000")
    ap.add_argument("--name")
    ap.add_argument("--base")
    ap.add_argument("--target")
    ap.add_argument("--json")
    ap.add_argument("--require-terminal", action="store_true",
                    help="grade the whole run: each roll's census must be its graceful-quit terminal report")
    ap.add_argument("--stranded-policy", choices=ts.STRANDED_POLICIES, default="allow",
                    help="with --require-terminal: a snapshot with stranded threads is terminal (allow) or not (forbid);"
                         " the count is reported either way")
    a = ap.parse_args()
    if a.base and a.target:
        d = compare(a.base, a.target, require_terminal=a.require_terminal, stranded_policy=a.stranded_policy)
        if a.json:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump(d, f, indent=1)
        bt, bu, tt, tu = d["base"]["total"], d["base"]["unstamped"], d["target"]["total"], d["target"]["unstamped"]
        bc, tc = d["base_coverage"], d["target_coverage"]
        print("OWNER_CENSUS (census-covered) base r0F=%d 63=%d (other %d; unstamped r0F=%d 63=%d, other %d) | target "
              "r0F=%d 63=%d (unstamped r0F=%d) | covered frames base %s target %s, tails %s / %s frames, %s / %s s, "
              "target tail 63=%s r0F=%s | frames %s / %s, keys reconciled %d / %d%s | %s" % (
                  bt["r0F"], bt["63"], d["r0F_other"], bu["r0F"], bu["63"], d["r0F_other_unstamped"], tt["r0F"],
                  tt["63"], tu["r0F"], bc["census_frames"], tc["census_frames"], bc["tail_frames"], tc["tail_frames"],
                  bc["tail_seconds"], tc["tail_seconds"], tc["tail"]["63"], tc["tail"]["r0F"],
                  "complete" if bc["frames_complete"] else "INCOMPLETE", "complete" if tc["frames_complete"] else "INCOMPLETE",
                  bc["keys_compared"], tc["keys_compared"],
                  "" if not (bc["keys_mismatched"] or tc["keys_mismatched"]) else
                  " (MISMATCH %s)" % "; ".join(bc["keys_mismatched"][:3] + tc["keys_mismatched"][:3]),
                  "PASS" if d["ok"] else "FAIL " + ",".join(k for k, v in d["checks"].items() if not v)))
        if a.require_terminal:
            def nst(x):   # the snapshot's stranded count; - when its complete line names none
                return "-" if x is None else x
            print("OWNER_CENSUS terminal: base %s (census %s), target %s (census %s); stranded base %s target %s, "
                  "policy %s" % (bc["terminal"]["status"], bc["census_why"], tc["terminal"]["status"],
                                 tc["census_why"], nst(bc["terminal"]["stranded"]), nst(tc["terminal"]["stranded"]),
                                 a.stranded_policy))
        return 0 if d["ok"] else 1
    if not a.roll:
        ap.error("a roll, or --base and --target")
    if a.require_terminal:
        cov = read_census(a.roll, True, a.stranded_policy)["coverage"]
        if not cov["census_is_terminal"]:
            print("no terminal census in %s: snapshot %s (stranded %s, policy %s), census %s, tail %s frames%s" % (
                a.roll, cov["terminal"]["status"], cov["terminal"]["stranded"], a.stranded_policy, cov["census_why"],
                cov["tail_frames"],
                "".join(" | " + x for x in (cov["rows_outside_snapshot"] + cov["producers_without_terminal_row"])[:5])))
            return 1
    k = producer_keys(a.roll, a.producer, a.name, a.require_terminal, a.stranded_policy)
    print(json.dumps(k, indent=1) if k is not None else "no census line for producer %s" % a.producer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
