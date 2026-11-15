#!/usr/bin/env python3
"""mem_res_census.py - grade mem.resources step 2a's shadow census per era (docs/RELEASE_COMPOSITION.md 9.5 item 1).

Reads the game logs of rolls made with SR_MEM_RESOURCES=census (gfx/resource_census.hpp): the cumulative census
windows - `[MEM_RES] census <why> swaps=.. ...` and its six section lines (census_missed / census_over / census_first /
census_missed_cells / census_hazard / census_ib), printed with the CP's [GPU_CP] census (swaps 1-4, every 120 swaps,
exit, the graceful quit's terminal snapshot) - and the capped samples (`[MEM_RES] MISSED #n` / CLOBBER / ALIAS / UNSEEN /
UNACCOUNTED), and grades each era.

    python tools/perf/mem_res_census.py <log> [<log> ...] [--label ERA [--entry REGEX|start] [--complete REGEX]]
                                        [--json out.json] [--samples N] [--no-require-checker]

THE ERAS and their evidence. boot = the process start to the first present (the first census window with swaps >= 1:
the CP prints one at its first swap); the intro starts there; the later eras start at the markers
tools/perf/pm4_census_eras.py uses (title, fe_menu, mods, loading, gameplay, ingame, arena). An era's numbers are the
difference between the last window at or before its start and the first window at or after its end, so a window that
straddles a boundary is charged to BOTH eras (a miss near a boundary can never land in neither); the last era ends at
the log's last window, which must be the terminal one (why = terminal / exit / final) - otherwise the era has no exit
evidence. --label ERA (the chapter-save rolls of the later eras): the eras before ERA's entry keep their names (a
seeded roll still boots through the title and the load), ERA runs from its entry - the gameplay HUD marker, or
--entry REGEX's first match, or --entry start (the whole log) - to the end; --complete REGEX names ERA's completion
evidence (a line that must appear inside it), without which ERA is NO DATA: a roll that does not reach an era's end
cannot certify the era.

THE GATE (section 9.5 step 2a): per era
  NO DATA  the evidence is incomplete: no window inside, a window missing a section or a required field, counters that
           disagree with each other (decisions != the sum of the outcomes, a marginal != its total - memres2: the missed
           attributes, the grants, the coherency statuses too) or go backwards (two processes in one log), zero
           decisions, no entry evidence; or PARTIAL evidence (no exit window - the quit's terminal snapshot missing - or
           no completion line) whose windows would pass. Partial evidence is final when its windows already GAP or FAIL
           (the counters only grow: the missing tail can add, never remove) - memres1's mrc2, whose quit could not
           quiesce after 90 failing warworld windows, grades FAIL, not NO DATA. The exit gap names its cause from the
           quit's own record: quit.json beside the log and the game's `[QUIT] terminal snapshot FAILED` line;
  GAP      the instrument cannot certify the era: cov.unseen > 0 (the model reused a decision the census never saw: an
           unhooked path), cov.unaccounted > 0 (a walk left a page no hook disposed of), the model's own checker found a
           stale keep (wt.stale, keep.*.stale) or a range-cache window mismatch (keep.rc.mismatch), or - unless
           --no-require-checker - a write-watch or range-cache keep went unchecked (each feed with keeps in the era must
           have every keep re-verified: SR_SHM_WT_CHECK=1 SR_NB_SHM_RANGE_CHECK=1; a feed with no keeps needs no checks);
           an UNSEEN / UNACCOUNTED sample inside the era likewise; memres6: an era whose computed bounds the soundness
           witness never checked (fetch.bounded.computed >= WITNESS_MIN_COMPUTED with bound.check.sampled = 0);
  FAIL     a missed boundary nobody names (missed - missed.movie > 0: memres3 - the registry's arena differs from RAM in
           bytes a request fetched, missed.stale included; memres5: fetched = index-bounded, dword-exact), the fallback
           share (fetch.fallback over FETCH_FALLBACK_MAX of the era's vertex fetch requests - memres5 25%, memres6 5%:
           the fetched bytes mostly declared ranges), memres6's soundness witness (bound.check.violations > 0: a draw
           whose exact per-vertex index left its computed bound), a GPU-data clobber (clobber > 0), a registry
           upload from a window whose bytes differ from the model's (win.alias_diff > 0), or such a sample inside the
           era that the counters do not carry (a sample after the last window; memres6: BOUNDV);
  PASS     (with the classified class) missed > 0, every one cpu_only (census_patch missed_cpu_only: the moved
           dwords only a named CPU-only writer wrote - RenderModel_SwapMeshChunkBindings' draw records, the
           orchestrator's 2026-10-17 decision; the evidence in docs/RELEASE_COMPOSITION.md 9.5) - reported as
           classified.cpu_only, never silent;
  KNOWN    missed > 0, all inside a movie plane's range (the decoder's planar branch: 9.2 item 5) or all prior
           (mem-native: census_patch missed_prior - every moved dword only an earlier owner version's draws fetched,
           the page's allocation since reused) - not a PASS;
  PASS     none of the above.
race.note / race.lock (a note in the hash window, a read inside a guest Lock..Unlock), over, first, win.alias_same,
clobber.both, reuse.noted and the IB arm (census_ib: a hypothetical conversion cache - outside the page gate) are
reported, never gated; so are memres2's classification lines (census_coher: every coherency request by status;
census_missed_attr: the misses by cb / seg / rng; census_missed_grant: by the grant holding the page) and its samples
(WRITER: a trap named a missed page's writer; COHER: an unclassified status). memres3's page_only (the registry kept a
page the model re-uploaded, and every 16-byte block any request ever fetched from it is unchanged since the registry's
copy: the registry's arena differs from RAM only in bytes nothing fetched; PAGE_ONLY samples) is reported, never gated:
memres3's NOTES state this gate before its rolls. The attributes and the grants cover missed + page_only (rng.same is
exactly page_only); note.shsync counts the shader-constant syncs (status 0x00010000). memres5's fetch.bounded /
fetch.fallback (shader + draw) and the registry's window rules (win.live / win.grant / win.default) are printed per era
and in by_era (fetch_bounded / fetch_fallback); fetch.fallback must equal its two reasons' sum (else NO DATA).
memres6: the world family's computed indices bounded through both levels (fetch.bounded.computed, part of
fetch.bounded), the soundness witness (bound.check.sampled draws evaluated exactly per vertex, bound.check.violations of
them outside their bound - printed per era as `bound.check: sampled= violations=`, by_era bound_sampled /
bound_violations), census_vsfb (the shader fallbacks by reason, r.* summing to fetch.fallback.shader, else NO DATA;
census_vsfb_top - the VS with the most - is read past); violations <= sampled and computed <= bounded (else NO DATA).
Exit code: 0 = every era PASS; 1 = any era FAIL or KNOWN; 2 = no census at all / an era with GAP or NO DATA / an
unreadable log.
"""
import argparse
import datetime as dt
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pm4_census_eras import ERAS  # noqa: E402  (the one era list of the per-era censuses)

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
SECTIONS = ("census", "census_missed", "census_over", "census_first", "census_missed_cells", "census_hazard",
            "census_ib", "census_coher", "census_missed_attr", "census_missed_grant", "census_vsfb")
