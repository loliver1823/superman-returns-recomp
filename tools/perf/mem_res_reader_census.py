#!/usr/bin/env python3
"""mem_res_reader_census.py - grade the guest-reader probe per era (mem.resources residual unknown
`resolve_writeback_readers`; Codex's gfx/resource_reader_probe + kernel/resource_reader_observe, committed on
release/emu-stage1 8874a034, whole-run since lane D's resproof 2026-10-17; SR_GUEST_READER_PROBE=1 with
SR_MEM_RESOURCES=1, dev / Oracle build with the regenerated midasm hooks).

    python tools/perf/mem_res_reader_census.py <run.log> [--label ERA [--entry REGEX|start]] [--json out.json]

THE PROBE WATCHES SITES (its own line says exhaustive_readers=0: no read trap on every guest load). It observes, per
page, only:
  copy        TextureCopySurfaceRows_sub_823A10F0's row memcpy (hooks 823A1120 / 823A1194 / 823A1198): an ACTUAL guest CPU
              read of a surface (actual_read=1 when the native memcpy ran the armed copy), each source page graded against
              the last recorded GPU write (reader_range phase=before_copy / after_copy status=...);
  texlock     the texture-lock coherency request 0x02000000 before its CPU note (reader_range
              phase=before_texlock_cpu_note): the guest opening a texture for CPU access over pages a resolve may have
              written - an access INTENT (actual_read=0), a reader or a writer;
  inval       (resproof) every other coherency the CP notes as a CPU write, 0x03000000 (phase
              before_inval_cpu_note): D3DVertexBuffer_Lock's status and GpuInvalidateMemoryRange's - a vertex-buffer lock
              (a possible reader of a resolve-to-VB output) or an announced CPU write - an INTENT as texlock;
  other       (resproof) any further CPU-note status except the command buffer's 0x01000000 (before_other_cpu_note);
              inval / other log their CPU-owned pages folded into one reader_range_bulk line per range;
  lockcore    (resproof) every lock of the lock core 820F3988 graded AFTER its body (the fence wait done, the pointer
              handed out; phase after_lock_core): vertex, index and texture buffers, READ-ONLY locks included (flags
              0x10 / 0x02 emit no coherency packet, so no coherency site sees them) - an INTENT as texlock;
  lock        reader_lock (kind=texture: sub_820FFCB0; kind=surface: 823A1120) and reader_lock_intent (the lock core
              820F3988): possible accesses, actual_read=0.
Per page status (gfx/resource_reader_probe.cpp name()): completed_prior_gpu_overlap (the page holds resolve output
whose GPU work completed before the access) is the finding; no_recorded_write_observed / latest_partial_write_does_not_
cover are CPU-owned pages; unknown_* / capacity / changed_owner / unsubmitted / ... are coverage gaps.
WHOLE RANGES (resproof): every page of a range is graded (the old 64-page cap is gone: unknown_range_or_page_cap was
the movie planes' texture locks). A range over 64 pages logs a reader_range line per GPU-overlap page and one
reader_range_bulk line counting its other pages by status (u.<status>=pages), which this grader adds to the same tallies.
COMPLETENESS (resproof: overflow = FAIL, never silent): the run's census is complete only when the graceful quit's
reader_summary printed (phase terminal) with events_capped / capacity / counter_overflow / failures / invalid /
stream_errors all 0 and no reader_events_capped line (the probe's once-only notice that its event cap refused a line);
else the verdict is INCOMPLETE with the reasons in `incomplete`.

Per era (tools/perf/mem_res_eras.py's ERAS, from the per-event lines' timestamps): the copy reads of resolve output
(copy_gpu = phase before_copy|after_copy with completed_prior_gpu_overlap), the texlock intents over resolve output
(texlock_gpu), the coverage gaps by status, the attempts. Verdict per era:
  READERS  copy_gpu > 0: a guest CPU reader of resolve output exists (the native route must copy the resolve's bytes
           into guest RAM before it: RELEASE_COMPOSITION 9.5 step 3's ensureVisible);
  INTENT   texlock_gpu / inval_gpu / other_gpu / lockcore_gpu > 0 only: the guest locked (or announced a CPU write over) resolve output
           (a reader or a writer; the lock is the write-back point if it reads);
  GAP      a coverage gap (unknown status, range/page cap, capped events) and neither of the above;
  NONE     the probe saw no access to resolve output (bounded: only its sites);
and per run: the verdict CENSUS (complete) / INCOMPLETE / NO DATA, the totals over every era (copy_gpu_total,
texlock_gpu_total, inval_gpu_total, other_gpu_total, lockcore_gpu_total, gaps_total, copy_actual_total) that a ledger item reads. Exit 0 for CENSUS, 1 for INCOMPLETE, 2 for
no probe lines.
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mem_res_eras as E  # noqa: E402

RANGE = re.compile(r"\[MEM_RES_CHECK\] reader_range id=\d+ phase=(\w+)(?: phys=([0-9A-F]+) bytes=(\d+))? status=(\w+)")
BULK = re.compile(r"\[MEM_RES_CHECK\] reader_range_bulk id=\d+ phase=(\w+) phys=([0-9A-F]+) bytes=(\d+) pages=(\d+) "
                  r"listed=(\d+)(.*)")
UNLISTED = re.compile(r"\bu\.(\w+)=(\d+)")
CAPPED = re.compile(r"\[MEM_RES_CHECK\] reader_events_capped ")
LOCK = re.compile(r"\[MEM_RES_CHECK\] reader_lock id=\d+ kind=(\w+)")
INTENT = re.compile(r"\[MEM_RES_CHECK\] reader_lock_intent ")
ATTEMPT = re.compile(r"\[MEM_RES_CHECK\] reader_copy_attempt ")
RETURN = re.compile(r"\[MEM_RES_CHECK\] reader_copy_return .*native_read=(\d) actual_read=(\d)")
SUMMARY = re.compile(r"\[MEM_RES_CHECK\] reader_summary phase=(\w+) (.*)")
ENABLED = re.compile(r"\[MEM_RES_CHECK\] reader_enabled=1 ")
GOOD = {"completed_prior_gpu_overlap"}
# the probe's phases -> the census sites (the copy reader, the texture lock, the other CPU-note coherencies)
SITES = {"before_copy": "copy", "after_copy": "copy", "before_texlock_cpu_note": "texlock",
         "before_inval_cpu_note": "inval", "before_other_cpu_note": "other", "after_lock_core": "lockcore"}
INTENT_SITES = ("texlock", "inval", "other", "lockcore")
CPU_OWNED = {"no_recorded_write_observed", "latest_partial_write_does_not_cover"}


def collect(path):
    events = []  # (t, kind, phase, status, phys, bytes); a bulk line: status = {status: unlisted pages}
    summary = []
    enabled = [False]
    capped = [False]

    def hook(t, line):
        if "[MEM_RES_CHECK] reader" not in line:
            return
        if ENABLED.search(line):
            enabled[0] = True
            return
        m = BULK.search(line)
        if m:
            events.append((t, "bulk", m.group(1), {k: int(v) for k, v in UNLISTED.findall(m.group(6))}, m.group(2),
                           int(m.group(3))))
            return
        if CAPPED.search(line):
            capped[0] = True
            return
        m = RANGE.search(line)
        if m:
            events.append((t, "range", m.group(1), m.group(4), m.group(2), int(m.group(3) or 0)))
            return
        m = LOCK.search(line)
        if m:
            events.append((t, "lock_" + m.group(1), None, None, None, 0))
            return
        if INTENT.search(line):
            events.append((t, "lock_intent", None, None, None, 0))
            return
        if ATTEMPT.search(line):
            events.append((t, "attempt", None, None, None, 0))
            return
        m = RETURN.search(line)
        if m:
            events.append((t, "return_actual" if m.group(2) == "1" else "return_unknown", None, None, None, 0))
            return
        m = SUMMARY.search(line)
        if m:
            summary.append((m.group(1), {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", m.group(2))}))

    run = E.parse(path, re.compile(r"\[MEM_RES_CHECK\] reader_summary phase=(\w+)"), line_hook=hook)
    return run, events, summary, enabled[0], capped[0]


def tally(evs):
    c = Counter()
    gaps = Counter()
    gpu_pages = defaultdict(int)
    for t, kind, phase, status, phys, nbytes in evs:
        if kind not in ("range", "bulk"):
            c[kind] += 1
            continue
        site = SITES.get(phase, phase)
        if kind == "bulk":  # a long range's unlisted pages, by status (its GPU-overlap pages have their own lines)
            c[site + "_bulk"] += 1
            for st, n in status.items():
                cls = "gpu" if st in GOOD else "cpu" if st in CPU_OWNED else "gap"
                c["%s_%s" % (site, cls)] += n
                if cls == "gap":
                    gaps[st] += n
            continue
        if status in GOOD:
            c[site + "_gpu"] += 1
            if phys:
                gpu_pages["%s:%s" % (site, phys)] += 1
        elif status in CPU_OWNED:
            c[site + "_cpu"] += 1
        else:
            c[site + "_gap"] += 1
            gaps[status] += 1
    return c, gaps, gpu_pages


# the reader_summary counters a complete whole-run census needs at 0 (gfx/resource_reader_probe.hpp Counts)
MUST_BE_ZERO = ("events_capped", "capacity", "counter_overflow", "failures", "invalid", "stream_errors")


def completeness(summary, capped):
    """The reasons the run's census is NOT complete (empty = complete)."""
    why = []
    if not summary:
        why.append("no reader_summary (the graceful quit's terminal family did not print)")
    else:
        ph, kv = summary
        if ph != "terminal":
            why.append("the last reader_summary is phase %s, not terminal" % ph)
        for k in MUST_BE_ZERO:
            if kv.get(k) is None:
                why.append("reader_summary has no %s" % k)
            elif kv[k]:
                why.append("%s=%d" % (k, kv[k]))
    if capped:
        why.append("reader_events_capped: the probe's event cap refused a line")
    return why


