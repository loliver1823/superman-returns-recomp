#!/usr/bin/env python3
"""mem_res_texid.py - grade the texture-identity census per era (mem.resources residual unknown
`textures_from_resolves`; gfx/resource_texid.hpp, SR_MEM_RES_TEXID=1, dev / Oracle build).

    python tools/perf/mem_res_texid.py <run.log> [--label ERA [--entry REGEX|start]] [--json out.json]

Reads the cumulative windows `[MEM_RES_TEXID] <why> binds=.. resolve_binds=.. plain=.. identity=.. arena=..
cpu_snapshot=.. guest_bytes=.. stale=.. unknown=.. | resolve_by_kind tex2d=.. tex2d_clean=.. array=.. | pages=..
gpu_pages=..` (every 5 s at a bind, and why=terminal at the graceful quit's snapshot) and the `SAMPLE` lines, and per
era (tools/perf/mem_res_eras.py: ERAS, straddling windows charged to both eras) reports the resolve binds by class.

Per era:
  NO DATA  no window at or after its start, or counters that go backwards;
  NONE     no bind sampled resolve output in the era (nothing to certify - boot, a movie);
  FAIL     guest_bytes > 0 (a CPU decode of guest RAM over resolve output: "a mirror decoded from guest memory"),
           stale > 0 (an older texture kept over newer resolve output), unknown > 0 (a resolve bind whose content
           origin the census never saw) or cpu_after_gpu > 0 (a bind over a resolve destination the CPU wrote
           after the GPU did - the orchestrator's "no CPU write between" rule, 2026-10-17: by bytes);
  PASS     resolve binds > 0 and none of the above - the orchestrator's criterion (2026-10-17): every resolve-
           destination binding comes from the GPU-produced resolve output (the host target by identity, or the arena
           range the native resolve wrote, with no CPU write between), 0 decoded from guest bytes. Identity binding
           is a later optimization, not a milestone-2 requirement. The split identity / arena / cpu_snapshot is
           printed: the STRICT reading of the ledger text ("bind the host target ... by identity") holds only where identity = resolve
           binds; at 1x without SR_NB_TARGETS_1X the resource arena (the resolve's GPU-written pages, decoded on the
           GPU) is the design and Xenia's own texture-cache behaviour, so the strict reading is reported, not gated.
Overall: FAIL if any era FAILs, NO DATA if any era with windows is NO DATA or no era PASSes, else PASS; `partial`
when the last window is not the terminal one (the quit's snapshot missing). Exit 0 PASS, 1 FAIL, 2 NO DATA.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mem_res_eras as E  # noqa: E402

WINDOW = re.compile(r"\[MEM_RES_TEXID\] (periodic|terminal|\w+) binds=")
SAMPLE = re.compile(r"\[MEM_RES_TEXID\] SAMPLE (\w+) #(\d+) (.*)")
CLASSES = ("identity", "arena", "cpu_snapshot", "guest_bytes", "stale", "unknown", "cpu_after_gpu")
# the by-bytes classes (cpu_after_gpu: the orchestrator's "no CPU write between" rule, 2026-10-17)
BAD = ("guest_bytes", "stale", "unknown", "cpu_after_gpu")


def grade(path, label=None, entry=None):
    samples = []

    def hook(t, line):
        m = SAMPLE.search(line)
        if m:
            samples.append((t, m.group(1), int(m.group(2)), m.group(3).strip()))

    run = E.parse(path, WINDOW, extra={"entry": entry} if entry and entry != "start" else None, line_hook=hook)
    # binds= is consumed by the window regex; re-read it from the line.
    for w in run["windows"]:
        m = re.search(r"\bbinds=(\d+)", w["line"])
        if m:
            w["kv"]["binds"] = int(m.group(1))
    out = {"log": path, "eras": [], "partial": not run["windows"] or run["windows"][-1]["why"] != "terminal",
           "quit_failed": run["quit_failed"], "samples": samples[:64]}
    if not run["windows"]:
        out["verdict"] = "NO DATA"
        out["why"] = "no [MEM_RES_TEXID] window (SR_MEM_RES_TEXID=1 missing, or an exe without the census)"
        return out
    verdicts = []
    for era, t0, t1, problem in E.spans(run, label, entry):
        d, inside, terminal, prob = E.era_delta(run, t0, t1)
        row = {"era": era, "windows_inside": inside, "delta": d}
        if problem or prob:
            row["verdict"] = "NO DATA"
            row["why"] = problem or prob
        else:
            rb = d.get("resolve_binds", 0)
            bad = {k: d.get(k, 0) for k in BAD if d.get(k, 0)}
            if bad:
                row["verdict"] = "FAIL"
                row["why"] = ", ".join("%s=%d" % kv for kv in bad.items())
            elif rb == 0:
                row["verdict"] = "NONE"
            else:
                row["verdict"] = "PASS"
            row["strict_identity"] = bool(rb) and d.get("identity", 0) == rb
        out["eras"].append(row)
        verdicts.append(row["verdict"])
    if "FAIL" in verdicts:
        out["verdict"] = "FAIL"
    elif "NO DATA" in verdicts or "PASS" not in verdicts:
        out["verdict"] = "NO DATA"
    else:
        out["verdict"] = "PASS"
    # the keyed form a ledger json item reads (by_era.<era>.verdict / .resolve_binds / .identity / ...)
    out["by_era"] = {row["era"]: dict({k: row["delta"].get(k, 0) for k in ("resolve_binds",) + CLASSES},
                                      verdict=row["verdict"], strict_identity=row.get("strict_identity"))
                     for row in out["eras"]}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("--label")
    ap.add_argument("--entry")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    r = grade(a.log, a.label, a.entry)
    print("MEM_RES_TEXID %s%s" % (r["verdict"], " (partial: no terminal window)" if r.get("partial") else ""))
    if r.get("why"):
        print("  " + r["why"])
    for row in r["eras"]:
        d = row["delta"]
        print("  %-9s %-7s resolve_binds=%-8s %s | plain=%s%s%s" % (
            row["era"], row["verdict"], d.get("resolve_binds", "-"),
            " ".join("%s=%s" % (k, d.get(k, 0)) for k in CLASSES), d.get("plain", "-"),
            (" | strict identity: %s" % ("yes" if row.get("strict_identity") else "no")) if d.get("resolve_binds") else "",
            (" | " + row["why"]) if row.get("why") else ""))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(r, fh, indent=1, sort_keys=True)
    return {"PASS": 0, "FAIL": 1}.get(r["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
