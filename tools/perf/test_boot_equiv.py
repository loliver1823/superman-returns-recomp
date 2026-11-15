#!/usr/bin/env python3
"""test_boot_equiv.py - tools/perf/boot_equiv.py's rule (BOOT_EQUIV design dcl-v2) on synthetic rolls, and a mutant
check that each guard is load-bearing (native/devcreate-close, 2026-10-14; revised after Astra round 1).

    python -m unittest discover -s tools/perf -p test_boot_equiv.py

A clean set of 8 alternating pairs after a declared warm-up is EQUIVALENT and only then prints the tag BOOT_EQUIV; its
line claims the mean regression only (no stall bound beyond the exact 31.23% of 0 in 8). The negative controls must
never be EQUIVALENT: a real +2 s gap and a +2.5 s gap (OPEN: the mean over its margin), dcc4's late attach (OPEN), a
single +6 s stall the bound alone would not call OPEN (the stall cap), an ON attach > 5 s in both arms (OPEN: no paired
difference hides it), a crash, a timeout, no HUD, a roll folder without outcome.json, the black-world race, a run row
ABORT with outcome SUCCESS, a THREAD_FATAL / ICALL_FAIL line with outcome SUCCESS, a recorded 0.0 ms wait beside a 25 s
interval (all OPEN), a missing marker and an attach line without its race fields (INCONCLUSIVE), a mis-ordered set, an
under-powered set (3 pairs; 8 pairs whose spread puts the bound over the margin; the Student-t boundary), no / a
crashed / a devcreate warm-up, a warm-up still running at the first graded launch, a warm-up in another invocation, the
warm-up also graded, foreign proof rows matched by basename, a proof JSON copied from another folder, another run's exe
or start time, two exes, two exe paths, a configuration difference, missing seeding, an arm not engaged, a roll never
run, a first-slot effect on ANY endpoint in either direction (all INCONCLUSIVE), an OFF-arm stall (INCONCLUSIVE).
The warm-up is never graded: a slow first roll in it leaves a clean set EQUIVALENT.
The bake's design dcf-v1 (native/devcreate-fix, --design dcf-v1: 4 pairs, the gate default ON): a clean baked set is
EQUIVALENT with the tag and the 4-boot stall sentence; the same rolls under dcl-v2, eight pairs, a pinned warm-up and an
OFF roll without =0 are INCONCLUSIVE; a real gap and a late attach are OPEN.

The mutant check (class Mutants) copies boot_equiv.py with boot_pairs.py, boot_stall.py and proof_rows.py under the
checkout's archive/, applies one source mutation at a time (to the evaluator or to proof_rows) and runs the tests that
guard it against the mutant (BOOT_EQUIV_TOOL): each must FAIL by an assertion. Scratch rolls go under the checkout's
archive/ (never C:).
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.environ.get("BOOT_EQUIV_TOOL") or os.path.join(HERE, "boot_equiv.py")
sys.path.insert(0, HERE)
import proof_rows as prs  # noqa: E402  (UMBRELLA: rows name rolls relative to it, as Runner.rel writes them)

STAGED = r"J:\sr_stage\content-x\exe-y\SupermanReturns.exe"
ASYNC = ("[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe of J:\\x\\dxil says "
         "an installed cache (probe 0.1 ms, from process +69.4 ms); the load runs on the loader thread, the CP waits for "
         "its installed stage (process +69.5 ms)")
DESIGN = [("on", "off") if k % 2 == 0 else ("off", "on") for k in range(8)]
CLEAN = [0.3, -0.2, 0.1, -0.4, 0.2, 0.0, -0.1, 0.3]  # paired HUD differences of a clean set (mean +0.03, sd 0.26)


def ts(base_s, sec):
    s = base_s + sec
    h, rem = divmod(s, 3600)
    m, s2 = divmod(rem, 60)
    return "2026-10-14 %02d:%02d:%06.3f" % (h, m, s2)


class BootEquiv(unittest.TestCase):
    def setUp(self):
        root = os.path.join(REPO, "archive")
        os.makedirs(root, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="boot_equiv_test_", dir=root)
        self.clock = 6 * 3600  # 06:00:00, each roll 5 minutes after the last
        self.sets = 0

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # -- a synthetic perf_run roll (render/boot-attach's async load on J:, the player log profile)
    def roll(self, name, on, hud=83.0, attach_at=0.7, cpup=1.3, load=0.12, load_end_at=0.3, exe="a" * 64,
             exe_path=STAGED, result="SUCCESS", engaged=None, extra_env=None, no_seed=False, no_run_log=False,
             no_load_line=False, swaps=0, short_attach=False, no_outcome=False, wait_status="attached", wait_ms=None,
             extra_lines=None, baked=False):
        """on: True / False the arms, None the default build (the warm-up). baked: the bake's provenance (dcf-v1) - the
        ON arm and the default leave SR_NATIVE_DEVCREATE unset, the OFF arm pins =0; else (dcl-v2) ON pins =1."""
        d = os.path.join(self.tmp, name, "drive", "attempt1")
        os.makedirs(d)
        env = {"SR_NATIVE_SEGMENTLESS": "1", "SR_LOG": "player", "SR_HIGHFPS": "uncapped",
               "SR_SHADER_CACHE_DIR": r"J:\sr_stage\shader-x\dxil", "SR_PERF_DIR": os.path.join(d, "perf"),
               "SR_CONTENT_DIR": os.path.join(d, "content"), "SR_PRESENT_DUMP_DIR": os.path.join(d, "frames")}
        if baked:
            if on is False:
                env["SR_NATIVE_DEVCREATE"] = "0"
        elif on:
            env["SR_NATIVE_DEVCREATE"] = "1"
        env.update(extra_env or {})
        with open(os.path.join(d, "provenance.json"), "w") as f:
            json.dump({"exeSha256": exe, "exePath": exe_path, "effectiveSrEnv": env}, f)
        if not no_outcome:
            with open(os.path.join(d, "outcome.json"), "w") as f:
                json.dump({"result": result}, f)
        b = self.clock
        self.clock += 300
        with open(os.path.join(d, "steps.log"), "w") as f:
            f.write("[06:00:00.000] === ATTEMPT 1 of 1 ===\n")
            if not no_seed:
                f.write("[06:00:00.000] saves: SR_CONTENT_DIR=%s (empty)\n" % env["SR_CONTENT_DIR"])
        L = [(0.0, "[LOG_PROFILE] player"), (0.05, ASYNC), (0.4, "[HIGHFPS] swap chain ALLOW_TEARING on"),
             (cpup, "[GPU_CP] command processor thread up (pcr=D00E1800 guestCaller=ok)")]
        if not no_load_line:
            L.append((load_end_at, "[SHADER_CACHE] boot load done: %.1f ms on thread 7 (main thread 2): installed "
                                   "cache x" % (1000 * load)))
        if short_attach:
            L.append((attach_at, "[DISPLAY] PM4 live backend attached"))
        else:
            L.append((attach_at, "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %d, draw commands queued "
                                 "0; process +%.1f ms; installed shader cache just after the attach: loaded" % (
                                     swaps, 1000 * attach_at + 69.0)))
        if attach_at > cpup:
            L.append((attach_at + 0.001, "[DISPLAY] CP waited %.1f ms for the live backend (%s)" % (
                1000 * (attach_at - cpup) if wait_ms is None else wait_ms, wait_status)))
        eng = (on if on is not None else baked) if engaged is None else engaged  # the default build engages when baked
        L.append((13.0, "[GPU_CP] census rptr=81 wptr=93 doorbells=21 packets=%d draws=235 isr=2" % (0 if eng else 1)))
        if result == "SUCCESS" and hud is not None:
            L.append((hud, "[APT_REQ] #17 path='screens/hud/main_hud' holder=0"))
        L += list(extra_lines or [])
        L.sort()
        if not no_run_log:
            with open(os.path.join(d, "run.log"), "w") as f:
                for t, text in L:
                    f.write("[%s] [INFO ] [BOOT] %s\n" % (ts(b, t), text))
        return os.path.join(self.tmp, name)

    def proof(self, name, rolls, verdicts=None, skip=(), out=None, rel_dir=None, exe_id="a" * 12,
              started="2026-10-14T05:00:00"):
        """A native_proof proof JSON beside the rolls, as Runner.write_proof writes it: its out (this folder), exe id,
        start time and the run's roll rows naming each roll by its umbrella-relative path (Runner.rel). verdicts:
        {roll: verdict}; skip: rolls the run skipped (a SKIP row without a path, as after the abort rule); out /
        rel_dir: another run's (a JSON copied from another folder; rows under another folder)."""
        def rel(p):
            if rel_dir is not None:
                return rel_dir + "/" + os.path.basename(p)
            return os.path.relpath(p, prs.UMBRELLA).replace("\\", "/")
        steps = []
        for p in rolls:
            if p in skip:
                steps.append({"step": os.path.basename(p), "kind": "roll", "verdict": "SKIP",
                              "detail": "plan aborted: x", "paths": {}})
            else:
                steps.append({"step": os.path.basename(p), "kind": "roll", "verdict": (verdicts or {}).get(p, "DONE"),
                              "detail": "perf_run exit 0", "paths": {"roll": rel(p)}})
        with open(os.path.join(self.tmp, name), "w") as f:
            json.dump({"plan": "devcreate_close", "prefix": "dcl", "out": out or self.tmp, "exe_id": exe_id,
                       "started": started, "steps": steps}, f)

    def make_set(self, diffs=CLEAN, over=None, both=None, warm=None, n=8, design=None, warm_after=None,
                 split_proof=False):
        """A warm-up roll (OFF config) then n pairs created (= run) in `design` order (default dcl-v2's alternation),
        recorded in one native_proof proof JSON (split_proof: the warm-up in a JSON of its own - another invocation).
        diffs: the paired HUD differences (ON = 83 + d, OFF = 83). over = {"p<k><arm>": kwargs}; both = kwargs for
        every roll; warm = the warm-up's kwargs (False: none); warm_after = create the warm-up after pair k.
        -> (pairs, warm-up path)."""
        pairs, wpath = self._make_set(diffs, over, both, warm, n, design, warm_after)
        rolls = [p for pr in pairs for _a, p in pr]
        if wpath and split_proof:
            self.proof("s%d_w_proof.json" % self.sets, [wpath])
            self.proof("s%d_dcl_proof.json" % self.sets, rolls)
        else:
            self.proof("s%d_dcl_proof.json" % self.sets, ([wpath] if wpath else []) + rolls)
        return pairs, wpath

    def _make_set(self, diffs, over, both, warm, n, design, warm_after):
        self.sets += 1
        over, both = over or {}, both or {}
        design = design or DESIGN[:n]

        def mk(tag, on, base):
            kw = dict(both, hud=base)
            kw.update(over.get(tag, {}))
            return self.roll("s%d_%s" % (self.sets, tag), on, **kw)
        wpath = None
        mkw = lambda: self.roll("s%d_warmup" % self.sets, None, **dict(both, **(warm or {})))  # the default build
        if warm is not False and warm_after is None:
            wpath = mkw()
        pairs = []
        for k, arms in enumerate(design):
            pr = []
            for arm in arms:
                tag = "p%d%s" % (k + 1, arm)
                pr.append((arm, mk(tag, arm == "on", 83.0 + (diffs[k % len(diffs)] if arm == "on" else 0.0))))
            pairs.append(pr)
            if warm is not False and warm_after == k + 1:
                wpath = mkw()
        return pairs, wpath

    def run_raw(self, pairs, warmup, extra=(), design=None):
        cmd = [sys.executable, TOOL]
        if design:
            cmd += ["--design", design]
        for p in pairs:
            cmd += ["--pair"] + ["%s:%s" % (arm, path) for arm, path in p]
        if warmup:
            cmd += ["--warmup", warmup]
        return subprocess.run(cmd + list(extra), capture_output=True, text=True)

    def run_tool(self, pairs, warmup, design=None):
        p = self.run_raw(pairs, warmup, design=design)
        self.assertEqual(p.returncode, 0, p.stderr)  # a crash (a KeyError) is never a verdict
        out = p.stdout
        last = out.strip().splitlines()[-1]
        self.assertNotIn("RESOLVED", out)
        verdicts = [v for v in (" -> EQUIVALENT ", " -> OPEN ", " -> INCONCLUSIVE ") if v in last]
        self.assertEqual(len(verdicts), 1, last)  # exactly one verdict
        if " -> EQUIVALENT " in last:
            head = "BOOT_EQUIV design=%s pairs=%d -> EQUIVALENT " % (design or "dcl-v2", 4 if design == "dcf-v1" else 8)
            self.assertTrue(last.startswith(head), last)
        else:
            self.assertNotIn("BOOT_EQUIV", out)  # the tag is the resolving outcome's alone
        return last

    def json_of(self, pairs, warmup, design=None):
        js = os.path.join(self.tmp, "out_%d.json" % self.sets)
        p = self.run_raw(pairs, warmup, ["--json", js], design=design)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(js, encoding="utf-8") as f:
            return json.load(f)

    # -- the bake's design dcf-v1 (native/devcreate-fix, 2026-10-15): 4 pairs, the gate default ON (the ON arm and the
    # warm-up leave SR_NATIVE_DEVCREATE unset, the OFF arm pins =0); the same rule otherwise
    def bake_set(self, diffs=CLEAN, over=None, warm=None, n=4):
        return self.make_set(diffs=diffs, over=over, both={"baked": True}, warm=warm, n=n, design=DESIGN[:n])

    def test_bake_clean_four_pairs_is_equivalent(self):
        pairs, w = self.bake_set()
        v = self.run_tool(pairs, w, design="dcf-v1")
        self.assertIn("-> EQUIVALENT", v)
        self.assertIn("these 4 ON boots", v)  # the stall sentence counts the design's boots, 1 - 0.05^(1/4)
        self.assertIn("52.71%", v)
        d = self.json_of(pairs, w, design="dcf-v1")
        self.assertEqual((d["design"], d["pairs"], d["registered_pairs"], d["baked"]), ("dcf-v1", 4, 4, True))

    def test_bake_set_read_under_the_old_design_is_inconclusive(self):
        # dcl-v2 reads an unset key as OFF: every ON roll's gate state is wrong there, and 4 pairs are not 8
        pairs, w = self.bake_set()
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("registered design dcl-v2 is 8 pairs", v)

    def test_bake_eight_pairs_is_inconclusive(self):
        pairs, w = self.bake_set(n=8)
        v = self.run_tool(pairs, w, design="dcf-v1")
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("registered design dcf-v1 is 4 pairs", v)

    def test_bake_pinned_warmup_is_inconclusive(self):
        for pin in ("1", "0"):
            pairs, w = self.bake_set(warm={"extra_env": {"SR_NATIVE_DEVCREATE": pin}})
            v = self.run_tool(pairs, w, design="dcf-v1")
            self.assertIn("-> INCONCLUSIVE", v)
            self.assertIn("the warm-up pins SR_NATIVE_DEVCREATE=%s" % pin, v)

    def test_bake_real_gap_is_open(self):
        pairs, w = self.bake_set(diffs=[2.6, 2.4, 2.5, 2.7])
        v = self.run_tool(pairs, w, design="dcf-v1")
        self.assertIn("-> OPEN", v)
        self.assertIn("a mean regression over its margin", v)

    def test_bake_off_roll_without_the_pin_is_inconclusive(self):
        # an OFF roll whose provenance leaves the key unset is the default build - the ON arm under the bake
        pairs, w = self.bake_set(over={"p1off": {"baked": False}})
        v = self.run_tool(pairs, w, design="dcf-v1")
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("pair 1 OFF roll's gate state", v)

    def test_bake_late_attach_is_open(self):
        pairs, w = self.bake_set(over={"p3on": {"attach_at": 26.7, "hud": 100.7}})
        v = self.run_tool(pairs, w, design="dcf-v1")
        self.assertIn("-> OPEN", v)
        self.assertIn("a late attach", v)

    # -- the clean set
    def test_clean_set_is_equivalent(self):
        pairs, w = self.make_set()
        v = self.run_tool(pairs, w)
        self.assertIn("-> EQUIVALENT within the tested conditions", v)
        # P3-9 / P1-1: the claim is the mean regression only; the stall bound is stated exactly and disowned
        self.assertIn("the MEAN paired regression of every endpoint is below its margin", v)
        self.assertIn("does NOT bound an intermittent stall", v)
        self.assertIn("only below 31.23% (exact, independent boots assumed)", v)
        self.assertNotIn("stall rate <", v)
        d = self.json_of(pairs, w)
        self.assertEqual((d["verdict"], d["tag"], d["design"], d["pairs"]), ("EQUIVALENT", "BOOT_EQUIV", "dcl-v2", 8))
        self.assertAlmostEqual(d["endpoints"]["hud"]["mean"], sum(CLEAN) / 8, places=6)
        self.assertTrue(d["proof"].endswith("s1_dcl_proof.json"))

    def test_async_load_ending_late_is_fine(self):
        # a slow disk: the load done at 20 s, long after the attach (0.7 s) - not an inconsistent log
        pairs, w = self.make_set(both=dict(load=19.5, load_end_at=20.0))
        self.assertIn("-> EQUIVALENT", self.run_tool(pairs, w))

    def test_t_table_matches_scipy(self):
        try:
            from scipy import stats
        except ImportError:
            self.skipTest("scipy not installed")
        import importlib.util
        sys.path.insert(0, os.path.dirname(TOOL))
        spec = importlib.util.spec_from_file_location("boot_equiv_under_test", TOOL)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for df, q in mod.T95.items():
            self.assertAlmostEqual(q, stats.t.ppf(0.95, df), places=3)
        self.assertEqual(mod.DESIGN[:3], [["on", "off"], ["off", "on"], ["on", "off"]])
        self.assertEqual(mod.N_PAIRS, 8)

    def test_exact_zero_in_n_bound(self):
        # P3-9: 0 of 8 bounds the rate at 31.23% (not "below 31%"); 0 of 29 at 9.81%
        sys.path.insert(0, os.path.dirname(TOOL))
        import boot_stall
        self.assertAlmostEqual(100 * boot_stall.upper95(8), 31.23, places=2)
        self.assertAlmostEqual(100 * boot_stall.upper95(29), 9.81, places=2)
        self.assertGreater(boot_stall.upper95(28), 0.10)  # 29 is the least N under 10%

    # -- OPEN: a regression, never discarded
    def test_real_gap_is_open(self):
        pairs, w = self.make_set(diffs=[x + 2.5 for x in CLEAN])  # a real +2.5 s gap in every pair
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("a mean regression over its margin", v)
        pairs, w = self.make_set(diffs=[x + 2.0 for x in CLEAN])  # a real +2 s gap: never EQUIVALENT
        v = self.run_tool(pairs, w)
        self.assertNotIn("EQUIVALENT", v)

    def test_dcc4_late_attach_is_open(self):
        # dcc4 p1: the ON roll's backend attached 20 s late (the CP waited), the HUD 17 s later
        pairs, w = self.make_set(over={"p1on": dict(attach_at=21.3, hud=100.7)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("stall cap", v)
        self.assertIn("ON uncovered wait 20.00 s", v)
        self.assertIn("ON attach at +21.30 s (> 5.0 s: a late attach)", v)

    def test_late_attach_in_both_arms_is_open(self):
        # every roll attaches 21 s in (before the CP starts, so no wait): the paired differences are all ~0, yet the ON
        # boots stalled - the absolute rule makes it OPEN (a paired mean alone would pass it)
        pairs, w = self.make_set(both=dict(attach_at=21.3, cpup=21.5), warm=dict(attach_at=0.7, cpup=1.3))
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("ON attach at +21.30 s", v)
        # the OFF arm alone late: never EQUIVALENT, the OFF stall named (its short attach -> HUD also puts that pair's
        # attach -> HUD difference over the stall cap: OPEN, the conservative reading)
        pairs, w = self.make_set(over={"p2off": dict(attach_at=21.3, cpup=21.5)})
        v = self.run_tool(pairs, w)
        self.assertNotIn("EQUIVALENT", v)
        self.assertIn("OFF attach at +21.30 s", v)
        # an OFF roll attaching just over 5 s (no paired difference over the cap): INCONCLUSIVE, the OFF stall named
        pairs, w = self.make_set(over={"p2off": dict(attach_at=5.3, cpup=5.5)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("OFF attach at +5.30 s", v)

    def test_single_stall_is_caught_by_the_cap(self):
        # one pair +6 s, the rest 0: mean +0.75, the bound alone would read INCONCLUSIVE - the cap makes it OPEN
        pairs, w = self.make_set(diffs=[6.0, 0, 0, 0, 0, 0, 0, 0])
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("pair 1 time to HUD (primary) ON - OFF +6.00 s over the 5.0 s stall cap", v)

    def test_crash_is_open(self):
        for tag, kw, word in (("p3on", dict(result="FAILURE"), "outcome FAILURE"),
                              ("p4off", dict(result="TIMEOUT"), "outcome TIMEOUT"),
                              ("p6on", dict(no_outcome=True), "outcome missing"),
                              ("p2on", dict(hud=None), "no HUD request"),
                              ("p5on", dict(attach_at=61.3, wait_status="TIMED OUT"), "backend wait TIMED OUT")):
            pairs, w = self.make_set(over={tag: kw})
            v = self.run_tool(pairs, w)
            self.assertIn("-> OPEN (kept, never discarded)", v, tag)
            self.assertIn(word, v, tag)

    def test_crash_with_the_rest_not_run_is_open(self):
        # the plan's abort rule: p3on crashed (its row ABORT), every later roll never ran (SKIP rows without a path) -
        # OPEN, the missing rolls named
        pairs, w = self.make_set(over={"p3on": dict(result="FAILURE")})
        later = [path for pr in pairs[3:] for _arm, path in pr]
        for path in later:
            shutil.rmtree(path)
        self.proof("s%d_dcl_proof.json" % self.sets, [w] + [p for pr in pairs for _a, p in pr],
                   verdicts={pairs[2][1][1]: "ABORT"}, skip=later)
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("the run's row says ABORT", v)
        self.assertIn("not run (no roll folder)", v)

    def test_failed_row_with_outcome_success_is_open(self):
        # Astra round 1 P1-2: outcome.json SUCCESS, the run's row ABORT (native_proof's abort rule saw a marker) - OPEN
        pairs, w = self.make_set()
        self.proof("s%d_dcl_proof.json" % self.sets, [w] + [p for pr in pairs for _a, p in pr],
                   verdicts={pairs[4][0][1]: "ABORT"})
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("the run's row says ABORT", v)

    def test_abort_marker_with_outcome_success_is_open(self):
        # P1-2: a THREAD_FATAL line, a DONE row, outcome SUCCESS - and an ICALL_FAIL long after the HUD
        for tag, line, word in (("p2off", "[THREAD_FATAL] tid=12 x", "thread fatal in the log"),
                                ("p6on", "[ICALL_FAIL] target 00000000 x", "indirect-call failure in the log"),
                                ("p7on", "[CP_WAIT] stalled 5000 ms", "CP stall in the log")):
            pairs, w = self.make_set(over={tag: dict(extra_lines=[(200.0, line)])})
            v = self.run_tool(pairs, w)
            self.assertIn("-> OPEN", v, tag)
            self.assertIn(word, v, tag)

    def test_recorded_zero_wait_cannot_hide_a_stall(self):
        # P2-6: "CP waited 0.0 ms" beside an attach 25 s after the CP came up - the interval is the wait (OPEN on ON)
        pairs, w = self.make_set(over={"p3on": dict(attach_at=26.3, wait_ms=0.0)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("ON uncovered wait 25.00 s", v)
        self.assertIn("recorded wait 0.00 s vs [CP up, attach] 25.00 s", v)

    def test_black_world_race_is_open(self):
        pairs, w = self.make_set(over={"p2on": dict(swaps=3)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> OPEN", v)
        self.assertIn("black-world race", v)

    # -- INCONCLUSIVE: the set is not evidence
    def test_missing_marker_is_inconclusive(self):
        pairs, w = self.make_set(over={"p5off": dict(no_load_line=True)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("no loaddone line", v)
        pairs, w = self.make_set(over={"p7on": dict(short_attach=True)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("no VdSwaps / queued-draws fields", v)
        pairs, w = self.make_set(over={"p1off": dict(no_run_log=True)})
        self.assertIn("no run.log", self.run_tool(pairs, w))

    def test_misordered_set_is_inconclusive(self):
        pairs, w = self.make_set()
        swapped = list(pairs)
        swapped[2] = [pairs[2][1], pairs[2][0]]  # pair 3 declared OFF/ON (ran ON first): design + order
        v = self.run_tool(swapped, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("pair 3 did not run in its declared order", v)
        pairs, w = self.make_set(design=[("on", "off")] * 8)  # created and declared ON-first every time
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("the registered design dcl-v2 is 8 pairs", v)
        pairs, w = self.make_set(warm_after=1)  # the warm-up ran after pair 1
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("the warm-up had not ended", v)
        pairs, w = self.make_set()
        v = self.run_tool([pairs[2], pairs[1], pairs[0]] + pairs[3:], w)  # declared out of the run sequence
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("declared sequence", v)

    def test_under_powered_set_is_inconclusive(self):
        pairs, w = self.make_set(n=3)
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("got 3", v)
        noisy = [3.5, -3.0, 3.0, -2.5, 3.8, -3.2, 2.9, -2.6]  # mean +0.24, sd 3.3: the bound over 2.0
        pairs, w = self.make_set(diffs=noisy)
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("under-powered as run", v)

    def test_student_t_boundary(self):
        # mean 0.6, s/sqrt(8) = 0.794: t(7) gives 0.6 + 1.895 * 0.794 = 2.10 >= 2.0 (INCONCLUSIVE); a normal quantile
        # would give 1.91 < 2.0 (EQUIVALENT) - the frozen statistic is Student's t with n-1 df. The spread is in the
        # OFF arm (every ON roll at 83.6 s), so pair 1's ON roll sits at its arm's median: no position effect here.
        a = 2.10
        s = a * math.sqrt(8 / 7)
        self.assertLess(0.6 + 1.6449 * s / math.sqrt(8), 2.0)
        self.assertGreaterEqual(0.6 + 1.8946 * s / math.sqrt(8), 2.0)
        pairs, w = self.make_set(diffs=[0.6], over={"p%doff" % (k + 1): dict(hud=83.0 + (a if k % 2 else -a))
                                                    for k in range(8)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("time to HUD (primary) ucb95 +2.10 >= margin 2.0", v)
        self.assertNotIn("position effect", v)

    def test_warmup_rules(self):
        pairs, w = self.make_set(warm=False)
        v = self.run_tool(pairs, None)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("no declared warm-up roll", v)
        pairs, w = self.make_set(warm=dict(result="FAILURE"))
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)  # never graded: its crash is named, not an OPEN of the set
        self.assertIn("did not complete to the HUD", v)
        pairs, w = self.make_set(warm=dict(extra_env={"SR_NATIVE_DEVCREATE": "1"}))
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("the warm-up ran SR_NATIVE_DEVCREATE=1", v)
        pairs, w = self.make_set()
        v = self.run_tool(pairs, pairs[0][0][1])  # the warm-up named as a graded roll
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("is also a graded roll", v)
        pairs, w = self.make_set()
        v = self.run_tool(pairs, os.path.join(self.tmp, "nowhere"))
        self.assertIn("was not run", v)
        pairs, w = self.make_set(split_proof=True)  # bat2's form: the warm-up in its own invocation
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("a roll folder the run s%d_dcl_proof.json does not record" % self.sets, v)

    def test_warmup_still_running_at_the_first_graded_launch(self):
        # P2-7: the warm-up started first, but its log runs to +400 s - past pair 1's launch at +300 s
        pairs, w = self.make_set(warm=dict(hud=400.0))
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("the warm-up had not ended (its log runs to 2026-10-14 06:06:40", v)

    def test_foreign_proof_rows_are_refused(self):
        # P1-2: rows naming the same BASENAMES under another folder are another run's - never this set's
        pairs, w = self.make_set(warm=False)
        rolls = [p for pr in pairs for _a, p in pr]
        w = self.roll("s%d_warmup" % self.sets, False)  # (after the set: the identity is what is under test)
        os.remove(os.path.join(self.tmp, "s%d_dcl_proof.json" % self.sets))
        self.proof("other_proof.json", [w] + rolls, rel_dir="_worktrees/elsewhere/archive/v1_native/dcl1")
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("no native_proof run beside the rolls records them", v)
        # a proof JSON copied here from another folder (its `out` is not this one) is refused
        os.remove(os.path.join(self.tmp, "other_proof.json"))
        self.proof("copied_proof.json", [w] + rolls, out=os.path.join(REPO, "archive", "elsewhere"))
        v = self.run_tool(pairs, w)
        self.assertIn("is not the folder it sits in", v)

    def test_another_runs_identity_is_refused(self):
        # P1-2: the run's exe id or start time contradicts the rolls (a later invocation's JSON over older rolls)
        pairs, w = self.make_set()
        rolls = [w] + [p for pr in pairs for _a, p in pr]
        self.proof("s%d_dcl_proof.json" % self.sets, rolls, exe_id="b" * 12)
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("is not the run's bbbbbbbbbbbb", v)
        self.proof("s%d_dcl_proof.json" % self.sets, rolls, started="2026-10-14T23:00:00")
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("before the run started", v)

    def test_position_effect_per_endpoint(self):
        # P2-8: ON-first is NOT conservative for every endpoint. The first graded roll's attach 1 s late (the HUD
        # unchanged): attach -> HUD falls 1 s in that pair; every bound would pass - the position rule catches it
        pairs, w = self.make_set(over={"p1on": dict(attach_at=1.7)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("a position effect", v)
        self.assertIn("time to attach +1.00 s from its arm's median", v)
        # the first slot +2.4 s to the HUD (bat's first-launch excess, larger): the bound passes, the position fails
        pairs, w = self.make_set(over={"p1on": dict(hud=85.4)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("time to HUD (primary) +2.40 s from its arm's median", v)
        # ...and in the FAVOURABLE direction (the first ON roll 2.3 s fast lowers the mean): still a position effect
        pairs, w = self.make_set(over={"p1on": dict(hud=80.7)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("time to HUD (primary) -2.30 s from its arm's median", v)

    def test_position_within_its_margin_is_reported(self):
        pairs, w = self.make_set(warm=dict(hud=84.4))
        out = self.run_raw(pairs, w).stdout
        pos = [ln for ln in out.splitlines() if ln.startswith("position, per endpoint:")]
        self.assertEqual(len(pos), 1)
        self.assertIn("hud 83.30 (median of the other ON rolls 83.00, dev +0.30)", pos[0])
        self.assertIn("the warm-up (the invocation's first launch) HUD 84.40 s", pos[0])
        self.assertIn("-> EQUIVALENT", out.strip().splitlines()[-1])

    def test_warmup_is_never_graded(self):
        # bat1's first-roll effect, and worse: the warm-up 30 s slow with a 20 s backend wait - the set stays clean
        pairs, w = self.make_set(warm=dict(hud=113.0, attach_at=21.3))
        v = self.run_tool(pairs, w)
        self.assertIn("-> EQUIVALENT", v)

    def test_design_faults_are_inconclusive(self):
        for over, word in (({"p4on": dict(exe="b" * 64)}, "exe hashes"),
                           ({"p6off": dict(exe_path=r"D:\Games\sr\SupermanReturns.exe")}, "exe paths"),
                           ({"p2off": dict(extra_env={"SR_NATIVE_TRANSPORT": "1"})}, "SR_NATIVE_TRANSPORT"),
                           ({"p8on": dict(no_seed=True)}, "save seeding not recorded"),
                           ({"p1on": dict(engaged=False)}, "pair 1 ON roll's gate state")):
            pairs, w = self.make_set(over=over)
            v = self.run_tool(pairs, w)
            self.assertIn("-> INCONCLUSIVE", v, word)
            self.assertIn(word, v)
        pairs, w = self.make_set()
        shutil.rmtree(pairs[7][1][1])  # the last roll never ran
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("not run (no roll folder)", v)
        pairs, w = self.make_set()
        same = list(pairs)
        same[4] = [pairs[4][0], ("on", pairs[3][1][1])]
        self.assertIn("path used twice", self.run_tool(same, w))

    def test_off_arm_stall_is_inconclusive(self):
        # an OFF roll 6 s slow: ON - OFF -6 s; a negative outlier must not help the bound
        pairs, w = self.make_set(over={"p3off": dict(hud=89.0)})
        v = self.run_tool(pairs, w)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("an OFF-arm stall", v)


# -- the mutant check: each guard is load-bearing (a mutated evaluator must break the test that guards it)
# (what, file, target, replacement, the guarding tests)
MUTANTS = (
    ("a normal quantile for Student's t", "boot_equiv.py",
     'return T95.get(df, 1.6449) if df >= 1 else float("nan")', "return 1.6449", ["test_student_t_boundary"]),
    ("no stall cap", "boot_equiv.py", "STALL_CAP = 5.0", "STALL_CAP = 50.0",
     ["test_single_stall_is_caught_by_the_cap", "test_off_arm_stall_is_inconclusive"]),
    ("a bound without the spread", "boot_equiv.py", "return m, s, m + t95(n - 1) * s / math.sqrt(n)", "return m, s, m",
     ["test_under_powered_set_is_inconclusive"]),
    ("a start-only warm-up check (P2-7)", "boot_equiv.py", "elif firsts and not wlast < min(firsts):",
     "elif firsts and not _wfirst < min(firsts):", ["test_warmup_still_running_at_the_first_graded_launch"]),
    ("no warm-up end check", "boot_equiv.py", "elif firsts and not wlast < min(firsts):", "elif False:",
     ["test_misordered_set_is_inconclusive", "test_warmup_still_running_at_the_first_graded_launch"]),
    ("a crash not OPEN", "boot_equiv.py", '        if r["open"]:\n            opens.append',
     '        if False:\n            opens.append', ["test_crash_is_open"]),
    ("no design-size check", "boot_equiv.py", "    if declared != DESIGN:", "    if False:",
     ["test_under_powered_set_is_inconclusive"]),
    ("a mean over its margin not OPEN", "boot_equiv.py", "        if over:", "        if False:", ["test_real_gap_is_open"]),
    ("the tag on every verdict", "boot_equiv.py", 'stem = TAG if verdict == "EQUIVALENT" else "BOOT_SET"', "stem = TAG",
     ["test_crash_is_open"]),
    ("the race not OPEN", "boot_equiv.py", '    if r.get("swaps_before") or r.get("queued_before"):', "    if False:",
     ["test_black_world_race_is_open"]),
    ("the run's identity ignored (P1-2)", "boot_equiv.py", '    design += run["problems"]', "    pass",
     ["test_warmup_rules", "test_foreign_proof_rows_are_refused", "test_another_runs_identity_is_refused"]),
    ("a failed run row not OPEN (P1-2)", "boot_equiv.py", '            elif v not in ("DONE", "SKIP"):',
     "            elif False:", ["test_failed_row_with_outcome_success_is_open"]),
    ("abort markers ignored (P1-2)", "boot_equiv.py", "    what, line = bp.abort_marker(path)\n    if what:",
     "    what, line = bp.abort_marker(path)\n    if False:", ["test_abort_marker_with_outcome_success_is_open"]),
    ("no absolute ON attach rule (P1-1)", "boot_equiv.py", '        if r["arm"] == "on" and late:',
     "        if False:", ["test_late_attach_in_both_arms_is_open"]),
    ("no position rule (P2-8)", "boot_equiv.py", "                if not abs(dev) <= margin:", "                if False:",
     ["test_position_effect_per_endpoint"]),
    ("a one-sided position rule (ON-first 'conservative', P2-8)", "boot_equiv.py",
     "                if not abs(dev) <= margin:", "                if not dev <= margin:",
     ["test_position_effect_per_endpoint"]),
    ("the stall bound claimed (P3-9)", "boot_equiv.py", "the stall claim is BOOT_STALL's",
     "the stall rate is < 31% at 95%", ["test_clean_set_is_equivalent"]),
    ("proof rows matched by basename (P1-2)", "proof_rows.py",
     'rows[key(os.path.join(umbrella, rel.replace("/", os.sep)))] = row',
     "rows[key(os.path.join(os.path.dirname(js), os.path.basename(rel)))] = row",
     ["test_foreign_proof_rows_are_refused"]),
    ("a proof JSON from another folder accepted (P1-2)", "proof_rows.py",
     'if key(doc.get("out") or "") != os.path.dirname(key(js)):', "if False:",
     ["test_foreign_proof_rows_are_refused"]),
    ("the run's exe not checked (P1-2)", "proof_rows.py", "if not exe_id or not sha.startswith(exe_id):", "if False:",
     ["test_another_runs_identity_is_refused"]),
    ("a recorded 0.0 ms wait trusted (P2-6)", "boot_pairs.py",
     'if "waited" in r and "cpup" in r and "attach" in r and r.get("waitst") == "attached":', "if False:",
     ["test_recorded_zero_wait_cannot_hide_a_stall"]),
    # the bake's design (native/devcreate-fix): its pair count, its warm-up rule, its reading of the gate
    ("the bake design's pair count", "boot_equiv.py", '"dcf-v1": {"pairs": 4, "baked": True}',
     '"dcf-v1": {"pairs": 3, "baked": True}', ["test_bake_clean_four_pairs_is_equivalent"]),
    ("the bake's warm-up pin unchecked", "boot_equiv.py", '                if bp.GATE in W["env"]:',
     "                if False:", ["test_bake_pinned_warmup_is_inconclusive"]),
    ("the bake's OFF arm read as the old design's", "boot_pairs.py", '        return "off" if v == "0" else "on"',
     '        return "on" if v == "1" else "off"', ["test_bake_clean_four_pairs_is_equivalent"]),
)
COPIED = ("boot_equiv.py", "boot_pairs.py", "boot_stall.py", "proof_rows.py")


@unittest.skipIf(os.environ.get("BOOT_EQUIV_TOOL"), "inside a mutant run")
class Mutants(unittest.TestCase):
    def test_every_mutant_breaks_its_test(self):
        texts = {}
        for name in COPIED:
            with open(os.path.join(HERE, name), encoding="utf-8") as f:
                texts[name] = f.read().replace("\r\n", "\n")
        root = os.path.join(REPO, "archive")
        os.makedirs(root, exist_ok=True)
        tmp = tempfile.mkdtemp(prefix="boot_equiv_mutants_", dir=root)
        try:
            for what, name, old, new, tests in MUTANTS:
                self.assertEqual(texts[name].count(old), 1, "mutant %r: its target is not exactly once in %s" % (
                    what, name))
                # a mutant of the same byte length written within the same second as the previous copy would be served
                # from the previous copy's .pyc (Python validates size + mtime): no cache between mutants
                shutil.rmtree(os.path.join(tmp, "__pycache__"), ignore_errors=True)
                for other in COPIED:
                    with open(os.path.join(tmp, other), "w", encoding="utf-8") as f:
                        f.write(texts[other].replace(old, new) if other == name else texts[other])
                env = dict(os.environ, BOOT_EQUIV_TOOL=os.path.join(tmp, "boot_equiv.py"))
                p = subprocess.run([sys.executable, "-m", "unittest"] + ["test_boot_equiv.BootEquiv." + t for t in tests],
                                   cwd=HERE, env=env, capture_output=True, text=True)
                self.assertNotEqual(p.returncode, 0, "mutant %r survived %s:\n%s" % (what, tests, p.stderr[-800:]))
                # killed by an assertion in a guarding test - never by an import error or a crash (a vacuous kill)
                self.assertRegex(p.stderr, r"FAILED \(failures=\d+\)", "%s:\n%s" % (what, p.stderr[-800:]))
                self.assertTrue(any(("FAIL: %s " % t) in p.stderr for t in tests), what)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
