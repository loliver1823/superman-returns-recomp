#!/usr/bin/env python3
"""terminal_snapshot.py - the graceful quit's TERMINAL SNAPSHOT in a roll's log (tools/graceful-quit, docs/TOOLKIT.md
1.8m): the shared reader the census and counter graders use to grade a WHOLE run instead of "all but the last
reporting period".

    python tools/perf/terminal_snapshot.py <roll dir or run.log> [--receipt quit.json] [--require] [--json out.json]

A roll ended by sr_drive -GracefulQuit (sr_roll / run_recomp_x -GracefulQuit, native_proof --graceful-quit) logs, at
its end (kernel/quit.cpp):
    [QUIT] request received: frames=.. swaps=.. vdswaps=.. -> quiesce (...)
    [QUIT] stage ring-cut: ... / stage cp-drain: ... / stage thread-cut: ... / stage settle: ...
    every counter family's lines with the why token `terminal`: [GPU_CP] terminal ..., [PM4_COV] CUM terminal ...,
    [PM4_COVMAP] terminal inner=..., [NATIVE_FLUSH] terminal ... (its segment lines "[NATIVE_FLUSH] terminal seg
    closed=..." / "terminal seg tile segs=..." included), [NATIVE_METER_RUN] terminal ..., [AUDIO_NATIVE] terminal ...,
    [AB_DRAIN] terminal stats ..., [GAUGE] terminal ... (and, when the drain ended inside a frame, "[PM4_COV] terminal
    tail frame=N+1" followed by that frame's ordinary F line)
    [QUIT] terminal snapshot complete frames=N swaps=S vdswaps=V tail=T rl_pending=R threads=.. parked=.. waiting=..
           host_idle=.. families=K fields=F elapsed_ms=.. stranded=N
or, when the game could not quiesce, "[QUIT] terminal snapshot FAILED stage=<s> reason=<r> ...".

STRANDED (native/quit-drain): a thread spinning in GpuBlockOnFence on a fence the ring cut made unreachable (its
publish dropped, the CP parked) - or (native/quit-ringspace) in GpuRingWaitForSpace on ring space only a dropped close's
progress word would have freed (wait=GpuRingWaitForSpace target=<end> completed=<the frozen progress word>
next=<generation>) - is acknowledged as stranded, never silently: one "[QUIT] stranded tid=.. wait=.. dev=..
target=.. completed=.. next=.. seq=.. locks=.." line each, after the settle line, and stranded=N LAST on the complete
line (an exe before native/quit-drain prints no field: None). read() returns "stranded" and "stranded_threads";
is_complete(snap, forbid_stranded=True) / --forbid-stranded refuse a snapshot with stranded > 0 - for a grader where a
stranded thread must count as the failure it was before (the game side: SR_QUIT_STRANDED=0). Round 2: a stranded count
that differs from its records (stranded_mismatch) is never complete; accept(snap, policy) takes the policy explicitly
("allow" | "forbid") and stranded_view(snap, policy) is what a grader's result carries (owner_census, residue_census).

THE EXIT EVIDENCE (Astra round 1, P1-3): the complete line is necessary, not sufficient - the game prints it and only
THEN closes its log and exits, and its hard deadline (exit 82) or a harness fallback (Stop-Process) can still end it.
So a snapshot counts as complete only with the harness's receipt of the process's end: quit.json (tools/sr_quit.ps1
Write-SrQuitRecord, schema sr-quit/1) with result "complete", graceful true, fellBack false and exitCode 0. Found
(find_receipt) beside the log as <log stem>.quit.json (run_recomp_x), as quit.json in the log's folder (sr_drive's
attempt folder; sr_roll copies it to its <out>), else in <roll>/drive/attempt<N>/ (the highest N).

read(log) -> a dict; "status" is one of
    complete     a request line, then the complete line, AND a receipt of a clean exit 0: the terminal lines between
                 them cover the whole run, frames 1..N (tail=1: plus the partial frame N+1 the drain ended in)
    unconfirmed  the complete line but no receipt (no quit.json): no exit evidence - not complete
    rejected     the complete line but the receipt says otherwise (exit 82 after the log closed, a fallback, a non-zero
                 exit): not complete; exit_reason says why
    failed       the FAILED line (stage, reason): no terminal snapshot
    incomplete   a request line and neither (the game died mid-quit, or its hard deadline fired): no terminal snapshot
    absent       no request line: a roll ended by Stop-Process (every roll before tools/graceful-quit, every roll
                 without the opt-in) - graders read it exactly as before
    pm4cov_frames / pm4cov_consistent: the frames= of the PM4 census's terminal armed / CUM / ENDS lines (the first of
                 each after the request) and whether they agree - every one is printed from the state with the drained
                 partial frame folded in (native/census-quit: tbk2's exe printed the armed line before the fold, one
                 less, and its verify failed); None when fewer than two of them printed
last_line(lines, rx, require_terminal, snap) -> the line a grader reads for one family: without the requirement the
    LAST line matching rx (today's rule, for every log); with it the last matching TERMINAL line inside a complete
    snapshot, else None.
Scanner: the same reading line by line, for a grader that already makes one pass over a big log (owner_census).

CLI exit: 0 = complete, or (without --require) any status reported; 1 = --require and not complete (with
--forbid-stranded: or stranded > 0); 2 = no log.
"""
import argparse
import glob
import json
import os
import re
import sys

