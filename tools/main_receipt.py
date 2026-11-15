#!/usr/bin/env python3
"""Which MAIN a roll registered, and did the patched Apt code run (lane E, RELEASE_PLAN step 5 E3-E6 r3, 2026-10-23).

    python tools/main_receipt.py <run.log> --expect override|retail [--live e4,e5] [--feature-json PATH] [--json out.json]

Since E3-E6 r3 the custom_frontend port feature patches classes inside the root movie MAIN (tools/apt/patch_movie.py,
THE FIRST-REGISTRAR RULE). The exe logs one [ASSET] line per registration of MAIN (kernel/asset_receipt.hpp):
    [ASSET] MAIN #<n> source=<retail|override|unknown> file="<where>" sha256=<entry sha256> const=match apt=<match|differs>
--expect override (a port-features-on roll): every MAIN registration must be source=override with the sha256 the
committed feature.json pins for DATA/_workinprogress/custom_frontend/MAIN.ast, and #1's .apt must match the file;
a retail (or unknown) MAIN is FAIL. --expect retail (the port-features-off natural control): every registration must
be source=retail. No registration line at all is NO-EVIDENCE (a pre-r3 exe, or a log cut before the front end), and
so is a log without the #1 line (only later, relocated registrations: E3-E6 r4).

--live (the step-5 standing rule: an Apt-patch unit needs a LIVE receipt that its patched code ran):
  e4  a [MENU_CENSUS] line of a ScrGenPop popup with cancel=1 and a 'BACK' hint - only the patched
      ScrGenPop.PopupInit (60_genpop_hints.patch) shows BACK on a popup;
  e5  a hint whose icon is known: a [MENU_CENSUS] hint with icon=..., or a [MOUSE_UI] "hint N key=K" line - only the
      patched ButtonIcon.SetIcon (65_buttonicon_id.patch) records srIcon;
  e5b (E5b r13) "[MOUSE_UI] glow record seen label=..." - the cursor read the glow record srOwn, which only the
      patched PartText.SetFontColor (66_parttext_glow.patch, a class patch inside MAIN) keeps.
A missing live receipt is FAIL on an override run (the patch was mounted and did not run) and NO-EVIDENCE otherwise.

Exit 0 PASS, 1 FAIL, 3 NO-EVIDENCE, 2 = the log / feature.json cannot be read or a bad argument.
menu_focus_check.py and hint_glow_grade.py call grade() on every log with "[PORT] applied custom_frontend".
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FEATURE_JSON = os.path.join(HERE, "..", "data", "port_features", "custom_frontend", "feature.json")
MAIN_ASSET = "DATA/_workinprogress/custom_frontend/MAIN.ast"
PORT_LINE = "[PORT] applied custom_frontend"
RE_REG = re.compile(r"\[ASSET\] MAIN #(\d+) source=(\S+?)(?::| |$)(?:.*?file=\"(.*?)\" sha256=([0-9a-f]{64}) "
                    r"const=(\S+) apt=(\S+))?")
RE_E4 = re.compile(r"\[MENU_CENSUS\] #\d+ screen=ScrGenPop .*? cancel=1 .*\['BACK'(?: icon=\S+)? ")
RE_E5 = re.compile(r"\[MENU_CENSUS\] #\d+ .*\['[^']*' icon=\S+ |\[MOUSE_UI\]\s+hint \d+ key=\d+ rect=")
RE_E5B = re.compile(r"\[MOUSE_UI\] glow record seen label=[0-9A-F]{8} ")
LIVE = {"e4": (RE_E4, "a ScrGenPop popup with cancel=1 shows a 'BACK' hint (the patched ScrGenPop.PopupInit ran)"),
        "e5": (RE_E5, "a hint names its button (icon= / key=: the patched ButtonIcon.SetIcon ran)"),
        "e5b": (RE_E5B, "a hovered hint's label carries the glow record (srOwn: the patched PartText.SetFontColor ran)")}


def expected_sha(feature_json=FEATURE_JSON):
    with open(feature_json, encoding="utf-8") as f:
        j = json.load(f)
    sha = j.get("assets_sha256", {}).get(MAIN_ASSET)
    if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
        raise ValueError("%s pins no sha256 for %s" % (feature_json, MAIN_ASSET))
    return sha


def registrations(text):
    out = []
    for line in text.splitlines():
        if "[ASSET] MAIN #" not in line:
            continue
        m = RE_REG.search(line)
        if not m:
            out.append({"n": None, "source": "malformed", "line": line.strip()})
            continue
        out.append({"n": int(m.group(1)), "source": m.group(2), "file": m.group(3), "sha256": m.group(4),
                    "const": m.group(5), "apt": m.group(6), "line": line.strip()})
    return out


def grade(text, expect, sha=None, live=()):
    """-> (verdict, [reasons]). expect 'override' needs sha (feature.json's)."""
    regs = registrations(text)
    why = []
    verdict = "PASS"
    if not regs:
        return "NO-EVIDENCE", ["no [ASSET] MAIN registration line (an exe before E3-E6 r3, or the log ends before "
                               "the front end loaded)"]
    for r in regs:
        if r["source"] == "malformed" or (r["source"] in ("retail", "override") and r["sha256"] is None):
            return "FAIL", ["a malformed [ASSET] MAIN line: %s" % r["line"][:160]]
        if r["source"] != expect:
            verdict = "FAIL"
            why.append("MAIN #%s registered source=%s, expected %s%s" % (
                r["n"], r["source"], expect,
                " (a port-features-on roll that ran the RETAIL MAIN: the E4 / E5 patches did not run)"
                if expect == "override" and r["source"] == "retail" else ""))
        elif expect == "override" and r["sha256"] != sha:
            verdict = "FAIL"
            why.append("MAIN #%s is an override with sha256 %s, but feature.json pins %s (a stale or foreign "
                       "generation)" % (r["n"], r["sha256"][:16], sha[:16]))
        if r["n"] == 1 and r["source"] in ("retail", "override") and r["apt"] != "match":
            verdict = "FAIL"
            why.append("MAIN #1's .apt differs from its file at the first registration")
    # E3-E6 r4 (Astra r3 P2): the first registration is the byte-for-byte proof (#1 apt=match); a log without it
    # (cut before it, or holding only a later, relocated registration) proves nothing
    if verdict == "PASS" and not any(r["n"] == 1 for r in regs):
        return "NO-EVIDENCE", ["no [ASSET] MAIN #1 line: the first registration (the byte-for-byte proof) is missing, "
                               "only later ones (#%s)" % ", #".join(str(r["n"]) for r in regs)]
    if verdict == "PASS":
        why.append("%d MAIN registration(s), all source=%s%s" % (
            len(regs), expect, (" sha256 %s (feature.json)" % sha[:16]) if expect == "override" else ""))
    for k in live:
        rx, what = LIVE[k]
        if rx.search(text):
            why.append("live %s: %s" % (k, what))
        else:
            miss = "live %s missing: no line shows that %s" % (k, what)
            why.append(miss)
            if expect == "override":
                verdict = "FAIL"
            elif verdict == "PASS":
                verdict = "NO-EVIDENCE"
    return verdict, why


def port_features_on(text):
    return PORT_LINE in text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--expect", required=True, choices=["override", "retail"])
    ap.add_argument("--live", default="", help="comma list of e4, e5, e5b")
    ap.add_argument("--feature-json", default=FEATURE_JSON)
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    live = [x for x in a.live.split(",") if x]
    bad = [x for x in live if x not in LIVE]
    if bad:
        print("main-receipt: unknown --live %s (e4, e5, e5b)" % ",".join(bad))
        return 2
    try:
        with open(a.log, encoding="utf-8", errors="replace") as f:
            text = f.read()
        sha = expected_sha(a.feature_json) if a.expect == "override" else None
    except (OSError, ValueError) as ex:
        print("main-receipt: cannot check: %s" % ex)
        return 2
    verdict, why = grade(text, a.expect, sha, live)
    for w in why:
        print("  " + w)
    print("main-receipt: %s (expect %s)" % (verdict, a.expect))
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump({"verdict": verdict, "expect": a.expect, "why": why}, f, indent=1)
        except OSError as ex:
            print("main-receipt: cannot write %s: %s" % (a.json, ex))
            return 2
    return {"PASS": 0, "FAIL": 1, "NO-EVIDENCE": 3}[verdict]


if __name__ == "__main__":
    sys.exit(main())
