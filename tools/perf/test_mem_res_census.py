#!/usr/bin/env python3
"""test_mem_res_census.py - tools/perf/mem_res_census.py (mem.resources step 2a's per-era grader) on synthetic logs,
and the grader against the census lines the C++ unit prints.

    python tools/perf/test_mem_res_census.py [--difftest-out FILE]

Cases: a clean run passes every era; a miss fails its era only, attributed; no census is INCOMPLETE; a log that ends
without the terminal window has no exit evidence (NO DATA); --label grades the eras before its entry under their own
names and needs entry (and, with --complete, completion) evidence; movie-only misses are KNOWN; a counter going
backwards is NO DATA. Astra's three counterexamples (archive/memres/astra_review_2a.txt) - a trailing MISSED sample, a
census line without `missed`, an era with zero decisions - each PASSed the first grader and must not now (T10-T12);
inconsistent counters and missing sections are NO DATA (T13); coverage gaps are GAP (T14); clobbers and window
disagreements FAIL while races pass (T15); each checker feed must have verified its keeps, and a feed with no keeps
needs no checks (T16); boot ends at the first present (T17); partial evidence (T18-T20); memres2's sums and samples
(T21, T22); memres3's page_only is reported, never gated, and its sums hold (T23); memres5's fallback share, at memres6's
5% bound (T24); memres6's soundness witness gates, its computed bounds and census_vsfb are consistent (T25); every
required key is printed by the unit (T8).
--difftest-out: the output of `difftest.exe --mem-resources-test` (C12 prints the census lines after a known sequence
and MEMRES_EXPECT with the unit's own totals) graded with --label --entry start: the grader must read exactly what the
unit counted (T9). Exit code = failed checks.
"""
import argparse
import io
import json
import os
import re
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import mem_res_census as M  # noqa: E402

FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


