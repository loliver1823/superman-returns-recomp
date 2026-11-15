#!/usr/bin/env python3
"""mem_res_reader_mutants.py - the named mutants of the whole-run guest-reader census (lane D resproof follow-up,
2026-10-17: gfx/resource_reader_probe.hpp walkRange / kEvents / takeCapNotice and tools/perf/mem_res_reader_census.py's
completeness and bulk lines): each must make its test fail.

    python tools/difftest/mem_res_reader_mutants.py [--tree .] [--build-dir build] [--jobs 6] [--only R1,G2] [--no-build]

Build mutants (R*) break one rule of the probe; each is applied in place, the metadata test is rebuilt
(`cmake --build <build-dir> --target sr_guest_reader_probe_test`) and must exit non-zero. Grader mutants (G*) break
one rule of the census grader; tools/perf/test_mem_res_reader_census.py must exit non-zero (no build). Every mutant is
an exact text replacement that must match once (one that no longer applies is an ERROR, never a pass); the file is
restored byte for byte, and the test is rebuilt from the restored sources at the end (it must pass again). Exit code =
mutants that survived + errors. The application / restore helpers are mem_res_ab_mutants.py's.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mem_res_ab_mutants import apply, restore, sh  # noqa: E402

PH = "gfx/resource_reader_probe.hpp"
PC = "gfx/resource_reader_probe.cpp"
GR = "tools/perf/mem_res_reader_census.py"
OB = "kernel/resource_reader_observe.cpp"
# (id, file, old, new, what it breaks)
BUILD = [
    ("R1", PH, "    ++t.pages;\n", "    if (t.pages == kDetailPages) break;  // MUTANT R1\n    ++t.pages;\n",
     "the page cap back: a long range (a movie plane's lock) stops after 64 pages"),
    ("R2", PH, "    if (detail || result.status == Status::CompletedOverlap) {", "    if (detail) {  // MUTANT R2",
     "a long range's GPU-overlap pages are only counted, never listed (the finding loses its page and phase)"),
    ("R3", PH, "    } else {\n      ++t.unlisted[uint32_t(result.status)];\n    }",
     "    } else if (result.status != Status::UnknownWitness) {  // MUTANT R3\n"
     "      ++t.unlisted[uint32_t(result.status)];\n    }",
     "a long range's gap page dropped from the count (an unclassified page reads as covered)"),
    ("R4", PH, "  const bool detail = ((end - 1) >> 12) - (phys >> 12) + 1 <= detailPages;",
     "  const bool detail = ((end - 1) >> 12) - (phys >> 12) + 1 < detailPages;  // MUTANT R4",
     "the detail boundary off by one (a 64-page range loses its per-page lines)"),
    ("R5", PH, "constexpr uint32_t kDetailPages = 64, kEvents = 1u << 22;",
     "constexpr uint32_t kDetailPages = 64, kEvents = 65536;  // MUTANT R5",
     "the event cap back at 65,536 (the new-game roll filled it: events_capped 5,962 / 6,407)"),
    ("R8", PH, "  const bool detail = ((end - 1) >> 12) - (phys >> 12) + 1 <= detailPages;",
     "  const bool detail = ((end - 1) >> 12) - (phys >> 12) + 1 <= kDetailPages;  // MUTANT R8\n  (void)detailPages;",
     "the detail argument ignored (the vertex-buffer lock / invalidate sites log every CPU page: the log floods)"),
    ("R6", PC,"  if(!counts_.eventsCapped || capNotice_) return false;", "  return false;  // MUTANT R6",
     "the cap silent: no once-only notice that the census is incomplete"),
    ("R7", PC, "  capNotice_=true; return true;", "  return true;  // MUTANT R7",
     "the notice repeated on every refused event (one per line past the cap)"),
]
# hook mutants: difftest is rebuilt and `difftest --resource-reader-hooks-test` must exit non-zero
HOOKS = [
    ("R9", OB, "    if(address<0x80000000u || !bytes) return;", "    return;  // MUTANT R9",
     "the lock core's post-body grading counts the lock but grades no page"),
    ("R10", OB, "    if(address<0x80000000u || !bytes) return;", "    if(!bytes) return;  // MUTANT R10",
     "a virtual-address lock graded as if it were GPU memory"),
]
GRADER = [
    ("G1", GR, '("events_capped", "capacity",', '("capacity",',
     "events_capped > 0 accepted as complete (T9)"),
    ("G2", GR, "    if capped:\n        why.append(", "    if False:\n        why.append(",
     "the probe's capped notice ignored (T10)"),
    ("G3", GR, "        why.append(\"no reader_summary (the graceful quit's terminal family did not print)\")",
     "        pass  # MUTANT G3", "a run without the terminal reader_summary accepted (T4)"),
    ("G4", GR, "        if kind == \"bulk\":  # a long range's unlisted pages",
     "        if kind == \"bulk\":  # MUTANT G4\n            continue\n        if kind == \"bulk\":  # a long range's unlisted pages",
     "a bulk line's pages not counted (T6 / T8)"),
    ("G5", GR, "                if cls == \"gap\":\n                    gaps[st] += n",
     "                if False:\n                    gaps[st] += n", "a bulk line's gap pages not counted as gaps (T6)"),
    ("G6", GR, "        if ph != \"terminal\":", "        if False:  # MUTANT G6", "a non-terminal summary accepted (T12)"),
    ("G7", GR, "                why.append(\"reader_summary has no %s\" % k)", "                pass  # MUTANT G7",
     "a summary missing a must-be-zero counter accepted (T13)"),
    ("G8", GR, 'copy_gpu_total=tot("copy_gpu")', 'copy_gpu_total=0',
     "the run's copy-read total hard-wired to 0 (T14)"),
    ("G9", GR, 'INTENT_SITES = ("texlock", "inval", "other", "lockcore")', 'INTENT_SITES = ("texlock",)  # MUTANT G9',
     "a vertex-buffer lock / invalidate / other coherency / lock-core lock over resolve output not an INTENT (T16-T19)"),
    ("G11", GR, ', "after_lock_core": "lockcore"}', '}', "the lock core's post-body phase not mapped to its site (T19)"),
    ("G10", GR, '"before_inval_cpu_note": "inval", ', '',
     "the vertex-buffer lock / invalidate phase not mapped to its site (T16)"),
]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=".")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--only", default="")
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args(argv)
    tree = os.path.abspath(a.tree)
    only = {x for x in a.only.split(",") if x}
    rows, bad = [], 0
    grader = [sys.executable, "-I", "-X", "utf8", os.path.join(tree, "tools", "perf", "test_mem_res_reader_census.py")]
    rc, _out = sh(grader, tree)
    rows.append(("clean", "test_mem_res_reader_census.py", "exit %d" % rc, "PASS" if rc == 0 else "ERROR"))
    bad += rc != 0
    for mid, rel, old, new, what in GRADER:
        if only and mid not in only:
            continue
        raw, err = apply(tree, rel, old, new)
        if err:
            rows.append((mid, what, err, "ERROR"))
            bad += 1
            continue
        try:
            rc, out = sh(grader, tree)
        finally:
            restore(tree, rel, raw)
        fails = [ln for ln in out.splitlines() if ln.startswith("FAIL")]
        rows.append((mid, what, "exit %d: %s" % (rc, (fails or out.strip().splitlines() or ["-"])[0][:110]),
                     "KILLED" if rc != 0 else "SURVIVED"))
        bad += rc == 0
    if not a.no_build:
        name = "sr_guest_reader_probe_test"
        exe = os.path.join(tree, a.build_dir, name + (".exe" if os.name == "nt" else ""))
        build = ["cmake", "--build", a.build_dir, "--target", name, "-j", str(a.jobs)]
        brc, bout = sh(build, tree)
        rc, out = sh([exe], tree, 600) if brc == 0 else (brc, bout[-400:])
        rows.append(("clean", name, "build %d, test exit %d: %s" % (brc, rc, (out.strip().splitlines() or ["-"])[-1][:90]),
                     "PASS" if brc == 0 and rc == 0 else "ERROR"))
        bad += not (brc == 0 and rc == 0)
        for mid, rel, old, new, what in BUILD:
            if only and mid not in only:
                continue
            raw, err = apply(tree, rel, old, new)
            if err:
                rows.append((mid, what, err, "ERROR"))
                bad += 1
                continue
            try:
                brc, bout = sh(build, tree)
                if brc != 0:
                    rows.append((mid, what, "the mutant did not build (exit %d): %s" % (
                        brc, " ".join(bout.strip().splitlines()[-2:])[:160]), "ERROR"))
                    bad += 1
                    continue
                rc, out = sh([exe], tree, 600)
            finally:
                restore(tree, rel, raw)
            tail = (out.strip().splitlines() or ["-"])[-1][:110]
            rows.append((mid, what, "exit %d: %s" % (rc, tail), "KILLED" if rc != 0 else "SURVIVED"))
            bad += rc == 0
        brc, _ = sh(build, tree)
        rc, out = sh([exe], tree, 600) if brc == 0 else (brc, "")
        rows.append(("restored", name + " rebuilt from the restored sources", "build %d, test exit %d" % (brc, rc),
                     "PASS" if brc == 0 and rc == 0 else "ERROR"))
        bad += not (brc == 0 and rc == 0)
        dexe = os.path.join(tree, a.build_dir, "difftest.exe" if os.name == "nt" else "difftest")
        dbuild = ["cmake", "--build", a.build_dir, "--target", "difftest", "-j", str(a.jobs)]
        hooks = [dexe, "--resource-reader-hooks-test"]
        brc, bout = sh(dbuild, tree)
        rc, out = sh(hooks, tree, 600) if brc == 0 else (brc, bout[-400:])
        rows.append(("clean", "difftest --resource-reader-hooks-test", "build %d, test exit %d" % (brc, rc),
                     "PASS" if brc == 0 and rc == 0 else "ERROR"))
        bad += not (brc == 0 and rc == 0)
        for mid, rel, old, new, what in HOOKS:
            if only and mid not in only:
                continue
            raw, err = apply(tree, rel, old, new)
            if err:
                rows.append((mid, what, err, "ERROR"))
                bad += 1
                continue
            try:
                brc, bout = sh(dbuild, tree)
                if brc != 0:
                    rows.append((mid, what, "the mutant did not build (exit %d): %s" % (
                        brc, " ".join(bout.strip().splitlines()[-2:])[:160]), "ERROR"))
                    bad += 1
                    continue
                rc, out = sh(hooks, tree, 600)
            finally:
                restore(tree, rel, raw)
            fails = [ln for ln in out.splitlines() if "FAILED" in ln]
            rows.append((mid, what, "exit %d: %s" % (rc, (fails or out.strip().splitlines() or ["-"])[0][:110]),
                         "KILLED" if rc != 0 else "SURVIVED"))
            bad += rc == 0
        brc, _ = sh(dbuild, tree)
        rc, out = sh(hooks, tree, 600) if brc == 0 else (brc, "")
        rows.append(("restored", "difftest rebuilt from the restored sources", "build %d, hooks test exit %d" % (brc, rc),
                     "PASS" if brc == 0 and rc == 0 else "ERROR"))
        bad += not (brc == 0 and rc == 0)
    print("%-9s %-9s %s" % ("mutant", "verdict", "what it breaks / the evidence"))
    for mid, what, ev, verdict in rows:
        print("%-9s %-9s %s\n%20s %s" % (mid, verdict, what, "", ev))
    print("guest-reader census mutants: %s" % ("every mutant killed" if not bad else "%d survived or errored" % bad))
    return bad


if __name__ == "__main__":
    sys.exit(main())
