#!/usr/bin/env python3
"""mem_res_ab_mutants.py - the named mutants of mem.resources' frozen-pass A/B (gfx/resource_ab.hpp, lane D): each
must make its test fail.

    python tools/difftest/mem_res_ab_mutants.py [--tree .] [--build-dir build] [--jobs 6] [--only AB1,G3] [--no-build]

Build mutants (AB*) break one rule of the dual mode (the toggle, INV-M, INV-R, the per-arm stamps, the executed-vs-
pending clobber, the re-establish hash, the empty drains / presents, the dual rebaseline, the model arm's drain note);
each is applied in place, difftest is rebuilt (`cmake --build <build-dir> --target difftest`) and
`difftest --resource-memory-d3d12-test --ab` (the WARP fixture's A/B cases) must exit non-zero. Grader mutants (G*)
break one rule of tools/perf/mem_res_ab_check.py; tools/perf/test_mem_res_ab_check.py must exit non-zero (no build).
Every mutant is an exact text replacement that must match once (a mutant that no longer applies is an ERROR, never a
pass); the file is restored byte for byte afterwards, and difftest is rebuilt from the restored sources at the end (the
clean --ab suite must pass again). --no-build runs the grader mutants only. Exit code = mutants that survived + errors.
"""
import argparse
import os
import subprocess
import sys

AB = "gfx/resource_ab.cpp"
SH = "gfx/d3d12_pm4_shmem.cpp"
BE = "gfx/d3d12_backend.cpp"
GR = "tools/perf/mem_res_ab_check.py"
# (id, file, old, new, what it breaks)
BUILD = [
    ("AB1", AB, "  if (a >= 0) return a != 0;", "  if (a >= 0) return true;  // MUTANT AB1",
     "a no-op toggle: every latched set decides natively (the witness' native_in_model)"),
    ("AB2", AB, "  if (a >= 0) return a != 0;", "  if (a >= 0) return false;  // MUTANT AB2",
     "an always-model toggle (model_in_native)"),
    ("AB3", SH, "  m_shmHash8[page] = m_shmHashA[page] = kAbForgotHash;",
     "  return;  // MUTANT AB3\n  m_shmHash8[page] = m_shmHashA[page] = kAbForgotHash;",
     "INV-M dropped: a native upload of other bytes leaves the model's hashes (the ABA case keeps stale bytes)"),
    ("AB4", SH, "    memory.forgetUpload(page);", "    (void)memory;  // MUTANT AB4",
     "INV-R's window revoke dropped: the registry keeps the model's 0xA upload as its own"),
    ("AB5", SH,
     "  if (memres::resourceAbDual()) {  // the frozen-pass A/B (gfx/resource_ab.hpp STAMPS): the native arm's own stamps",
     "  if (false) {  // MUTANT AB5",
     "shared stamps (Astra review1 P1-1): a later native set's stamp satisfies an earlier model set's render request"),
    ("AB6", SH, "    memres::resourceAbCount(executed ? memres::AbEvent::kCrossClobber : memres::AbEvent::kModelPreserve);",
     "    memres::resourceAbCount(memres::AbEvent::kCrossClobber);  // MUTANT AB6",
     "pending GPU data counted as clobbered (Astra review1 P1-2)"),
    ("AB7", SH,
     "    reestablish = src && page < m_abForgotHash.size() && ShaderCache::hashUcode(src, 4096) == m_abForgotHash[page];",
     "    (void)src;  // MUTANT AB7",
     "a re-establish with other bytes is no repair (Astra review1 P2-5)"),
    ("AB8", BE,
     "      if (!consumed && memres::resourceAbDual() && (memres::resourceAbArm() < 0 || !be->m_pm4DrainOpen)) {",
     "      if (false) {  // MUTANT AB8",
     "an empty drain before the first set consults the arm (Astra review1 P2-6: unlatched)"),
    ("AB9", AB,
     "  if (!t_setBegun) {  // no drain began a set since the last present: this present is a set of its own",
     "  if (false) {  // MUTANT AB9",
     "an empty present reuses the previous set's arm (Astra review1 P2-6: the alternation breaks)"),
    ("AB10", SH, "    if (!memres::resourceAbDual()) return;\n  }\n#if SR_ORACLE\n  if (memres::census()) memres::noteGpuWrite",
     "    return;  // MUTANT AB10\n  }\n#if SR_ORACLE\n  if (memres::census()) memres::noteGpuWrite",
     "the dual rebaseline dropped: the model forgets a native set's resolve (it uploads CPU bytes over GPU data)"),
    ("AB11", SH, "    memres::resourceMemory().noteGpuAt(start, bytes, batch.resourceGpuGeneration);",
     "    (void)start;  // MUTANT AB11",
     "the model arm's drain note dropped: the registry never hears a model set's resolve"),
    ("AB12", SH, "      m_abClob[p] = 1u;", "      (void)p;  // MUTANT AB12",
     "an executed-write clobber never recorded: cross_clobber blind"),
    ("AB13", SH, "        ++*cpuAfter;", "        (void)cpuAfter;  // MUTANT AB13",
     "the texture-identity census blind to a CPU write after the GPU write (cpu_after_gpu)"),
]
GRADER = [
    ("G1", GR, "        missing = [f for f in FIELDS if f not in t]", "        missing = []  # MUTANT G1",
     "an incomplete totals line accepted (T15)"),
    ("G2", GR, "    checks[\"terminal\"] = bool(term)\n    if not term:", "    checks[\"terminal\"] = bool(term)\n    if False:",
     "no terminal totals accepted (T13)"),
    ("G3", GR, "    same = [(f, a, b) for f, a, b, _k in pairs if arm[a] == arm[b]]", "    same = []  # MUTANT G3",
     "a compared pair from one decider accepted (T5, T17)"),
    ("G4", GR, "            if arm[c0] != arm[c1]:", "            if False:  # MUTANT G4",
     "a cross-arm control accepted (T19)"),
    ("G5", GR, "        if p in seen:\n            dup.append(p)", "        if False:\n            dup.append(p)",
     "a present dumped twice accepted (T16)"),
    ("G6", GR, "    if len(static) < min_pairs:", "    if False:  # MUTANT G6", "too few static pairs accepted (T11)"),
    ("G7", GR, "    if g(\"decisions.native_in_model\") or g(\"decisions.model_in_native\"):", "    if False:  # MUTANT G7",
     "wrong-arm decisions accepted (T2, T3)"),
    ("G8", GR, "    if g(\"inv_r.cross_clobber\"):", "    if False:  # MUTANT G8", "a cross-clobber accepted (T9)"),
    ("G9", GR, "    if g(\"dumped.alt_break\"):", "    if False:  # MUTANT G9", "the render thread's alt_break accepted (T12)"),
]


