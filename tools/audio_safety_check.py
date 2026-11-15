#!/usr/bin/env python3
"""Audio-device safety check for a roll's logs (lane E, RELEASE_PLAN step 5 item E0, 2026-10-22).

The rule (owner / orchestrator, 2026-10-18): an automated run never plays to the owner's real devices; audio
tests use only a silent virtual endpoint ("Steam Streaming Speakers"). The exe enforces it since E0
(kernel/apu/audio_system.hpp chooseAudioDevice); this is the second barrier: it reads what a run actually opened.

    python tools/audio_safety_check.py <log or dir> [...] [--allow "Steam Streaming Speakers"] [--all-game]
                                       [--exe <the exe under test>] [--json out.json]

A directory is searched (recursively) for *.log. --all-game: every given log is a game log (tools/check_roll.py's
run logs are), so an empty one is NO-EVIDENCE instead of skipped. Exit 0 = PASS, 1 = FAIL, 2 = no logs,
3 = NO-EVIDENCE, 4 = N/A (a pre-E0 build).

The receipts (E0 exes; AUDIO_OPEN is a milestone tag, so every line survives SR_LOG=player). Each is written as soon
as SDL has opened the device (before any setup that can close it again):
  "[AUDIO_OPEN] client N device=D target=default|named name='NAME' driver=DRV source=SRC"   the SDL sink
  "[AUDIO_OPEN] client N device=0 target=none name='null' driver=null source=SRC"         the null sink
  "[AUDIO_OPEN] legacy device=D target=default|named name='NAME' driver=DRV source=SRC"     SR_AUDIO_OUT=0
  "[AUDIO_OPEN] legacy device=0 target=none name='null' driver=null source=SRC"           it opened nothing
target=default = SDL's system-default device (the owner's speakers on a real driver); target=named = a listed
device by id, judged by its NAME (so a device literally named "default" is a named device).
Older open lines (Info: DROPPED under SR_LOG=player) are read too, in every log:
  "[AUDIO_OUT] client N SDL device D (NAME): driver=DRV freq=..."   (an E0 exe writes it after its receipt)
  "[AUDIO] SDL audio device opened: ..."                             (no driver named: cannot be proven silent)
Every recognised open is judged once: an older line and a receipt of the same client and device id (the legacy line
and a legacy receipt) are one event, in either order, judged by the receipt, and the two must agree. An older line
with no receipt is judged on its own ("(default)" = the system default). Names are parsed from the right (the
fields after a name are fixed), so a name holding "): driver=" or "' driver=" cannot pose as a driver.

The verdict:
  FAIL         an open on a real driver (anything but disk / dummy) of the system default (unless the allowlist is
               "*") or of a NAME off the allowlist; an old legacy open (no driver named); a malformed [AUDIO_OPEN]
               line or an [AUDIO_OUT] open line that cannot be parsed; a receipt that disagrees with its open line;
               a log that cannot be read.
  NO-EVIDENCE  no FAIL, but no game log, a given path that does not exist, or a run with no well-formed receipt
               whose audio never visibly ran: the absence of an open line is not proof of silence. A log can hold
               several runs (a run starts at each [LOG_PROFILE] line after the first); each needs its own receipt,
               and an open pairs only with a receipt of its own run.
  N/A          (coordinator 2026-10-23: rolls of exes built before E0 must not fail other lanes' gates) no FAIL and no
               NO-EVIDENCE, but a run with no receipt whose audio DID run (an [AUDIO_OUT_SUM] or [AUDIO_TIMING] line
               with cb > 0: a sink was open), in a roll of an exe the CALLER names as the one under test (--exe; for
               tools/check_roll.py the exe it launched the roll with, recorded at the launch) that is a VERIFIED
               pre-E0 build: it is readable and does not contain the receipt's format ("[AUDIO_OPEN] ", compiled
               into every E0 exe). A log cannot prove its own build (Astra E0 r5-r7: a truncated E0 log looks the
               same, and a provenance file cannot be bound to a run by time or PID), so the build identity is the
               gate's input, as for every other row of a check roll. Each such run must also be one of that exe: the
               log holds one run and the roll's provenance.json beside it names the same sha256. Anything short of
               that - no exe named, an exe WITH the receipt format, no / bad provenance, a provenance naming another
               build, several runs in one log - stays NO-EVIDENCE. A pre-E0 run's old-format open lines are still
               judged above: a real-device open in them is a FAIL, never N/A.
  PASS         every game log carries at least one receipt and nothing failed.
A game log is a log with a [LOG_PROFILE] line or any [AUDIO*] line (all of them with --all-game); other *.log files
are listed as skipped. "[AUDIO_DEV] ... REFUSED ..." lines (the exe refused a device: safe, but the harness is
misconfigured) are warnings. Used by tools/check_roll.py (an always-on row, lane "audio-safety", --all-game).
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

DEFAULT_ALLOW = "Steam Streaming Speakers"
SILENT_DRIVERS = {"disk", "dummy"}
TAG_OPEN = "[AUDIO_OPEN]"
RE_OPEN = re.compile(r"\[AUDIO_OPEN\] (?:client (\d+)|legacy) device=(\d+) target=(default|named|none) name='(.*)' "
                     r"driver=(\S+) source=(\S+)\s*$")
TAG_SINK_OLD = re.compile(r"\[AUDIO_OUT\] client \d+ SDL device ")
# every shape since iter-463 (the first had no name: it opened the default device only)
RE_SINK_OLD = re.compile(r"\[AUDIO_OUT\] client (\d+) SDL device (\d+)(?: \((.*)\))?: driver=(\S+) freq=-?\d+ ch=")
RE_LEGACY_OLD = re.compile(r"\[AUDIO\] SDL audio device opened:")
RE_REFUSED = re.compile(r"\[AUDIO_DEV\] (?:client \d+: output device REFUSED|legacy driver: .*no device)")
RE_GAME = re.compile(r"\[LOG_PROFILE\]|\[AUDIO[A-Z_]*\]")
RE_PROFILE = re.compile(r"\[LOG_PROFILE\]")   # the logger's start-up line: one per run
E0_RECEIPT_FORMAT = b"[AUDIO_OPEN] "   # in every E0 exe (the receipt's format string); in no exe built before it
# the output callbacks ran (every exe since iter-463 prints these; a pre-E0 null sink too): a sink was open
RE_AUDIO_RAN = re.compile(r"\[AUDIO_(?:OUT_SUM|TIMING)\] cb=([1-9]\d*)\b")


def allowed(name, allow):
    """Mirror of audioDeviceAllowed (audio_system.cpp): ';'-separated, trimmed, case-insensitive substrings; '*' = any;
    an empty name (the system default) matches only '*'."""
    lst = allow if allow else DEFAULT_ALLOW
    n = name.lower()
    for item in (s.strip() for s in lst.split(";")):
        if item == "*":
            return True
        if item and n and item.lower() in n:
            return True
    return False


def _judge(rec, allow, violations):
    drv = rec["driver"]
    if drv is None:
        violations.append(dict(rec, why="a device open with no driver named (a pre-E0 exe): cannot be proven silent"))
    elif drv not in SILENT_DRIVERS and not allowed("" if rec["target"] == "default" else rec["device"], allow):
        violations.append(dict(rec, why="a real driver opened %s" % (
            "the system default (the owner's speakers)" if rec["target"] == "default"
            else "'%s', not on the allowlist" % rec["device"])))


def _bad(source, lineno, line, why):
    return {"file": source, "line": lineno, "kind": "line", "device": None, "driver": None, "target": None,
            "why": why, "text": line.strip()[:240]}


def scan_text(text, allow, source="<text>"):
    """One log's text -> {opened, violations, warnings, receipts, game, runs, runs_without_receipt}. `opened` = every
    device open (null receipts excluded), each event once; `receipts` = the well-formed [AUDIO_OPEN] lines (null ones
    included). A log can hold several runs (a process appending to it): a run starts at each [LOG_PROFILE] line after
    the first (the logger writes one at start-up; lines before the first belong to the first run). Opens pair only
    within their run, and every run needs its own receipt (`runs_without_receipt` = the first line of each that has
    none)."""
    opened, violations, warnings = [], [], []
    receipts = 0
    game = False
    runs = []
    cur = {"line": 1, "receipts": 0, "ran": False}
    seen_profile = False
    # Waiting halves of an event, keyed (kind, client, device id): the older open lines and the receipts.
    old_wait, rcpt_wait = {}, {}

    def end_run():
        for q in old_wait.values():   # older open lines with no receipt in this run: judged on their own
            for rec in q:
                opened.append(rec)
                _judge(rec, allow, violations)
        old_wait.clear()
        rcpt_wait.clear()
        runs.append(dict(cur))

    def pair(key, rec, is_receipt):
        """Match rec with the other half of its event (either order); returns that half or None."""
        mine, other = (rcpt_wait, old_wait) if is_receipt else (old_wait, rcpt_wait)
        q = other.get(key)
        if q:
            return q.pop(0)
        mine.setdefault(key, []).append(rec)
        return None

    def agree(rcpt, old):
        if old["driver"] is not None and (old["device"], old["driver"]) != (rcpt["device"], rcpt["driver"]):
            violations.append(dict(rcpt, why="the receipt disagrees with its open line %s:%d (%s on %s)" % (
                old["file"], old["line"], old["device"], old["driver"])))

    for lineno, line in enumerate(text.splitlines(), 1):
        if RE_PROFILE.search(line):
            if seen_profile:   # the next run starts here
                end_run()
                cur = {"line": lineno, "receipts": 0, "ran": False}
            seen_profile = True
        if RE_GAME.search(line):
            game = True
        if not cur["ran"] and RE_AUDIO_RAN.search(line):
            cur["ran"] = True
        if TAG_OPEN in line:
            m = RE_OPEN.search(line)
            bad = None
            if not m:
                bad = "a malformed [AUDIO_OPEN] receipt"
            elif (m.group(3) == "none") != (m.group(5) == "null"):
                bad = "a receipt whose target and driver disagree (none <-> null)"
            elif m.group(3) == "none" and (m.group(2) != "0" or m.group(4) != "null"):
                bad = "a null receipt naming a device"
            elif m.group(3) == "default" and m.group(4) != "default":
                bad = "a system-default receipt naming a device"
            if bad:
                violations.append(_bad(source, lineno, line, bad))
                continue
            receipts += 1
            cur["receipts"] += 1
            if m.group(3) == "none":
                continue
            kind = "sdl" if m.group(1) is not None else "legacy"
            rec = {"file": source, "line": lineno, "kind": kind, "device": m.group(4), "driver": m.group(5),
                   "target": m.group(3), "source": m.group(6)}
            old = pair((kind, m.group(1), m.group(2) if kind == "sdl" else None), rec, True)
            if old is not None:
                agree(rec, old)
            opened.append(rec)
            _judge(rec, allow, violations)
            continue
        if RE_REFUSED.search(line):
            warnings.append({"file": source, "line": lineno, "text": line.strip()[:240]})
            continue
        if TAG_SINK_OLD.search(line):
            m = RE_SINK_OLD.search(line)
            if not m:
                violations.append(_bad(source, lineno, line, "an [AUDIO_OUT] device-open line that cannot be parsed"))
                continue
            name = m.group(3) if m.group(3) is not None else "default"
            rec = {"file": source, "line": lineno, "kind": "sdl", "device": name, "driver": m.group(4),
                   "target": "default" if name == "default" else "named"}
            rcpt = pair(("sdl", m.group(1), m.group(2)), rec, False)
            if rcpt is not None:
                agree(rcpt, rec)
            continue
        if RE_LEGACY_OLD.search(line):
            rec = {"file": source, "line": lineno, "kind": "legacy", "device": "default", "driver": None,
                   "target": "default"}
            pair(("legacy", None, None), rec, False)
            continue
    end_run()
    opened.sort(key=lambda r: r["line"])
    violations.sort(key=lambda r: r["line"])
    return {"opened": opened, "violations": violations, "warnings": warnings, "receipts": receipts, "game": game,
            "runs": len(runs),
            # no receipt and no sign that audio ran: no evidence either way
            "runs_without_receipt": [r["line"] for r in runs if not r["receipts"] and not r["ran"]],
            # no receipt, but the callbacks ran: an exe built before E0 (every E0 exe writes one at its open)
            "runs_pre_e0": [r["line"] for r in runs if not r["receipts"] and r["ran"]]}


def exe_identity(exe):
    """The build of the exe the caller grades: ("pre-E0" | "E0" | None, its sha256 or None, why). "E0" if its bytes
    hold E0_RECEIPT_FORMAT (searched across the 1 MiB read boundaries), else "pre-E0"; None if it cannot be read."""
    if not isinstance(exe, str) or not exe or "\0" in exe:
        return None, None, "no usable exe path was given"
    try:
        h, found, tail = hashlib.sha256(), False, b""
        with open(exe, "rb") as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                h.update(b)
                found = found or E0_RECEIPT_FORMAT in tail + b
                tail = b[-(len(E0_RECEIPT_FORMAT) - 1):]
    except (OSError, ValueError) as e:
        return None, None, "the exe under test %s cannot be read (%s)" % (exe, type(e).__name__)
    sha = h.hexdigest().upper()
    return ("E0" if found else "pre-E0"), sha, "exe %s %s the receipt format" % (sha[:12], "holds" if found else "lacks")


def provenance_sha(log):
    """(the sha256 that the roll's provenance.json beside `log` names, or None, why)."""
    try:
        p = json.loads((Path(log).parent / "provenance.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return None, "no readable provenance.json beside the log (%s)" % type(e).__name__
    sha = str(p.get("exeSha256") or "").upper() if isinstance(p, dict) else ""
    if not re.fullmatch(r"[0-9A-F]{64}", sha):
        return None, "provenance.json names no exeSha256"
    return sha, ""


def log_files(paths):
    """(the *.log files, the given paths that do not exist). A directory is searched recursively."""
    out, missing = [], []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            out += sorted(p.rglob("*.log"))
        elif p.exists():
            out.append(p)
        else:
            missing.append(str(p))
    return out, missing


def check(paths, allow="", all_game=False, exe=None):
    """{verdict, opened, violations, warnings, logs, game_logs, no_evidence, pre_e0, skipped, missing, allow, exe} over
    logs / directories. all_game: every log is a game log (none is skipped; an empty one has no receipt). A given path
    that does not exist is missing evidence (NO-EVIDENCE); a log with a run that has no receipt is in no_evidence,
    unless exe (the exe under test, named by the caller) is a verified pre-E0 build and the run is one of it (N/A)."""
    opened, violations, warnings, game_logs, no_evidence, skipped, pre_e0 = [], [], [], [], [], [], []
    ident = None   # (build, sha, why) of the exe under test, read once and only when a receipt-less run needs it
    files, missing = log_files(paths)
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            violations.append({"file": str(f), "line": 0, "kind": "log", "device": None, "driver": None, "target": None,
                               "why": "the log cannot be read (%s): cannot be proven silent" % e})
            continue
        r = scan_text(text, allow, str(f))
        opened += r["opened"]
        violations += r["violations"]
        warnings += r["warnings"]
        if not (r["game"] or all_game):
            skipped.append(str(f))
            continue
        game_logs.append(str(f))
        if r["runs_without_receipt"]:
            no_evidence.append("%s: no receipt, and its audio never visibly ran" % (
                str(f) if r["runs"] == 1 else "%s (the run from line %s)" % (
                    f, ", ".join(str(n) for n in r["runs_without_receipt"]))))
        if r["runs_pre_e0"]:
            which = str(f) if r["runs"] == 1 else "%s (the run from line %s)" % (
                f, ", ".join(str(n) for n in r["runs_pre_e0"]))
            if exe is None:
                why = "no exe under test was named (--exe), so the build is unknown"
            else:
                if ident is None:
                    ident = exe_identity(exe)
                build, sha, why = ident
                if build == "E0":
                    why = "the exe under test is an E0 build (%s)" % why
                elif build == "pre-E0":
                    psha, pwhy = provenance_sha(f)
                    if r["runs"] != 1:
                        why = "the log holds %d runs: its provenance cannot attest each of them" % r["runs"]
                    elif psha is None:
                        why = pwhy
                    elif psha != sha:
                        why = "its provenance names exe %s, not the exe under test %s" % (psha[:12], sha[:12])
                    else:
                        pre_e0.append("%s: %s" % (which, why))
                        continue
            # an E0 exe that lost its receipt, or a build that cannot be verified: no evidence either way
            no_evidence.append("%s: audio ran with no receipt, and %s" % (which, why))
    if violations:
        verdict = "FAIL"
    elif not game_logs or no_evidence or missing:
        verdict = "NO-EVIDENCE"
    else:
        verdict = "N/A" if pre_e0 else "PASS"
    return {"verdict": verdict, "opened": opened, "violations": violations, "warnings": warnings,
            "logs": [str(f) for f in files], "game_logs": game_logs, "no_evidence": no_evidence, "pre_e0": pre_e0,
            "skipped": skipped, "missing": missing, "allow": allow or DEFAULT_ALLOW, "exe": exe}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--allow", default="", help="the allowlist (default: %s; '*' = any)" % DEFAULT_ALLOW)
    ap.add_argument("--all-game", action="store_true", help="every given log is a game log (none is skipped)")
    ap.add_argument("--exe", help="the exe these logs are a roll of (N/A needs it: a verified pre-E0 build)")
    ap.add_argument("--json", help="write the report here")
    a = ap.parse_args()
    r = check(a.paths, a.allow, a.all_game, a.exe)
    if not r["logs"] and not r["missing"]:
        print("audio_safety_check: no logs found in %s" % ", ".join(a.paths))
        return 2
    if a.json:
        Path(a.json).write_text(json.dumps(r, indent=2), encoding="utf-8")
    print("audio_safety_check: %s - %d log(s) (%d game, %d skipped), %d device open(s), %d violation(s), "
          "%d refusal(s), %d game log(s) with no receipt" % (
              r["verdict"], len(r["logs"]), len(r["game_logs"]), len(r["skipped"]), len(r["opened"]),
              len(r["violations"]), len(r["warnings"]), len(r["no_evidence"])))
    for v in r["violations"]:
        print("  VIOLATION %s:%d %s (driver=%s, device=%s)" % (v["file"], v["line"], v["why"], v["driver"], v["device"]))
    for w in r["warnings"]:
        print("  refused   %s:%d %s" % (w["file"], w["line"], w["text"]))
    for f in r["no_evidence"]:
        print("  NO EVIDENCE %s" % f)
    for f in r["pre_e0"]:
        print("  N/A (pre-E0 build) %s (audio ran with no [AUDIO_OPEN] receipt)" % f)
    for f in r["missing"]:
        print("  MISSING   %s" % f)
    return {"PASS": 0, "FAIL": 1, "N/A": 4}.get(r["verdict"], 3)


if __name__ == "__main__":
    sys.exit(main())
