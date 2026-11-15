#!/usr/bin/env python3
"""Focus and parent-restore check for a menu-census roll (lane E, RELEASE_PLAN step 5 item E6, 2026-10-23).

    python tools/menu_focus_check.py <log> --spec <spec.json> [--json out.json]

The roll runs with SR_MENU_CENSUS=1 (every change of the menu on screen is one complete [MENU_CENSUS] line,
kernel/mouse_ui.cpp censusLine; incomplete snapshots are never logged) and, for the release check, SR_KB_PAD_OBS=1
([KBPAD_OBS] lines carry the pad mask the keyboard / harness keys produce, every nonzero sample and every change).
The spec names the visits the scenario makes, in order:

  {"visits": [
     {"name": "settings", "parent": {"page": "MAIN MENU"}, "row": 2, "child": {"page": "SETTINGS"},
      "no_select": "\\\\[SR_FSCMD\\\\].*menu button=[0-9]", "back_mask": "0x2000"},
     {"name": "exit-no", "parent": {"page": "MAIN MENU"}, "row": 0, "return_sel": 4, "child": {"page": "WARNING"},
      "child_also": [{"page": "SOME SUB PAGE"}], "via": [{"what": "title"}], "via_ms": 1000}
  ]}

A surface matcher compares census fields exactly: any of screen, page, what (the kind before '@', or the whole
field when the value has '@'), cancel. For each visit, searching on from where the previous one ended (so a RETURN
line can be the next visit's OPEN):
  OPEN    the first line of the parent with sel == row;
  CHILD   the next surface after OPEN (lines of the parent are its own focus moves; "screen=-" lines - no menu, a
          transition - are skipped everywhere) must be the child - else FAIL "opened <it>, not the child" - and the
          last parent line before it must still have sel == row (else FAIL "opened from row N"). A "via" surface (the game's own transition: the screen under a popup, shown for a
          moment while one popup replaces another) is skipped before the child when the whole run of via records,
          from the first to the next surface that is not a via, lasts at most via_ms (default 1000; 0 < via_ms <=
          10000), else FAIL "stayed on <it>";
  RETURN  the next surface after the child's lines (the child's own focus moves, and any child_also surface; a via
          surface as above) must be the parent - else FAIL "BACK went to <it>" - with sel == return_sel (default
          row) - else FAIL "focus not restored";
  SELECT  with no_select (a regex): from [KBPAD_OBS] EDGES (a bit set that the previous sample lacked; a held key
          repeats its samples): the BACK press is the last press edge of back_mask between the child's first line and
          RETURN (required with no_select; none = NO-EVIDENCE), its release the first later sample without it, and
          the window runs from the press to the next press edge of any button after the release (a key pressed while
          BACK was still held is not one). A line matching no_select in what the log shows of that window is FAIL "a
          SELECT fired after BACK". The release must be seen (else NO-EVIDENCE); a window with no next input is
          complete only if the log runs on for settle_ms (default 2000; 0 < settle_ms <= 600000) after the release
          by its timestamps; else NO-EVIDENCE.
Every line holding "[MENU_CENSUS]" must be a complete record of the producer's format (fields, exactly hints=N
hint entries - the count splits labels the producer does not escape - and the trailer); one that is not is FAIL. Verdict: PASS (every visit passed), FAIL, NO-EVIDENCE (no FAIL, but a
visit's OPEN / CHILD / RETURN / release window never appears; an empty spec). Exit 0 / 1 / 3; 2 = the log or the
spec cannot be read, the spec is invalid, or --json cannot be written.
A log with "[PORT] applied custom_frontend" (port features on) is also graded by tools/main_receipt.py --expect
override (E3-E6 r3): the roll must have registered the port feature's MAIN (feature.json's sha256), never the retail
one - a "main-receipt" row, FAIL / NO-EVIDENCE like a visit.
"""
import argparse
import datetime
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import main_receipt  # noqa: E402  (E3-E6 r3: which MAIN ran)

TAG = "[MENU_CENSUS]"   # "[MENU_CENSUS_SKIP]" does not contain it
HINT = r"\['.*?'(?: icon=\S+)? (?:\(-?\d+,-?\d+\)-\(-?\d+,-?\d+\)|hidden)\]"
RE_HEAD = re.compile(r"\[MENU_CENSUS\] #(\d+) screen=(\S+) page='(.*?)' what=(\S+) cancel=(\S+) items=(\d+) "
                     r"sel=(-?\d+) locked=[01] title=[01] hints=(\d+)")
