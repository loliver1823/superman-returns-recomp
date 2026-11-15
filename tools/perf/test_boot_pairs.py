#!/usr/bin/env python3
"""test_boot_pairs.py - tools/perf/boot_pairs.py's rule on synthetic rolls (dcb, 2026-10-14).

    python -m unittest discover -s tools/perf -p test_boot_pairs.py

Astra's round-1 counterexamples must not pass: devcreate lengthening the boot load by 30 s (the old H' rule called
that "gap within noise"), a post-attach regression, a missing boot-load line in an OFF roll, a TIMED OUT wait, unequal
arms, a repeated path, two exe hashes, an unverified gate, the wrong order. Astra's round-2 ones neither: a roll
without a parsed save seed (all of them unseeded included), a roll without a run.log (INCONCLUSIVE with the evidence
named, not a KeyError), a nan / infinite / negative budget (rejected). A clean set prints at most
"NO REGRESSION OBSERVED IN THESE PAIRS"; no output ever says "gap within noise" or RESOLVED.

The async load (native/devcreate-close, 2026-10-14): a load on the loader thread may end before the swap chain (J:)
or after the attach (a slow disk) without the set going INCONCLUSIVE, and its whole recorded wait is uncovered; the
same timestamps without the loader-thread line (an older exe, SR_SHADER_LOAD_ASYNC=0, a load that RAN ON THIS
THREAD) keep the old order rule; a load-done line after the HUD is still read. Scratch rolls go under the checkout's
archive/ (never C:).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(HERE, "boot_pairs.py")
MODE_LINE = {
    "async": "[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe of J:\\x\\dxil "
             "says an installed cache (probe 0.1 ms, from process +69.4 ms); the load runs on the loader thread, the CP "
             "waits for its installed stage (process +69.5 ms)",
    "inline": "[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe of J:\\x\\dxil "
              "says an installed cache (probe 0.1 ms, from process +69.4 ms); the load RAN ON THIS THREAD (no thread "
              "could be created): the attach waited for it (process +69.5 ms)",
    "sync": "[SHADER_CACHE] SR_SHADER_LOAD_ASYNC=0: the boot load runs inside the backend init, before the attach (the "
            "old order)",
}


def ts(base_s, sec):
    s = base_s + sec
    h, rem = divmod(s, 3600)
    m, s2 = divmod(rem, 60)
    return "2026-10-14 %02d:%02d:%06.3f" % (h, m, s2)


class BootPairs(unittest.TestCase):
    def setUp(self):
        root = os.path.join(REPO, "archive")
        os.makedirs(root, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="boot_pairs_test_", dir=root)
        self.clock = 6 * 3600  # 06:00:00, each roll 5 minutes after the last

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def roll(self, name, on, load=0.2, hud=83.0, attach_after_load=0.1, cpup=1.3, exe="a" * 64, timeout=False,
             no_load_line=False, result="SUCCESS", engaged=None, extra_env=None, no_seed=False, no_run_log=False,
             mode=None, load_end_at=None, attach_at=None, swaps=0, load_ms=None):
        """mode: None (no load-mode line: an exe before render/boot-attach), 'async' (the loader thread), 'inline'
        (RAN ON THIS THREAD) or 'sync' (SR_SHADER_LOAD_ASYNC=0). load_end_at / attach_at place the load's end and the
        attach independently (the async load keeps no order with them)."""
        d = os.path.join(self.tmp, name, "drive", "attempt1")
        os.makedirs(d)
        env = {"SR_NATIVE_SEGMENTLESS": "1", "SR_LOG": "player", "SR_PERF_DIR": os.path.join(d, "perf")}
        if on:
            env["SR_NATIVE_DEVCREATE"] = "1"
        env.update(extra_env or {})
        with open(os.path.join(d, "provenance.json"), "w") as f:
            json.dump({"exeSha256": exe, "effectiveSrEnv": env}, f)
        with open(os.path.join(d, "outcome.json"), "w") as f:
            json.dump({"result": result}, f)
        b = self.clock
        self.clock += 300
        with open(os.path.join(d, "steps.log"), "w") as f:
            f.write("[06:00:00.000] roll start\n")
            if not no_seed:
                f.write("[06:00:00.000] saves: SR_CONTENT_DIR=%s (empty)\n" % os.path.join(d, "content"))
        swap = 0.4
        load_end = swap + 0.1 + load if load_end_at is None else load_end_at
        attach = load_end + attach_after_load if attach_at is None else attach_at
        lines = [(0.0, "[LOG_PROFILE] player"), (swap, "[HIGHFPS] swap chain ALLOW_TEARING on"), (cpup,
                 "[GPU_CP] command processor thread up (pcr=D00E1800 guestCaller=ok)")]
        if mode:
            lines.append((0.05, MODE_LINE[mode]))
        if (on if engaged is None else engaged):
            lines.append((cpup - 0.01, "[NATIVE_DEVCREATE] ring init native: dev=40002880"))
        if not no_load_line:
            lines.append((load_end, "[SHADER_CACHE] boot load done: %.1f ms on thread 1 (main thread 1): x" % (
                1000 * min(load + 0.1, load_end - 0.05) if load_ms is None else load_ms)))
        if timeout:
            lines.append((cpup + 60.0, "[DISPLAY] CP waited 60000.2 ms for the live backend (TIMED OUT)"))
        lines.append((attach, "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %d, draw commands queued 0"
                      % swaps))
        if attach > cpup and not timeout:
            lines.append((attach + 0.001, "[DISPLAY] CP waited %.1f ms for the live backend (attached)" % (
                1000 * (attach - cpup))))
        lines.append((max(attach, cpup) + 5, "[GPU_CP] census rptr=1 wptr=2 doorbells=3 packets=%d draws=4 " % (
            0 if (on if engaged is None else engaged) else 1)))
        if result == "SUCCESS":
            lines.append((hud, "[APT_REQ] #17 path='screens/hud/main_hud' holder=0"))
        lines.sort()
        if not no_run_log:
            with open(os.path.join(d, "run.log"), "w") as f:
                for t, text in lines:
                    f.write("[%s] [INFO ] [BOOT] %s\n" % (ts(b, t), text))
        return os.path.join(self.tmp, name)

    def run_raw(self, pairs, budget=None):
        cmd = [sys.executable, TOOL]
        for p in pairs:
            cmd += ["--pair"] + ["%s:%s" % (arm, path) for arm, path in p]
        if budget is not None:
            cmd += ["--budget=%s" % budget]  # the = form: "-nan" would otherwise read as an option
        return subprocess.run(cmd, capture_output=True, text=True)

    def run_tool(self, pairs, budget=None):
        p = self.run_raw(pairs, budget)
        self.assertEqual(p.returncode, 0, p.stderr)  # a crash (e.g. a KeyError) is never a verdict
        out = p.stdout
        self.assertNotIn("gap within noise", out)
        self.assertNotIn("RESOLVED", out)
        self.assertNotIn("BOOT_EQUIV", out)
        return out.strip().splitlines()[-1]

    def clean_set(self, over=None, both=None, **on_kw):
        """Six rolls created (= run) in the declared order p1 ON/OFF, p2 OFF/ON, p3 ON/OFF; over = {name: kwargs};
        both = kwargs for every roll (the ON rolls' on_kw and over win)."""
        over = over or {}
        self.sets = getattr(self, "sets", 0) + 1
        mk = lambda name, on: self.roll("s%d_%s" % (self.sets, name), on,
                                        **dict(both or {}, **dict(on_kw if on else {}, **over.get(name, {}))))
        p1 = [("on", mk("p1on", True)), ("off", mk("p1off", False))]
        p2 = [("off", mk("p2off", False)), ("on", mk("p2on", True))]
        p3 = [("on", mk("p3on", True)), ("off", mk("p3off", False))]
        return [p1, p2, p3]

    def test_clean_set(self):
        v = self.run_tool(self.clean_set())
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)

    def test_devcreate_lengthening_the_load_is_open(self):
        # Astra's counterexample: +30 s boot load in ON, the wait and the HUD both move by it; H' would hide it
        v = self.run_tool(self.clean_set(load=30.2, hud=113.0))
        self.assertIn("-> OPEN", v)

    def test_post_attach_regression_is_open(self):
        v = self.run_tool(self.clean_set(hud=90.0))
        self.assertIn("-> OPEN", v)
        self.assertIn("HUD", v)

    def test_missing_load_line_in_off_is_inconclusive(self):
        v = self.run_tool(self.clean_set({"p1off": {"no_load_line": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no loaddone line", v)

    def test_timeout_is_open(self):
        v = self.run_tool(self.clean_set({"p3on": {"load": 70.0, "hud": 150.0, "timeout": True}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("TIMED OUT", v)

    def test_crash_is_open(self):
        self.assertIn("-> OPEN", self.run_tool(self.clean_set({"p2on": {"result": "FAILURE"}})))

    def test_design_violations_are_inconclusive(self):
        pairs = self.clean_set()
        same = [pairs[0], [("off", pairs[0][1][1]), pairs[1][1]], pairs[2]]
        self.assertIn("path used twice", self.run_tool(same))
        self.assertIn("exe hashes", self.run_tool(self.clean_set({"p3off": {"exe": "b" * 64}})))
        self.assertIn("gate state", self.run_tool(self.clean_set({"p1on": {"engaged": False}})))
        self.assertIn("not one ON and one OFF", self.run_tool([[pairs[0][0], pairs[1][1]]]))
        swapped = [[pairs[0][1], pairs[0][0]], pairs[1], pairs[2]]  # declared OFF first, ran ON first
        self.assertIn("declared order", self.run_tool(swapped))
        self.assertIn("configuration differs",
                      self.run_tool(self.clean_set({"p2off": {"extra_env": {"SR_NATIVE_TRANSPORT": "1"}}})))

    # Astra round 2 (2026-10-14)
    def test_missing_seed_is_inconclusive(self):
        one = self.run_tool(self.clean_set({"p2on": {"no_seed": True}}))
        self.assertIn("INCONCLUSIVE", one)
        self.assertIn("save seeding not recorded", one)
        self.assertIn("s%d_p2on" % self.sets, one)
        # every roll unseeded: {None} has one element, which the old rule passed
        none = self.run_tool(self.clean_set({n: {"no_seed": True} for n in
                                             ("p1on", "p1off", "p2off", "p2on", "p3on", "p3off")}))
        self.assertIn("INCONCLUSIVE", none)
        self.assertIn("save seeding not recorded", none)

    def test_missing_run_log_is_inconclusive(self):
        v = self.run_tool(self.clean_set({"p1off": {"no_run_log": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no run.log", v)
        self.assertIn("s%d_p1off" % self.sets, v)
        v = self.run_tool(self.clean_set({"p3on": {"no_run_log": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no run.log", v)

    def test_bad_budget_is_rejected(self):
        pairs = self.clean_set(load=30.2, hud=113.0)  # +30 s in ON: nan or a negative budget must not hide it
        for b in ("nan", "-nan", "inf", "-1", "-0.5", "x"):
            p = self.run_raw(pairs, b)
            self.assertNotEqual(p.returncode, 0, "budget %s accepted" % b)
            self.assertNotIn("NO REGRESSION", p.stdout)
            self.assertIn("budget", p.stderr)
        self.assertIn("-> OPEN", self.run_tool(pairs, "0"))
        self.assertIn("-> OPEN", self.run_tool(pairs, "1.0"))

    # the async load (render/boot-attach; native/devcreate-close 2026-10-14)
    def test_async_load_ending_before_the_swap_chain(self):
        # bat1 on J: the loader thread finished (0.30 s) before the swap chain (0.40 s); the attach at 0.70 s
        fast = dict(mode="async", load_end_at=0.3, attach_at=0.7)
        v = self.run_tool(self.clean_set(both=fast))
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        self.assertNotIn("out of order", v)

    def test_async_load_ending_after_the_attach(self):
        # a slow disk: the attach at 0.70 s (before the CP's start: no wait), the load done at 20 s
        slow = dict(mode="async", load=19.5, load_end_at=20.0, attach_at=0.7)
        v = self.run_tool(self.clean_set(both=slow))
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        # ...and its done line printed after the HUD and the census is still read (no 'no loaddone line')
        late = dict(mode="async", load=89.5, load_end_at=90.0, attach_at=0.7)
        v = self.run_tool(self.clean_set(both=late))
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)

    def test_the_old_order_is_kept_without_the_loader_thread(self):
        # the same timestamps without the loader-thread line: the load ran inside the backend init, so a load end
        # before the swap chain or after the attach is an inconsistent log, as before
        for mode in (None, "sync", "inline"):
            v = self.run_tool(self.clean_set(both=dict(mode=mode, load_end_at=0.3, attach_at=0.7)))
            self.assertIn("INCONCLUSIVE", v, mode)
            self.assertIn("timestamps out of order (swap 0.40, load end 0.30, attach 0.70)", v, mode)
            v = self.run_tool(self.clean_set(both=dict(mode=mode, load=19.5, load_end_at=20.0, attach_at=0.7)))
            self.assertIn("INCONCLUSIVE", v, mode)
            self.assertIn("timestamps out of order", v, mode)
        # an async roll whose attach precedes the swap chain is still an inconsistent log
        v = self.run_tool(self.clean_set({"p2on": dict(mode="async", load_end_at=0.3, attach_at=0.35)},
                                         both=dict(mode="async", load_end_at=0.3, attach_at=0.7)))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("timestamps out of order (swap 0.40, attach 0.35; the load on the loader thread)", v)

    def test_async_wait_is_uncovered(self):
        # every roll's CP waits 3 s for the attach (CP up 1.3 s, attach 4.3 s) while its load runs 0.4-4.25 s.
        # Inside the backend init (the old order) the load covers that wait: U ~0.05 s, no regression in these pairs.
        covered = dict(mode="sync", load=3.65, load_end_at=4.25, attach_at=4.3)
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", self.run_tool(self.clean_set(both=covered)))
        # On the loader thread the load is off the attach's path: the 3 s are uncovered in each ON roll -> OPEN.
        uncovered = dict(covered, mode="async")
        v = self.run_tool(self.clean_set(both=uncovered))
        self.assertIn("-> OPEN", v)
        self.assertIn("ON uncovered wait 3.00 3.00 3.00", v)

    # Astra round 1 on native/devcreate-close (P2-6)
    def test_recorded_zero_wait_must_match_the_interval(self):
        # "CP waited 0.0 ms" beside 25 s between CP up and the attach: an inconsistent log, and off the attach's path
        # the uncovered wait is the interval itself
        bad = dict(mode="async", load_end_at=0.3, attach_at=26.3, cpup=1.3)
        sets = self.clean_set(both=bad)
        for pr in sets:
            for _arm, path in pr:
                log = os.path.join(path, "drive", "attempt1", "run.log")
                text = open(log).read().replace("CP waited 25000.0 ms", "CP waited 0.0 ms")
                open(log, "w").write(text)
        v = self.run_tool(sets)
        self.assertIn("INCONCLUSIVE", v)  # the diagnostic: an inconsistent log is not evidence
        self.assertIn("recorded wait 0.00 s vs [CP up, attach] 25.00 s", v)

    def test_arm_of_under_both_gate_defaults(self):
        # native/devcreate-fix: the gate's default decides what an unset key reads as; an A/B roll is neither arm
        sys.path.insert(0, HERE)
        import boot_pairs as bp
        on, off, unset = {"SR_NATIVE_DEVCREATE": "1"}, {"SR_NATIVE_DEVCREATE": "0"}, {}
        self.assertEqual([bp.arm_of(unset, False), bp.arm_of(on, False), bp.arm_of(off, False)], ["off", "on", "off"])
        self.assertEqual([bp.arm_of(unset, True), bp.arm_of(on, True), bp.arm_of(off, True)], ["on", "on", "off"])
        self.assertIsNone(bp.arm_of({"SR_AB_TOGGLE": "SR_NATIVE_DEVCREATE", "SR_AB_PRESENT": "1"}, True))
        self.assertIsNone(bp.arm_of({"SR_AB_TOGGLE": "SR_NATIVE_GATE+SR_NATIVE_DEVCREATE"}, False))
        self.assertTrue(bp.declared_on_boot(unset, True))
        self.assertFalse(bp.declared_on_boot(unset, False))
        self.assertFalse(bp.declared_on_boot({"SR_NATIVE_FLUSH_CHECK": "1"}, True))
        self.assertFalse(bp.declared_on_boot(off, True))

    def test_infeasible_load_is_inconclusive(self):
        # a 20 s load "done" 0.3 s after the log's first line
        v = self.run_tool(self.clean_set(both=dict(mode="async", load_ms=20000.0, load_end_at=0.3, attach_at=0.7)))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("infeasible load: 20.00 s done at +0.30 s", v)


if __name__ == "__main__":
    unittest.main()
