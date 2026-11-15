#!/usr/bin/env python3
"""Tests of tools/perf/mem_res_reader_census.py (the bounded guest-reader probe's per-era census grader).
    python tools/perf/test_mem_res_reader_census.py
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mem_res_reader_census as R  # noqa: E402


def line(sec, text):
    return "[2026-10-17 05:00:%06.3f] [INFO ] [BOOT] %s\n" % (sec, text)


def rng(sec, phase, status, phys="0A000000", nbytes=4096):
    return line(sec, "[MEM_RES_CHECK] reader_range id=1 phase=%s phys=%s bytes=%d status=%s" % (
        phase, phys, nbytes, status))


BASE = [
    line(0.0, "[BOOT] start"),
    line(0.5, "[MEM_RES_CHECK] reader_enabled=1 exhaustive_readers=0"),
    line(2.0, "[PM4_BIND_CENSUS] last frame: 2D 0 binds"),
    line(4.0, "[APT_REQ] #3 path='screens/bootflow/ScrPressStart'"),
    line(6.0, "[APT_REQ] #9 path='screens/hud/main_hud'"),
]
CLEAN = dict(attempts=3, native_read_returned=3, completed_overlaps=1, overlap_unknown=0, events=9, events_capped=0,
             capacity=0, invalid=0, stream_errors=0, counter_overflow=0, failures=0)


def summary(phase="terminal", **over):
    kv = dict(CLEAN, **over)
    return line(9.0, "[MEM_RES_CHECK] reader_summary phase=%s %s" % (phase, " ".join("%s=%d" % x for x in kv.items())))


SUMMARY = summary()


def bulk(sec, phase, pages, listed, **unlisted):
    return line(sec, "[MEM_RES_CHECK] reader_range_bulk id=4 phase=%s phys=0B000000 bytes=%d pages=%d listed=%d%s" % (
        phase, pages * 4096, pages, listed, "".join(" u.%s=%d" % x for x in unlisted.items())))


def grade(*parts, **kw):
    fd, path = tempfile.mkstemp(suffix=".log")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write("".join(parts))
    try:
        return R.grade(path, **kw)
    finally:
        os.remove(path)


def era(r, name):
    return next((x for x in r["eras"] if x["era"] == name), None)


def expect(name, ok, r):
    print("%s %s -> %s %s" % ("PASS" if ok else "FAIL", name, r["verdict"],
                              [(x["era"], x["verdict"]) for x in r.get("eras", [])]))
    return ok


def main():
    res = []
    r = grade(*BASE, rng(7.0, "before_copy", "completed_prior_gpu_overlap"), SUMMARY)
    res.append(expect("T1 a copy read of resolve output in gameplay = READERS", r["verdict"] == "CENSUS" and
                      era(r, "gameplay")["verdict"] == "READERS" and era(r, "title")["verdict"] == "NONE" and
                      r["summary"] is not None and r["summary_present"] is True and
                      r["by_era"]["gameplay"] == {"verdict": "READERS", "copy_gpu": 1, "texlock_gpu": 0, "inval_gpu": 0,
                                                     "other_gpu": 0, "lockcore_gpu": 0, "gaps": 0}, r))
    r = grade(*BASE, rng(7.0, "before_texlock_cpu_note", "completed_prior_gpu_overlap"), SUMMARY)
    res.append(expect("T2 a texture lock over resolve output only = INTENT",
                      era(r, "gameplay")["verdict"] == "INTENT", r))
    r = grade(*BASE, rng(5.0, "before_texlock_cpu_note", "unknown_range_or_page_cap"),
              rng(7.0, "before_copy", "no_recorded_write_observed"), SUMMARY)
    res.append(expect("T3 a coverage gap in the title, a CPU-owned copy in gameplay = GAP / NONE",
                      era(r, "title")["verdict"] == "GAP" and era(r, "gameplay")["verdict"] == "NONE" and
                      era(r, "title")["gaps"] == {"unknown_range_or_page_cap": 1}, r))
    r = grade(*BASE, rng(7.0, "before_copy", "completed_prior_gpu_overlap"))
    res.append(expect("T4 no reader_summary: INCOMPLETE (a crashed or unquit run is no whole-run census)",
                      r["verdict"] == "INCOMPLETE" and r["summary"] is None and r["complete"] is False and
                      any("no reader_summary" in w for w in r["incomplete"]), r))
    r = grade(BASE[0], BASE[2])
    res.append(expect("T5 no probe lines = NO DATA", r["verdict"] == "NO DATA", r))
    # whole ranges (resproof): a long texture lock's unlisted pages come in one bulk line, by status
    r = grade(*BASE, bulk(5.0, "before_texlock_cpu_note", 297, 0, no_recorded_write_observed=296, unknown_witness=1),
              SUMMARY)
    t = era(r, "title")
    res.append(expect("T6 a bulk line's CPU pages and its gap page counted (title GAP, 296 cpu, 1 gap)",
                      r["verdict"] == "CENSUS" and t["verdict"] == "GAP" and t["counts"].get("texlock_cpu") == 296 and
                      t["counts"].get("texlock_gap") == 1 and t["gaps"] == {"unknown_witness": 1} and
                      r["gaps_total"] == 1 and r["bulk_ranges_total"] == 1, r))
    r = grade(*BASE, rng(7.0, "before_texlock_cpu_note", "completed_prior_gpu_overlap", phys="0B004000"),
              bulk(7.0, "before_texlock_cpu_note", 300, 1, no_recorded_write_observed=299), SUMMARY)
    g = era(r, "gameplay")
    res.append(expect("T7 a long lock with one GPU-overlap page (its own line) + 299 CPU pages = INTENT",
                      g["verdict"] == "INTENT" and g["counts"].get("texlock_gpu") == 1 and
                      g["counts"].get("texlock_cpu") == 299 and r["texlock_gpu_total"] == 1 and
                      r["copy_gpu_total"] == 0, r))
    r = grade(*BASE, bulk(5.0, "before_texlock_cpu_note", 120, 0, no_recorded_write_observed=100,
                          latest_partial_write_does_not_cover=20), SUMMARY)
    res.append(expect("T8 a bulk line of CPU-owned pages only = NONE, complete", r["verdict"] == "CENSUS" and
                      r["complete"] is True and era(r, "title")["verdict"] == "NONE" and
                      era(r, "title")["counts"].get("texlock_cpu") == 120 and r["gaps_total"] == 0, r))
    # overflow = FAIL, never silent
    r = grade(*BASE, summary(events_capped=7))
    res.append(expect("T9 events_capped > 0 = INCOMPLETE", r["verdict"] == "INCOMPLETE" and
                      "events_capped=7" in r["incomplete"], r))
    r = grade(*BASE, line(8.0, "[MEM_RES_CHECK] reader_events_capped event_cap=4194304 the census of this process is "
                               "INCOMPLETE"), SUMMARY)
    res.append(expect("T10 the probe's capped notice alone = INCOMPLETE (a summary claiming 0 cannot hide it)",
                      r["verdict"] == "INCOMPLETE" and r["capped_notice"] is True, r))
    r = grade(*BASE, summary(capacity=1))
    res.append(expect("T11 capacity > 0 = INCOMPLETE", r["verdict"] == "INCOMPLETE" and "capacity=1" in r["incomplete"],
                      r))
    r = grade(*BASE, summary(phase="fixture"))
    res.append(expect("T12 the last summary not terminal = INCOMPLETE", r["verdict"] == "INCOMPLETE", r))
    r = grade(*BASE, line(9.0, "[MEM_RES_CHECK] reader_summary phase=terminal attempts=0 events=3 events_capped=0 "
                               "failures=0"))
    res.append(expect("T13 a summary without the capacity / invalid / stream / overflow counters = INCOMPLETE",
                      r["verdict"] == "INCOMPLETE" and "reader_summary has no capacity" in r["incomplete"], r))
    r = grade(*BASE, rng(3.0, "before_copy", "completed_prior_gpu_overlap"),
              rng(7.0, "before_copy", "completed_prior_gpu_overlap"),
              rng(7.5, "before_texlock_cpu_note", "unknown_witness"), SUMMARY)
    res.append(expect("T14 the run totals sum every era (copy_gpu 2 over intro + gameplay, gaps 1)",
                      r["copy_gpu_total"] == 2 and r["gaps_total"] == 1 and r["texlock_gpu_total"] == 0, r))
    fd, path = tempfile.mkstemp(suffix=".log")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write("".join(BASE + [summary(events_capped=1)]))
    try:
        rc_bad = R.main([path])
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("".join(BASE + [SUMMARY]))
        rc_ok = R.main([path])
    finally:
        os.remove(path)
    res.append(expect("T15 exit 1 for INCOMPLETE, 0 for CENSUS", rc_bad == 1 and rc_ok == 0, {"verdict": "-"}))
    # the other CPU-note coherencies (resproof): a vertex-buffer lock / invalidate (0x03000000) or another status over
    # resolve output is an INTENT; their CPU-owned pages come folded in bulk lines (detail 0)
    r = grade(*BASE, rng(7.0, "before_inval_cpu_note", "completed_prior_gpu_overlap", phys="0C000000"),
              bulk(7.0, "before_inval_cpu_note", 3, 1, no_recorded_write_observed=2), SUMMARY)
    g = era(r, "gameplay")
    res.append(expect("T16 a vertex-buffer lock / invalidate over resolve output = INTENT (inval_gpu)",
                      g["verdict"] == "INTENT" and g["counts"].get("inval_gpu") == 1 and
                      g["counts"].get("inval_cpu") == 2 and r["inval_gpu_total"] == 1 and r["texlock_gpu_total"] == 0
                      and r["by_era"]["gameplay"]["inval_gpu"] == 1 and r["inval_pages_total"] == 3, r))
    r = grade(*BASE, rng(5.0, "before_other_cpu_note", "completed_prior_gpu_overlap"),
              bulk(7.0, "before_inval_cpu_note", 1, 0, no_recorded_write_observed=1), SUMMARY)
    res.append(expect("T17 another CPU-note status over resolve output = INTENT (other_gpu); a CPU-owned VB lock = NONE",
                      era(r, "title")["verdict"] == "INTENT" and r["other_gpu_total"] == 1 and
                      era(r, "gameplay")["verdict"] == "NONE" and era(r, "gameplay")["counts"].get("inval_cpu") == 1, r))
    r = grade(*BASE, bulk(7.0, "before_inval_cpu_note", 2, 0, unknown_witness=2), SUMMARY)
    res.append(expect("T18 an inval bulk line's gap pages are gaps (GAP, gaps_total 2)",
                      era(r, "gameplay")["verdict"] == "GAP" and r["gaps_total"] == 2, r))
    # the lock core after its body (resproof): a lock - a read-only one too - over resolve output is an INTENT
    r = grade(*BASE, rng(7.0, "after_lock_core", "completed_prior_gpu_overlap", phys="0D000000"),
              bulk(7.0, "after_lock_core", 4, 1, no_recorded_write_observed=3), SUMMARY)
    g = era(r, "gameplay")
    res.append(expect("T19 a lock-core lock over resolve output = INTENT (lockcore_gpu), its CPU pages counted",
                      g["verdict"] == "INTENT" and r["lockcore_gpu_total"] == 1 and g["counts"].get("lockcore_cpu") == 3
                      and r["lockcore_pages_total"] == 4 and r["by_era"]["gameplay"]["lockcore_gpu"] == 1, r))
    n = res.count(False)
    print("test_mem_res_reader_census: %d checks, %d failed" % (len(res), n))
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