_TAILS = {}


def tail_re(n):
    """The rest of a record with EXACTLY n hint entries: the producer does not escape labels, so a label shaped like
    an entry boundary ("X' hidden] ['Y") is split by the count, never miscounted (Astra E6 r2)."""
    if n not in _TAILS:
        _TAILS[n] = re.compile(r"(?: " + HINT + r"){%d} prev=\S+ reads_left=-?\d+ incomplete_skipped=\d+\s*$" % n)
    return _TAILS[n]
RE_KBPAD = re.compile(r"\[KBPAD_OBS\] poll=\d+ .*?mappedMask=0x([0-9A-Fa-f]{1,4})\b")
RE_TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
FIELDS = ("screen", "page", "what", "cancel")


class SpecError(ValueError):
    pass


def validate(spec):
    if not isinstance(spec, dict) or not isinstance(spec.get("visits"), list):
        raise SpecError("the spec needs a list 'visits'")
    out = []
    for n, v in enumerate(spec["visits"]):
        if not isinstance(v, dict):
            raise SpecError("visit %d is not an object" % n)
        for k in ("parent", "child"):
            m = v.get(k)
            if not isinstance(m, dict) or not m:
                raise SpecError("visit %d: '%s' must be a non-empty matcher" % (n, k))
            bad = [f for f in m if f not in FIELDS]
            if bad:
                raise SpecError("visit %d: unknown matcher field(s) %s" % (n, bad))
        also = v.get("child_also", [])
        if not isinstance(also, list) or any(not isinstance(a, dict) or not a or
                                             any(f not in FIELDS for f in a) for a in also):
            raise SpecError("visit %d: child_also must be a list of matchers" % n)
        via = v.get("via", [])
        if not isinstance(via, list) or any(not isinstance(a, dict) or not a or
                                            any(f not in FIELDS for f in a) for a in via):
            raise SpecError("visit %d: via must be a list of matchers" % n)
        if any(a == v["parent"] or a == v["child"] for a in via):
            raise SpecError("visit %d: a via matcher equals the parent's or the child's" % n)
        via_ms = v.get("via_ms", 1000)
        if isinstance(via_ms, bool) or not isinstance(via_ms, (int, float)) or not 0 < via_ms <= 10000 or \
                (isinstance(via_ms, float) and not math.isfinite(via_ms)):
            raise SpecError("visit %d: via_ms must be a number in (0, 10000]" % n)
        if v["child"] == v["parent"]:
            raise SpecError("visit %d: the child matcher equals the parent's" % n)
        def integral(x):
            return isinstance(x, int) and not isinstance(x, bool)
        if not integral(v.get("row")) or not integral(v.get("return_sel", 0)):
            raise SpecError("visit %d: 'row' (and 'return_sel') must be integers" % n)
        row = v["row"]
        back_sel = v.get("return_sel", row)
        rx, mask = None, 0
        if v.get("no_select") is not None:
            if not isinstance(v["no_select"], str):
                raise SpecError("visit %d: no_select must be a regex string" % n)
            try:
                rx = re.compile(v["no_select"])
            except re.error as ex:
                raise SpecError("visit %d: no_select: %s" % (n, ex))
            try:
                mask = int(str(v["back_mask"]), 16)
            except (KeyError, ValueError):
                raise SpecError("visit %d: no_select needs back_mask (the BACK key's pad bit, hex)" % n)
            if not 0 < mask <= 0xFFFF:
                raise SpecError("visit %d: back_mask out of range" % n)
        settle = v.get("settle_ms", 2000)
        if isinstance(settle, bool) or not isinstance(settle, (int, float)) or                 not 0 < settle <= 600000 or (isinstance(settle, float) and not math.isfinite(settle)):
            raise SpecError("visit %d: settle_ms must be a number in (0, 600000]" % n)   # no float() of a huge int
        settle = float(settle)
        out.append({"name": str(v.get("name", "visit %d" % n)), "parent": v["parent"], "child": v["child"],
                    "also": also, "row": row, "back_sel": back_sel, "rx": rx, "mask": mask, "settle": settle,
                    "via": via, "via_ms": float(via_ms)})
    return out


def stamp(line):
    m = RE_TIME.match(line)
    if not m:
        return None
    return datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")