REQUEST = re.compile(r"\[QUIT\] request received: frames=(\d+) swaps=(\d+) vdswaps=(\d+)")
COMPLETE = re.compile(r"\[QUIT\] terminal snapshot complete frames=(\d+) swaps=(\d+) vdswaps=(\d+) tail=(\S+) "
                      r"rl_pending=(\d+) threads=(\d+) parked=(\d+) waiting=(\d+) host_idle=(\d+) families=(\d+) "
                      r"fields=(\d+) elapsed_ms=(\d+)")
FAILED = re.compile(r"\[QUIT\] terminal snapshot FAILED stage=(\S+) reason=(\S+)")
# native/quit-drain: the complete line's last field, and one line per stranded thread
STRANDED_FIELD = re.compile(r" stranded=(\d+)\s*$")
STRANDED_LINE = re.compile(r"\[QUIT\] stranded tid=(\d+) wait=(\S+) dev=([0-9A-Fa-f]+) target=([0-9A-Fa-f]+)"
                           r"(?: completed=([0-9A-Fa-f]+) next=([0-9A-Fa-f]+))? seq=(\d+)(?: spin=(\d+))? locks=(\d+)")
# The explicit acceptance policies a grader passes for stranded threads (native/quit-drain round 2): "allow" - a
# stranded quit is complete (the measured window closes at the ring cut; the stranded thread's dropped work is after
# it), its count and records carried in the result; "forbid" - stranded > 0 is not complete.
STRANDED_POLICIES = ("allow", "forbid")
# A family line of the terminal snapshot: the why token "terminal" right after the tag (or after the word the printer
# puts first: "[PM4_COV] CUM terminal", "[PM4_COV] ENDS terminal", "[PM4_COV] armed terminal" (native/composed-census),
# "[NATIVE_METER_KINDS] run terminal").
TERMINAL = re.compile(r"\[([A-Z0-9_]+)\] (?:CUM |run |ENDS |armed )?terminal\b")
# The PM4 census's terminal lines that carry its frame count (gfx/pm4_coverage terminal(): each printed from the state
# with the drained partial frame folded in - one frames= value; native/census-quit, tbk2: the armed line printed before
# the fold read one less, and the quit's verify failed on it)
PM4COV_FRAMES = re.compile(r"\[PM4_COV\] (armed|CUM|ENDS) terminal\b.*? frames=(\d+)")
RECEIPT_SCHEMA = "sr-quit/1"
_AUTO = object()  # read(): find the receipt beside the log


def run_log(roll):
    """A run.log path, or the log of a roll folder: sr_roll's <out>/run.log, else sr_drive's drive/attempt1/run.log, else
    an sr_drive -Out's attempt1/run.log."""
    if os.path.isfile(roll):
        return roll
    for cand in (os.path.join(roll, "run.log"), os.path.join(roll, "drive", "attempt1", "run.log"),
                 os.path.join(roll, "attempt1", "run.log")):
        if os.path.isfile(cand):
            return cand
    return os.path.join(roll, "drive", "attempt1", "run.log")


