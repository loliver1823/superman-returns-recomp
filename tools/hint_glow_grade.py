#!/usr/bin/env python3
"""Grade a mouse roll's button-hint clicks and hover glow from its log (lane E, RELEASE_PLAN step 5, E5 / E5b).

  python tools/hint_glow_grade.py <run.log> [--clicks 304:2,302:2] [--json out.json]

It reads the cursor's own lines (gates SR_MOUSE_UI_LOG=1; kernel/mouse_ui.cpp):
  [MOUSE_UI] call gEH.HandleEvent(K) (hint click) obj=... done|NOT CALLED sel=.. '..'   a hint click (E5)
  [MOUSE_UI] call <method>(<arg>) (<why>) obj=... done|NOT CALLED ...                      any other cursor call
  [MOUSE_UI] glow on label=L done|NOT CALLED                                              E5b: PartText.SetFontColor(on)
  [MOUSE_UI] glow off label=L done|NOT CALLED[ (kept; retried later)]                     the restore (the guest puts
                                                                                          back what L itself recorded)
  [MOUSE_UI] glow dropped label=L (gone: no hint label on any screen)                     nothing left to restore
  [MOUSE_UI] glow record seen label=L (srOwn: the patched PartText.SetFontColor kept      E5b r13: the live receipt,
             the label's own colour)                                                      once (main_receipt e5b)

Glow rules (FAIL):
  G1 an on that was done needs no OTHER glow standing (an on of the standing label is the cursor's re-apply: a new
     object at that address, or the game recoloured it - it stays the standing glow);
  G2 an off (done or not) or a drop names the standing glow's label;
  G4 nothing but an on / off / drop / record-seen line of the format above (a malformed glow line is FAIL);
  G5 (E5b r13) the record-seen receipt names the standing glow's label (the cursor reads the record of the label it
     made glow; it changes no state).
The colour itself is the guest's (tools/apt/mods/custom_frontend/66_parttext_glow.patch: the restore puts back what
that very object recorded at the on); the frames show it.
A glow still standing at the end of the log is reported (the process ended with it), not a FAIL.
Click rules (FAIL):
  C1 with --clicks, each key's hint clicks that were done = the expected count;
  C2 exactly once: no other done ACTIVATION call within 250 ms (inclusive) after a done hint click (one click, one
     event). Navigation is not an activation: the hover's focus steps ("focus") and wheel steps ("wheel");
  C3 a hint click NOT CALLED is reported (FAIL: the click was lost);
  C4 a cursor call line without the log's millisecond time stamp cannot be graded for C2 (FAIL);
  C5 every "[MOUSE_UI] call " record has the producer's whole format through its end (a truncated or garbled
     record FAILs: it could hide a second event).
  M1 (E3-E6 r3) a log with "[PORT] applied custom_frontend" (port features on) must have registered the port
     feature's MAIN (tools/main_receipt.py --expect override: feature.json's sha256), never the retail one - the hint
     icon ids and the glow are class patches inside MAIN; no [ASSET] MAIN line is NO-EVIDENCE; an M1 FAIL wins.
Verdict: PASS, FAIL, NO-EVIDENCE (no [MOUSE_UI] call or glow line at all, or M1 has no [ASSET] line). Exit 0 PASS /
1 FAIL / 3 NO-EVIDENCE / 2 unreadable log or feature.json, bad --clicks or unwritable json.
"""
import argparse
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import main_receipt  # noqa: E402  (E3-E6 r3: which MAIN ran)

RE_TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
RE_CALL = re.compile(r"\[MOUSE_UI\] call (\S+?)\(([^)]*)\) \(([^)]*)\) obj=[0-9A-F]{8} (done|NOT CALLED) sel=-?\d+ '.*'$")
RE_GLOW_ON = re.compile(r"\[MOUSE_UI\] glow on label=([0-9A-F]{8}) (done|NOT CALLED)$")
RE_GLOW_OFF = re.compile(r"\[MOUSE_UI\] glow off label=([0-9A-F]{8}) (done|NOT CALLED)( \(kept; retried later\))?$")
RE_GLOW_DROP = re.compile(r"\[MOUSE_UI\] glow dropped label=([0-9A-F]{8}) \(gone: no hint label on any screen\)$")
# E5b r13: the live receipt (kernel/mouse_ui.cpp io.glowing; tools/main_receipt.py --live e5b)
RE_GLOW_SEEN = re.compile(r"\[MOUSE_UI\] glow record seen label=([0-9A-F]{8}) \(srOwn: the patched "
                          r"PartText\.SetFontColor kept the label's own colour\)$")
NAVIGATION = ("focus", "wheel")   # cursor call reasons that move the focus and activate nothing
CLICK_WINDOW_S = 0.25


def stamp(line):
    m = RE_TIME.match(line)
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def parse_clicks(text):
    want = {}
    if not text:
        return want
    for part in text.split(","):
        k, _, n = part.strip().partition(":")
        if not k.isdigit() or not n.isdigit():
            raise ValueError("--clicks wants KEY:N[,KEY:N...] (e.g. 304:2,302:2), not %r" % text)
        want[int(k)] = int(n)
    return want