# mem-native: optional sections - reported when a log carries them, never required (logs before them grade as before):
# census_patch, the exact CPU-write dwords (the D3D driver's header fence stamps) the registry copies alone.
OPTIONAL = ("census_patch",)
LINE = re.compile(r"\[MEM_RES\] (%s) (\S+)((?: \S+=\d+)*)\s*$" % "|".join(SECTIONS + OPTIONAL))
SAMPLE = re.compile(r"\[MEM_RES\] (MISSED|CLOBBER|ALIAS|UNSEEN|UNACCOUNTED|WRITER|COHER|PAGE_ONLY|BOUNDV) #(\d+) (.*)$")
# memres6: census_vsfb's reasons (gfx/resource_census.cpp kFbName, in order)
FB_REASONS = ("slot", "op", "rel", "ctl", "fmt", "read", "many", "range", "state", "clob", "long", "vs", "lit")
GATE = re.compile(r"\[MEM_RES\] gate SR_MEM_RESOURCES=(\S+): (.*)$")
QUIT_FAILED = re.compile(r"\[QUIT\] (terminal snapshot FAILED .*)$")
KV = re.compile(r"(\S+?)=(\d+)")
OUTCOMES = ("agree_up", "agree_keep", "over", "missed", "clobber", "race.note", "race.lock", "page_only")
CHKS = ("checked", "stale", "mismatch", "unchecked")
# The keys every census line must carry (gfx/resource_census.cpp buildLines; test_mem_res_census.py holds the two
# together). A missing one is malformed evidence, never a zero.
REQUIRED = (("swaps", "decisions") + OUTCOMES +
            ("missed.content", "missed.window", "missed.gpu", "missed.stale", "missed.movie", "first", "win.heur", "win.alias_same",
             "win.alias_diff", "win.unmapped", "clobber.both", "reuse.noted", "cov.req", "cov.val_cands", "cov.decided",
             "cov.cand", "cov.val", "cov.reuse", "cov.reuse_failed", "cov.unseen", "cov.deferred", "cov.failed",
             "cov.rebase", "cov.unaccounted") +
            tuple("keep.%s.%s" % (f, c) for f in ("wt", "rc") for c in CHKS) +
            ("note.whole", "note.refused", "note.lock", "note.shsync", "lock.open", "clock", "wt.checks", "wt.stale") +
            ("note.resolve", "note.resolve_pages", "coher.requests", "up.allocs", "up.zpass", "trap.armed",
             "trap.fired", "trap.host", "trap.arm_fail", "trap.declined") +
            ("fetch.bounded", "fetch.fallback", "fetch.fallback.shader", "fetch.fallback.draw", "win.live", "win.grant",
             "win.default") +
            ("fetch.bounded.computed", "bound.check.sampled", "bound.check.violations"))