def _attempt_no(path):
    m = re.search(r"attempt(\d+)", os.path.basename(os.path.dirname(path)))
    return int(m.group(1)) if m else -1


def find_receipt(log):
    """The quit receipt (quit.json) for a log path, or None: <stem>.quit.json beside it, quit.json in its folder, else
    <folder>/drive/attempt<N>/quit.json with the highest N (an sr_roll folder whose copy is missing)."""
    if not isinstance(log, str):
        return None
    d = os.path.dirname(os.path.abspath(log))
    stem = os.path.splitext(os.path.basename(log))[0]
    for cand in (os.path.join(d, stem + ".quit.json"), os.path.join(d, "quit.json")):
        if os.path.isfile(cand):
            return cand
    found = sorted(glob.glob(os.path.join(d, "drive", "attempt*", "quit.json")), key=_attempt_no)
    return found[-1] if found else None


def read_receipt(path):
    """quit.json -> dict, or None when it is missing or unreadable (ASCII or UTF-8, with or without a BOM)."""
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def receipt_verdict(rec):
    """(ok, reason): ok only for a receipt of a clean graceful exit 0 - result complete, graceful, no fallback."""
    if rec is None:
        return False, "no quit receipt (quit.json): no exit evidence"
    if not isinstance(rec, dict) or rec.get("schema") != RECEIPT_SCHEMA:
        return False, "the quit receipt is not %s" % RECEIPT_SCHEMA
    bad = []
    if rec.get("result") != "complete":
        bad.append("result %s" % rec.get("result"))
    if rec.get("graceful") is not True:
        bad.append("not graceful")
    if rec.get("fellBack") is not False:
        bad.append("fell back to Stop-Process")
    code = rec.get("exitCode")
    if not (isinstance(code, int) and not isinstance(code, bool) and code == 0):
        bad.append("exit code %s" % code)
    return (not bad), ("clean exit 0" if not bad else "the quit receipt says: " + ", ".join(bad))


