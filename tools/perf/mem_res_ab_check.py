#!/usr/bin/env python3
"""mem_res_ab_check.py - the witness of the mem.resources frozen-pass A/B (gfx/resource_ab.hpp; ledger mem.resources,
pixels guard frozen-ab). The pixels are graded by the flicker gate's freeze grade (freeze_diff --exact --ab-control);
this grades whether the toggle really alternated the DECIDER under the pairs that grade COMPARED - a no-op toggle passes
any pixel comparison.

    python tools/perf/mem_res_ab_check.py --roll <roll dir> [--freeze-table T] [--json out.json] [--min-pairs N]
    python tools/perf/mem_res_ab_check.py <run.log> --freeze-table T [...]

Reads the roll's game log (<roll>/drive/attempt1/run.log):
  [MEM_RES_AB] dual=1 bit=.. present_mode=1: ...           the toggle latched (SR_MEM_RESOURCES=1 + SR_AB_TOGGLE)
  [MEM_RES_AB] <why> sets native=.. model=.. empty=.. presented native=.. model=.. rt_enter=.. | decisions native=..
      model=.. native_in_model=.. model_in_native=.. unlatched=.. dec_unlatched=.. | uploads native=.. model=.. |
      inv_m forgot=.. adopted=.. | inv_r cross_window=.. cross_clobber=.. cross_unannounced=.. preserve=..
      reestablished=.. | model_readback=.. gpu_note_model_arm=.. | dumped native=.. model=.. alt_pairs=.. alt_break=..
      (periodic every 600 presented sets; why=terminal in the graceful quit's snapshot)
  [MEM_RES_AB] present=<n> arm=N|M                          each dumped present's decider (the render thread)
  [SIM_FREEZE_DUMP] freeze=<f> k=<k> present=<n> ...        each dumped frozen present (kernel/highfps.cpp)
and the pixel grade's own table (<roll>_freeze.txt, freeze_diff.py): `compared | freeze | a | b | static|unsettled|
plain`, every pair it compared (Astra round 2 on native/zpd-bake) - the presents this joins to their arms.

Verdict (exit 0 PASS, 1 FAIL, 2 NO DATA). Astra review1 P2-4: the evidence must be COMPLETE, so
  NO DATA  no dual line, present_mode 0, no terminal totals (the graceful quit's snapshot; the last periodic totals
           are graded only for a FAIL - the counters only grow), a totals line missing a field of the schema, counters
           that go backwards (two processes in one log), a present dumped twice, no freeze table or no compared row in
           it, a compared present with no [SIM_FREEZE_DUMP] or no arm line;
  FAIL     a decision by the wrong arm (native_in_model / model_in_native > 0: a no-op or inverted toggle), a decision
           on an unlatched thread (unlatched / dec_unlatched > 0), an arm that decided < --min-decisions pages, a
           CROSS_CLOBBER (the model overwrote executed GPU data the registry holds: outside what a shared-arena A/B can
           show), the totals' alt_break > 0, a present with two different arm lines, an arm line `?`; and on the joined
           table: a compared pair whose two presents share one arm (the A/B compared a decider with itself), a static
           pair's control (freeze_diff's p-1 -> p+1, p -> p+2 for a hold's first pair) whose two presents do NOT share
           one arm, fewer than --min-pairs static A/B pairs;
  PASS     otherwise. cross_window (a window disagreement: visible per arm), cross_unannounced (the model repairing a
           write no note announced: the shared arena's stated blind spot, the boundaries_complete checker's class),
           preserve, reestablished, empty sets, model_readback and the INV-M counts are reported.
"""
import argparse
import json
import os
import re
import sys