def grade(path, label=None, entry=None):
    run, events, summary, enabled, capped = collect(path)
    out = {"log": path, "enabled": enabled, "eras": [], "summary": summary[-1] if summary else None,
           "quit_failed": run["quit_failed"], "capped_notice": capped}
    if not enabled and not events:
        out["verdict"] = "NO DATA"
        out["why"] = "no reader probe lines (SR_GUEST_READER_PROBE=1 + SR_MEM_RESOURCES=1, an exe with the hooks)"
        return out
    for era, t0, t1, problem in E.spans(run, label, entry):
        evs = [e for e in events if e[0] >= t0 and (t1 is None or e[0] < t1)]
        c, gaps, gpu_pages = tally(evs)
        if c["copy_gpu"]:
            v = "READERS"
        elif any(c[s + "_gpu"] for s in INTENT_SITES):
            v = "INTENT"
        elif gaps:
            v = "GAP"
        else:
            v = "NONE"
        out["eras"].append({"era": era, "verdict": v, "counts": dict(c), "gaps": dict(gaps),
                            "gpu_pages_top": sorted(gpu_pages.items(), key=lambda kv: -kv[1])[:12],
                            "problem": problem})
    out["events_capped"] = out["summary"][1].get("events_capped", 0) if out["summary"] else None
    out["summary_present"] = bool(out["summary"])
    # the keyed form a ledger json item reads (by_era.<era>.verdict / .copy_gpu / .texlock_gpu / .gaps)
    out["by_era"] = {row["era"]: {"verdict": row["verdict"], "copy_gpu": row["counts"].get("copy_gpu", 0),
                                  "texlock_gpu": row["counts"].get("texlock_gpu", 0),
                                  "inval_gpu": row["counts"].get("inval_gpu", 0),
                                  "other_gpu": row["counts"].get("other_gpu", 0),
                                  "lockcore_gpu": row["counts"].get("lockcore_gpu", 0),
                                  "gaps": sum(row["gaps"].values())} for row in out["eras"]}

    def tot(k):
        return sum(row["counts"].get(k, 0) for row in out["eras"])
    out.update(copy_gpu_total=tot("copy_gpu"), texlock_gpu_total=tot("texlock_gpu"),
               inval_gpu_total=tot("inval_gpu"), other_gpu_total=tot("other_gpu"),
               lockcore_gpu_total=tot("lockcore_gpu"),
               lockcore_pages_total=tot("lockcore_cpu") + tot("lockcore_gpu") + tot("lockcore_gap"),
               gaps_total=sum(sum(row["gaps"].values()) for row in out["eras"]), copy_actual_total=tot("return_actual"),
               bulk_ranges_total=sum(tot(s + "_bulk") for s in ("copy",) + INTENT_SITES),
               inval_pages_total=tot("inval_cpu") + tot("inval_gpu") + tot("inval_gap"))
    out["incomplete"] = completeness(out["summary"], capped)
    out["complete"] = not out["incomplete"]
    out["verdict"] = "CENSUS" if out["complete"] else "INCOMPLETE"
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("--label")
    ap.add_argument("--entry")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    r = grade(a.log, a.label, a.entry)
    print("MEM_RES_READER %s (site probe, whole ranges: exhaustive_readers=0)%s" % (
        r["verdict"], "" if r.get("summary") else " | no reader_summary (the graceful quit's snapshot missing)"))
    if r.get("why"):
        print("  " + r["why"])
    for why in r.get("incomplete") or []:
        print("  INCOMPLETE: " + why)
    if "copy_gpu_total" in r:
        print("  totals: copy_gpu=%d texlock_gpu=%d inval_gpu=%d other_gpu=%d lockcore_gpu=%d gaps=%d copy_actual=%d "
              "bulk_ranges=%d inval_pages=%d lockcore_pages=%d" % (
                  r["copy_gpu_total"], r["texlock_gpu_total"], r["inval_gpu_total"], r["other_gpu_total"],
                  r["lockcore_gpu_total"], r["gaps_total"], r["copy_actual_total"], r["bulk_ranges_total"],
                  r["inval_pages_total"], r["lockcore_pages_total"]))
    for row in r["eras"]:
        c = row["counts"]
        print("  %-9s %-7s copy: attempts=%d actual=%d gpu=%d cpu=%d gap=%d | texlock: gpu=%d cpu=%d gap=%d | inval: "
              "gpu=%d cpu=%d gap=%d | other: gpu=%d cpu=%d gap=%d | lockcore: gpu=%d cpu=%d gap=%d | locks texture=%d surface=%d "
              "intent=%d%s" % (
                  row["era"], row["verdict"], c.get("attempt", 0), c.get("return_actual", 0), c.get("copy_gpu", 0),
                  c.get("copy_cpu", 0), c.get("copy_gap", 0), c.get("texlock_gpu", 0), c.get("texlock_cpu", 0),
                  c.get("texlock_gap", 0), c.get("inval_gpu", 0), c.get("inval_cpu", 0), c.get("inval_gap", 0),
                  c.get("other_gpu", 0), c.get("other_cpu", 0), c.get("other_gap", 0),
                  c.get("lockcore_gpu", 0), c.get("lockcore_cpu", 0), c.get("lockcore_gap", 0),
                  c.get("lock_texture", 0), c.get("lock_surface", 0), c.get("lock_intent", 0),
                  (" | gaps " + json.dumps(row["gaps"])) if row["gaps"] else ""))
    if r.get("summary"):
        ph, kv = r["summary"]
        print("  reader_summary %s: attempts=%s native_read_returned=%s completed_overlaps=%s overlap_unknown=%s "
              "events=%s events_capped=%s failures=%s" % (ph, kv.get("attempts"), kv.get("native_read_returned"),
                                                          kv.get("completed_overlaps"), kv.get("overlap_unknown"),
                                                          kv.get("events"), kv.get("events_capped"), kv.get("failures")))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(r, fh, indent=1, sort_keys=True)
    return {"NO DATA": 2, "INCOMPLETE": 1}.get(r["verdict"], 0)


if __name__ == "__main__":
    sys.exit(main())