def sh(cmd, cwd, timeout=1800):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def apply(tree, rel, old, new):
    path = os.path.join(tree, rel)
    with open(path, "rb") as fh:
        raw = fh.read()
    text = raw.decode("utf-8")
    o, n = old, new
    if b"\r\n" in raw:  # the tree may be checked out with CRLF line ends
        o, n = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
    hits = text.count(o)
    if hits != 1:
        return None, "the mutant's text matches %d times in %s" % (hits, rel)
    with open(path, "wb") as fh:
        fh.write(text.replace(o, n).encode("utf-8"))
    return raw, None


def restore(tree, rel, raw):
    with open(os.path.join(tree, rel), "wb") as fh:
        fh.write(raw)


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
    grader = [sys.executable, "-I", "-X", "utf8", os.path.join(tree, "tools", "perf", "test_mem_res_ab_check.py")]
    rc, _out = sh(grader, tree)
    rows.append(("clean", "test_mem_res_ab_check.py", "exit %d" % rc, "PASS" if rc == 0 else "ERROR"))
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
        last = (out.strip().splitlines() or ["-"])[-1][:110]
        rows.append((mid, what, "exit %d: %s" % (rc, last), "KILLED" if rc != 0 else "SURVIVED"))
        bad += rc == 0
    if not a.no_build:
        exe = os.path.join(tree, a.build_dir, "difftest.exe" if os.name == "nt" else "difftest")
        build = ["cmake", "--build", a.build_dir, "--target", "difftest", "-j", str(a.jobs)]
        suite = [exe, "--resource-memory-d3d12-test", "--ab"]
        brc, bout = sh(build, tree)
        rc, out = sh(suite, tree, 600) if brc == 0 else (brc, bout[-400:])
        rows.append(("clean", "difftest --resource-memory-d3d12-test --ab", "build %d, suite exit %d" % (brc, rc),
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
                rc, out = sh(suite, tree, 600)
            finally:
                restore(tree, rel, raw)
            fails = [ln for ln in out.splitlines() if "FAIL" in ln and "[RESOURCE_D3D12_TEST]" in ln]
            tail = fails[0][:110] if fails else (out.strip().splitlines() or ["-"])[-1][:110]
            rows.append((mid, what, "exit %d, %d failing lines: %s" % (rc, len(fails), tail),
                         "KILLED" if rc != 0 else "SURVIVED"))
            bad += rc == 0
        brc, _ = sh(build, tree)
        rc, out = sh(suite, tree, 600) if brc == 0 else (brc, "")
        rows.append(("restored", "difftest rebuilt from the restored sources", "build %d, --ab exit %d" % (brc, rc),
                     "PASS" if brc == 0 and rc == 0 else "ERROR"))
        bad += not (brc == 0 and rc == 0)
    print("%-9s %-9s %s" % ("mutant", "verdict", "what it breaks / the evidence"))
    for mid, what, ev, verdict in rows:
        print("%-9s %-9s %s\n%20s %s" % (mid, verdict, what, "", ev))
    print("mem.resources A/B mutants: %s" % ("every mutant killed" if not bad else "%d survived or errored" % bad))
    return bad


if __name__ == "__main__":
    sys.exit(main())