SCHEMA = "mem-res-ab/2"
DUAL = re.compile(r"\[MEM_RES_AB\] dual=1 bit=(\d+) present_mode=(\d)")
TOTALS = re.compile(r"\[MEM_RES_AB\] (\w+) sets (.*)$")
ARM = re.compile(r"\[MEM_RES_AB\] present=(\d+) arm=([NM?])")
DUMP = re.compile(r"\[SIM_FREEZE_DUMP\] freeze=(\d+) k=(\d+) present=(\d+)")
COMPARED = re.compile(r"^compared \| (\d+) \| (\d+) \| (\d+) \| (static|unsettled|plain)\s*$")
AB_PRESENT = re.compile(r"\[AB_PRESENT\]")
TOKEN = re.compile(r"([A-Za-z_]+)=(\d+)|([A-Za-z_]+)")
# Every field gfx/resource_ab.cpp resourceAbReport prints (parse_totals' names).
FIELDS = ("sets.native", "sets.model", "sets.empty", "presented.native", "presented.model", "presented.rt_enter",
          "decisions.native", "decisions.model", "decisions.native_in_model", "decisions.model_in_native",
          "decisions.unlatched", "decisions.dec_unlatched", "uploads.native", "uploads.model", "inv_m.forgot",
          "inv_m.adopted", "inv_r.cross_window", "inv_r.cross_clobber", "inv_r.cross_unannounced", "inv_r.preserve",
          "inv_r.reestablished", "model_readback", "gpu_note_model_arm", "dumped.native", "dumped.model",
          "dumped.alt_pairs", "dumped.alt_break")
MIN_PAIRS = 100  # static A/B pairs (a 40-dump hold gives <= 39): the toggle witnessed over >= 3 holds' compared pairs


def run_log(roll):
    p = os.path.join(roll, "drive", "attempt1", "run.log")
    return p if os.path.exists(p) else None


def parse_totals(why, rest):
    """'native=1 model=2 presented native=3 ... | decisions native=..' -> {'sets.native': 1, 'presented.native': 3,
    'decisions.native': .., 'model_readback': ..}: a bare word names the section of the keys after it (within its
    '|'-separated part); keys before any bare word belong to `sets` (the line's first section)."""
    out = {"why": why}
    first = True
    for part in rest.split("|"):
        prefix = "sets" if first else ""
        first = False
        for m in TOKEN.finditer(part):
            if m.group(3):
                prefix = m.group(3)
                continue
            key = m.group(1)
            out[(prefix + "." + key) if prefix else key] = int(m.group(2))
    return out


def parse(path):
    r = {"dual": None, "totals": [], "arms": {}, "arm_conflicts": [], "dumps": [], "ab_present": 0}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "[MEM_RES_AB]" in line:
                m = DUAL.search(line)
                if m:
                    r["dual"] = {"bit": int(m.group(1)), "present_mode": int(m.group(2))}
                    continue
                m = ARM.search(line)
                if m:
                    p, a = int(m.group(1)), m.group(2)
                    if p in r["arms"] and r["arms"][p] != a:
                        r["arm_conflicts"].append((p, r["arms"][p], a))
                    r["arms"][p] = a
                    continue
                m = TOTALS.search(line)
                if m:
                    r["totals"].append(parse_totals(m.group(1), m.group(2)))
                    continue
            elif "[SIM_FREEZE_DUMP]" in line:
                m = DUMP.search(line)
                if m:
                    r["dumps"].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
            elif AB_PRESENT.search(line):
                r["ab_present"] += 1
    return r


def parse_table(text):
    """{freeze: [(a, b, kind), ...]} in the table's order (each freeze's pairs in present order)."""
    out = {}
    for ln in text.splitlines():
        m = COMPARED.match(ln.strip())
        if m:
            out.setdefault(int(m.group(1)), []).append((int(m.group(2)), int(m.group(3)), m.group(4)))
    return out


def counter_fails(t, min_decisions):
    g = lambda k: int(t.get(k, 0))
    fails = []
    if g("decisions.native_in_model") or g("decisions.model_in_native"):
        fails.append("wrong-arm decisions: native_in_model=%d model_in_native=%d (a no-op or inverted toggle)" % (
            g("decisions.native_in_model"), g("decisions.model_in_native")))
    if g("decisions.unlatched") or g("decisions.dec_unlatched"):
        fails.append("decisions on unlatched threads: calls=%d pages=%d" % (g("decisions.unlatched"),
                                                                           g("decisions.dec_unlatched")))
    if g("inv_r.cross_clobber"):
        fails.append("inv_r cross_clobber=%d: the model overwrote executed GPU data the registry holds - outside what a "
                     "shared-arena A/B can show" % g("inv_r.cross_clobber"))
    if g("dumped.alt_break"):
        fails.append("the render thread saw %d consecutive dumped presents with one arm (alt_break)" % g("dumped.alt_break"))
    return fails