class Scanner:
    """Feed every line of one log in order (feed(line)); result() is read()'s dict."""

    def __init__(self):
        self.n = 0
        self.request = None   # (line number, frames, swaps, vdswaps) of the LAST request line
        self.complete = None  # (line number, match) of the last complete line
        self.failed = None    # (line number, stage, reason)
        self.terminal = []    # (line number, tag) of every terminal family line
        self.pm4cov = []      # (line number, kind, frames) of the PM4 census's terminal frame counts
        self.stranded = []    # (line number, dict) of every "[QUIT] stranded ..." line (native/quit-drain)

    def feed(self, line):
        self.n += 1
        if "[QUIT]" in line:
            m = REQUEST.search(line)
            if m:  # a later request starts over: result() only counts what follows the LAST one
                self.request = (self.n, int(m.group(1)), int(m.group(2)), int(m.group(3)))
                return
            m = COMPLETE.search(line)
            if m:
                f = STRANDED_FIELD.search(line)
                self.complete = (self.n, m, int(f.group(1)) if f else None)
                return
            m = STRANDED_LINE.search(line)
            if m:
                self.stranded.append((self.n, {
                    "tid": int(m.group(1)), "wait": m.group(2), "dev": m.group(3), "target": m.group(4),
                    "completed": m.group(5), "next": m.group(6), "seq": int(m.group(7)),
                    "spin": int(m.group(8)) if m.group(8) else None, "locks": int(m.group(9))}))
                return
            m = FAILED.search(line)
            if m:
                self.failed = (self.n, m.group(1), m.group(2))
                return
        if "terminal" in line:
            t = TERMINAL.search(line)
            if t and t.group(1) != "QUIT":
                self.terminal.append((self.n, t.group(1)))
                p = PM4COV_FRAMES.search(line) if t.group(1) == "PM4_COV" else None
                if p:
                    self.pm4cov.append((self.n, p.group(1), int(p.group(2))))

    def result(self, log=None, receipt=None):
        """receipt: the quit.json dict (read_receipt), or None when there is none."""
        s = {"log": log, "status": "absent", "request_line": None, "complete_line": None, "frames": None,
             "swaps": None, "vdswaps": None, "tail": None, "rl_pending": None, "threads": None, "parked": None,
             "waiting": None, "host_idle": None, "families": None, "fields": None, "elapsed_ms": None,
             "failed_stage": None, "failed_reason": None, "family_lines": {}, "lines_after_complete": 0,
             "receipt": None, "exit_result": None, "exit_code": None, "fell_back": None, "exit_reason": None,
             "pm4cov_frames": {}, "pm4cov_consistent": None, "stranded": None, "stranded_threads": [],
             "receipt_stranded": None, "stranded_mismatch": False}
        if isinstance(receipt, dict):
            s.update(receipt=True, exit_result=receipt.get("result"), exit_code=receipt.get("exitCode"),
                     fell_back=receipt.get("fellBack"), receipt_stranded=receipt.get("stranded"))
        if not self.request:
            return s
        s["request_line"] = self.request[0]
        # the PM4 census's terminal frame counts (the snapshot's own render, the first of each kind after the request):
        # one value when every line was printed from the folded state; None when fewer than two kinds printed
        ends = [x[0] for x in (self.complete, self.failed) if x and x[0] > self.request[0]]
        stop = min(ends) if ends else self.n + 1
        for n, kind, frames in self.pm4cov:
            if self.request[0] < n < stop:
                s["pm4cov_frames"].setdefault(kind, frames)
        if len(s["pm4cov_frames"]) >= 2:
            s["pm4cov_consistent"] = len(set(s["pm4cov_frames"].values())) == 1
        if self.failed and self.failed[0] > self.request[0]:
            s.update(status="failed", failed_stage=self.failed[1], failed_reason=self.failed[2])
            return s
        if not (self.complete and self.complete[0] > self.request[0]):
            s["status"] = "incomplete"
            return s
        ln, m, stranded = self.complete
        tail = m.group(4)
        s.update(complete_line=ln, frames=int(m.group(1)), swaps=int(m.group(2)),
                 vdswaps=int(m.group(3)), tail=int(tail) if tail.lstrip("-").isdigit() else None,
                 rl_pending=int(m.group(5)), threads=int(m.group(6)), parked=int(m.group(7)), waiting=int(m.group(8)),
                 host_idle=int(m.group(9)), families=int(m.group(10)), fields=int(m.group(11)),
                 elapsed_ms=int(m.group(12)), lines_after_complete=self.n - ln, stranded=stranded,
                 stranded_threads=[d for n, d in self.stranded if self.request[0] < n < ln])
        # round 2: every stranded thread is named - a count that differs from the records is refused (is_complete)
        s["stranded_mismatch"] = stranded is not None and stranded != len(s["stranded_threads"])
        fam = {}
        for n, tag in self.terminal:
            if self.request[0] < n < ln:
                fam[tag] = fam.get(tag, 0) + 1
        s["family_lines"] = fam
        ok, why = receipt_verdict(receipt)
        s["exit_reason"] = why
        s["status"] = "complete" if ok else ("unconfirmed" if receipt is None else "rejected")
        return s


def read_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.readlines()


def read(log, receipt=_AUTO):
    """read(path or list of lines, receipt) -> the snapshot dict (see the module doc). receipt: a quit.json path or
    dict; by default found beside a log PATH (find_receipt) - a list of lines has none unless one is passed."""
    lines = read_lines(log) if isinstance(log, str) else log
    if receipt is _AUTO:
        receipt = find_receipt(log) if isinstance(log, str) else None
    rec = read_receipt(receipt) if isinstance(receipt, str) else receipt
    sc = Scanner()
    for line in lines:
        sc.feed(line)
    s = sc.result(log if isinstance(log, str) else None, rec)
    if isinstance(receipt, str):
        s["receipt"] = receipt if rec is not None else None
    return s


def is_complete(snap, forbid_stranded=False):
    """complete; with forbid_stranded, only with no stranded thread (an exe before the field: none could strand).
    Never complete when the stranded count differs from the "[QUIT] stranded" records (round 2: none silent)."""
    if not (bool(snap) and snap.get("status") == "complete") or snap.get("stranded_mismatch"):
        return False
    return not (forbid_stranded and (snap.get("stranded") or 0) > 0)