def parse_log(text):
    """(lines, census [(line_index, record | None)], pad [(line_index, mask)])"""
    lines = text.splitlines()
    census, pad = [], []
    for i, ln in enumerate(lines):
        if TAG in ln:
            m = RE_HEAD.search(ln)
            ok = m is not None and int(m.group(8)) <= 12 and tail_re(int(m.group(8))).match(ln, m.end()) is not None
            census.append((i, {"seq": int(m.group(1)), "screen": m.group(2), "page": m.group(3), "what": m.group(4),
                               "cancel": m.group(5), "items": int(m.group(6)), "sel": int(m.group(7))}
                           if ok else None))
            continue
        k = RE_KBPAD.search(ln)
        if k:
            pad.append((i, int(k.group(1), 16)))
    return lines, census, pad


def matches(c, matcher):
    for k, want in matcher.items():
        if k not in FIELDS:
            raise SpecError("unknown matcher field %r" % k)
        have = c[k]
        if k == "what" and "@" not in str(want):
            have = have.split("@", 1)[0]
        if have != str(want):
            return False
    return True


def skip_via(lines, surf, k, v):
    """-> (k, stay): past the visit's via surfaces at surf[k:] (the game's own transition between two popups), only
    while the whole run of them - from its first record to the next surface that is not a via - lasts at most via_ms
    (E3-E6 r5, Astra r4 P2: per record, hint or lock changes on the via would hide a long stay); stay = the FAIL
    reason when it lasted longer (or a time cannot be read), else "". A via run the log ends on is left for the
    caller (nothing opened / never closed)."""
    k0 = k
    while k < len(surf) and any(matches(surf[k][1], a) for a in v["via"]):
        if stamp(lines[surf[k][0]]) is None:   # every via record (E3-E6 r6, Astra r5 P2), not only the first
            return k, "a via surface (%s) without a readable time stamp" % describe(surf[k][1])
        k += 1
    if k == k0 or k >= len(surf):
        return k, ""
    t0, t1 = stamp(lines[surf[k0][0]]), stamp(lines[surf[k][0]])
    if t1 is None:
        return k0, "the surface after a via run (%s) without a readable time stamp" % describe(surf[k][1])
    ms = (t1 - t0).total_seconds() * 1000.0
    if ms > v["via_ms"]:
        return k0, "stayed on %s for %.0f ms over %d record(s) (more than via_ms %.0f: not a transition)" % (
            describe(surf[k0][1]), ms, k - k0, v["via_ms"])
    return k, ""


def describe(c):
    return "screen=%s page='%s' sel=%d" % (c["screen"], c["page"], c["sel"])


def release_window(lines, pad, child_first, ret_line, mask, settle):
    """(start, end, why): the no-SELECT window [start, end) as far as the log shows it; why = "" when it is complete,
    else why it is not (no BACK press: start None; no release seen; no next input and less than settle_ms of log
    after the release)."""
    # edges, not samples (Astra E6 r2): a held key repeats its samples, and a key pressed while BACK is still held
    # is no new input after the release
    new = [mk & ~(pad[j - 1][1] if j else 0) for j, (_, mk) in enumerate(pad)]
    press = [j for j, (li, mk) in enumerate(pad) if child_first < li < ret_line and new[j] & mask]
    if not press:
        return None, None, "no BACK press (%s) seen between the child and its return" % hex(mask)
    p = press[-1]
    start = pad[p][0]
    rel = next((j for j in range(p + 1, len(pad)) if not pad[j][1] & mask), None)
    if rel is None:   # a release must be SEEN: a SELECT it triggers comes after it (Astra E6 r3)
        return start, len(lines), "the BACK key's release was never seen"
    nxt = next((j for j in range(rel + 1, len(pad)) if new[j]), None)
    if nxt is not None:
        return start, pad[nxt][0], ""
    # no next input: complete only if the log runs on for settle_ms after the release
    t0 = stamp(lines[pad[rel][0]])
    t_end = next((stamp(lines[j]) for j in range(len(lines) - 1, -1, -1) if stamp(lines[j])), None)
    if t0 is None or t_end is None or (t_end - t0).total_seconds() * 1000.0 < settle:
        return start, len(lines), "the BACK key's release window was not observed for %.0f ms" % settle
    return start, len(lines), ""