def stamp(s):
    return "[2026-10-15 20:%02d:%06.3f] [INFO ] [BOOT] " % (s // 60, s % 60)


W = len(M.SECTIONS) + 1  # the lines of one census window (memres6: + census_vsfb_top, which the grader reads past)


def census(s, why, swaps, decisions=100, missed=0, movie=0, clobber=0, alias=0, race=0, unseen=0, unacc=0,
           wt_keep=None, wt_unchecked=0, rc_keep=None, rc_unchecked=0, rc_mismatch=0, stale=0, drop=None,
           drop_section=None, skew=0, attr_skew=0, grant_skew=0, coher_skew=0, po=0, mstale=0, rng_skew=0, fbs=0,
           fbd=0, fb_skew=0, comp=0, bsamp=0, bviol=0, vsfb_skew=0, vsfb_drop=None):
    """A census window (the twelve lines the unit prints) at second s with cumulative, consistent counts (`skew` breaks
    decisions = the outcomes' sum, `attr_skew` / `grant_skew` / `coher_skew` memres2's sums, `rng_skew` moves page-only
    decisions from rng.same to rng.unk; `po` = memres3's page_only, `mstale` = how many of the misses are why stale;
    `fbs` / `fbd` = memres5's fetch fallbacks (shader / draw), `fb_skew` breaks their sum; memres6's `comp` = the
    computed-bound requests (of fetch.bounded), `bsamp` / `bviol` the witness's sampled draws and violations, census_vsfb's
    reasons sum to fbs (`vsfb_skew` breaks it, `vsfb_drop` removes a reason); `drop` removes a key, `drop_section` a
    section line)."""
    agree = decisions - missed - clobber - race - po
    wt_keep = decisions // 4 if wt_keep is None else wt_keep
    rc_keep = decisions // 8 if rc_keep is None else rc_keep
    c = [("swaps", swaps), ("decisions", decisions + skew), ("agree_up", agree // 2), ("agree_keep", agree - agree // 2),
         ("over", 0), ("missed", missed), ("clobber", clobber), ("race.note", race), ("race.lock", 0),
         ("page_only", po), ("missed.content", missed - mstale), ("missed.window", 0), ("missed.gpu", 0),
         ("missed.stale", mstale), ("missed.movie", movie), ("first", 3),
         ("win.heur", 1), ("win.alias_same", 0), ("win.alias_diff", alias), ("win.unmapped", 0), ("clobber.both", 0),
         ("reuse.noted", 0), ("cov.req", decisions * 2), ("cov.val_cands", decisions), ("cov.decided", decisions // 2),
         ("cov.cand", decisions), ("cov.val", decisions), ("cov.reuse", decisions), ("cov.reuse_failed", 0),
         ("cov.unseen", unseen), ("cov.deferred", 0), ("cov.failed", 0), ("cov.rebase", 0), ("cov.unaccounted", unacc),
         ("keep.wt.checked", wt_keep - wt_unchecked), ("keep.wt.stale", 0), ("keep.wt.mismatch", 0),
         ("keep.wt.unchecked", wt_unchecked), ("keep.rc.checked", rc_keep - rc_unchecked - rc_mismatch),
         ("keep.rc.stale", 0), ("keep.rc.mismatch", rc_mismatch), ("keep.rc.unchecked", rc_unchecked),
         ("note.inval", 5), ("note.inval_pages", 9), ("note.whole", 2), ("note.refused", 0), ("note.lock", 0),
         ("note.shsync", 1), ("lock.open", 0), ("note.resolve", 3), ("note.resolve_pages", 6),
         ("coher.requests", 11 + decisions // 100),
         ("up.allocs", 4), ("up.zpass", 1), ("cb.lo", 0x1E820000), ("cb.hi", 0x1EF20000), ("trap.armed", 0),
         ("trap.fired", 0), ("trap.host", 0), ("trap.arm_fail", 0), ("trap.declined", 0),
         ("fetch.bounded", decisions), ("fetch.fallback", fbs + fbd + fb_skew), ("fetch.fallback.shader", fbs),
         ("fetch.fallback.draw", fbd), ("win.live", 5), ("win.grant", 1), ("win.default", 0),
         ("fetch.bounded.computed", comp), ("bound.check.sampled", bsamp), ("bound.check.violations", bviol),
         ("clock", 40), ("wt.checks", wt_keep - wt_unchecked + rc_keep), ("wt.stale", stale)]
    reasons = [(r, (fbs if r == "op" else 0) + (vsfb_skew if r == "lit" else 0)) for r in M.FB_REASONS]
    vsfb = " ".join("r.%s=%d" % kv for kv in reasons if kv[0] != vsfb_drop)
    body = " ".join("%s=%d" % kv for kv in c if kv[0] != drop)
    lines = [
        ("census", stamp(s) + "[MEM_RES] census %s %s" % (why, body)),
        ("census_missed", stamp(s) + "[MEM_RES] census_missed %s kind.other=0 kind.vb=%d kind.tex=%d prov.8=%d "
                                     "src.none=%d site.drain=%d" % (why, missed - movie, movie, missed, missed, missed)),
        ("census_over", stamp(s) + "[MEM_RES] census_over %s kind.vb=0 src.inval=0" % why),
        ("census_first", stamp(s) + "[MEM_RES] census_first %s kind.vb=3 prov.8=3" % why),
        ("census_missed_cells", stamp(s) + "[MEM_RES] census_missed_cells %s vb/8/none/drain=%d cells=1" % (why, missed)),
        ("census_hazard", stamp(s) + "[MEM_RES] census_hazard %s clobber.kind.vb=%d clobber.src.inval=%d "
                                     "clobber.site.drain=%d race.note.kind.vb=%d race.note.site.drain=%d "
                                     "race.lock.kind.vb=0 race.lock.site.drain=0" % (why, clobber, clobber, clobber,
                                                                                     race, race)),
        ("census_ib", stamp(s) + "[MEM_RES] census_ib %s reads=10 agree_up=5 agree_keep=4 missed=1 over=0 "
                                 "missed.prov.8=1 missed.src.none=1" % why),
        ("census_coher", stamp(s) + "[MEM_RES] census_coher %s requests=%d overflow=0 calls.03000000=%d pages.03000000=9 "
                                    "calls.01000000=2 pages.01000000=4 calls.03000100=3 pages.03000100=6 "
                                    "calls.00010000=1 pages.00010000=0" % (
                                        why, 11 + decisions // 100, 5 + decisions // 100 + coher_skew)),
        ("census_missed_attr", stamp(s) + "[MEM_RES] census_missed_attr %s cb.unk=0 cb.in=%d cb.out=%d seg.none=%d "
                                          "seg.up=%d seg.zp=0 rng.unk=%d rng.same=%d rng.moved=0 "
                                          "vb/cmdbuf/in/up/unk=%d vb/none/out/none/unk=%d vb/none/out/none/same=%d "
                                          "cells=3" % (
                                              why, missed - movie + attr_skew, movie + po, movie + po, missed - movie,
                                              missed + rng_skew, po - rng_skew, missed - movie, movie, po)),
        ("census_missed_grant", stamp(s) + "[MEM_RES] census_missed_grant %s none=%d grants=1 overflow=0 "
                                           "g.AAE30000+DFF0000@826FAFB8=%d" % (why, movie + po + grant_skew,
                                                                               missed - movie)),
        ("census_vsfb", stamp(s) + "[MEM_RES] census_vsfb %s %s vs=%d" % (why, vsfb, 1 if fbs else 0)),
        ("census_vsfb_top", stamp(s) + "[MEM_RES] census_vsfb_top %s top=%s" % (
            why, "31168A8B9ADBEEFA:%d:op" % fbs if fbs else "-")),
    ]
    return [ln for sec, ln in lines if sec != drop_section]


def run(lines, *args, files=None):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "run.log")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        for name, text in (files or {}).items():  # beside the log (quit.json)
            with open(os.path.join(d, name), "w", encoding="utf-8-sig") as fh:
                fh.write(text)
        buf = io.StringIO()
        jp = os.path.join(d, "out.json")
        with redirect_stdout(buf):
            rc = M.main([p, "--json", jp] + list(args))
        with open(jp, encoding="utf-8") as fh:
            js = json.load(fh)
    return rc, buf.getvalue(), js


def eras_of(js):
    return {e["era"]: e for e in js["logs"][0]["eras"]}


TITLE = "[APT_REQ] #3 path='screens/bootflow/ScrPressStart'"
HUD = "[APT_REQ] #9 path='screens/hud/main_hud'"


def base_log(miss=0, movie=0, last_why="exit", **kw):
    """boot (to the first present at 2 s), intro, title (20 s), gameplay (40 s) - a window right after each marker."""
    L = [stamp(0) + "[BOOT] start", stamp(0.5) + "[MEM_RES] gate SR_MEM_RESOURCES=census: census - shadow"]
    L += census(2, "census", 1, 50)           # the first present: closes boot
    L += census(10, "census", 120, 400)       # intro
    L += [stamp(20) + TITLE]
    L += census(21, "census", 150, 450)       # just after the title marker
    L += census(30, "census", 240, 900)       # title
    L += [stamp(40) + HUD]
    L += census(41, "census", 300, 1000)      # just after the HUD
    L += census(50, "census", 360, 2000, miss, movie, **kw)   # gameplay
    L += census(60, last_why, 480, 2500, miss, movie, **kw)
    return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--difftest-out")
    a = ap.parse_args()

    print("-- T1 a clean run: every era PASS")
    rc, out, js = run(base_log())
    e = eras_of(js)
    check(rc == 0, "T1 exit 0")
    check(set(e) == {"boot", "intro", "title", "gameplay"}, "T1 eras boot / intro / title / gameplay: %s" % sorted(e))
    check(all(x["verdict"] == "PASS" for x in e.values()), "T1 every era PASS %s" % {k: v["verdict"] for k, v in e.items()})
    check(e["boot"]["totals"]["decisions"] == 50 and e["intro"]["totals"]["decisions"] == 400,
          "T1 boot = the process start to the first present (50); intro = to the first window after the title (450-50)")
    check(e["title"]["totals"]["decisions"] == 600 and e["gameplay"]["totals"]["decisions"] == 1600,
          "T1 a window straddling a boundary is charged to both eras (title 1000-400, gameplay 2500-900)")
    check(e["intro"]["design_era"] == "movies" and e["title"]["design_era"] == "menus", "T1 the design's era names")
    check(js["verdict"] == "PASS" and set(js["by_era"]) == {"boot", "movies", "menus", "gameplay"} and
          all(v["verdict"] == "PASS" for v in js["by_era"].values()) and js["by_era"]["gameplay"]["decisions"] == 1600,
          "T1 the keyed JSON the ledger reads: by_era.<design era>.verdict / .decisions")

    print("-- T2 a miss in gameplay fails gameplay only, attributed")
    rc, out, js = run(base_log(miss=3))
    e = eras_of(js)
    check(rc == 1, "T2 exit 1")
    check(e["gameplay"]["verdict"] == "FAIL" and e["title"]["verdict"] == "PASS" and e["boot"]["verdict"] == "PASS",
          "T2 gameplay FAIL, the others PASS")
    check(e["gameplay"]["missed_by"].get("kind.vb") == 3 and e["gameplay"]["missed_cells"].get("vb/8/none/drain") == 3,
          "T2 the attribution (kind.vb 3, the cell vb/8/none/drain 3)")
    check("missed: content=3" in out and "verdict: FAIL" in out, "T2 printed")
    check(js["verdict"] == "FAIL" and js["by_era"]["gameplay"]["verdict"] == "FAIL" and
          js["by_era"]["gameplay"]["missed"] == 3 and js["by_era"]["menus"]["verdict"] == "PASS",
          "T2 by_era.gameplay FAIL (missed 3), by_era.menus PASS")

    print("-- T3 no census: INCOMPLETE, never a pass")
    rc, out, js = run([stamp(0) + "[BOOT] start", stamp(5) + HUD])
    check(rc == 2 and "NO CENSUS" in out and js["logs"][0]["verdict"] == "NO CENSUS" and js["verdict"] == "INCOMPLETE",
          "T3 exit 2, NO CENSUS")

    print("-- T4 the log ends without the terminal census window: the last era has no exit evidence")
    rc, out, js = run(base_log(last_why="census"))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "NO DATA" and rc == 2 and
          any("terminal" in w for w in e["gameplay"]["why"]), "T4 gameplay NO DATA (no terminal window), exit 2")
    check(e["title"]["verdict"] == "PASS", "T4 the eras that ended are still graded")

    print("-- T5 --label: the eras before its entry keep their names; entry and completion evidence")
    rc, out, js = run(base_log(), "--label", "ending")
    e = eras_of(js)
    check(set(e) == {"boot", "intro", "title", "ending"} and e["ending"]["verdict"] == "PASS" and
          e["ending"]["totals"]["decisions"] == 1600,
          "T5 the label era runs from the HUD (its entry) to the end; boot / intro / title graded as themselves")
    check(rc == 0 and js["by_era"]["ending"]["verdict"] == "PASS" and "boot" in js["by_era"], "T5 exit 0, by_era.ending PASS")
    rc, out, js = run(base_log(), "--label", "ending", "--entry", "start")
    e = eras_of(js)
    check(list(e) == ["ending"] and e["ending"]["totals"]["decisions"] == 2500, "T5 --entry start: the whole log")
    rc, out, js = run([ln for ln in base_log() if HUD not in ln], "--label", "ending")
    e = eras_of(js)
    check(e["ending"]["verdict"] == "NO DATA" and rc == 2, "T5 no HUD marker: the label era has no entry evidence")
    rc, out, js = run(base_log(), "--label", "ending", "--complete", r"\[MOVIE\] ending done")
    check(eras_of(js)["ending"]["verdict"] == "NO DATA" and js["by_era"]["ending"]["completion"] is False and rc == 2,
          "T5 --complete without its line inside the era: NO DATA (the roll did not reach the era's end)")
    L = base_log()
    L.insert(-W, stamp(55) + "[MOVIE] ending done")
    rc, out, js = run(L, "--label", "ending", "--complete", r"\[MOVIE\] ending done")
    check(eras_of(js)["ending"]["verdict"] == "PASS" and js["by_era"]["ending"]["completion"] is True and rc == 0,
          "T5 ... with it: PASS, completion true")

    print("-- T6 misses all in movie planes: KNOWN, not a pass")
    rc, out, js = run(base_log(miss=2, movie=2))
    check(eras_of(js)["gameplay"]["verdict"] == "KNOWN" and rc == 1, "T6 KNOWN, exit 1")
    rc, out, js = run(base_log(miss=3, movie=2))
    check(eras_of(js)["gameplay"]["verdict"] == "FAIL", "T6 one miss outside the planes: FAIL")

    print("-- T6b mem-native: misses all prior (census_patch missed_prior): KNOWN; one not prior: FAIL; vtx_stale: GAP")

    def with_patch(L, prior, stale=0):
        # census_patch beside each window (cumulative): the gameplay windows (50 s, 60 s) carry `prior` / `stale`
        out = []
        for ln in L:
            out.append(ln)
            if "[MEM_RES] census " in ln:
                late = " swaps=360 " in ln or " swaps=480 " in ln
                why = ln.split("[MEM_RES] census ")[1].split(" ")[0]
                out.append(ln.split("[MEM_RES]")[0] + "[MEM_RES] census_patch %s notes=1 missed_prior=%d vtx_stale=%d" % (
                    why, prior if late else 0, stale if late else 0))
        return out
    rc, out, js = run(with_patch(base_log(miss=2), 2))
    check(eras_of(js)["gameplay"]["verdict"] == "KNOWN" and rc == 1,
          "T6b every miss prior: KNOWN, exit 1 (%s)" % eras_of(js)["gameplay"]["verdict"])
    rc, out, js = run(with_patch(base_log(miss=3), 2))
    check(eras_of(js)["gameplay"]["verdict"] == "FAIL", "T6b one miss not prior: FAIL")
    rc, out, js = run(with_patch(base_log(), 0, 1))
    check(eras_of(js)["gameplay"]["verdict"] == "GAP" and rc == 2, "T6b vtx_stale > 0: GAP")

    print("-- T6c mem-native: misses all cpu_only (the orchestrator's decision): PASS with the class reported")

    def with_cpu_only(L, n):
        out = []
        for ln in L:
            out.append(ln)
            if "[MEM_RES] census " in ln:
                late = " swaps=360 " in ln or " swaps=480 " in ln
                why = ln.split("[MEM_RES] census ")[1].split(" ")[0]
                out.append(ln.split("[MEM_RES]")[0] + "[MEM_RES] census_patch %s notes=1 missed_cpu_only=%d" % (
                    why, n if late else 0))
        return out
    rc, out, js = run(with_cpu_only(base_log(miss=2), 2))
    gp = eras_of(js)["gameplay"]
    check(gp["verdict"] == "PASS" and gp.get("classified") == {"cpu_only": 2} and rc == 0 and "classified: cpu_only=2" in out,
          "T6c every miss cpu_only: PASS, classified.cpu_only=2 reported (%s %s)" % (gp["verdict"], gp.get("classified")))
    rc, out, js = run(with_cpu_only(base_log(miss=3), 2))
    check(eras_of(js)["gameplay"]["verdict"] == "FAIL", "T6c one miss not cpu_only: FAIL")

    print("-- T7 a counter going backwards (two processes in one log): NO DATA")
    L = base_log()[:-W] + census(55, "census", 400, 10) + census(60, "exit", 480, 2500)
    rc, out, js = run(L)
    check(eras_of(js)["gameplay"]["verdict"] == "NO DATA" and rc == 2, "T7 gameplay NO DATA, exit 2")

    print("-- T10 (Astra counterexample 1) a MISSED sample after the last window, the counters all 0: not a PASS")
    L = base_log() + [stamp(61) + "[MEM_RES] MISSED #1 page=00500000 kind=vb prov=8 src=none site=drain why=content "
                                  "reg=keep cpu=0 gpu=0 up=1 clock=9 movie=0"]
    rc, out, js = run(L)
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and rc == 1 and
          any("do not carry" in w for w in e["gameplay"]["why"]), "T10 the trailing sample FAILs its era")
    L = base_log() + [stamp(61) + "[MEM_RES] CLOBBER #1 page=00B00000 kind=vb prov=8 src=inval site=drain cpu=5 gpu=4 "
                                  "up=1 clock=9"]
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "FAIL", "T10 a trailing CLOBBER sample likewise")

    print("-- T11 (Astra counterexample 2) a census line without `missed`: NO DATA, never a zero")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, drop="missed")
    rc, out, js = run(L)
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "NO DATA" and rc == 2 and
          any("missing field missed" in w for w in e["gameplay"]["why"]), "T11 gameplay NO DATA (missing field missed)")

    print("-- T12 (Astra counterexample 3) an era with zero decisions: NO DATA")
    L = [stamp(0) + "[BOOT] start"] + census(2, "census", 1, 50) + census(10, "census", 120, 50) + \
        [stamp(20) + TITLE] + census(21, "census", 150, 50) + census(30, "exit", 240, 50)
    rc, out, js = run(L)
    e = eras_of(js)
    check(e["title"]["verdict"] == "NO DATA" and any("zero decisions" in w for w in e["title"]["why"]) and rc == 2,
          "T12 the title era decided nothing: NO DATA")

    print("-- T13 inconsistent counters / a missing section: NO DATA")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, skew=1)
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and any("outcomes' sum" in w for w in e["gameplay"]["why"]),
          "T13 decisions != the outcomes' sum")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, drop_section="census_hazard")
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and any("census_hazard" in w for w in e["gameplay"]["why"]),
          "T13 a window without its census_hazard line")
    L = base_log()
    L[-W + 1] = L[-W + 1].replace("kind.vb=0 kind.tex=0", "kind.vb=1 kind.tex=0")  # a marginal that disagrees with missed
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA", "T13 a marginal that does not sum to its total")

    print("-- T14 coverage gaps: GAP")
    for kw, what in (({"unseen": 2}, "cov.unseen"), ({"unacc": 1}, "cov.unaccounted")):
        e = eras_of(run(base_log(**kw))[2])
        check(e["gameplay"]["verdict"] == "GAP" and any(what in w for w in e["gameplay"]["why"]), "T14 %s > 0: GAP" % what)
    L = base_log() + [stamp(61) + "[MEM_RES] UNSEEN #1 page=00E00000 stamp=8 seen=7/0 mode=drain"]
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "GAP", "T14 a trailing UNSEEN sample: GAP")

    print("-- T15 clobbers and window disagreements FAIL; races are reported, not gated")
    e = eras_of(run(base_log(clobber=1))[2])
    check(e["gameplay"]["verdict"] == "FAIL" and any("clobber" in w for w in e["gameplay"]["why"]), "T15 clobber > 0: FAIL")
    e = eras_of(run(base_log(alias=1))[2])
    check(e["gameplay"]["verdict"] == "FAIL" and any("alias_diff" in w for w in e["gameplay"]["why"]),
          "T15 win.alias_diff > 0: FAIL")
    rc, out, js = run(base_log(race=4))
    check(eras_of(js)["gameplay"]["verdict"] == "PASS" and "race.note=4" in out, "T15 race.note > 0: PASS, printed")

    print("-- T16 the checker feeds")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, wt_unchecked=3)
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "GAP" and any("keep.wt.unchecked" in w for w in e["gameplay"]["why"]),
          "T16 write-watch keeps not all re-verified: GAP")
    check(eras_of(run(L, "--no-require-checker")[2])["gameplay"]["verdict"] == "PASS", "T16 --no-require-checker: PASS")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, rc_unchecked=2)
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "GAP", "T16 range-cache keeps not all re-verified: GAP")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, rc_mismatch=1)
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "GAP", "T16 a range-cache window mismatch: GAP")
    L = base_log()[:-W] + census(60, "exit", 480, 2500, stale=1)
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "GAP", "T16 the model's checker found a stale keep: GAP")
    L = [stamp(0) + "[BOOT] start"] + census(2, "census", 1, 50, wt_keep=0, rc_keep=0) + \
        census(10, "exit", 120, 400, wt_keep=0, rc_keep=0)
    e = eras_of(run(L)[2])
    check(e["intro"]["verdict"] == "PASS" and e["intro"]["totals"]["wt.checks"] == 0,
          "T16 no keeps and no checks: PASS (zero checks with no eligible keeps is not a failure)")

    print("-- T17 boot ends at the first present (swaps >= 1), not at the first window")
    L = [stamp(0) + "[BOOT] start"] + census(1, "census", 0, 20) + census(3, "census", 1, 60) + \
        census(10, "exit", 120, 400)
    e = eras_of(run(L)[2])
    check(e["boot"]["totals"]["decisions"] == 60 and e["intro"]["totals"]["decisions"] == 340,
          "T17 boot = 60 (to the swap-1 window), intro = 340")

    print("-- T18 (memres1's mrc2, the counterexample) partial evidence that already fails: FAIL, final")
    quit_json = json.dumps({"schema": "sr-quit/1", "result": "failed", "graceful": False, "exitCode": 81,
                            "detail": "the game could not quiesce (exit 81): see its [QUIT] terminal snapshot FAILED line"})
    qline = stamp(61) + "[QUIT] terminal snapshot FAILED stage=settle reason=threads-not-quiescent frames=14412"
    rc, out, js = run(base_log(last_why="census", clobber=5) + [qline], files={"quit.json": quit_json})
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and rc == 1 and e["gameplay"]["partial"],
          "T18 the log ends without the terminal window after failing windows: FAIL (not NO DATA), exit 1")
    check(any("terminal" in w and "quit.json: result=failed" in w and "exitCode=81" in w and
              "threads-not-quiescent" in w for w in e["gameplay"]["why"]),
          "T18 ... the missing exit evidence cites quit.json and the game's [QUIT] FAILED line")
    check(any("partial evidence, final" in w for w in e["gameplay"]["why"]) and "quit: quit.json" in out,
          "T18 ... and says why the partial evidence is final")
    rc, out, js = run(base_log(last_why="census", miss=3), files={"quit.json": quit_json})
    check(eras_of(js)["gameplay"]["verdict"] == "FAIL" and rc == 1, "T18 a partial era with a missed boundary: FAIL")

    print("-- T19 partial evidence that would pass (or be KNOWN): NO DATA - the missing tail could hold a miss")
    rc, out, js = run(base_log(last_why="census"), files={"quit.json": quit_json})
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "NO DATA" and rc == 2 and e["gameplay"]["partial"], "T19 would pass: NO DATA")
    rc, out, js = run(base_log(last_why="census", miss=2, movie=2))
    check(eras_of(js)["gameplay"]["verdict"] == "NO DATA" and rc == 2, "T19 would be KNOWN: NO DATA")
    L = base_log()
    rc, out, js = run(L, "--label", "ending", "--complete", r"\[MOVIE\] ending done")
    check(eras_of(js)["ending"]["verdict"] == "NO DATA", "T19 no completion line, would pass: NO DATA")
    rc, out, js = run(base_log(clobber=2), "--label", "ending", "--complete", r"\[MOVIE\] ending done")
    check(eras_of(js)["ending"]["verdict"] == "FAIL", "T19 no completion line, already failing: FAIL")

    print("-- T20 partial evidence with a coverage gap: GAP, final; malformed evidence stays NO DATA")
    rc, out, js = run(base_log(last_why="census", unseen=2))
    check(eras_of(js)["gameplay"]["verdict"] == "GAP" and rc == 2, "T20 partial + cov.unseen: GAP")
    L = base_log(last_why="census", clobber=5)[:-W] + census(60, "census", 480, 2500, clobber=5, skew=1)
    check(eras_of(run(L)[2])["gameplay"]["verdict"] == "NO DATA", "T20 partial + inconsistent counters: NO DATA")

    print("-- T21 memres2's sums: the missed attributes, the grants, the coherency statuses")
    for kw, what in (({"attr_skew": 1}, "attribute cb."), ({"grant_skew": 1}, "grants"),
                     ({"coher_skew": 1}, "census_coher")):
        L = base_log(miss=3)[:-W] + census(60, "exit", 480, 2500, 3, **kw)
        e = eras_of(run(L)[2])
        check(e["gameplay"]["verdict"] == "NO DATA" and any(what in w for w in e["gameplay"]["why"]),
              "T21 %s that does not sum: NO DATA" % what)
    rc, out, js = run(base_log(miss=3))
    e = eras_of(js)
    check(e["gameplay"]["missed_attr"].get("cb.in") == 3 and e["gameplay"]["missed_grant"].get(
        "g.AAE30000+DFF0000@826FAFB8") == 3 and "page_only by grant:" in out and "coherency requests by status" in out,
          "T21 the attributes, the grants and the statuses reported per era")

    print("-- T22 WRITER / COHER samples: reported, never gated")
    L = base_log() + [stamp(55) + "[MEM_RES] WRITER #1 page=17571000 va=B7571010 writer=guest tid=7 rip=exe+0x1234 "
                                  "lr=82612345 ctr=0 r1=7000FF00 kind=vb src=readfile grant=AAE30000+DFF0000@826FAFB8 "
                                  "armed_clock=9 clock=12"]
    rc, out, js = run(L)
    check(eras_of(js)["gameplay"]["verdict"] == "PASS" and rc == 0 and "WRITER #1 page=17571000" in out,
          "T22 a WRITER sample is listed and gates nothing")

    print("-- T23 memres3: page_only is reported, never gated; missed (fetched bytes, stale included) still FAILs")
    rc, out, js = run(base_log(po=4))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "PASS" and rc == 0 and e["gameplay"]["totals"].get("page_only") == 4 and
          "page_only=4" in out and js["by_era"]["gameplay"]["page_only"] == 4,
          "T23 page_only=4, missed=0: PASS, reported per era (and in by_era)")
    check(e["gameplay"]["missed_attr"].get("rng.same") == 4 and e["gameplay"]["missed_grant"].get("none") == 4 and
          "missed + page_only by grant:" in out, "T23 the page-only decisions carry the attributes and the grants")
    L = base_log(po=4) + [stamp(55) + "[MEM_RES] PAGE_ONLY #1 page=17571000 kind=vb prov=8 src=none site=drain "
                                      "why=content reg=keep cpu=1 gpu=0 up=1 clock=9 movie=0 cb=out seg=none rng=same "
                                      "moved=- grant=none cell=1"]
    rc, out, js = run(L)
    check(eras_of(js)["gameplay"]["verdict"] == "PASS" and rc == 0 and
          any(s[1] == "PAGE_ONLY" for s in eras_of(js)["gameplay"]["samples"]),
          "T23 a PAGE_ONLY sample is read and gates nothing (not a MISSED sample the counters do not carry)")
    rc, out, js = run(base_log(miss=2, po=4, mstale=1))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and rc == 1 and "gpu=0 stale=1" in out,
          "T23 missed=2 (1 stale) beside page_only=4: FAIL")
    # (the skews in both gameplay windows: a skew in the last one only would also read as counters going backwards -
    # memres3's first mutant run, G23 survived on that)
    e = eras_of(run(base_log(po=4, rng_skew=1))[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and
          any("the attribute rng.same 3 != page_only 4" in w for w in e["gameplay"]["why"]) and
          not any("backwards" in w for w in e["gameplay"]["why"]),
          "T23 rng.same != page_only (the rng marginal still summing): NO DATA")
    e = eras_of(run(base_log(po=4, grant_skew=-1))[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and
          any("do not sum to missed + page_only" in w for w in e["gameplay"]["why"]) and
          not any("backwards" in w for w in e["gameplay"]["why"]),
          "T23 grants short of missed + page_only: NO DATA")
    for k in ("page_only", "missed.stale", "note.shsync"):
        L = base_log()[:-W] + census(60, "exit", 480, 2500, drop=k)
        e = eras_of(run(L)[2])
        check(e["gameplay"]["verdict"] == "NO DATA" and any("missing field %s" % k in w for w in e["gameplay"]["why"]),
              "T23 a census line without %s: NO DATA" % k)
    L = [ln.replace(" missed.stale=1 ", " missed.stale=0 ") for ln in base_log(miss=2, mstale=1)]
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and
          any("missed 2 != content + window + gpu + stale" in w for w in e["gameplay"]["why"]) and
          not any("backwards" in w for w in e["gameplay"]["why"]),
          "T23 missed != content + window + gpu + stale: NO DATA")

    print("-- T24 memres5: the fallback share is a gate counter; the fetch and window counters are required and consistent")
    # gameplay: 1,600 bounded requests (its baseline the title's last window, decisions 900 -> 2,500); fallbacks in its
    # last two windows only
    rc, out, js = run(base_log(fbs=600))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and rc == 1 and
          any("fetch.fallback=600 of 2200" in w for w in e["gameplay"]["why"]) and
          e["title"]["verdict"] == "PASS" and js["by_era"]["gameplay"]["fetch_fallback"] == 600,
          "T24 fallbacks 600 of 2,200 vertex fetch requests (27%% > %d%%): FAIL, that era only"
          % int(100 * M.FETCH_FALLBACK_MAX))
    # memres6: the bound is 5% (memres5's 25% passed 400 of 2,000)
    rc, out, js = run(base_log(fbs=400))
    e = eras_of(js)
    check(M.FETCH_FALLBACK_MAX == 0.05 and e["gameplay"]["verdict"] == "FAIL" and rc == 1,
          "T24 fallbacks 400 of 2,000 (20%% > %d%%): FAIL under memres6's bound" % int(100 * M.FETCH_FALLBACK_MAX))
    rc, out, js = run(base_log(fbs=85))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and any("fetch.fallback=85 of 1685" in w for w in e["gameplay"]["why"]),
          "T24 fallbacks 85 of 1,685 (5.04%%): FAIL - the bound's edge")
    rc, out, js = run(base_log(fbs=64, fbd=20))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "PASS" and rc == 0 and "fallback=84 (shader 64, draw 20)" in out and
          js["by_era"]["gameplay"]["fetch_bounded"] == 1600,
          "T24 fallbacks 84 of 1,684 (4.99 percent): PASS, the fetch line printed per era")
    L = [ln for ln in base_log(fbs=10, fb_skew=1)]
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and any("fetch.fallback" in w and "shader + draw" in w
                                                        for w in e["gameplay"]["why"]),
          "T24 fetch.fallback != its two reasons: NO DATA")
    for k in ("fetch.bounded", "fetch.fallback.shader", "win.grant"):
        L = base_log()[:-W] + census(60, "exit", 480, 2500, drop=k)
        e = eras_of(run(L)[2])
        check(e["gameplay"]["verdict"] == "NO DATA" and any("missing field %s" % k in w for w in e["gameplay"]["why"]),
              "T24 a census line without %s: NO DATA" % k)

    print("-- T25 memres6: the soundness witness is a gate counter; the computed bounds and census_vsfb are consistent")
    rc, out, js = run(base_log(comp=1200, bsamp=30))
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "PASS" and rc == 0 and "bound.check: sampled=30 violations=0" in out and
          js["by_era"]["gameplay"]["bound_sampled"] == 30 and js["by_era"]["gameplay"]["fetch_bounded_computed"] == 1200,
          "T25 computed bounds checked by the witness, no violation: PASS, `bound.check:` printed per era")
    L = base_log(comp=1200, bsamp=30, bviol=1)
    L.insert(len(L) - W, stamp(55) + "[MEM_RES] BOUNDV #1 vs=31168A8B9ADBEEFA slot=94 cls=L1 v=3 exact=3 "
                                     "bound=[0,2] vrange=[0,11]")
    rc, out, js = run(L)
    e = eras_of(js)
    check(e["gameplay"]["verdict"] == "FAIL" and rc == 1 and
          any("bound.check.violations=1 of 30" in w for w in e["gameplay"]["why"]) and
          any(s[1] == "BOUNDV" for s in e["gameplay"]["samples"]) and e["title"]["verdict"] == "PASS" and
          js["by_era"]["gameplay"]["bound_violations"] == 1,
          "T25 a witness violation FAILS its era (the BOUNDV sample read)")
    L = base_log()
    L.insert(len(L) - W, stamp(55) + "[MEM_RES] BOUNDV #1 vs=31168A8B9ADBEEFA slot=95 cls=L2 v=7 exact=31 "
                                     "bound=[7,30] vrange=[0,11]")
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "FAIL" and
          any("BOUNDV sample(s) inside the era that the counters do not carry" in w for w in e["gameplay"]["why"]),
          "T25 a BOUNDV sample the counters do not carry: FAIL")
    e = eras_of(run(base_log(comp=1200, bsamp=0))[2])
    check(e["gameplay"]["verdict"] == "GAP" and any("the soundness witness checked none" in w
                                                     for w in e["gameplay"]["why"]),
          "T25 %d+ computed-bound requests and no witness sample: GAP" % M.WITNESS_MIN_COMPUTED)
    e = eras_of(run(base_log(comp=1000, bsamp=0))[2])
    check(e["gameplay"]["verdict"] == "PASS", "T25 below %d computed requests the witness may not have sampled: PASS"
          % M.WITNESS_MIN_COMPUTED)
    for kw, what, why in ((dict(comp=1200, bsamp=2, bviol=3), "violations > sampled", "bound.check.violations 3 >"),
                          (dict(comp=2600), "computed > bounded", "fetch.bounded.computed 2600 > fetch.bounded"),
                          (dict(fbs=10, vsfb_skew=1), "census_vsfb's reasons != fetch.fallback.shader",
                           "census_vsfb's reasons sum 11 != fetch.fallback.shader 10"),
                          (dict(fbs=10, vsfb_drop="lit"), "census_vsfb without r.lit", "census_vsfb without r.lit")):
        e = eras_of(run(base_log(**kw))[2])
        check(e["gameplay"]["verdict"] == "NO DATA" and any(why in w for w in e["gameplay"]["why"]),
              "T25 %s: NO DATA" % what)
    for k in ("fetch.bounded.computed", "bound.check.sampled", "bound.check.violations"):
        L = base_log()[:-W] + census(60, "exit", 480, 2500, drop=k)
        e = eras_of(run(L)[2])
        check(e["gameplay"]["verdict"] == "NO DATA" and any("missing field %s" % k in w for w in e["gameplay"]["why"]),
              "T25 a census line without %s: NO DATA" % k)
    L = base_log()[:-W] + census(60, "exit", 480, 2500, drop_section="census_vsfb")
    e = eras_of(run(L)[2])
    check(e["gameplay"]["verdict"] == "NO DATA" and any("missing section census_vsfb" in w
                                                        for w in e["gameplay"]["why"]),
          "T25 a window without census_vsfb: NO DATA")

    print("-- T8 every key the grader requires is printed by gfx/resource_census.cpp")
    src = open(os.path.join(REPO, "gfx", "resource_census.cpp"), encoding="utf-8").read()
    for k in M.REQUIRED:
        parts = k.split(".")
        ok = ('"%s"' % k in src) or all(('"%s"' % p in src) or ('"%s."' % p in src) for p in parts)
        check(ok, "T8 the census line prints %s" % k)
    for sec in M.SECTIONS:
        check('"[MEM_RES] %s "' % sec in src, "T8 the unit prints the section %s" % sec)
    for smp in ("CLOBBER", "ALIAS", "UNSEEN", "UNACCOUNTED", "WRITER", "COHER", "BOUNDV"):
        check('"[MEM_RES] %s #%%u ' % smp in src, "T8 the unit prints the %s sample" % smp)
    check('"[MEM_RES] %s #%u page=' in src and 'po ? "PAGE_ONLY" : "MISSED"' in src,
          "T8 the unit prints the MISSED and PAGE_ONLY samples (one format)")
    for smp in ("MISSED", "PAGE_ONLY", "WRITER", "COHER", "BOUNDV"):
        check(smp in M.SAMPLE.pattern, "T8 the grader reads the %s sample" % smp)
    check('gate SR_MEM_RESOURCES=%s: %s' in src, "T8 the gate line")

    if a.difftest_out:
        print("-- T9 the grader on the unit's own lines (difftest --mem-resources-test C12)")
        raw = open(a.difftest_out, "rb").read()
        # (a PowerShell redirection writes UTF-16; cmd's writes the bytes as printed)
        text = raw.decode("utf-16", errors="replace") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else \
            raw.decode("utf-8", errors="replace")
        exp = {}
        for ln in text.splitlines():
            if ln.startswith("MEMRES_EXPECT "):
                exp = {k: int(v) for k, v in M.KV.findall(ln)}
        lines = [ln for ln in text.splitlines() if M.TS.match(ln)]
        check(bool(exp) and any("[MEM_RES] census final swaps=42" in ln for ln in lines),
              "T9 the output carries MEMRES_EXPECT and the census lines")
        rc, out, js = run(lines, "--label", "difftest", "--entry", "start")
        e = eras_of(js).get("difftest", {})
        t = e.get("totals", {})
        check(e.get("verdict") == "GAP" and rc == 2, "T9 graded GAP (C18's unseen reuses, C19's unaccounted page): %s"
              % e.get("why", [])[:3])
        bad = {k: (t.get(k), v) for k, v in exp.items() if k != "ib.missed" and t.get(k) != v}
        check(not bad and t.get("decisions", 0) > 0, "T9 every total the unit printed is the grader's: %s" % bad)
        check(e.get("ib", {}).get("missed") == exp.get("ib.missed"), "T9 the IB arm read apart (census_ib)")
        check(not [w for w in e.get("why", []) if w.startswith("window")], "T9 the unit's window is consistent")
        kinds = {s[1] for s in e.get("samples", [])}
        check({"MISSED", "CLOBBER", "ALIAS", "UNSEEN", "UNACCOUNTED"} <= kinds, "T9 every sample kind printed: %s" % kinds)
    print("mem_res_census test: %s" % ("PASS" if not FAILS else "%d FAILED" % FAILS))
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