def accept(snap, stranded_policy):
    """is_complete under an EXPLICIT stranded policy ("allow" | "forbid", STRANDED_POLICIES)."""
    if stranded_policy not in STRANDED_POLICIES:
        raise ValueError("stranded policy must be one of %s, not %r" % (STRANDED_POLICIES, stranded_policy))
    return is_complete(snap, forbid_stranded=stranded_policy == "forbid")


def stranded_view(snap, stranded_policy):
    """What a grader's result carries about stranded threads: the count, every record, whether they agree, the policy."""
    snap = snap or {}
    return {"stranded": snap.get("stranded"), "stranded_threads": list(snap.get("stranded_threads") or []),
            "stranded_mismatch": bool(snap.get("stranded_mismatch")), "stranded_policy": stranded_policy}


def last_line(lines, rx, require_terminal=False, snap=None):
    """The line one family's grader reads. Without require_terminal: the LAST line matching `rx` (a pattern or a
    compiled regex) - today's rule, unchanged for every log. With it: the last line matching `rx` that is a terminal
    family line inside a complete snapshot (between the request and the complete line); None when the roll has no
    complete terminal snapshot (snap: read()'s dict WITH the receipt - a list of lines alone is never complete) or the
    family printed no terminal line."""
    rx = re.compile(rx) if isinstance(rx, str) else rx
    if not require_terminal:
        hit = None
        for line in lines:
            if rx.search(line):
                hit = line
        return hit
    snap = snap if snap is not None else read(lines)
    if not is_complete(snap):
        return None
    hit = None
    for i in range(snap["request_line"], snap["complete_line"] - 1):  # 1-based lines request+1 .. complete-1
        line = lines[i]
        t = TERMINAL.search(line)
        if t and t.group(1) != "QUIT" and rx.search(line):
            hit = line
    return hit


def summary(snap):
    """One line for a grader's console output."""
    if snap["status"] == "complete" and snap.get("stranded_mismatch"):
        return ("terminal snapshot NOT complete: stranded=%s on the complete line but %d [QUIT] stranded record(s)" % (
            snap.get("stranded"), len(snap.get("stranded_threads") or [])))
    if snap["status"] == "complete":
        st = snap.get("stranded")
        strand = ""
        if st:  # never silent (native/quit-drain)
            strand = ", STRANDED=%d (%s)" % (st, "; ".join(
                "tid %d in %s target %s completed %s locks %d" % (d["tid"], d["wait"], d["target"], d["completed"],
                                                                  d["locks"]) for d in snap.get("stranded_threads", [])))
        return ("terminal snapshot complete: frames 1..%d%s, %d families, %d fields verified, threads parked=%d "
                "waiting=%d host_idle=%d%s, %s" % (
                    snap["frames"], " + tail frame" if snap["tail"] == 1 else "", snap["families"], snap["fields"],
                    snap["parked"], snap["waiting"], snap["host_idle"], strand, snap["exit_reason"]))
    if snap["status"] in ("unconfirmed", "rejected"):
        return "terminal snapshot NOT complete (%s): the complete line, but %s" % (snap["status"], snap["exit_reason"])
    if snap["status"] == "failed":
        return "terminal snapshot FAILED (stage %s, %s)" % (snap["failed_stage"], snap["failed_reason"])
    if snap["status"] == "incomplete":
        return "terminal snapshot INCOMPLETE (a quit request, no complete or FAILED line)"
    return "no terminal snapshot (the roll ended without a graceful quit)"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roll")
    ap.add_argument("--receipt", help="the quit.json to use (default: found beside the log)")
    ap.add_argument("--require", action="store_true", help="exit 1 unless the roll ended on a complete snapshot")
    ap.add_argument("--forbid-stranded", action="store_true",
                    help="with --require: a snapshot with a stranded thread (stranded > 0) is not complete")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    log = run_log(a.roll)
    if not os.path.isfile(log):
        print("terminal_snapshot: no log at %s" % log, file=sys.stderr)
        return 2
    snap = read(log, a.receipt if a.receipt else _AUTO)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(snap, f, indent=1)
    print("%s: %s%s" % (log, summary(snap), (" | family lines: " + ", ".join(
        "%s %d" % kv for kv in sorted(snap["family_lines"].items()))) if snap["family_lines"] else ""))
    return 0 if is_complete(snap, a.forbid_stranded) or not a.require else 1


if __name__ == "__main__":
    sys.exit(main())
