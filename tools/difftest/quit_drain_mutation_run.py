#!/usr/bin/env python3
"""quit_drain_mutation_run.py - compiled mutants of the stranded-wait rules (native/quit-drain: the fence wait;
native/quit-ringspace: the ring-space wait) against difftest --quit-test.

    python tools/difftest/quit_drain_mutation_run.py --out archive/qd/mutants [--build-dir build] [-j 12] [--check]
           [--only name,name]   # run just these mutants (every line is still checked once; the restore still runs)

The graceful quit's STRANDED state (kernel/quit.cpp strandedWait, recordDroppedFence / recordDroppedProgress /
ibTailFence, the settle's overflow and verify; kernel/quit.hpp's wait marks, spin site (gpuSpinSite), spin phase and
ring-space test (ringSpaceFree); kernel/gpu_hang_wd.cpp's strong-defs, the
GpuRingWaitProgress pass-through among them; the drop sites in kernel/gpu_ringless.cpp and kernel/native_flush.cpp;
docs/TOOLKIT.md 1.8m step 5). Each mutant edits ONE production line (exactly one occurrence in its source, else the
run stops), rebuilds the difftest target, runs the suite and must make it FAIL (a non-zero exit with at least one named
FAIL line); then the bytes are restored, the target rebuilt and the suite must pass (exit 0). Restoration runs in a
finally even when a build or run fails. Run it on the build worker, never on the local PC; --check only verifies that
every mutated line occurs exactly once (no build). Writes <out>/mutants.json and per-step logs.
Exit 0 = every mutant failed the suite and the restored build passed it; 1 otherwise; 2 = a setup error.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

QUIT = "kernel/quit.cpp"
HPP = "kernel/quit.hpp"
WD = "kernel/gpu_hang_wd.cpp"
RL = "kernel/gpu_ringless.cpp"
NF = "kernel/native_flush.cpp"
# (name, the production line's text, its mutated form, what it breaks, the source) - round 2 (Astra round 1)
MUTANTS = [
    # strandedWait: each condition dropped alone (the F case breaking that one condition then fails)
    ("cp_parked_ignored",
     '  if (!g_cpStopped.load(std::memory_order_acquire)) return no("the CP drain is not over");',
     '  if (false) return no("the CP drain is not over");',
     "the CP drain not over, yet stranded (F6, the real wait)", QUIT),
    ("readptr_wrap_stranded",
     "  if (m.kind != kGpuWaitFence && m.kind != kGpuWaitRingSpace) return false;  // ReadPtrWrap (or none): no rule",
     "  if (m.kind == kGpuWaitNone) return false;  // mutant",
     "ringspace: GpuRingWaitReadPtrWrap (no stranded rule) read with a rule's conditions (F11)", QUIT),
    ("phase_pre_ignored",
     "  if (m.phase == kGpuPhasePre)",
     "  if (false)",
     "P2: the PRE phase (the early return, the flush; the ring wait's space test) no longer named as not in the spin "
     "loop (F15, RS1)", QUIT),
    ("phase_exited_ignored",
     '  if (m.phase != kGpuPhaseSpin) return no("its spin loop exited (GpuRingWaitProgress returned 0)");',
     '  if (false) return no("its spin loop exited (GpuRingWaitProgress returned 0)");',
     "P2: an EXITED spin (GpuRingWaitProgress returned 0) read as the loop (F16)", QUIT),
    ("same_wait_ignored",
     '    return no("the wait or its spin changed during the window");',
     "    (void)0;  // mutant",
     "a wait left and re-entered (a new residence) counted as the same one (F7)", QUIT),
    ("cut_seq_ignored",
     '  if (cut == kNoCut || m.spin <= cut) return no("its spin began before the ring cut");',
     '  if (false) return no("its spin began before the ring cut");',
     "P1: a spin that began before the ring cut (a hang the quit did not cause) accepted (F12 real, F13)", QUIT),
    ("progress_ignored",
     '  if (!turned) return no("its spin loop made no progress since the last check");',
     '  if (false) return no("its spin loop made no progress since the last check");',
     "P2: a spin loop that stopped turning (a thread at an exit) accepted (F17)", QUIT),
    ("predicate_ignored",
     '    if (!(next - target < next - completed)) return no("the fence passed (its own test is true)");',
     '    if (false) return no("the fence passed (its own test is true)");',
     "a fence that has passed (the wait ends at its next test) counted as stranded (F8)", QUIT),
    ("nogpu_ignored",
     '  if ((sr::kernel::readGuestU32BE(dev + 0x2A38u) >> 16) & 2u) return no("the no-GPU mode is set (dev+0x2A39 & 2)");',
     '  if (false) return no("the no-GPU mode is set (dev+0x2A39 & 2)");',
     "P2: the no-GPU mode's exit (GpuRingWaitProgress returns 0) not closed (F18)", QUIT),
    ("drop_set_ignored",
     '    if (!droppedFence(dev, target)) return no("its fence was in no publish the ring cut dropped");',
     '    if (false) return no("its fence was in no publish the ring cut dropped");',
     "P1: a fence the cut never dropped (another dropped / none) accepted (F14)", QUIT),
    ("forbid_ignored",
     "      if (g_allowStranded.load(std::memory_order_acquire)) {",
     "      if (true) {",
     "SR_QUIT_STRANDED=0 / Config::allowStranded false no longer keeps the failure (F4)", QUIT),
    # the record of what the cut dropped
    ("cut_seq_not_taken",
     "  g_cutWaitSeq.store(g_gpuWaitSeqGen.load(std::memory_order_acquire), std::memory_order_release);",
     "  g_cutWaitSeq.store(0u, std::memory_order_release);",
     "P1: the cut's wait-sequence mark not taken: every spin looks later than the cut (F12, F13)", QUIT),
    ("record_before_cut",
     "  if (!dev || !fence || !g_ringCut.load(std::memory_order_acquire)) return;  // the cut's drops only",
     "  if (!dev || !fence) return;  // the cut's drops only",
     "P1: drops recorded before any cut (T2)", QUIT),
    ("tail_fence_wrong",
     "  return sr::kernel::readGuestU32BE(t + 12u);",
     "  return sr::kernel::readGuestU32BE(t + 8u);",
     "P1: the dropped IB's fence read from the wrong dword (T1, F2)", QUIT),
    ("tail_fb_unchecked",
     "      sr::kernel::readGuestU32BE(t + 8u) != (mangle(fb) | 2u))",
     "      false)",
     "P1: the tail packet's address not checked - the progress word's packet read as a fence (T1)", QUIT),
    # never silent; verify compares identities; no silent cap
    ("count_hidden",
     "       th.threads, th.parked, th.waiting, th.hostIdle, fams, fields, (unsigned long long)since(t0), th.stranded);",
     "       th.threads, th.parked, th.waiting, th.hostIdle, fams, fields, (unsigned long long)since(t0), 0);",
     "silent: the complete line's stranded= no longer the count (S18, F2)", QUIT),
    ("lines_hidden",
     "  for (int i = 0; i < th.stranded; ++i) {",
     "  for (int i = 0; i < 0; ++i) {",
     "silent: no [QUIT] stranded line naming the thread (S18, F2, F20)", QUIT),
    ("verify_count_only",
     "    if (!sameStranded(th, tv, why, sizeof(why))) {",
     "    if (th.stranded != tv.stranded) {",
     "P2: verify comparing the count only - the {A,7} -> {B,12} swap passes (S21)", QUIT),
    ("overflow_silent",
     "  if (th.strandedOverflow) {",
     "  if (false) {",
     "P2: more stranded threads than the census names, and the quit completes (S22)", QUIT),
    # the marks (kernel/quit.hpp)
    ("leave_restores_outer",
     "  gpuWaitWrite(g, prev.kind, prev.dev, prev.arg, prev.gen, prev.rec, kGpuPhasePre, prev.kind ? nextGpuWaitSeq() : 0u, 0u);",
     "  gpuWaitWrite(g, prev.kind, prev.dev, prev.arg, prev.gen, prev.rec, prev.phase, prev.seq, prev.spin);",
     "P2: a resumed outer wait keeps its old seq / spin - a nested excursion invisible (F10, F19)", HPP),
    ("seq_not_unique",
     "inline uint64_t nextGpuWaitSeq() { return g_gpuWaitSeqGen.fetch_add(1, std::memory_order_relaxed) + 1u; }",
     "inline uint64_t nextGpuWaitSeq() { return 1u + 0u * g_gpuWaitSeqGen.load(std::memory_order_relaxed); }",
     "every residence and spin the same number: a re-entered wait looks like the same one (F7, F10)", HPP),
    ("spin_never_promoted",
     "  if (ph == kGpuPhasePre)",
     "  if (false)",
     "P2: the fence wait never marked as in its spin loop (F2, F9, PT1)", HPP),
    # the strong-defs (kernel/gpu_hang_wd.cpp)
    ("fence_unmarked",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitFence, ctx.r3.u32, ctx.r4.u32, 0u,",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitNone, ctx.r3.u32, ctx.r4.u32, 0u,",
     "the production GpuBlockOnFence strong-def no longer marking its wait: the bug back (F2, W9)", WD),
    ("spin_unmarked",
     "  if (spin) sr::kernel::quit::gpuSpinTick(sr::kernel::readGuestU32BE(ctx.r3.u32));  // rec+0 = dev",
     "  // mutant: no spin tick",
     "the GpuRingWaitProgress override no longer marking the spin loop (F2, RW1b, PT1, W10)", WD),
    ("exit_unmarked",
     "  if (spin && ctx.r3.u32 == 0u) sr::kernel::quit::gpuSpinExit();",
     "  (void)spin;  // mutant: no exit mark",
     "a 0 return (the loop ends) no longer marking the spin EXITED (PT1, W10)", WD),
    ("passthrough_return_altered",
     "  if (spin && ctx.r3.u32 == 0u) sr::kernel::quit::gpuSpinExit();",
     "  if (spin) ctx.r3.u64 = 1u;",
     "the pass-through altering the body's return (PT1, W10)", WD),
    ("passthrough_skips_delegate",
     "  __imp__sub_820F33E8(ctx, base);",
     "  if (!spin) __imp__sub_820F33E8(ctx, base);",
     "the pass-through skipping the generated body (PT1, W10)", WD),
    # the drop sites
    ("drop_ib_unrecorded",
     "    if (sr::native::flush::cutPublicationKind(phys, dwords) == 0) sr::kernel::quit::recordDroppedIb(dev, phys, dwords);",
     "    (void)phys;  // mutant",
     "P1: the ringless cut branch no longer recording the dropped IB's fence (NC2, W11)", RL),
    ("drop_seg_unrecorded",
     "      sr::kernel::quit::recordDroppedFence(dr.dev, fence);  // the fence it carried (0: none) - never written now",
     "      (void)fence;  // mutant",
     "P1: publishSegment's cut branch no longer recording the segment's fence (W12)", NF),
    ("seg_fence_not_passed",
     "    after = publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, S, progress);  // carries S and its progress",
     "    after = publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, 0u, progress);  // mutant",
     "P1: the segmentless close publishing without its fence (W12)", NF),
    # round 3 (Astra round 2): one mutant per fix, each caught by behaviour (no source-text check sees either)
    ("native_tail_scanned",
     "  if (t_segPublishing) return t_segPublishing->phys == phys && t_segPublishing->dwords == dwords ? 1 : 3;",
     "  if (t_segPublishing) return 0;  // mutant",
     "round 3 P1: the tail scan run on a native publication - a stale EVENT_WRITE_SHD F recorded beside S (NC1)", NF),
    ("record_double_check",
     "      th = t;  // the record: this check, after a whole window of acknowledged checks",
     "      th = h.threadsCheck(h.user, true); if (th.unacked) { stableSince = h.nowMs(h.user); h.threadsBegin(h.user); "
     "continue; }  // mutant",
     "round 3 P2: the record re-checked straight after the stable check - a one-CPU waiter never moves between the two "
     "(F21)", QUIT),
    # native/quit-ringspace: the ring-space wait's rule, its marks, its spin site and its drop records
    ("ring_rec_unchecked",
     "  if (kind == kGpuWaitRingSpace) return rec == g->gpuWaitRec.load(std::memory_order_relaxed) ? kind : 0u;",
     "  if (kind == kGpuWaitRingSpace) return kind;  // mutant",
     "ringspace: any call inside a ring wait's mark read as its spin - a nested caller's record promotes it (RW4, PT1)",
     HPP),
    ("fence_rec_unchecked",
     "  if (kind == kGpuWaitFence && lr == kFenceSpinReturn && rec == g->gpuWaitRec.load(std::memory_order_relaxed)) return kind;",
     "  if (kind == kGpuWaitFence && lr == kFenceSpinReturn) return kind;  // mutant",
     "ringspace hardening: the fence site's stale lr with a foreign record promotes a PRE fence wait (RW4, PT1)", HPP),
    ("ring_tick_unpromoted",
     "  if ((kind != kGpuWaitFence && kind != kGpuWaitRingSpace) || g.gpuWaitDev.load(std::memory_order_relaxed) != dev)",
     "  if (kind != kGpuWaitFence || g.gpuWaitDev.load(std::memory_order_relaxed) != dev)",
     "ringspace: the ring wait's spin call never promotes it to SPIN (RS0, RW1b, PT1)", HPP),
    ("ring_exit_unmarked",
     "  if ((kind != kGpuWaitFence && kind != kGpuWaitRingSpace) || g.gpuWaitPhase.load(std::memory_order_relaxed) != kGpuPhaseSpin)",
     "  if (kind != kGpuWaitFence || g.gpuWaitPhase.load(std::memory_order_relaxed) != kGpuPhaseSpin)",
     "ringspace: a 0 return in the ring wait's loop no longer marking it EXITED (RS2, PT1)", HPP),
    ("ring_test_cursor_dropped",
     "  return d == 0u || (d == 1u && end <= (progress & ~3u));",
     "  return d == 0u;  // mutant",
     "ringspace: the wait's own test without its cursor arm - space free one generation behind read as blocked (RS6, "
     "RS0)", HPP),
    ("ring_progress_unrecorded_pm4",
     "    recordDroppedProgress(dev, progress);  // 0 (no progress packet before the fence): nothing",
     "    (void)progress;  // mutant",
     "ringspace: a dropped PM4 close's progress word not recorded (RW1b, RW3, NC2b)", HPP),
    ("ring_predicate_ignored",
     '    if (ringSpaceFree(completed, target, next)) return no("the space is free (its own test is true)");',
     '    if (false) return no("the space is free (its own test is true)");',
     "ringspace: a ring wait whose space is already free (its next test ends it) counted as stranded (RS6)", QUIT),
    ("ring_drop_any",
     '    if (!droppedProgressFrees(dev, target, next)) return no("no progress word the ring cut dropped frees its space");',
     '    if (false) return no("no progress word the ring cut dropped frees its space");',
     "ringspace: a ring wait no dropped publish would have freed accepted (RS8)", QUIT),
    ("ring_drop_other_device",
     "    if (g_dropDev[i].dev == dev)  // the waiting device's own words: another ring's progress frees nothing here",
     "    if (true)  // mutant",
     "Astra round 1 P2: another device's dropped progress word frees the wait (RS8 'other')", QUIT),
    ("ring_progress_tail_unchecked",
     "      sr::kernel::readGuestU32BE(p + 8u) == (mangle(fb + 4u) | 2u))",
     "      true)",
     "ringspace: the progress packet's address not checked - another EVENT_WRITE_SHD's value read as progress (RW3)",
     QUIT),
    ("ring_progress_before_cut",
     "  if (!dev || !progress || !g_ringCut.load(std::memory_order_acquire)) return;  // the cut's drops only (progress)",
     "  if (!dev || !progress) return;  // mutant",
     "ringspace: progress words recorded before any cut (RW3)", QUIT),
    ("ring_unmarked",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitRingSpace, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitNone, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,",
     "ringspace: the production GpuRingWaitForSpace strong-def no longer marking its wait: the bug back (RW1b, W9)", WD),
    ("ring_gen_unmarked",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitRingSpace, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,",
     "      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitRingSpace, ctx.r3.u32, ctx.r4.u32, 0u,",
     "ringspace: the ring wait's generation (r5) not marked - its test read against generation 0 (RW1b, W9)", WD),
    ("ring_mark_rec_wrong",
     "                                     ctx.r1.u32 - sr::kernel::quit::kWaitRecBelowSp);  // its one spin call's record",
     "                                     ctx.r1.u32);  // mutant",
     "ringspace: the ring wait's record marked at the wrong address - the real body's spin call never recognised (RW1b, "
     "W9)", WD),
    ("fence_mark_rec_wrong",
     "                                     ctx.r1.u32 - sr::kernel::quit::kWaitRecBelowSp);  // the record its spin passes",
     "                                     ctx.r1.u32);  // mutant",
     "ringspace hardening: the fence wait's record marked at the wrong address - the real body's spin call never "
     "recognised (F2, W9)", WD),
    ("ring_progress_unrecorded_native",
     "      sr::kernel::quit::recordDroppedProgress(dr.dev, progress);  // its progress word (0: none) - never written now",
     "      (void)progress;  // mutant",
     "ringspace: publishSegment's cut branch no longer recording the close's progress word (RW2, W12)", NF),
    # native/quit-ringspace follow-up: the GPU hang watchdog stands down at the quit's ring cut (qrd1 d2 ch18's H0)
    ("wd_standdown_removed",
     "  if (s.quitCut) return 0u;",
     "  // mutant: no stand-down",
     "ringspace: the watchdog's triggers no longer stand down after the ring cut - the provoked qrd1 state fires H0 "
     "(HW1)", WD),
    ("wd_standdown_unwired",
     "  s.quitCut = sr::kernel::quit::g_ringCut.load(std::memory_order_acquire);",
     "  s.quitCut = false;  // mutant",
     "ringspace: the tick never reads the ring cut - no stand-down, no line (HW2, HW3)", WD),
    ("wd_hostwait_not_gated",
     "  if (!s.quitCut && s.cpPhase == kCpPhaseHostWait && s.cpPhaseSinceNs) {",
     "  if (s.cpPhase == kCpPhaseHostWait && s.cpPhaseSinceNs) {",
     "ringspace: the host-wait report not gated by the ring cut (HW3)", WD),
    ("seg_progress_not_passed",
     "    after = publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, S, progress);  // carries S and its progress",
     "    after = publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, S, 0u);  // mutant",
     "ringspace: the segmentless close publishing without its progress word (W12)", NF),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("-j", "--jobs", type=int, default=12)
    ap.add_argument("--check", action="store_true", help="only verify every mutated line occurs exactly once")
    ap.add_argument("--only", default="", help="comma-separated mutant names to run (default: all)")
    a = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    paths = sorted({m[4] for m in MUTANTS})
    originals = {p: (root / p).read_bytes() for p in paths}
    texts = {p: originals[p].decode("utf-8") for p in paths}
    newline = "\r\n" if "\r\n" in texts[QUIT] else "\n"
    exe = root / a.build_dir / "difftest.exe"
    receipt = {"sources": {p: sha(originals[p]) for p in paths}, "tool_sha256": sha(Path(__file__).read_bytes()),
               "mutants": []}

    def restore_all():
        for p in paths:
            (root / p).write_bytes(originals[p])

    def run(name, cmd, timeout):
        log = out / (name + ".log")
        with open(log, "wb") as f:
            try:
                rc = subprocess.run(cmd, cwd=root, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                rc = 124
        return rc, log.read_text(encoding="utf-8", errors="replace")

    def build(name):
        return run(name, ["cmake", "--build", a.build_dir, "--target", "difftest", "-j", str(a.jobs)], 3600)[0]

    def suite(name):
        rc, txt = run(name, [str(exe), "--quit-test"], 900)
        fails = [ln.strip() for ln in txt.splitlines() if ln.startswith("FAIL:")]
        summary = [ln.strip() for ln in txt.splitlines() if ln.startswith("--quit-test:")]
        return rc, fails, summary[-1] if summary else ""

    bad = 0
    for name, old, new, why, path in MUTANTS:
        if texts[path].count(old) != 1:
            print("setup error: %s: the production line occurs %d times in %s" % (name, texts[path].count(old), path))
            bad += 1
    if bad:
        return 2
    if a.check:
        print("quit-drain mutants: %d mutants, every line found exactly once" % len(MUTANTS))
        return 0
    only = [x for x in a.only.split(",") if x]
    unknown = [x for x in only if x not in {m[0] for m in MUTANTS}]
    if unknown:
        print("setup error: unknown mutant(s) %s" % unknown)
        return 2
    receipt["only"] = only
    ok = True
    try:
        for name, old, new, why, path in MUTANTS:
            if only and name not in only:
                continue
            (root / path).write_bytes(texts[path].replace(old, new).encode("utf-8"))
            b = build("build_" + name)
            rc, fails, summary = suite("suite_" + name) if b == 0 else (None, [], "")
            caught = b == 0 and rc not in (None, 0) and len(fails) > 0
            ok &= caught
            row = {"mutant": name, "source": path, "breaks": why, "old": old.strip(), "new": new.strip(),
                   "build_exit": b, "suite_exit": rc, "fail_lines": fails[:12], "fail_count": len(fails),
                   "summary": summary, "caught": caught}
            receipt["mutants"].append(row)
            print(json.dumps({k: row[k] for k in ("mutant", "build_exit", "suite_exit", "fail_count", "caught")}),
                  flush=True)
            (root / path).write_bytes(originals[path])
    finally:
        restore_all()
        b = build("build_restored")
        rc, fails, summary = suite("suite_restored") if b == 0 else (None, [], "")
        now = {p: sha((root / p).read_bytes()) for p in paths}
        receipt["restored"] = {"sources": now, "build_exit": b, "suite_exit": rc, "fail_count": len(fails),
                               "summary": summary}
        ok &= b == 0 and rc == 0 and now == receipt["sources"]
        receipt["verdict"] = "PASS" if ok else "FAIL"
        (out / "mutants.json").write_text(json.dumps(receipt, indent=1) + newline, encoding="utf-8")
    print("quit-drain mutants: %s (%d mutants; restored suite exit %s)" % (
        receipt["verdict"], len(receipt["mutants"]), receipt["restored"]["suite_exit"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