def check(text, spec, feature_json=main_receipt.FEATURE_JSON):
    visits = validate(spec)
    lines, census, pad = parse_log(text)
    bad = [i for i, c in census if c is None]
    if bad:
        return "FAIL", [{"name": "-", "verdict": "FAIL",
                         "why": "%d malformed [MENU_CENSUS] line(s), first at log line %d" % (len(bad), bad[0] + 1)}]
    surf = [(i, c) for i, c in census if c["screen"] != "-"]   # transitions / no menu are skipped
    results = []
    pos = 0
    for v in visits:
        parent, child, row = v["parent"], v["child"], v["row"]
        in_child = lambda c: matches(c, child) or any(matches(c, a) for a in v["also"])  # noqa: E731
        r = {"name": v["name"], "verdict": "NO-EVIDENCE", "why": ""}
        results.append(r)
        k = pos
        while k < len(surf) and not (matches(surf[k][1], parent) and surf[k][1]["sel"] == row):
            k += 1
        if k >= len(surf):
            r["why"] = "the parent never showed with row %d focused" % row
            continue
        k += 1
        while k < len(surf) and matches(surf[k][1], parent):
            k += 1   # the parent's own focus moves after OPEN
        last_parent = k - 1
        k, stay = skip_via(lines, surf, k, v)
        if stay:
            r["verdict"], r["why"] = "FAIL", stay
            pos = k
            continue
        if k >= len(surf):
            r["why"] = "nothing opened from the parent"
            continue
        if not matches(surf[k][1], child):
            r["verdict"], r["why"] = "FAIL", "opened %s, not the child" % describe(surf[k][1])
            pos = k
            continue
        if surf[last_parent][1]["sel"] != row:
            r["verdict"], r["why"] = "FAIL", "the child opened from row %d, not %d" % (surf[last_parent][1]["sel"], row)
            pos = k
            continue
        child_first = surf[k][0]
        while k < len(surf) and in_child(surf[k][1]):
            k += 1
        k, stay = skip_via(lines, surf, k, v)
        if stay:
            r["verdict"], r["why"] = "FAIL", stay
            pos = k
            continue
        if k >= len(surf):
            r["why"] = "the child never closed"
            continue
        ret = surf[k][1]
        if not matches(ret, parent):
            r["verdict"], r["why"] = "FAIL", "BACK went to %s, not the parent" % describe(ret)
            pos = k
            continue
        if ret["sel"] != v["back_sel"]:
            r["verdict"], r["why"] = "FAIL", "focus not restored: the parent came back on row %d, not %d" % (
                ret["sel"], v["back_sel"])
            pos = k
            continue
        if v["rx"] is not None:
            start, end, why = release_window(lines, pad, child_first, surf[k][0], v["mask"], v["settle"])
            # a SELECT in what the log shows of the window FAILs even when the window is incomplete
            hit = next((j for j in range(start + 1, end) if v["rx"].search(lines[j])), None)                 if start is not None else None
            if hit is not None:
                r["verdict"], r["why"] = "FAIL", "a SELECT fired after BACK (log line %d: %s)" % (
                    hit + 1, lines[hit].strip()[:120])
                pos = k
                continue
            if why:
                r["why"] = why
                pos = k
                continue
        r["verdict"], r["why"] = "PASS", "row %d -> %s -> back on row %d" % (row, child, v["back_sel"])
        pos = k
    if results and main_receipt.port_features_on(text):
        # E3-E6 r3 (coordinator): a port-features-on roll that registered the retail MAIN ran none of the class
        # patches inside MAIN (the E4 BACK hints among them) - FAIL; no [ASSET] line = NO-EVIDENCE
        mv, mwhy = main_receipt.grade(text, "override", main_receipt.expected_sha(feature_json))
        results.append({"name": "main-receipt", "verdict": mv, "why": "; ".join(mwhy)})
    verdicts = [r["verdict"] for r in results]
    if "FAIL" in verdicts:
        return "FAIL", results
    if not results or "NO-EVIDENCE" in verdicts:
        return "NO-EVIDENCE", results
    return "PASS", results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--spec", required=True)
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    try:
        with open(a.log, encoding="utf-8", errors="replace") as f:
            text = f.read()
        with open(a.spec, encoding="utf-8") as f:
            spec = json.load(f)
        verdict, results = check(text, spec)
    except (OSError, ValueError) as ex:   # SpecError is a ValueError; so is bad JSON
        print("menu-focus: cannot check: %s" % ex)
        return 2
    for r in results:
        print("  %-12s %-11s %s" % (r["name"], r["verdict"], r["why"]))
    print("menu-focus: %s (%d visit(s))" % (verdict, len(results)))
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump({"verdict": verdict, "visits": results}, f, indent=1)
        except OSError as ex:
            print("menu-focus: cannot write %s: %s" % (a.json, ex))
            return 2
    return {"PASS": 0, "FAIL": 1, "NO-EVIDENCE": 3}[verdict]


if __name__ == "__main__":
    sys.exit(main())
