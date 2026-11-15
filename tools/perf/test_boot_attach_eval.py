#!/usr/bin/env python3
"""test_boot_attach_eval.py - tools/perf/boot_attach_eval.py's rule on synthetic rolls (render/boot-attach).

    python -m unittest discover -s tools/perf -p test_boot_attach_eval.py

Mirrors native/devcreate-boot's test_boot_pairs.py (the rule's design / marker / OPEN / budget checks are the same as
that branch's boot_pairs.py, an in-repo tool), plus Astra's round-1 counterexamples against the first version of this evaluator: one pair, three
ON-first pairs, a different SR_HIGHFPS, a missing "CP waited" line read as a 0 s wait, a crash or timeout without
markers (OPEN, never INCONCLUSIVE), a nan / infinite / negative budget (rejected). No output ever says the attach "did
not wait for the disk", RESOLVED, faster or equivalent. Scratch rolls go under the checkout's archive/ (never C:).
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
TOOL = os.path.join(HERE, "boot_attach_eval.py")
sys.path.insert(0, HERE)
import proof_rows as prs  # noqa: E402  (UMBRELLA: the rows' paths are relative to it, as Runner.rel writes them)
PROC0 = 0.05  # the log's first line at process +50 ms


def ts(base_s, sec):
    s = base_s + sec
    h, rem = divmod(s, 3600)
    m, s2 = divmod(rem, 60)
    return "2026-10-14 %02d:%02d:%06.3f" % (h, m, s2)


class BootAttachEval(unittest.TestCase):
    def setUp(self):
        root = os.path.join(REPO, "archive")
        os.makedirs(root, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="boot_attach_eval_test_", dir=root)
        self.clock = 6 * 3600  # 06:00:00, each roll 5 minutes after the last

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def roll(self, name, on, s1=2.0, s2=0.5, init=0.9, cpup=1.0, hud=83.0, exe="a" * 64, result="SUCCESS",
             extra_env=None, flag=None, no_seed=False, no_run_log=False, no_wait_line=False, wait_status="attached",
             inline=False, swaps=0, wp1="loaded", truncate_after=None, no_proc_fields=False, wait_skew=0.0,
             off_attach_early=False, fatal=False, attach_cache=None, wait_ms=None, first_delay=0.05,
             first_backend="attached", host_stalls=0, load_on_main=False, extra_lines=None):
        d = os.path.join(self.tmp, name, "drive", "attempt1")
        os.makedirs(d)
        env = {"SR_HIGHFPS": "uncapped", "SR_HIGHFPS_PACE": "1", "SR_INSTANCE_ID": "native",
               "SR_PERF_DIR": os.path.join(d, "perf"), "SR_CONTENT_DIR": os.path.join(d, "content")}
        if flag is not None:
            env["SR_SHADER_LOAD_ASYNC"] = flag
        elif not on:
            env["SR_SHADER_LOAD_ASYNC"] = "0"
        env.update(extra_env or {})
        with open(os.path.join(d, "provenance.json"), "w") as f:
            json.dump({"exeSha256": exe, "effectiveSrEnv": env}, f)
        if result is not None:
            with open(os.path.join(d, "outcome.json"), "w") as f:
                json.dump({"result": result}, f)
        b = self.clock
        self.clock += 300
        with open(os.path.join(d, "steps.log"), "w") as f:
            f.write("[06:00:00.000] roll start\n")
            if not no_seed:
                f.write("[06:00:00.000] saves: SR_CONTENT_DIR=%s (empty)\n" % os.path.join(d, "content"))
        P = lambda t: 1000.0 * (t + PROC0)  # log seconds -> process ms
        pf = (lambda t: "") if no_proc_fields else None
        begin = 0.4
        L = [(0.0, "[LOG_PROFILE] player"), (cpup, "[GPU_CP] command processor thread up (pcr=D00E1800 guestCaller=ok)")]
        if on:
            L.append((begin, "[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe "
                             "of D:\\sr\\data\\shader_cache\\dxil says an installed cache (probe 1.3 ms, from process "
                             "+%.1f ms); the load %s (process +%.1f ms)" % (
                                 P(begin), "RAN ON THIS THREAD (no thread could be created): the attach waited for it"
                                 if inline else "runs on the loader thread, the CP waits for its installed stage",
                                 P(begin + 0.0015))))
            load0 = begin + 0.001
            tid, tname = (4242, "tid 4242") if inline or load_on_main else (5150, "SR shader load")
            s1_end = load0 + s1
            s2_end = s1_end + s2
            init_end = (s2_end if inline else begin) + init
        else:
            L.append((begin, "[SHADER_CACHE] SR_SHADER_LOAD_ASYNC=0: the boot load runs inside the backend init, "
                             "before the attach (the old order)"))
            load0 = begin + 0.5  # createXlatPipeline, after the device
            tid, tname = 4242, "tid 4242"
            s1_end = load0 + s1
            s2_end = s1_end + s2
            init_end = s2_end + 0.1
        # the dev merge's line set (dcb's per-load fields + the stage ends); main thread 4242
        L.append((load0, "[SHADER_CACHE] boot load: started at process +%.1f ms on thread %d (main thread 4242), "
                         "named '%s'" % (P(load0), tid, tname)))
        L.append((s1_end, "[SHADER_CACHE] boot load: installed stage %.1f ms (100 VS + 100 PS, 300 files read), ended "
                          "at process +%.1f ms, on thread %d (main thread 4242) - 300 wrapped read calls, 6.70 MiB "
                          "returned, wrapper time 900.0 ms, longest call 40.0 ms, 12 calls over 5 ms; uncovered by the "
                          "wrapper: 100.0 ms (directory enumeration, parsing, allocation)" % (1000 * s1, P(s1_end), tid)))
        L.append((s2_end - 0.01, "[SHADER_CACHE] boot load: runtime translator - LoadLibrary dxil.dll 5.0 ms, "
                                 "dxcompiler.dll 20.0 ms, DLL hash 30.0 ms (18.00 MiB), persisted-dir scan 3.0 ms (0 "
                                 "admitted, 0 shadowed, 0 unreadable)"))
        L.append((s2_end, "[SHADER_CACHE] boot load done: %.1f ms on thread %d (main thread 4242): installed cache %.1f "
                          "ms, runtime translator %.1f ms - 0 wrapped read calls, 0.00 MiB returned, wrapper time 0.0 "
                          "ms, longest call 0.0 ms, 0 calls over 5 ms; uncovered by the wrapper: %.1f ms (the DXC load "
                          "+ hash on their own line, the translator's other work); ended at process +%.1f ms" % (
                              1000 * (s1 + s2), tid, 1000 * s1, 1000 * s2, 1000 * s2, P(s2_end))))
        L.append((init_end, "[DISPLAY] backend init %.1f ms (device, swap chain, PSOs, xlat objects; process +%.1f to "
                            "+%.1f ms); the shader cache load is x" % (1000 * (init_end - begin), P(begin), P(init_end))))
        attach = init_end + 0.01
        if off_attach_early:
            attach = s1_end - 0.2
        if no_proc_fields:
            L.append((attach, "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %d, draw commands queued 0"
                      % swaps))
        else:
            cache = attach_cache or ("loaded" if s1_end <= attach else "still loading")
            L.append((attach, "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %d, draw commands queued 0; "
                              "process +%.1f ms; installed shader cache just after the attach: %s" % (
                                  swaps, P(attach), cache)))
        if attach > cpup and not no_wait_line:
            wait = 1000 * (attach - cpup) + 1000 * wait_skew if wait_ms is None else wait_ms
            L.append((attach + 0.001, "[DISPLAY] CP waited %.1f ms for the live backend (%s), 0 still-waiting warnings"
                      % (wait, wait_status)))
        for k in range(host_stalls):
            L.append((cpup + 60.0 + 300.0 * k, "[HOST_WAIT_STALL] ==== the CP has waited %d s for the live backend's "
                                               "attach (escalation #%d; ...)" % (60 + 300 * k, k + 1)))
        if fatal:
            L.append((attach + 0.002, "[FATAL] the live D3D12 backend is not there: x"))
        cpready = max(attach, s1_end) + 0.001
        L.append((cpready, "[SHADER_CACHE] CP start: waited %.1f ms for the installed shader cache (%s; translator stage "
                           "%s), ready after WP1 at process +%.1f ms" % (
                               1000 * max(0.0, s1_end - attach), wp1, "done" if s2_end <= cpready else "still loading",
                               P(cpready))))
        if first_delay is not None:
            cpfirst = cpready + first_delay
            L.append((cpfirst, "[GPU_CP] first command consumed at process +%.1f ms (rptr 0, wptr 64; the live "
                               "backend %s)" % (P(cpfirst), first_backend)))
        if result == "SUCCESS":
            L.append((hud, "[APT_REQ] #17 path='screens/hud/main_hud' holder=0"))
        L += list(extra_lines or [])
        L.sort()
        if truncate_after is not None:
            L = [x for x in L if x[0] <= truncate_after]
        if not no_run_log:
            with open(os.path.join(d, "run.log"), "w") as f:
                for t, text in L:
                    f.write("[%s] [INFO ] [BOOT] %s\n" % (ts(b, t), text))
        return os.path.join(self.tmp, name)

    def run_raw(self, pairs, budget=None, warmup=None):
        cmd = [sys.executable, TOOL]
        for p in pairs:
            cmd += ["--pair"] + ["%s:%s" % (arm, path) for arm, path in p]
        if budget is not None:
            cmd += ["--budget=%s" % budget]  # the = form: "-nan" would otherwise read as an option
        if warmup is not None:
            cmd += ["--warmup", warmup]
        return subprocess.run(cmd, capture_output=True, text=True)

    def run_tool(self, pairs, budget=None, warmup=None, full=False):
        p = self.run_raw(pairs, budget, warmup)
        self.assertEqual(p.returncode, 0, p.stderr)  # a crash (e.g. a KeyError) is never a verdict
        out = p.stdout
        for banned in ("did not wait for the disk", "does not wait for the disk", "RESOLVED", "faster", "equivalent",
                       "within noise"):
            self.assertNotIn(banned, out)
        return out if full else out.strip().splitlines()[-1]

    def clean_set(self, over=None, warm=None, warm_after=None, mirror=False, split_proof=False, **on_kw):
        """Six rolls created (= run) in the declared order p1 ON/OFF, p2 OFF/ON, p3 ON/OFF (mirror: p1 OFF/ON, p2
        ON/OFF, p3 OFF/ON); over = {name: kwargs}. warm = kwargs of a declared warm-up roll (the default build, ON)
        created first - or after pair `warm_after` - returned as self.warm."""
        over = over or {}
        self.sets = getattr(self, "sets", 0) + 1
        mk = lambda name, on: self.roll("s%d_%s" % (self.sets, name), on,
                                        **dict(on_kw if on else {}, **over.get(name, {})))
        mkw = lambda: self.roll("s%d_warmup" % self.sets, True, **warm)
        self.warm = mkw() if warm is not None and warm_after is None else None
        order = [("off", "on"), ("on", "off"), ("off", "on")] if mirror else [("on", "off"), ("off", "on"), ("on", "off")]
        pairs = []
        for k, arms in enumerate(order):
            pairs.append([(arm, mk("p%d%s" % (k + 1, arm), arm == "on")) for arm in arms])
            if warm is not None and warm_after == k + 1:
                self.warm = mkw()
        rolls = [p for pr in pairs for _a, p in pr]
        if self.warm and split_proof:  # the warm-up in its own native_proof run (bat2w), the set in another (bat2)
            self.proof("s%d_w_proof.json" % self.sets, [self.warm])
            self.proof("s%d_bat_proof.json" % self.sets, rolls)
        elif self.warm:
            self.proof("s%d_bat_proof.json" % self.sets, [self.warm] + rolls)
        return pairs

    def proof(self, name, rolls, verdicts=None, out=None, rel_dir=None):
        """A native_proof proof JSON beside the rolls, as Runner.write_proof writes it: its out (this folder), exe id,
        start time and one run's roll rows naming each roll by its umbrella-relative path (Runner.rel). verdicts:
        {roll path: row verdict}; out / rel_dir: a foreign run's (another folder; rows under another folder)."""
        def rel(p):
            if rel_dir is not None:
                return rel_dir + "/" + os.path.basename(p)
            return os.path.relpath(p, prs.UMBRELLA).replace("\\", "/")
        steps = [{"step": os.path.basename(p), "kind": "roll", "verdict": (verdicts or {}).get(p, "DONE"),
                  "detail": "perf_run exit 0", "paths": {"roll": rel(p)}} for p in rolls]
        with open(os.path.join(self.tmp, name), "w") as f:
            json.dump({"plan": "boot_attach", "prefix": "bat", "out": out or self.tmp, "exe_id": "a" * 12,
                       "started": "2026-10-14T05:00:00", "steps": steps}, f)

    # the clean set and its reported bounds
    def test_clean_set(self):
        v = self.run_tool(self.clean_set())
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        self.assertIn("attach <= 1360", v)  # (0.4 + 0.9 + 0.01 + 0.05) s
        self.assertIn("probe <= 1.3 ms", v)
        self.assertIn("loader thread started <= 451", v)
        self.assertIn("in 3 of 3 ON rolls", v)

    def test_fast_load_witnesses_nothing(self):
        v = self.run_tool(self.clean_set(s1=0.2))  # stage 1 ends before the attach: no witness, no regression
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        self.assertIn("in 0 of 3 ON rolls", v)

    # Astra round 1 (2026-10-14), against the first version
    def test_one_pair_is_inconclusive(self):
        pairs = self.clean_set()
        v = self.run_tool(pairs[:1])
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("three pairs", v)

    def test_three_on_first_pairs_are_inconclusive(self):
        pairs = self.clean_set()
        on_first = [pairs[0], [pairs[1][1], pairs[1][0]], pairs[2]]  # p2 declared ON/OFF, so its order is wrong too
        v = self.run_tool(on_first)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("ON/OFF, OFF/ON, ON/OFF", v)
        # created ON-first as well: the logs agree with the declaration, the design still does not
        self.sets += 1
        mk = lambda name, on: self.roll("s%d_%s" % (self.sets, name), on)
        three = [[("on", mk("a_on", True)), ("off", mk("a_off", False))],
                 [("on", mk("b_on", True)), ("off", mk("b_off", False))],
                 [("on", mk("c_on", True)), ("off", mk("c_off", False))]]
        v = self.run_tool(three)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("the registered design", v)

    def test_different_highfps_is_inconclusive(self):
        v = self.run_tool(self.clean_set({"p2on": {"extra_env": {"SR_HIGHFPS": "60"}}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("configuration differs", v)
        self.assertIn("SR_HIGHFPS", v)

    def test_missing_wait_line_is_not_a_zero_wait(self):
        v = self.run_tool(self.clean_set({"p3on": {"no_wait_line": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no backend-wait line", v)

    def test_crash_before_attach_is_open(self):
        v = self.run_tool(self.clean_set({"p2on": {"result": "FAILURE", "truncate_after": 0.5}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("outcome FAILURE", v)
        self.assertIn("also missing / inconsistent evidence", v)  # the missing markers named, not dropped

    def test_timeout_without_markers_is_open(self):
        v = self.run_tool(self.clean_set({"p1off": {"result": "TIMEOUT", "truncate_after": 0.0}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("outcome TIMEOUT", v)
        v = self.run_tool(self.clean_set({"p3on": {"result": None, "no_run_log": True}}))  # no outcome.json either
        self.assertIn("-> OPEN", v)
        self.assertIn("missing", v)

    def test_bad_budget_is_rejected(self):
        pairs = self.clean_set(hud=90.0)  # ON +7 s to the HUD: nan or a negative budget must not hide it
        for b in ("nan", "-nan", "inf", "-inf", "-1", "-0.5", "x"):
            p = self.run_raw(pairs, b)
            self.assertNotEqual(p.returncode, 0, "budget %s accepted" % b)
            self.assertNotIn("NO REGRESSION", p.stdout)
            self.assertIn("budget", p.stderr)
        self.assertIn("-> OPEN", self.run_tool(pairs, "0"))
        self.assertIn("-> OPEN", self.run_tool(pairs, "1.0"))

    # boot_pairs' checks, ported
    def test_missing_seed_is_inconclusive(self):
        one = self.run_tool(self.clean_set({"p2on": {"no_seed": True}}))
        self.assertIn("INCONCLUSIVE", one)
        self.assertIn("save seeding not recorded", one)
        self.assertIn("s%d_p2on" % self.sets, one)
        none = self.run_tool(self.clean_set({n: {"no_seed": True} for n in
                                             ("p1on", "p1off", "p2off", "p2on", "p3on", "p3off")}))
        self.assertIn("save seeding not recorded", none)

    def test_missing_run_log_is_inconclusive(self):
        v = self.run_tool(self.clean_set({"p1off": {"no_run_log": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no run.log", v)
        self.assertIn("s%d_p1off" % self.sets, v)

    def test_design_violations_are_inconclusive(self):
        pairs = self.clean_set()
        same = [pairs[0], [("off", pairs[0][1][1]), pairs[1][1]], pairs[2]]
        self.assertIn("path used twice", self.run_tool(same))
        self.assertIn("exe hashes", self.run_tool(self.clean_set({"p3off": {"exe": "b" * 64}})))
        self.assertIn("ON roll's arm", self.run_tool(self.clean_set({"p1on": {"flag": "0"}})))
        pairs = self.clean_set()
        reorder = [pairs[1], pairs[0], pairs[2]]  # declared OFF/ON first: the design and the sequence both wrong
        v = self.run_tool(reorder)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("declared sequence", v)

    def test_declared_order_must_match_the_logs(self):
        # the rolls ran ON, OFF / OFF, ON / ON, OFF; declare pair 2's rolls in the other run order in a new set where
        # the design holds: create pair 2 ON first, declare it OFF first
        self.sets = getattr(self, "sets", 0) + 1
        mk = lambda name, on: self.roll("s%d_%s" % (self.sets, name), on)
        p1 = [("on", mk("p1on", True)), ("off", mk("p1off", False))]
        p2on = mk("p2on", True)
        p2off = mk("p2off", False)
        p3 = [("on", mk("p3on", True)), ("off", mk("p3off", False))]
        v = self.run_tool([p1, [("off", p2off), ("on", p2on)], p3])
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("declared order", v)

    def test_off_arm_must_be_the_old_order(self):
        v = self.run_tool(self.clean_set({"p2off": {"off_attach_early": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("attach after the load's end False", v)

    def test_post_attach_regression_is_open(self):
        v = self.run_tool(self.clean_set(hud=90.0))
        self.assertIn("-> OPEN", v)
        self.assertIn("HUD", v)

    def test_attach_regression_is_open(self):
        v = self.run_tool(self.clean_set(init=5.0))  # ON's init 5 s: its attach later than OFF's
        self.assertIn("-> OPEN", v)
        self.assertIn("attach (process)", v)

    def test_init_failed_wait_is_open(self):
        v = self.run_tool(self.clean_set({"p3on": {"wait_status": "INIT FAILED", "fatal": True, "result": "FAILURE"}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("backend wait INIT FAILED", v)
        self.assertIn("[FATAL]", v)

    def test_black_world_race_is_open(self):
        v = self.run_tool(self.clean_set({"p1on": {"swaps": 3}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("black-world race", v)

    def test_inline_load_is_open(self):
        v = self.run_tool(self.clean_set({"p2on": {"inline": True}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("ran on init's thread", v)

    def test_no_installed_cache_is_open(self):
        v = self.run_tool(self.clean_set({"p1on": {"wp1": "none installed"}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("none installed", v)

    def test_wait_line_must_match_the_span(self):
        v = self.run_tool(self.clean_set({"p1off": {"wait_skew": 5.0}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("recorded backend wait", v)

    def test_old_log_format_is_inconclusive(self):
        v = self.run_tool(self.clean_set({"p3on": {"no_proc_fields": True}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("no attach (with its process time and the cache's state just after it) line", v)

    # Astra round 2 (2026-10-14)
    def test_recorded_zero_wait_is_checked(self):
        # Astra's repro: an OFF roll's 2.51 s backend wait rewritten as "CP waited 0.0 ms", timestamps unchanged
        v = self.run_tool(self.clean_set({"p2off": {"wait_ms": 0.0}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("recorded backend wait 0.00 s vs [CP up, attach] 2.51 s", v)

    def test_witness_reads_the_attach_not_timestamps(self):
        # the interleaving: the loader sampled stage 1's end (+651 ms, before the attach at +1360 ms) and was
        # descheduled before publishing; the attach read the cache still loading - a true witness
        v = self.run_tool(self.clean_set(s1=0.25, attach_cache="still loading"))
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        self.assertIn("in 3 of 3 ON rolls", v)
        # the timestamps say "before the stage-1 end" but the attach read the cache loaded: no witness, and since this
        # build samples the end before publication, the log is inconsistent and named
        v = self.run_tool(self.clean_set({"p1on": {"attach_cache": "loaded"}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("inconsistent log", v)

    def test_first_command_is_its_own_line(self):
        v = self.run_tool(self.clean_set(first_delay=0.4))  # consumed 0.4 s after WP1's readiness
        self.assertIn("CP first command <= 2852", v)       # (2.401 + 0.4 + 0.05) s, not the readiness
        self.assertIn("CP ready after WP1 <= 2452", v)
        v = self.run_tool(self.clean_set({"p3off": {"first_backend": "NOT attached"}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("without the live backend", v)
        v = self.run_tool(self.clean_set({"p2on": {"first_delay": -0.2}}))
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("precedes its WP1 readiness", v)
        v = self.run_tool(self.clean_set({"p2off": {"first_delay": None}}))
        self.assertIn("no CP first command consumed line", v)

    # the dev merge (dcb's per-load boot-load lines + render/boot-attach's stage fields: one line per stage)
    def test_on_load_on_the_main_thread_is_open(self):
        v = self.run_tool(self.clean_set({"p2on": {"load_on_main": True}}))
        self.assertIn("-> OPEN", v)
        self.assertIn("ran on the main thread 4242", v)

    def test_dcb_consumers_read_the_merged_lines(self):
        import importlib.util
        mods = {}
        for m in ("boot_pairs", "boot_threads", "boot_census"):
            spec = importlib.util.spec_from_file_location(m, os.path.join(HERE, m + ".py"))
            mods[m] = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mods[m])
        d = self.roll("consumers", True)
        lines = open(os.path.join(d, "drive", "attempt1", "run.log"), encoding="utf-8").read().splitlines()
        done = [m for m in (mods["boot_pairs"].MARKS["loaddone"].search(l) for l in lines) if m]
        self.assertEqual(len(done), 1)  # one 'boot load done' line per load
        self.assertEqual(float(done[0].group(1)), 2500.0)  # the whole load (installed 2.0 s + translator 0.5 s)
        census = dict(mods["boot_census"].MARKS)
        self.assertEqual(sum(1 for l in lines if census["loaddone"].search(l)), 1)
        first = next(m for m in (mods["boot_threads"].MAIN_RX.search(l) for l in lines) if m)
        self.assertEqual(first.groups(), ("5150", "4242"))  # the load's thread, then the main thread

    def test_host_wait_stalls_are_counted(self):
        v = self.run_tool(self.clean_set({"p1off": {"host_stalls": 2}}))
        self.assertIn("host-wait stall escalations ON 0, OFF 2", v)

    # the declared warm-up (native/devcreate-close, 2026-10-14): run before pair 1, never graded
    def test_warmup_is_never_graded(self):
        # bat1's pair 1 was the session's first roll (+1.88 s): a warm-up 7 s slow to the HUD, attaching late with a
        # 3 s backend wait and a host-wait escalation, leaves a clean set clean - none of it is graded (it ended before
        # pair 1 began: a second escalation, 300 s later, would have run past pair 1's launch - P2-7's rule)
        pairs = self.clean_set(warm=dict(hud=90.0, init=3.0, host_stalls=1))
        out = self.run_tool(pairs, warmup=self.warm, full=True)
        last = out.strip().splitlines()[-1]
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", last)
        self.assertIn("host-wait stall escalations ON 0, OFF 0", last)  # the warm-up's escalation is not the set's
        self.assertIn("(warm-up: never graded)", out)
        self.assertIn("warm-up (never graded): %s HUD 90.00 s" % self.warm, out)
        # the same rolls without --warmup grade exactly as before (bat1's form)
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", self.run_tool(pairs))

    def test_warmup_faults_are_inconclusive(self):
        pairs = self.clean_set(warm={}, warm_after=1)  # it ran after pair 1
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("the warm-up had not ended", v)
        pairs = self.clean_set(warm=dict(result="FAILURE", truncate_after=20.0))  # crashed: named, never an OPEN
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("the warm-up did not complete to the HUD (outcome FAILURE", v)
        pairs = self.clean_set(warm=dict(flag="0"))
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("the warm-up ran SR_SHADER_LOAD_ASYNC=0", v)
        pairs = self.clean_set(warm=dict(exe="b" * 64))
        self.assertIn("the warm-up's exe bbbbbbbbbbbb is not the set's", self.run_tool(pairs, warmup=self.warm))
        pairs = self.clean_set(warm=dict(extra_env={"SR_HIGHFPS": "60"}))
        self.assertIn("the warm-up's effective SR_* configuration differs", self.run_tool(pairs, warmup=self.warm))
        pairs = self.clean_set(warm=dict(no_seed=True))
        self.assertIn("the warm-up's save seeding is not the set's", self.run_tool(pairs, warmup=self.warm))
        pairs = self.clean_set(warm={}, split_proof=True)  # bat2's form: the warm-up in its own invocation (bat2w)
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("not rows of one native_proof run", v)
        pairs = self.clean_set()
        v = self.run_tool(pairs, warmup=pairs[0][0][1])  # a graded roll named as the warm-up
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("is also a graded roll", v)
        v = self.run_tool(pairs, warmup=os.path.join(self.tmp, "never_ran"))
        self.assertIn("the warm-up %s was not run" % os.path.join(self.tmp, "never_ran"), v)

    # Astra round 1 on native/devcreate-close: the counterexamples to the first warm-up rule
    def test_warmup_still_running_at_the_first_graded_launch(self):
        # P2-7: the warm-up STARTED first but its log runs on (HUD at +400 s) past pair 1's launch (+300 s): a
        # start-only check passed it
        pairs = self.clean_set(warm=dict(hud=400.0))
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("the warm-up had not ended (its log runs to 2026-10-14 06:06:40", v)

    def test_foreign_rows_by_basename_are_refused(self):
        # P1-2: a proof JSON beside the rolls whose rows name the same BASENAMES under another folder (another run's
        # rolls) - the basename match accepted it
        pairs = self.clean_set()
        rolls = [p for pr in pairs for _a, p in pr]
        warm = self.roll("s%d_warmup2" % self.sets, True)  # (after the set: only the identity is under test here)
        self.proof("foreign_proof.json", [warm] + rolls, rel_dir="_worktrees/elsewhere/archive")
        v = self.run_tool(pairs, warmup=warm)
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("not rows of one native_proof run (no native_proof run beside the rolls records them", v)
        # ...and a JSON whose `out` is another folder (a copy of another run's proof) is refused the same way
        os.remove(os.path.join(self.tmp, "foreign_proof.json"))
        self.proof("copied_proof.json", [warm] + rolls, out=os.path.join(self.tmp, "elsewhere"))
        v = self.run_tool(pairs, warmup=warm)
        self.assertIn("is not the folder it sits in", v)

    def test_failed_row_or_abort_marker_is_open_whatever_the_outcome(self):
        # P1-2: outcome.json SUCCESS, but the run's row says ABORT (native_proof's abort rule saw the marker) - OPEN
        pairs = self.clean_set(warm={})
        p1on = pairs[0][0][1]
        self.proof("s%d_bat_proof.json" % self.sets, [self.warm] + [p for pr in pairs for _a, p in pr],
                   verdicts={p1on: "ABORT"})
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("-> OPEN", v)
        self.assertIn("the run's row says ABORT", v)
        # a THREAD_FATAL in the log with outcome SUCCESS and a DONE row (no proof JSON at all: bat1's form) - OPEN
        pairs = self.clean_set({"p2off": dict(extra_lines=[(50.0, "[THREAD_FATAL] tid=12 x")])})
        v = self.run_tool(pairs)
        self.assertIn("-> OPEN", v)
        self.assertIn("thread fatal in the log", v)
        # an ICALL_FAIL long after the HUD (the scan stops early; the abort scan reads the whole log)
        pairs = self.clean_set({"p3on": dict(extra_lines=[(200.0, "[ICALL_FAIL] target 0 x")])})
        v = self.run_tool(pairs)
        self.assertIn("indirect-call failure in the log", v)

    # the position effect (bat2, 2026-10-14): the first launch of each native_proof invocation consumed its first CP
    # command ~1 s late, and every set so far put ON first - the plan now runs an in-invocation warm-up and the mirror
    def test_mirrored_order_is_the_registered_alternative(self):
        pairs = self.clean_set(warm={}, mirror=True)  # warm-up, then OFF/ON, ON/OFF, OFF/ON
        v = self.run_tool(pairs, warmup=self.warm)
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", v)
        # an unbalanced order (OFF first twice in a row) is neither the design nor its mirror
        pairs = self.clean_set(mirror=True)
        v = self.run_tool([pairs[0], pairs[1][::-1], pairs[2]])
        self.assertIn("INCONCLUSIVE", v)
        self.assertIn("or the mirror OFF/ON, ON/OFF, OFF/ON", v)

    def test_position_is_reported_never_graded(self):
        # the warm-up (the invocation's first launch) 1 s late to its first command, the set clean: the position line
        # shows both, the verdict is the pairs' alone
        pairs = self.clean_set(warm=dict(first_delay=1.05), mirror=True)
        out = self.run_tool(pairs, warmup=self.warm, full=True)
        self.assertIn("NO REGRESSION OBSERVED IN THESE PAIRS", out.strip().splitlines()[-1])
        pos = [ln for ln in out.splitlines() if ln.startswith("position (information, never graded)")]
        self.assertEqual(len(pos), 1)
        self.assertIn("the first graded roll (pair 1 OFF) CP first command +", pos[0])
        self.assertIn("the warm-up (the invocation's first launch) +", pos[0])
        # a first graded ON roll ~2 s late to its first command (bat2's pattern, no warm-up): OPEN on that pair, and the
        # position line puts it beside the other ON rolls' median
        pairs = self.clean_set({"p1on": dict(first_delay=2.2)})
        out = self.run_tool(pairs, full=True)
        self.assertRegex(out.strip().splitlines()[-1], r"-> OPEN: CP first command \(process\) \+1\.\d\d ")
        self.assertRegex(out, r"pair 1 ON\) CP first command \+\d+ ms, HUD 83\.00 s; the other ON rolls' median \+\d+ ms")


if __name__ == "__main__":
    unittest.main()