def grade(text, want_clicks=None, feature_json=main_receipt.FEATURE_JSON):
    fails, notes = [], []
    standing = None   # (label, line number)
    calls = []        # (time, kind, key or None, done, line number)
    seen = 0
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip()
        if "[MOUSE_UI] glow " in line:
            seen += 1
            m_on, m_off, m_drop = RE_GLOW_ON.search(line), RE_GLOW_OFF.search(line), RE_GLOW_DROP.search(line)
            m_seen = RE_GLOW_SEEN.search(line)
            if m_seen:
                if not standing or standing[0] != m_seen.group(1):
                    fails.append("line %d: G5 the glow record of %s was seen, but %s" % (
                        n, m_seen.group(1), "%s glows" % standing[0] if standing else "nothing glows"))
                continue   # the receipt changes no glow state
            if m_on:
                label, res = m_on.group(1), m_on.group(2)
                if res != "done":
                    continue   # a refused on changes nothing
                if standing and standing[0] != label:
                    fails.append("line %d: G1 an on of %s while %s still glows (line %d)" % (n, label, standing[0], standing[1]))
                standing = (label, n)   # an on of the standing label = its re-apply
            elif m_off:
                label, res = m_off.group(1), m_off.group(2)
                if (res == "done") == bool(m_off.group(3)):
                    # the producer adds the suffix exactly to a refused restore
                    fails.append("line %d: G4 a malformed glow line: %s" % (n, line[-120:]))
                    continue
                if not standing or standing[0] != label:
                    fails.append("line %d: G2 an off of %s, but %s" % (n, label, "%s glows" % standing[0] if standing else "nothing glows"))
                    continue
                if res == "done":
                    standing = None
            elif m_drop:
                label = m_drop.group(1)
                if not standing or standing[0] != label:
                    fails.append("line %d: G2 a drop of %s, but %s" % (n, label, "%s glows" % standing[0] if standing else "nothing glows"))
                    continue
                standing = None
            else:
                fails.append("line %d: G4 a malformed glow line: %s" % (n, line[-120:]))
            continue
        m = RE_CALL.search(line)
        if not m and "[MOUSE_UI] call " in line:
            seen += 1
            fails.append("line %d: C5 a malformed call record: %s" % (n, line[-120:]))
            continue
        if m:
            seen += 1
            method, arg, why, res = m.groups()
            t = stamp(line)
            if t is None:
                fails.append("line %d: C4 a cursor call without a time stamp cannot be graded" % n)
            hint = why == "hint click"
            key = int(arg) if hint and arg.isdigit() else None
            if hint and res != "done":
                fails.append("line %d: C3 a hint click (%s) was NOT CALLED" % (n, arg))
            calls.append((t, "hint" if hint else why, key, res == "done", n))
    if standing:
        notes.append("a glow of %s (line %d) still stood at the end of the log" % (standing[0], standing[1]))
    counts = {}
    for i, (t, kind, key, done, n) in enumerate(calls):
        if kind != "hint" or not done:
            continue
        counts[key] = counts.get(key, 0) + 1
        for t2, kind2, key2, done2, n2 in calls[i + 1:]:
            if t is None or t2 is None:
                break   # C4 already FAILed this roll
            if (t2 - t).total_seconds() > CLICK_WINDOW_S:
                break
            if done2 and kind2 not in NAVIGATION:
                fails.append("line %d: C2 a second call (%s) %d ms after the hint click of line %d" % (
                    n2, kind2 if kind2 != "hint" else "hint click %s" % key2, int((t2 - t).total_seconds() * 1000), n))
    for key, n in sorted((want_clicks or {}).items()):
        if counts.get(key, 0) != n:
            fails.append("C1 hint clicks of key %d: %d done, expected %d" % (key, counts.get(key, 0), n))
    main_missing = False
    if main_receipt.port_features_on(text):
        mv, mwhy = main_receipt.grade(text, "override", main_receipt.expected_sha(feature_json))
        if mv == "FAIL":
            fails.append("M1 " + "; ".join(mwhy))
        else:
            notes.append("M1 " + "; ".join(mwhy))
            main_missing = mv == "NO-EVIDENCE"
    # E3-E6 r4 (Astra r3 P2): a retail (or foreign) MAIN on a port-features-on roll is FAIL whatever else the log
    # lacks; the other rules keep their boundary (no call line at all = NO-EVIDENCE)
    if any(f.startswith("M1 ") for f in fails):
        return "FAIL", fails, notes, counts
    if not seen or (main_missing and not fails):
        return "NO-EVIDENCE", fails, notes, counts
    return ("FAIL" if fails else "PASS"), fails, notes, counts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--clicks", default="")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    try:
        want = parse_clicks(a.clicks)
        with open(a.log, encoding="utf-8", errors="replace") as f:
            text = f.read()
        verdict, fails, notes, counts = grade(text, want)
    except (OSError, ValueError) as ex:
        print("hint-glow: cannot grade: %s" % ex)
        return 2
    for f_ in fails:
        print("  FAIL %s" % f_)
    for x in notes:
        print("  note %s" % x)
    print("hint-glow: %s (hint clicks done by key: %s)" % (
        verdict, ", ".join("%s=%d" % (k, v) for k, v in sorted(counts.items(), key=lambda kv: str(kv[0]))) or "none"))
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump({"verdict": verdict, "fails": fails, "notes": notes,
                           "clicks": {str(k): v for k, v in counts.items()}}, f, indent=1)
        except OSError as ex:
            print("hint-glow: cannot write %s: %s" % (a.json, ex))
            return 2
    return {"PASS": 0, "FAIL": 1, "NO-EVIDENCE": 3}[verdict]


if __name__ == "__main__":
    sys.exit(main())