GAUGES = {"lock.open", "cells", "cb.lo", "cb.hi", "grants", "vs"}  # may go down between windows (or are values)
# The share of an era's vertex fetch requests whose fetched bytes fell back to the declared range (the VS's vfetch not
# provably bounded, or no draw index range) above which the era FAILs - fixed before each round's rolls (its NOTES): a
# gate that passes mostly on declared ranges is not the index-bounded gate. memres5: 0.25; memres6 (the world family
# bounded through its computed indices): 0.05.
FETCH_FALLBACK_MAX = 0.05
# memres6: the computed-bound requests an era may carry before its witness must have sampled a draw (the census
# evaluates its first 256 family draws exactly, then 1 in 64; a draw makes at most 16 computed requests - one per slot).
WITNESS_MIN_COMPUTED = 1024
TERMINAL = ("terminal", "exit", "final")
# The design's eras (9.5): the per-era censuses' markers grouped.
DESIGN_ERA = {"boot": "boot", "intro": "movies", "title": "menus", "fe_menu": "menus", "mods": "menus",
              "loading": "menus", "gameplay": "gameplay", "ingame": "gameplay", "arena": "warworld"}


def ts(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def parse(path, extra=None):
    """The census windows ({t, why, sec: {section: {key: value}}}), the samples (t, kind, n, text), the era marks, the
    gate, and the first match time of each `extra` regex (name -> regex)."""
    windows, samples, marks, gate, found, quit_failed = [], [], {}, None, {}, None
    first_t = last_t = None
    pending = [(e, re.compile(rx), after) for e, rx, after in ERAS if rx and not rx.startswith("@")]
    extra = {k: re.compile(v) for k, v in (extra or {}).items() if v}
    cur = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = ts(line)
            if t is None:
                continue
            if first_t is None:
                first_t = t
            last_t = t
            for k, rx in extra.items():
                if k not in found and rx.search(line):
                    found[k] = t
            if "[MEM_RES]" in line:
                m = LINE.search(line)
                if m:
                    sec, why, rest = m.group(1), m.group(2), m.group(3)
                    kv = {k: int(v) for k, v in KV.findall(rest)}
                    if sec == "census":
                        cur = {"t": t, "why": why, "sec": {"census": kv}}
                        windows.append(cur)
                    elif cur is not None and cur["why"] == why and sec not in cur["sec"]:
                        cur["sec"][sec] = kv
                    continue
                m = SAMPLE.search(line)
                if m:
                    samples.append((t, m.group(1), int(m.group(2)), m.group(3).strip()))
                    continue
                m = GATE.search(line)
                if m:
                    gate = (m.group(1), m.group(2))
                    continue
            if "[QUIT]" in line:
                m = QUIT_FAILED.search(line)
                if m:
                    quit_failed = m.group(1).strip()
            for i, (era, rx, after) in enumerate(pending):
                if after and after not in marks:
                    continue
                if rx.search(line):
                    marks[era] = t
                    pending.pop(i)
                    break
    first_present = next((w["t"] for w in windows if w["sec"]["census"].get("swaps", 0) >= 1), None)
    return dict(path=path, t0=first_t, end=last_t, windows=windows, samples=samples, marks=marks, gate=gate,
                found=found, first_present=first_present, quit_failed=quit_failed, quit=read_quit(path))


def read_quit(log_path):
    """The graceful quit's result beside the log (tools/drive writes quit.json next to run.log), or None."""
    p = os.path.join(os.path.dirname(os.path.abspath(log_path)), "quit.json")
    try:
        with open(p, encoding="utf-8-sig") as fh:
            q = json.load(fh)
    except (OSError, ValueError):
        return None
    return q if isinstance(q, dict) else None


def quit_note(run):
    """Why the exit evidence is missing, from the quit's own record: quit.json and the game's FAILED line."""
    out = []
    q = run.get("quit")
    if q:
        out.append("quit.json: result=%s graceful=%s exitCode=%s (%s)" % (
            q.get("result"), q.get("graceful"), q.get("exitCode"), str(q.get("detail", ""))[:160]))
    if run.get("quit_failed"):
        out.append("[QUIT] %s" % run["quit_failed"][:240])
    return "; ".join(out)


def sumpre(d, prefix):
    return sum(v for k, v in d.items() if k.startswith(prefix))


def window_problems(w):
    """Why a window is not usable evidence: a missing section or field, counters that disagree."""
    s = w["sec"]
    out = ["missing section %s" % x for x in SECTIONS if x not in s]
    if out:
        return out
    c = s["census"]
    out += ["missing field %s" % k for k in REQUIRED if k not in c]
    if out:
        return out
    if c["decisions"] != sum(c[o] for o in OUTCOMES):
        out.append("decisions %d != the outcomes' sum %d" % (c["decisions"], sum(c[o] for o in OUTCOMES)))
    if c["missed"] != c["missed.content"] + c["missed.window"] + c["missed.gpu"] + c["missed.stale"]:
        out.append("missed %d != content + window + gpu + stale" % c["missed"])
    if c["missed.movie"] > c["missed"]:
        out.append("missed.movie > missed")
    m, ov, f, hz, ib = (s[x] for x in ("census_missed", "census_over", "census_first", "census_hazard", "census_ib"))
    for sec, prefix, total, name in ((m, "kind.", c["missed"], "missed"), (m, "prov.", c["missed"], "missed"),
                                     (m, "src.", c["missed"], "missed"), (m, "site.", c["missed"], "missed"),
                                     (ov, "kind.", c["over"], "over"), (ov, "src.", c["over"], "over"),
                                     (f, "kind.", c["first"], "first"), (f, "prov.", c["first"], "first"),
                                     (hz, "clobber.kind.", c["clobber"], "clobber"),
                                     (hz, "clobber.src.", c["clobber"], "clobber"),
                                     (hz, "clobber.site.", c["clobber"], "clobber"),
                                     (hz, "race.note.kind.", c["race.note"], "race.note"),
                                     (hz, "race.note.site.", c["race.note"], "race.note"),
                                     (hz, "race.lock.kind.", c["race.lock"], "race.lock"),
                                     (hz, "race.lock.site.", c["race.lock"], "race.lock")):
        if sumpre(sec, prefix) != total:
            out.append("%s's %s* sum %d != %d" % (name, prefix, sumpre(sec, prefix), total))
    cells = s["census_missed_cells"]
    if cells.get("cells", 0) < 96 and sum(v for k, v in cells.items() if k != "cells") != c["missed"]:
        out.append("the missed cells do not sum to missed")
    # memres2 / memres3: the missed and page-only decisions' attributes (each marginal sums to missed + page_only, and
    # rng.same is exactly page_only), their grants (none + every grant = missed + page_only), the coherency requests by
    # status (every status + overflow = requests)
    at, gr, co = s["census_missed_attr"], s["census_missed_grant"], s["census_coher"]
    mp = c["missed"] + c["page_only"]
    for prefix in ("cb.", "seg.", "rng."):
        if sumpre(at, prefix) != mp:
            out.append("the missed attribute %s* sum %d != missed + page_only %d" % (prefix, sumpre(at, prefix), mp))
    if at.get("rng.same", -1) != c["page_only"]:
        out.append("the attribute rng.same %d != page_only %d" % (at.get("rng.same", -1), c["page_only"]))
    if gr.get("none", -1) < 0 or gr["none"] + sumpre(gr, "g.") != mp:
        out.append("the missed grants (none + g.*) do not sum to missed + page_only")
    if c["fetch.fallback"] != c["fetch.fallback.shader"] + c["fetch.fallback.draw"]:
        out.append("fetch.fallback %d != shader + draw" % c["fetch.fallback"])
    # memres6: the computed bounds are bounds, the witness's violations are draws it sampled, the shader fallbacks'
    # reasons sum to them
    if c["fetch.bounded.computed"] > c["fetch.bounded"]:
        out.append("fetch.bounded.computed %d > fetch.bounded %d" % (c["fetch.bounded.computed"], c["fetch.bounded"]))
    if c["bound.check.violations"] > c["bound.check.sampled"]:
        out.append("bound.check.violations %d > bound.check.sampled %d" % (c["bound.check.violations"],
                                                                           c["bound.check.sampled"]))
    fbr = s.get("census_vsfb", {})
    missing = [r for r in FB_REASONS if "r." + r not in fbr]
    if missing:
        out.append("census_vsfb without r.%s" % missing[0])
    elif sumpre(fbr, "r.") != c["fetch.fallback.shader"]:
        out.append("census_vsfb's reasons sum %d != fetch.fallback.shader %d" % (sumpre(fbr, "r."),
                                                                                 c["fetch.fallback.shader"]))
    if "requests" not in co or sumpre(co, "calls.") + co.get("overflow", 0) != co["requests"] or \
            co["requests"] != c["coher.requests"]:
        out.append("census_coher's statuses do not sum to its requests (or to coher.requests)")
    if any(k not in ib for k in ("reads", "agree_up", "agree_keep", "missed", "over")):
        out.append("census_ib incomplete")
    elif ib["reads"] != ib["agree_up"] + ib["agree_keep"] + ib["missed"] + ib["over"] or \
            sumpre(ib, "missed.prov.") != ib["missed"] or sumpre(ib, "missed.src.") != ib["missed"]:
        out.append("census_ib counters disagree")
    return out


def backwards(a, b):
    """Counters of window b below window a's (a restart: two processes in one log)."""
    out = []
    for sec in SECTIONS + OPTIONAL:
        for k, v in a["sec"].get(sec, {}).items():
            if k in GAUGES:
                continue
            if b["sec"].get(sec, {}).get(k, 0) < v:
                out.append("%s.%s" % (sec, k))
    return out


def diff(a, b):
    """b - a, section by section (a missing key counts 0: the cell lines print nonzero cells only)."""
    out = {}
    for sec in SECTIONS + OPTIONAL:
        sa, sb = (a["sec"] if a else {}).get(sec, {}), b["sec"].get(sec, {})
        out[sec] = {k: sb.get(k, 0) - sa.get(k, 0) for k in set(sa) | set(sb) if k not in GAUGES}
    return out


def nonzero(d, prefix=None):
    return {k: v for k, v in sorted(d.items(), key=lambda kv: -kv[1]) if v and (prefix is None or k.startswith(prefix))}


def spans(run, label=None, entry=None):
    """[(era, t_start, t_end or None, problem)] in time order: the boot, the first present, the markers; with --label
    the eras before its entry, then the label to the end."""
    marks = {"boot": run["t0"]}
    if run["first_present"] is not None:
        marks["intro"] = run["first_present"]
    for e, t in run["marks"].items():
        if e != "intro":
            marks[e] = t
    order = sorted(marks.items(), key=lambda kv: kv[1])
    problem = None
    if label:
        te = run["t0"] if entry == "start" else run["found"].get("entry") if entry else run["marks"].get("gameplay")
        if te is None:
            problem = "no entry evidence for %s (%s)" % (label, "--entry %s" % entry if entry else "the gameplay HUD marker")
            te = run["end"]
        order = [(e, t) for e, t in order if t < te] + [(label, te)]
    out = []
    for i, (e, t) in enumerate(order):
        out.append((e, t, order[i + 1][1] if i + 1 < len(order) else None,
                    problem if (label and e == label) else None))
    return out


def grade_span(run, name, t0, t1, require_checker=True, problem=None, complete=None, design=None):
    W = run["windows"]
    end = t1 if t1 is not None else run["end"]
    res = dict(era=name, design_era=design or DESIGN_ERA.get(name, name), t0=t0 - run["t0"], dur=end - t0, windows=0)
    # the baseline: the last window at or before the era's start (an era from the process start counts from zero)
    ia = None if t0 <= run["t0"] else max((i for i, w in enumerate(W) if w["t"] <= t0), default=None)
    if t1 is None:
        ib = len(W) - 1 if W else None
        exit_ok = bool(W) and W[-1]["why"] in TERMINAL
        exit_why = "the log ends without the terminal census window (last: '%s')" % (W[-1]["why"] if W else "-")
    else:
        ib = min((i for i, w in enumerate(W) if w["t"] >= t1), default=None)
        exit_ok = ib is not None
        exit_why = "no census window at or after the era's end"
        if ib is None and W:
            ib = len(W) - 1
    lo = 0 if ia is None else ia + 1
    # partial = the era's evidence ends early (no exit window, no completion line) though every window it has is sound:
    # the counters only grow, so a FAIL or a GAP the partial windows show is final; NO DATA only where they would pass
    # (memres1's mrc2: the quit could not quiesce, the warworld era's 90 windows already failed - item 5 of memres2).
    nodata, partial, gap, fail = [], [], [], []
    if problem:
        nodata.append(problem)
    if ib is None or ib < lo:
        res["verdict"] = "NO DATA"
        res["why"] = nodata + ["no census window inside the era"]
        return res
    res["windows"] = ib - lo + 1
    if not exit_ok:
        qn = quit_note(run) if t1 is None else ""
        partial.append(exit_why + ("; " + qn if qn else ""))
    for i in range(lo if ia is None else ia, ib + 1):
        p = window_problems(W[i])
        if p:
            nodata.append("window %d ('%s' at %.1fs): %s" % (i, W[i]["why"], W[i]["t"] - run["t0"], "; ".join(p[:3])))
        if i > 0 and i >= lo:
            bk = backwards(W[i - 1], W[i])
            if bk:
                nodata.append("counters went backwards at window %d (%s): two processes in one log" % (i, bk[0]))
    if complete is not None:
        tc = run["found"].get("complete")
        res["completion"] = tc is not None and t0 <= tc <= end
        if not res["completion"]:
            partial.append("no completion evidence inside the era (--complete %s)" % complete)
    d = diff(W[ia] if ia is not None else None, W[ib])
    c = d["census"]
    res["totals"] = {k: c.get(k, 0) for k in REQUIRED if k not in GAUGES}
    res["missed_by"] = nonzero(d["census_missed"])
    res["missed_cells"] = nonzero(d["census_missed_cells"])
    res["over_by"] = nonzero(d["census_over"])
    res["first_by"] = nonzero(d["census_first"])
    res["hazard"] = nonzero(d["census_hazard"])
    res["ib"] = nonzero(d["census_ib"])
    res["notes"] = nonzero(c, "note.")
    res["coher"] = nonzero(d["census_coher"])
    res["missed_attr"] = nonzero(d["census_missed_attr"])
    res["missed_grant"] = nonzero(d["census_missed_grant"])
    res["vsfb"] = nonzero(d["census_vsfb"], "r.")
    res["traps"] = nonzero(c, "trap.")
    res["patch"] = nonzero(d.get("census_patch", {}))
    # the samples logged inside the era's time span (to the log's end for the last era: after its last window too)
    s_lo = t0 if t0 > run["t0"] else run["t0"] - 1
    s_hi = end if t1 is not None else run["end"]
    res["samples"] = [(t - run["t0"], k, n, s) for t, k, n, s in run["samples"] if s_lo <= t <= s_hi]
    if not nodata and c.get("decisions", 0) <= 0:
        nodata.append("zero decisions: nothing was graded")
    g = lambda k: c.get(k, 0)  # noqa: E731
    if g("cov.unseen") > 0:
        gap.append("cov.unseen=%d: the model reused decisions the census never saw (an unhooked path)" % g("cov.unseen"))
    if g("cov.unaccounted") > 0:
        gap.append("cov.unaccounted=%d: walks left pages no hook disposed of" % g("cov.unaccounted"))
    if g("wt.stale") > 0 or g("keep.wt.stale") + g("keep.rc.stale") > 0:
        gap.append("the model's checker found stale keeps (wt.stale=%d keep.wt.stale=%d keep.rc.stale=%d): both blind "
                   "there" % (g("wt.stale"), g("keep.wt.stale"), g("keep.rc.stale")))
    if g("keep.rc.mismatch") > 0:
        gap.append("keep.rc.mismatch=%d: the range cache kept pages under another window" % g("keep.rc.mismatch"))
    for feed, gate_env in (("wt", "SR_SHM_WT_CHECK=1"), ("rc", "SR_NB_SHM_RANGE_CHECK=1 + SR_SHM_WT_CHECK=1")):
        keeps = sum(g("keep.%s.%s" % (feed, x)) for x in CHKS)
        if require_checker and keeps > 0 and g("keep.%s.unchecked" % feed) > 0:
            gap.append("keep.%s.unchecked=%d of %d keeps: that feed's keeps not all re-verified (roll with %s)" % (
                feed, g("keep.%s.unchecked" % feed), keeps, gate_env))
    kinds = {}
    for _, k, _, s in res["samples"]:
        if k == "MISSED" and "movie=1" in s.split():
            k = "MISSED_MOVIE"
        elif k == "MISSED" and "cpu_only=1" in s.split():
            k = "MISSED_CPU_ONLY"  # mem-native: only the named CPU-only writer wrote what moved
        elif k == "MISSED" and "prior=1" in s.split():
            k = "MISSED_PRIOR"  # mem-native: only an earlier owner version's draws fetched what moved
        kinds[k] = kinds.get(k, 0) + 1
    if kinds.get("UNSEEN", 0) and not g("cov.unseen"):
        gap.append("UNSEEN samples inside the era that the counters do not carry")
    if kinds.get("UNACCOUNTED", 0) and not g("cov.unaccounted"):
        gap.append("UNACCOUNTED samples inside the era that the counters do not carry")
    if g("fetch.bounded.computed") >= WITNESS_MIN_COMPUTED and g("bound.check.sampled") == 0:
        gap.append("fetch.bounded.computed=%d with bound.check.sampled=0: the soundness witness checked none of the "
                   "era's computed bounds" % g("fetch.bounded.computed"))
    # mem-native: census_patch's missed_prior (classified, still MISSED) and vtx_stale (the used-vertex bitmap lacked
    # a converted index: a coverage gap)
    prior = res["patch"].get("missed_prior", 0)
    cpu_only = res["patch"].get("missed_cpu_only", 0)
    named = g("missed") - g("missed.movie") - prior - cpu_only
    if named > 0:
        fail.append("missed=%d (movie %d, prior %d, cpu_only %d): a CPU write the guest never announced" % (
            g("missed"), g("missed.movie"), prior, cpu_only))
    if cpu_only:
        res["classified"] = {"cpu_only": cpu_only}
    if res["patch"].get("vtx_stale", 0) > 0:
        gap.append("vtx_stale=%d: converted indices the census's used-vertex bitmap did not hold (the index buffer "
                   "changed after the census read it)" % res["patch"]["vtx_stale"])
    if g("clobber") > 0:
        fail.append("clobber=%d: registry uploads over GPU data the model kept" % g("clobber"))
    fb, fbn = g("fetch.fallback"), g("fetch.fallback") + g("fetch.bounded")
    if fbn and fb > FETCH_FALLBACK_MAX * fbn:
        fail.append("fetch.fallback=%d of %d vertex fetch requests (%.1f%% > %.0f%%): the era's fetched bytes are mostly "
                    "declared ranges, not index-bounded" % (fb, fbn, 100.0 * fb / fbn, 100.0 * FETCH_FALLBACK_MAX))
    if g("win.alias_diff") > 0:
        fail.append("win.alias_diff=%d: registry uploads from a window whose bytes differ from the model's" %
                    g("win.alias_diff"))
    if g("bound.check.violations") > 0:
        fail.append("bound.check.violations=%d of %d sampled draws: an exact per-vertex index outside its computed bound "
                    "(BOUNDV samples)" % (g("bound.check.violations"), g("bound.check.sampled")))
    for k, have in (("MISSED", named), ("CLOBBER", g("clobber")), ("ALIAS", g("win.alias_diff")),
                    ("BOUNDV", g("bound.check.violations"))):
        if kinds.get(k, 0) and not have:
            fail.append("%d %s sample(s) inside the era that the counters do not carry (after the last window?)" % (
                kinds[k], k))
    if kinds.get("MISSED_MOVIE", 0) and not g("missed.movie"):
        fail.append("movie MISSED samples inside the era that the counters do not carry")
    if kinds.get("MISSED_PRIOR", 0) and not prior:
        fail.append("prior MISSED samples inside the era that the counters do not carry")
    if kinds.get("MISSED_CPU_ONLY", 0) and not cpu_only:
        fail.append("cpu_only MISSED samples inside the era that the counters do not carry")
    res["partial"] = bool(partial)
    if partial and (gap or fail):
        partial.append("partial evidence, final: the counters only grow - the windows it has already %s" %
                       ("show a coverage gap" if gap else "fail"))
    res["why"] = nodata + partial + gap + fail
    if nodata:
        res["verdict"] = "NO DATA"
    elif gap:
        res["verdict"] = "GAP"
    elif fail:
        res["verdict"] = "FAIL"
    elif partial:
        res["verdict"] = "NO DATA"  # the windows it has would pass (or be KNOWN): the missing tail could hold a miss
    elif g("missed") > 0 and g("missed") > cpu_only:
        res["verdict"] = "KNOWN"
    else:
        res["verdict"] = "PASS"  # (with a cpu_only class: res["classified"], printed beside the verdict)
        if cpu_only:
            res["why"] = res["why"] + ["classified: cpu_only=%d - the named CPU-only writer's dwords (RenderModel_"
                                       "SwapMeshChunkBindings' draw records; the orchestrator's 2026-10-17 decision)"
                                       % cpu_only]
    return res


def grade(run, label=None, require_checker=True, entry=None, complete=None):
    if not run["windows"]:
        return []
    out = []
    for era, t0, t1, problem in spans(run, label, entry):
        is_label = bool(label) and era == label
        out.append(grade_span(run, era, t0, t1, require_checker, problem, complete if is_label else None,
                              label if is_label else None))
    return out


def fmt_kv(d, n=12):
    return " ".join("%s=%d" % kv for kv in list(d.items())[:n]) or "-"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--label", help="the chapter-save roll's era: from its entry to the end of the log")
    ap.add_argument("--entry", help="the label era's entry: a regex (its first match) or 'start' (default: the HUD)")
    ap.add_argument("--complete", help="the label era's completion evidence: a regex that must match inside it")
    ap.add_argument("--json")
    ap.add_argument("--samples", type=int, default=4, help="samples listed per era that is not PASS")
    ap.add_argument("--no-require-checker", action="store_true",
                    help="do not require every write-watch / range-cache keep re-verified (default: required - GAP)")
    a = ap.parse_args(argv)
    if (a.entry or a.complete) and not a.label:
        ap.error("--entry / --complete need --label")
    extra = {}
    if a.entry and a.entry != "start":
        extra["entry"] = a.entry
    if a.complete:
        extra["complete"] = a.complete
    rc = 0
    out = []
    for p in a.logs:
        try:
            run = parse(p, extra)
        except OSError as e:
            print("== %s: unreadable (%s)" % (p, e))
            rc = 2
            continue
        print("== %s" % p)
        if run["gate"]:
            print("   gate SR_MEM_RESOURCES=%s: %s" % run["gate"])
        if not run["windows"]:
            print("   NO CENSUS: no [MEM_RES] census line (was the roll made with SR_MEM_RESOURCES=census?)")
            rc = 2
            out.append(dict(path=p, eras=[], verdict="NO CENSUS"))
            continue
        last = run["windows"][-1]
        print("   %d census windows, the last '%s' at %.1fs (swaps %s)" % (
            len(run["windows"]), last["why"], last["t"] - run["t0"], last["sec"]["census"].get("swaps", "?")))
        if run["quit"] or run["quit_failed"]:
            print("   quit: %s" % quit_note(run))
        eras = grade(run, a.label, not a.no_require_checker, a.entry, a.complete)
        print("   eras: " + ", ".join("%s@%.1fs" % (r["era"], r["t0"]) for r in eras))
        for r in eras:
            t = r.get("totals", {})
            print("  -- era %-9s (%s) t0=%7.1fs dur=%6.1fs windows=%d  %s  decisions=%d agree_up=%d agree_keep=%d "
                  "over=%d missed=%d page_only=%d clobber=%d race.note=%d race.lock=%d first=%d" % (
                      r["era"], r["design_era"], r["t0"], r["dur"], r["windows"], r["verdict"], t.get("decisions", 0),
                      t.get("agree_up", 0), t.get("agree_keep", 0), t.get("over", 0), t.get("missed", 0),
                      t.get("page_only", 0), t.get("clobber", 0), t.get("race.note", 0), t.get("race.lock", 0),
                      t.get("first", 0)))
            for w in r.get("why", []):
                print("     %s" % w)
            if not t:
                rc = max(rc, 2)
                continue
            print("     window: heur=%d alias_same=%d alias_diff=%d unmapped=%d | clobber.both=%d reuse.noted=%d | "
                  "cov: req=%d reuse=%d deferred=%d failed=%d unseen=%d unaccounted=%d | keep.wt %d/%d checked, "
                  "keep.rc %d/%d checked" % (
                      t["win.heur"], t["win.alias_same"], t["win.alias_diff"], t["win.unmapped"], t["clobber.both"],
                      t["reuse.noted"], t["cov.req"], t["cov.reuse"], t["cov.deferred"], t["cov.failed"],
                      t["cov.unseen"], t["cov.unaccounted"], t["keep.wt.checked"],
                      sum(t["keep.wt.%s" % x] for x in CHKS), t["keep.rc.checked"],
                      sum(t["keep.rc.%s" % x] for x in CHKS)))
            fbn = t["fetch.bounded"] + t["fetch.fallback"]
            print("     fetch: bounded=%d fallback=%d (shader %d, draw %d) = %.1f%% of %d (FAIL above %.0f%%) | registry "
                  "window: live=%d grant=%d default=%d" % (
                      t["fetch.bounded"], t["fetch.fallback"], t["fetch.fallback.shader"], t["fetch.fallback.draw"],
                      100.0 * t["fetch.fallback"] / fbn if fbn else 0.0, fbn, 100.0 * FETCH_FALLBACK_MAX,
                      t["win.live"], t["win.grant"], t["win.default"]))
            print("     bound.check: sampled=%d violations=%d | fetch.bounded.computed=%d | fallback reasons: %s" % (
                t["bound.check.sampled"], t["bound.check.violations"], t["fetch.bounded.computed"],
                fmt_kv(r.get("vsfb", {}), 13)))
            print("     notes: %s" % fmt_kv(r["notes"], 20))
            if r.get("patch"):
                print("     exact CPU-write patches (never gated): %s" % fmt_kv(r["patch"], 10))
            if r["ib"]:
                print("     ib (outside the gate): %s" % fmt_kv(r["ib"], 10))
            if r["coher"]:
                print("     coherency requests by status: %s" % fmt_kv(r["coher"], 12))
            if t["missed"]:
                print("     missed: content=%d window=%d gpu=%d stale=%d movie=%d | %s" % (
                    t["missed.content"], t["missed.window"], t["missed.gpu"], t["missed.stale"], t["missed.movie"],
                    fmt_kv(r["missed_by"], 24)))
                print("     missed cells: %s" % fmt_kv(r["missed_cells"], 16))
            if t["missed"] or t["page_only"]:
                print("     missed + page_only attributes (cb / seg / rng - rng.same = page_only, then "
                      "kind/src/cb/seg/rng): %s" % fmt_kv(r["missed_attr"], 24))
                print("     missed + page_only by grant: %s" % fmt_kv(r["missed_grant"], 12))
            if r["hazard"]:
                print("     hazards: %s" % fmt_kv(r["hazard"], 16))
            if r["traps"]:
                print("     traps: %s" % fmt_kv(r["traps"], 8))
            writers = [s for s in r["samples"] if s[1] == "WRITER"]
            for s in writers:
                print("       %.1fs %s #%d %s" % s)
            if r["verdict"] != "PASS":
                for s in [s for s in r["samples"] if s[1] != "WRITER"][:a.samples]:
                    print("       %.1fs %s #%d %s" % s)
            rc = max(rc, {"PASS": 0, "KNOWN": 1, "FAIL": 1, "GAP": 2, "NO DATA": 2}[r["verdict"]])
        out.append(dict(path=p, eras=eras, windows=len(run["windows"]), gate=run["gate"], quit=run["quit"],
                        quit_failed=run["quit_failed"]))
    verdict = {0: "PASS", 1: "FAIL", 2: "INCOMPLETE"}[rc]
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(dict(verdict=verdict, rc=rc, by_era=by_design_era(out), logs=out), fh, indent=1)
    print("verdict: %s" % {0: "PASS (every era: missed, clobber, alias_diff = 0 with full coverage)",
                           1: "FAIL (a gate counter: see the eras)",
                           2: "INCOMPLETE (no census / an era without data or with a coverage gap)"}[rc])
    return rc


RANK = {"PASS": 0, "KNOWN": 1, "FAIL": 2, "GAP": 3, "NO DATA": 4}


def by_design_era(logs):
    """The design's eras (boot, movies, menus, gameplay, warworld, a --label) over every log: the counts summed, the
    worst verdict (NO DATA > GAP > FAIL > KNOWN > PASS) - the keyed form the ledger's json evidence reads
    (by_era.<era>.verdict / .missed / .page_only / .clobber / .alias_diff / .completion)."""
    agg = {}
    for lg in logs:
        for r in lg.get("eras", []):
            k = r["design_era"]
            a = agg.setdefault(k, dict(verdict="PASS", windows=0, decisions=0, missed=0, missed_movie=0, clobber=0,
                                       alias_diff=0, over=0, first=0, unseen=0, unaccounted=0, page_only=0,
                                       fetch_bounded=0, fetch_fallback=0, fetch_bounded_computed=0, bound_sampled=0,
                                       bound_violations=0, eras=[], completion=None))
            a["eras"].append(r["era"])
            a["windows"] += r.get("windows", 0)
            t = r.get("totals", {})
            for f, src in (("decisions", "decisions"), ("missed", "missed"), ("missed_movie", "missed.movie"),
                           ("page_only", "page_only"), ("fetch_bounded", "fetch.bounded"),
                           ("fetch_fallback", "fetch.fallback"), ("fetch_bounded_computed", "fetch.bounded.computed"),
                           ("bound_sampled", "bound.check.sampled"), ("bound_violations", "bound.check.violations"),
                           ("clobber", "clobber"), ("alias_diff", "win.alias_diff"), ("over", "over"),
                           ("first", "first"), ("unseen", "cov.unseen"), ("unaccounted", "cov.unaccounted")):
                a[f] += t.get(src, 0)
            if "completion" in r:
                a["completion"] = bool(r["completion"]) if a["completion"] is None else a["completion"] and r["completion"]
            if RANK[r["verdict"]] > RANK[a["verdict"]]:
                a["verdict"] = r["verdict"]
    return agg


if __name__ == "__main__":
    sys.exit(main())
