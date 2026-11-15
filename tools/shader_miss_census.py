#!/usr/bin/env python3
"""shader_miss_census.py - every shader a run had to translate at run time, waited for, or drew without (render/shader-coverage).

Reads game logs (run.log / superman_returns_run<N>.log) and names, per log:
  - every runtime translation request   [SHADER_XLAT] request VS|PS <hash>        (an AOT-cache miss)
  - every miss-policy wait and outcome  [SHADER_WAIT] VS|PS <hash>: <outcome> after N ms ...
  - every admission / refusal           [SHADER_XLAT] admitted|WAITING|REFUSED|FAILED ...
  - every draw without its shader       [SHADER_DROP] ... / [SHADER_MISS] ... (dev)
  - a release stop on one               [NATIVE_RECORDING] FAILED stage=untranslated-draw ...
The AOT cache covers a run when it requested nothing; the miss policy held when no draw was dropped and the release
did not stop.

    python tools/shader_miss_census.py <log> [<log> ...] [--json out.json] [--strict]

Exit 0 = no drop and no untranslated-draw stop in any log (the policy held); 1 = a drop or a stop; with --strict also
1 when any log requested a runtime translation (the AOT cache did not cover the run); 2 = a log is missing or holds no
[SHADER_CACHE] loaded line (not a game log of this era: no vacuous pass).
Logs are read whole: the [SHADER_WAIT] / [SHADER_DROP] tags are milestone tags (kept in a player log); the
[SHADER_XLAT] request line is Info (a dev log only), so a player log's requests are counted from its waits.
"""
import argparse
import json
import os
import re
import sys

RX = {
    "loaded": re.compile(r"\[SHADER_CACHE\] loaded (\d+) VS \+ (\d+) PS translated shaders from (\S+)"),
    "request": re.compile(r"\[SHADER_XLAT\] request (VS|PS) ([0-9A-F]{16})"),
    "wait": re.compile(r"\[SHADER_WAIT\] (VS|PS) ([0-9A-F]{16}): (.*?) after ([0-9.]+) ms of waiting"),
    "wait_timeout": re.compile(r"\[SHADER_WAIT\] (VS|PS) ([0-9A-F]{16}): NOT translated within"),
    "admitted": re.compile(r"\[SHADER_XLAT\] admitted (VS|PS) ([0-9A-F]{16}) \((\w+)"),
    "waiting": re.compile(r"\[SHADER_XLAT\] WAITING (VS|PS) ([0-9A-F]{16})"),
    "refused": re.compile(r"\[SHADER_XLAT\] (?:REFUSED|FAILED|NO-TEMPLATE|\w+) (VS|PS) ([0-9A-F]{16}): .* - its draws"),
    "drop": re.compile(r"\[SHADER_DROP\] (VS|PS) ([0-9A-F]{16})"),
    "miss": re.compile(r"\[SHADER_MISS\] VS ([0-9A-F]{16}) has no translation"),
    "stop": re.compile(r"\[NATIVE_RECORDING\] FAILED stage=untranslated-draw .*?vs=([0-9A-F]{16}) ps=([0-9A-F]{16}) "
                       r"reason=(\S+)(?: code=\S+)?(?: xlat=(\S+))?"),
}


def census(path):
    r = {"log": path, "aot": None, "requests": [], "waits": [], "admitted": [], "no_template": [], "refused": [],
         "drops": [], "stops": []}
    seen_req = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[SHADER_" not in line and "[NATIVE_RECORDING]" not in line:
                continue
            m = RX["loaded"].search(line)
            if m and r["aot"] is None:
                r["aot"] = {"vs": int(m.group(1)), "ps": int(m.group(2)), "dir": m.group(3)}
                continue
            m = RX["request"].search(line)
            if m:
                k = m.group(1) + " " + m.group(2)
                if k not in seen_req:
                    seen_req.add(k)
                    r["requests"].append(k)
                continue
            m = RX["wait"].search(line)
            if m:
                r["waits"].append({"shader": m.group(1) + " " + m.group(2), "outcome": m.group(3),
                                   "ms": float(m.group(4))})
                continue
            m = RX["wait_timeout"].search(line)
            if m:
                r["waits"].append({"shader": m.group(1) + " " + m.group(2), "outcome": "budget exceeded", "ms": None})
                continue
            m = RX["admitted"].search(line)
            if m:
                r["admitted"].append({"shader": m.group(1) + " " + m.group(2), "path": m.group(3)})
                continue
            m = RX["waiting"].search(line)
            if m:
                r["no_template"].append(m.group(1) + " " + m.group(2))
                continue
            m = RX["drop"].search(line)
            if m:
                k = m.group(1) + " " + m.group(2)
                if k not in r["drops"]:
                    r["drops"].append(k)
                continue
            m = RX["miss"].search(line)
            if m:
                k = "VS " + m.group(1)
                if k not in r["drops"]:
                    r["drops"].append(k)
                continue
            m = RX["stop"].search(line)
            if m:
                r["stops"].append({"vs": m.group(1), "ps": m.group(2), "reason": m.group(3), "xlat": m.group(4)})
                continue
            m = RX["refused"].search(line)
            if m:
                r["refused"].append(m.group(1) + " " + m.group(2))
    # A player log has no request lines (Info): every wait, admission and WAITING line implies one request.
    for k in [w["shader"] for w in r["waits"]] + [a["shader"] for a in r["admitted"]] + r["no_template"]:
        if k not in seen_req:
            seen_req.add(k)
            r["requests"].append(k)
    r["wait_ms_total"] = round(sum(w["ms"] or 0 for w in r["waits"]), 1)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--strict", action="store_true", help="also fail on any runtime translation request")
    a = ap.parse_args()
    out, rc = [], 0
    for p in a.logs:
        if not os.path.isfile(p):
            print("MISSING %s" % p)
            rc = 2
            continue
        r = census(p)
        out.append(r)
        if r["aot"] is None:
            print("NO DATA %s: no [SHADER_CACHE] loaded line" % p)
            rc = max(rc, 2)
            continue
        policy_ok = not r["drops"] and not r["stops"]
        print("%s\n  AOT %d VS + %d PS | runtime translations %d | waits %d (%.1f ms) | admitted %d | no template %d | "
              "refused/failed %d | dropped %d | release stops %d -> %s" % (
                  p, r["aot"]["vs"], r["aot"]["ps"], len(r["requests"]), len(r["waits"]), r["wait_ms_total"],
                  len(r["admitted"]), len(r["no_template"]), len(r["refused"]), len(r["drops"]), len(r["stops"]),
                  "POLICY HELD" if policy_ok else "POLICY FAILED"))
        for w in r["waits"]:
            print("    wait  %s: %s%s" % (w["shader"], w["outcome"], "" if w["ms"] is None else " (%.1f ms)" % w["ms"]))
        for k in r["requests"]:
            if not any(w["shader"] == k for w in r["waits"]):
                print("    request %s (no wait: SR_SHADER_RUNTIME_XLAT_WAIT=0 or a pre-policy exe)" % k)
        for k in r["drops"]:
            print("    DROP  %s" % k)
        for s in r["stops"]:
            print("    STOP  vs=%s ps=%s reason=%s xlat=%s" % (s["vs"], s["ps"], s["reason"], s["xlat"]))
        if not policy_ok or (a.strict and r["requests"]):
            rc = max(rc, 1)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