def grade(r, table=None, min_decisions=1, min_pairs=MIN_PAIRS):
    res = {"schema": SCHEMA, "verdict": "NO DATA", "problems": [], "partial": False, "checks": {}}
    probs, checks = res["problems"], res["checks"]

    def nodata(why):
        probs.append(why)
        return res

    if not r["dual"]:
        return nodata("no '[MEM_RES_AB] dual=1' line: the toggle never latched (SR_MEM_RESOURCES=1 + "
                      "SR_AB_TOGGLE=SR_MEM_RESOURCES)")
    checks["dual"] = True
    if not r["dual"]["present_mode"]:
        return nodata("present_mode=0: the arm follows pace windows, not presents (SR_AB_PRESENT=1 missing)")
    if not r["totals"]:
        return nodata("no [MEM_RES_AB] totals line")
    for a, b in zip(r["totals"], r["totals"][1:]):
        for k, v in a.items():
            if k != "why" and isinstance(v, int) and b.get(k, v) < v:
                return nodata("counter %s went backwards (%d -> %d): two processes in one log?" % (k, v, b.get(k)))
    for t in r["totals"]:
        missing = [f for f in FIELDS if f not in t]
        if missing:
            return nodata("a %s totals line lacks %s (an exe before the schema, or a cut line)" % (
                t["why"], ", ".join(missing)))
    checks["schema_complete"] = True
    term = [t for t in r["totals"] if t["why"] == "terminal"]
    t = term[-1] if term else r["totals"][-1]
    res["totals"] = t
    checks["terminal"] = bool(term)
    if not term:
        res["partial"] = True
        fails = counter_fails(t, min_decisions)
        if fails:  # a FAIL is final whatever came after: the counters only grow
            probs.extend(fails)
            res["verdict"] = "FAIL"
            return res
        return nodata("no terminal [MEM_RES_AB] totals (the graceful quit's snapshot): the run's end is unmeasured")
    fails = counter_fails(t, min_decisions)
    g = lambda k: int(t.get(k, 0))
    if g("decisions.native") < min_decisions or g("decisions.model") < min_decisions:
        fails.append("an arm decided too little: native=%d model=%d pages (min %d)" % (
            g("decisions.native"), g("decisions.model"), min_decisions))
    for p, a0, a1 in r["arm_conflicts"][:1]:
        fails.append("present %d has two arm lines (%s, %s): %d conflict(s)" % (p, a0, a1, len(r["arm_conflicts"])))
    if any(a == "?" for a in r["arms"].values()):
        fails.append("a dumped present on a thread with no latched arm (arm=?)")
    checks["counters"] = not fails
    res["ab_present_lines"] = r["ab_present"]
    res["reported"] = {k: g(k) for k in ("inv_r.cross_window", "inv_r.cross_unannounced", "inv_r.preserve",
                                          "inv_r.reestablished", "model_readback", "inv_m.forgot", "inv_m.adopted",
                                          "uploads.native", "uploads.model", "gpu_note_model_arm", "sets.native",
                                          "sets.model", "sets.empty", "dumped.alt_pairs")}
    # The dumped presents: unique identities.
    seen, dup = set(), []
    for f, k, p in r["dumps"]:
        if p in seen:
            dup.append(p)
        seen.add(p)
    res["dumped"] = len(r["dumps"])
    if not r["dumps"]:
        probs.extend(fails)
        return nodata("no [SIM_FREEZE_DUMP] present: not a frozen roll")
    if dup:
        probs.extend(fails)
        return nodata("%d present(s) dumped twice (first %d): two processes or a replayed log" % (len(dup), dup[0]))
    checks["dumps_unique"] = True
    # The join: every compared pair of the pixel grade's table, each present by its own arm line.
    if table is None:
        probs.extend(fails)
        return nodata("no freeze table (the pixel grade's freeze_diff output, <roll>_freeze.txt)")
    rows = parse_table(table)
    pairs = [(f, a, b, kind) for f, lst in sorted(rows.items()) for a, b, kind in lst]
    res["compared_pairs"] = len(pairs)
    if not pairs:
        probs.extend(fails)
        return nodata("the freeze table lists no compared pair (freeze_diff.py before the compared rows, or no hold)")
    dumped = {p for _f, _k, p in r["dumps"]}
    unjoined = sorted({p for _f, a, b, _k in pairs for p in (a, b) if p not in dumped or p not in r["arms"]})
    if unjoined:
        probs.extend(fails)
        return nodata("%d compared present(s) without a [SIM_FREEZE_DUMP] or an arm line (first %d)" % (
            len(unjoined), unjoined[0]))
    checks["compared_joined"] = True
    arm = r["arms"]
    same = [(f, a, b) for f, a, b, _k in pairs if arm[a] == arm[b]]
    static = [(f, a, b) for f, a, b, kind in pairs if kind == "static"]
    # freeze_diff's control of the pair (a, b): the dumped present before a -> b; a hold's first pair: a -> the next
    ctl_bad = []
    for f, lst in rows.items():
        order = sorted({p for a, b, _k in lst for p in (a, b)})
        idx = {p: i for i, p in enumerate(order)}
        for a, b, kind in lst:
            if kind != "static":
                continue
            i = idx[a]
            c0, c1 = (order[i - 1], b) if i > 0 else (a, order[idx[b] + 1] if idx[b] + 1 < len(order) else None)
            if c1 is None or c0 not in arm or c1 not in arm:
                continue
            if arm[c0] != arm[c1]:
                ctl_bad.append((f, c0, c1))
    res["static_pairs"] = len(static)
    res["same_arm_pairs"] = len(same)
    res["control_cross_arm"] = len(ctl_bad)
    res["arms_in_static"] = {"N": sum(1 for _f, a, b in static for p in (a, b) if arm[p] == "N"),
                             "M": sum(1 for _f, a, b in static for p in (a, b) if arm[p] == "M")}
    checks["compared_alternate"] = not same
    checks["controls_same_arm"] = not ctl_bad
    checks["pairs_min"] = len(static) >= min_pairs
    if same:
        f, a, b = same[0]
        fails.append("%d compared pair(s) from ONE decider (first: freeze %d presents %d->%d arm %s): the A/B compared "
                     "an arm with itself" % (len(same), f, a, b, arm[a]))
    if ctl_bad:
        f, a, b = ctl_bad[0]
        fails.append("%d static pair control(s) from two deciders (first: freeze %d presents %d->%d): the same-arm "
                     "control is no control" % (len(ctl_bad), f, a, b))
    if len(static) < min_pairs:
        fails.append("only %d static A/B pairs joined to their arms (min %d)" % (len(static), min_pairs))
    probs.extend(fails)
    res["verdict"] = "FAIL" if fails else "PASS"
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", nargs="?", help="the game log (default: <roll>/drive/attempt1/run.log)")
    ap.add_argument("--roll", default="", help="the roll's folder (the log and <roll>_freeze.txt)")
    ap.add_argument("--freeze-table", default="", help="the pixel grade's table (default <roll>_freeze.txt)")
    ap.add_argument("--json")
    ap.add_argument("--min-decisions", type=int, default=1)
    ap.add_argument("--min-pairs", type=int, default=MIN_PAIRS)
    a = ap.parse_args(argv)
    log = a.log or (run_log(a.roll) if a.roll else None)
    tpath = a.freeze_table or ((a.roll.rstrip("/\\") + "_freeze.txt") if a.roll else "")
    if not log or not os.path.exists(log):
        res = {"schema": SCHEMA, "verdict": "NO DATA", "problems": ["no run.log (%s)" % (log or a.roll)], "checks": {}}
    else:
        table = None
        if tpath and os.path.exists(tpath):
            with open(tpath, encoding="utf-8", errors="replace") as fh:
                table = fh.read()
        res = grade(parse(log), table, a.min_decisions, a.min_pairs)
    res["log"], res["freeze_table"] = log, tpath or None
    print("MEM_RES_AB %s%s" % (res["verdict"], " (partial: no terminal totals)" if res.get("partial") else ""))
    for p in res["problems"]:
        print("  - " + p)
    if "totals" in res:
        t = res["totals"]
        print("  decisions native=%s model=%s native_in_model=%s model_in_native=%s | compared pairs %s (static %s, "
              "same-arm %s, cross-arm controls %s) | reported %s" % (
                  t.get("decisions.native"), t.get("decisions.model"), t.get("decisions.native_in_model"),
                  t.get("decisions.model_in_native"), res.get("compared_pairs"), res.get("static_pairs"),
                  res.get("same_arm_pairs"), res.get("control_cross_arm"), json.dumps(res.get("reported", {}))))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1, sort_keys=True)
    return {"PASS": 0, "FAIL": 1}.get(res["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
