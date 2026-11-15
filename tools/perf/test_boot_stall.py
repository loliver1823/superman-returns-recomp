#!/usr/bin/env python3
"""test_boot_stall.py - tools/perf/boot_stall.py's rule (BOOT_STALL design dcl-stall-v1, Astra round 1 P1-1) on
synthetic native_proof runs, and a mutant check that each guard is load-bearing (native/devcreate-close, 2026-10-14).

    python -m unittest discover -s tools/perf -p test_boot_stall.py

29 clean ON boots of one run (with OFF rolls and meter ON rolls that keep their dumps) are BOUNDED at 9.81% and only
then print the tag BOOT_STALL. The negative controls must never be BOUNDED: 28 clean ON boots (under-counted, 10.15%),
Astra's intermittent stall (+30 s on ~10% of boots: 3 of 29), one late attach, one CP wait over 1 s, one race, a
run row ABORT with outcome SUCCESS, a THREAD_FATAL line, a crashed boot whose row FAILED without a roll folder (all
OPEN); an ON boot of the run that --on omits, a roll no declared run records, two exes, an arm not engaged, an
unclassifiable boot, a proof JSON from another folder (all INCONCLUSIVE). Two declared runs on one exe pool; on two
exes they do not. Scratch runs go under the checkout's archive/ (never C:).
The bake's design dcf-stall-v2 (native/devcreate-fix, --design dcf-stall-v2 --allow-key ...; after Astra's round 1 on
the branch): the devcreate_bake plan's 29 ON boots - the warm-up, four default rolls, cppm4 (a pixel-graded roll whose
run row says PASS, the runner's own row shape: v1 read it as a failure), census1, the stall block s1_on..s20_on and the
flicker gate's freeze and span boots (enumerated from the gate's row by proof_rows.gate_boots) - are BOUNDED at 9.81%
with the declared instruments and the staged cache condition; 28 are under-counted; a gate boot omitted from --on, an
unstaged run, two exe paths, a path outside the stage root, a boot without the WP1 line, the A/B or CHECK roll named,
an undeclared instrument, a foreign allow key, the allow key under dcl, or the set read under dcl are INCONCLUSIVE; a
stalled gate boot, a gate roll that exited nonzero, a crashed retained attempt (span_try1), a WP1 wait over 1 s (C) and
a HUD after 95 s with a normal attach (H) are OPEN; a healthy retained attempt is counted (30 boots).
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
TOOL = os.environ.get("BOOT_STALL_TOOL") or os.path.join(HERE, "boot_stall.py")
sys.path.insert(0, HERE)
import proof_rows as prs  # noqa: E402

ASYNC = ("[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe of J:\\x\\dxil says "
         "an installed cache (probe 0.1 ms, from process +69.4 ms); the load runs on the loader thread, the CP waits for "
         "its installed stage (process +69.5 ms)")
STAGE = r"J:\sr_stage"
EXE_PATH = STAGE + r"\content-7c70b22d13d02b89\exe-aaaaaaaaaaaa-9b89fd71\SupermanReturns.exe"
# the runner's row of a pixel-graded roll that passed (native/archive/v1_native/tcmp3/tcmp_proof.json, cppm4)
GRADED_DETAIL = "perf_run exit 0 | freeze grade PASS counted=15 bad=[] world 15 (gameplay 15 of 15 >= 11)"


def ts(base_s, sec):
    """A log timestamp base_s + sec seconds after 2026-10-14 00:00 (days roll over: a test with seven 35-roll runs passes
    midnight)."""
    from datetime import datetime, timedelta
    t = datetime(2026, 10, 14) + timedelta(seconds=base_s + sec)
    return t.strftime("%Y-%m-%d %H:%M:%S.") + "%03d" % (t.microsecond // 1000)


class BootStall(unittest.TestCase):
    def setUp(self):
        root = os.path.join(REPO, "archive")
        os.makedirs(root, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="boot_stall_test_", dir=root)
        self.clock = 6 * 3600

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def roll(self, run, name, on, attach_at=0.7, cpup=1.3, hud=83.0, exe="a" * 64, result="SUCCESS", swaps=0,
             engaged=None, dumps=False, extra_lines=None, no_attach=False, baked=False, extra_env=None, cpstart=0.0,
             no_cpstart=False, exepath=EXE_PATH):
        """on: True / False the arms, None the default build. baked (dcf-stall-v2's provenance): the ON arm and the
        default leave SR_NATIVE_DEVCREATE unset, the OFF arm pins =0; else ON pins =1. extra_env: instruments. cpstart:
        the WP1 wait (s) the CP start line reports; no_cpstart: an exe without that line. name may be "gate/freeze"."""
        d = os.path.join(self.tmp, run, name, "drive", "attempt1")
        os.makedirs(d)
        env = {"SR_NATIVE_SEGMENTLESS": "1", "SR_LOG": "player", "SR_PERF_DIR": os.path.join(d, "perf"),
               "SR_CONTENT_DIR": os.path.join(d, "content")}
        env.update({"SR_FRAME_DUMP": "0"} if dumps else {"SR_FRAME_DUMP": "0", "SR_PRESENT_DUMP": "0"})
        if baked:
            if on is False:
                env["SR_NATIVE_DEVCREATE"] = "0"
        elif on:
            env["SR_NATIVE_DEVCREATE"] = "1"
        env.update(extra_env or {})
        with open(os.path.join(d, "provenance.json"), "w") as f:
            json.dump({"exeSha256": exe, "exePath": exepath, "effectiveSrEnv": env}, f)
        with open(os.path.join(d, "outcome.json"), "w") as f:
            json.dump({"result": result}, f)
        with open(os.path.join(d, "steps.log"), "w") as f:
            f.write("[06:00:00.000] saves: SR_CONTENT_DIR=%s (empty)\n" % env["SR_CONTENT_DIR"])
        b = self.clock
        self.clock += 300
        L = [(0.0, "[LOG_PROFILE] player"), (0.05, ASYNC), (0.4, "[HIGHFPS] swap chain ALLOW_TEARING on"),
             (cpup, "[GPU_CP] command processor thread up (pcr=D00E1800 guestCaller=ok)"),
             (0.3, "[SHADER_CACHE] boot load done: 120.0 ms on thread 7 (main thread 2): installed cache x")]
        if not no_attach:
            L.append((attach_at, "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %d, draw commands "
                                 "queued 0; process +%.1f ms; installed shader cache just after the attach: loaded" % (
                                     swaps, 1000 * attach_at + 69.0)))
            if attach_at > cpup:
                L.append((attach_at + 0.001, "[DISPLAY] CP waited %.1f ms for the live backend (attached)" % (
                    1000 * (attach_at - cpup))))
            if not no_cpstart:  # WP1: the CP's wait for the installed cache after the attach, before its first command
                L.append((max(attach_at, cpup) + 0.5 + cpstart,
                          "[SHADER_CACHE] CP start: waited %.1f ms for the installed shader cache (loaded; translator "
                          "stage done), ready after WP1 at process +%.1f ms" % (
                              1000 * cpstart, 1000 * (max(attach_at, cpup) + 0.5 + cpstart) + 69.0)))
        eng = (on if on is not None else baked) if engaged is None else engaged
        L.append((13.0, "[GPU_CP] census rptr=81 wptr=93 doorbells=21 packets=%d draws=235 isr=2" % (0 if eng else 1)))
        if result == "SUCCESS":
            L.append((hud, "[APT_REQ] #17 path='screens/hud/main_hud' holder=0"))
        L += list(extra_lines or [])
        L.sort()
        with open(os.path.join(d, "run.log"), "w") as f:
            for t, text in L:
                f.write("[%s] [INFO ] [BOOT] %s\n" % (ts(b, t), text))
        return os.path.normpath(os.path.join(self.tmp, run, name))   # normpath: a nested "gate/freeze" name

    # -- the bake's design dcf-stall-v2 (native/devcreate-fix): every ON boot of the devcreate_bake plan
    ALLOW = ("SR_NATIVE_BRACKETS", "SR_NATIVE_FLUSH_CPPM4", "SR_NATIVE_CONST_DEDUP_CHECK", "SR_DIAG_SIM_FREEZE",
             "SR_DRIVE_GRACEFUL_QUIT")
    FREEZE_ENV = {"SR_DIAG_SIM_FREEZE": "8:2000:80", "SR_PRESENT_DUMP_FREEZE": "2", "SR_PRESENT_DUMP_FREEZE_CAP": "40",
                  "SR_PRESENT_DUMP": "1"}
    SPAN_ENV = {"SR_PRESENT_DUMP": "1", "SR_PRESENT_DUMP_EVERY": "1", "SR_PRESENT_DUMP_FROM": "20000",
                "SR_PRESENT_DUMP_CAP": "360", "SR_PRESENT_DUMP_FROM_MARK": "1"}

    def gate(self, run, over=None, exits=(0, 0), retained=None, exe="a" * 64, json_file=True, verdict="PASS"):
        """The flicker gate's folder as flicker_gate.py leaves it: span and freeze rolls (the gate's own env: the freeze
        instrument and the dump keys, no SR_FRAME_DUMP=0), flicker_gate.json with their exit codes, and optionally a
        RETAINED attempt span_try1 (retained = its roll kwargs). -> (gate dir, [freeze, span], [retained])."""
        over = over or {}
        g = os.path.join(self.tmp, run, "gate")
        kept = []
        if retained is not None:
            kw = dict(dict(baked=True, dumps=True, exe=exe, extra_env=self.SPAN_ENV), **retained)
            kept.append(self.roll(run, "gate/span_try1", True, **kw))
        span = self.roll(run, "gate/span", True, baked=True, dumps=True, exe=exe, extra_env=self.SPAN_ENV,
                         **over.get("span", {}))
        freeze = self.roll(run, "gate/freeze", True, baked=True, dumps=True, exe=exe, extra_env=self.FREEZE_ENV,
                           **over.get("freeze", {}))
        for d in (span, freeze) + tuple(kept):  # the gate rolls carry no SR_FRAME_DUMP=0 (no NO_TIMED on a gate roll)
            p = os.path.join(d, "drive", "attempt1", "provenance.json")
            doc = json.load(open(p))
            doc["effectiveSrEnv"].pop("SR_FRAME_DUMP", None)
            json.dump(doc, open(p, "w"))
        if json_file:
            with open(os.path.join(g, "flicker_gate.json"), "w") as f:
                json.dump({"exe": "x", "gates": "", "span_exit": exits[0], "freeze_exit": exits[1], "verdict": verdict,
                           "span_attempts": [{"from": 20000, "in_play": [120, 360], "replace": "window missed play"},
                                             {"from": 24000, "in_play": [360, 360]}] if kept else
                           [{"from": "mark p8580", "in_play": [360, 360]}],
                           "stage": {"stage": STAGE, "stage_source": None}}, f)
        return g, [freeze, span], kept

    def bake_run(self, run="b1", over=None, with_cppm4=True, exe="a" * 64, n_stall=20, gate_over=None, gate_exits=(0, 0),
                 retained=None, stage=True, cppm4_verdict="PASS"):
        """The devcreate_bake plan's rolls in plan order: the warm-up (the default), four default / =0 pairs (the sparse
        dump kept), the CHECK roll, the A/B roll, cppm4 (the CPPM4 split, the freeze instrument; its row PASS), census1
        (the census, the graceful quit), the stall block s1_on..s<n_stall>_on, the gate (freeze + span, and a retained
        span attempt when asked). -> (proof, [ON boots], [all rolls], abp, chk). over = {k: kwargs} for the k-th pair's
        ON roll; "s3" etc. for a stall boot."""
        over = over or {}
        rolls, on, graded = [], [], {}
        w = self.roll(run, "warmup", None, baked=True, dumps=True, exe=exe, **over.get("warmup", {}))
        rolls.append(w)
        on.append(w)
        for k in range(4):
            p = self.roll(run, "p%d_on" % (k + 1), True, baked=True, dumps=True, exe=exe, **over.get(k, {}))
            rolls.append(p)
            on.append(p)
            rolls.append(self.roll(run, "p%d_off" % (k + 1), False, baked=True, dumps=True, exe=exe))
        ck = self.roll(run, "chk", True, baked=True, engaged=False, exe=exe, extra_env={"SR_NATIVE_FLUSH_CHECK": "1"})
        ab = self.roll(run, "abp", True, baked=True, engaged=False, exe=exe,
                       extra_env=dict(self.FREEZE_ENV, SR_AB_TOGGLE="SR_NATIVE_DEVCREATE", SR_AB_PRESENT="1"))
        graded[ab] = "PASS"
        rolls += [ck, ab]
        if with_cppm4:
            cp = self.roll(run, "cppm4", True, baked=True, exe=exe,
                           extra_env=dict(self.FREEZE_ENV, SR_NATIVE_FLUSH_CPPM4="1", SR_NATIVE_CONST_DEDUP_CHECK="1"),
                           **over.get("cppm4", {}))
            rolls.append(cp)
            on.append(cp)
            graded[cp] = cppm4_verdict
        c1 = self.roll(run, "census1", True, baked=True, exe=exe,
                       extra_env={"SR_NATIVE_BRACKETS": "1", "SR_DRIVE_GRACEFUL_QUIT": "1"})
        rolls.append(c1)
        on.append(c1)
        for k in range(n_stall):
            s = self.roll(run, "s%d_on" % (k + 1), True, baked=True, exe=exe, **over.get("s%d" % (k + 1), {}))
            rolls.append(s)
            on.append(s)
        g, gb, _kept = self.gate(run, over=gate_over, exits=gate_exits, retained=retained, exe=exe)
        on += gb
        js = self.proof(run, rolls, graded=graded, gate=g, stage=stage, plan="devcreate_bake", prefix="dcf")
        return js, on, rolls, ab, ck

    def test_bake_every_on_boot_is_bounded(self):
        js, on, _r, _ab, _ck = self.bake_run()
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> BOUNDED", v)
        self.assertIn("on_boots=29", v)
        self.assertIn("is 9.81%", v)
        self.assertIn("instruments=", v)
        self.assertIn("staged at " + STAGE, v)
        self.assertIn("(F / A / W / R / C / H)", v)

    def test_bake_28_on_boots_are_under_counted(self):
        js, on, _r, _ab, _ck = self.bake_run(n_stall=19)
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE (under-counted): 28 ON boots, the design needs 29", v)
        self.assertIn("only below 10.15%", v)

    def test_bake_graded_roll_rows_are_successes(self):
        # the runner's row of a pixel-graded roll says PASS, not DONE (tcmp3's cppm4): a success, never the F event
        # (Astra round 1 on the branch: v1 made a healthy bake unable to close); its FAIL row is the event
        js, on, _r, _ab, _ck = self.bake_run()
        doc = json.load(open(js))
        cp = [r for r in doc["steps"] if r["step"] == "cppm4"][0]
        self.assertEqual((cp["verdict"], cp["detail"], sorted(cp["paths"])), ("PASS", GRADED_DETAIL, ["freeze_table", "roll"]))
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> BOUNDED", v)
        self.assertNotIn("row says PASS", v)
        shutil.rmtree(os.path.dirname(js))
        js, on, _r, _ab, _ck = self.bake_run(cppm4_verdict="FAIL")
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN", v)
        self.assertIn("cppm4: F: the run's row says FAIL", v)

    def test_bake_gate_boots_are_on_boots(self):
        # the flicker gate's two boots are ON boots of the proof (proof_rows.gate_boots): a pool that omits them is a
        # design fault, never a quiet discard
        js, on, _r, _ab, _ck = self.bake_run()
        v = self.run_tool([js], [p for p in on if os.sep + "gate" + os.sep not in p], design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("gate%sfreeze: an ON boot of dcf_proof.json that --on does not count" % os.sep, v)
        self.assertIn("gate%sspan: an ON boot" % os.sep, v)

    def test_bake_gate_boot_stall_is_open(self):
        js, on, _r, _ab, _ck = self.bake_run(gate_over={"freeze": dict(attach_at=26.7, cpup=27.0, hud=108.7)})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN (kept, never discarded): 1 of 29 ON boots had the stall event", v)
        self.assertIn("gate%sfreeze: A: attach at +26.70 s" % os.sep, v)
        self.assertIn("H: the HUD request at +108.70 s", v)

    def test_bake_gate_roll_exit_is_a_failure(self):
        # flicker_gate.json records perf_run's exit for each roll: a nonzero exit is the F event of that boot
        js, on, _r, _ab, _ck = self.bake_run(gate_exits=(0, 1))
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN", v)
        self.assertIn("gate%sfreeze: F: the run's row says FAIL (perf_run exit 1 (flicker_gate freeze_exit))" % os.sep, v)

    def test_bake_retained_attempt_is_counted(self):
        # a span the gate re-placed (span_try1) is a boot too: healthy, it is the 30th boot; crashed, it is the event
        js, on, _r, _ab, _ck = self.bake_run(retained={})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> BOUNDED", v)
        self.assertIn("on_boots=30", v)
        self.assertIn("retained=1", v)
        shutil.rmtree(os.path.dirname(js))
        js, on, _r, _ab, _ck = self.bake_run(retained=dict(result="FAILURE"))
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN (kept, never discarded): 1 of 30 ON boots had the stall event", v)
        self.assertIn("span_try1: F: the run's row says FAIL (a retained attempt of the gate's span_try1 (outcome FAILURE))", v)

    def test_bake_wp1_wait_is_open(self):
        # the post-attach wait (WP1): a normal attach, then the CP's first command waits 12 s for the installed cache
        js, on, _r, _ab, _ck = self.bake_run(over={"s3": dict(cpstart=12.0, hud=95.0)})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN", v)
        self.assertIn("s3_on: C: the CP's first command waited 12.00 s for the installed shader cache after the attach (> 1.0 s)", v)
        self.assertNotIn("H:", v)   # the HUD at exactly 95.0 s is within the bound; the WP1 event alone names it

    def test_bake_late_hud_is_open(self):
        # the catch-all: a normal attach and WP1, the HUD 112 s in (a stall the markers do not name)
        js, on, _r, _ab, _ck = self.bake_run(over={"warmup": dict(hud=112.0)})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> OPEN", v)
        self.assertIn("warmup: H: the HUD request at +112.00 s (> 95.0 s)", v)

    def test_bake_boot_without_wp1_line_is_unclassifiable(self):
        js, on, _r, _ab, _ck = self.bake_run(over={"s7": dict(no_cpstart=True)})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("s7_on: unclassifiable: no CP start (WP1) line", v)

    def test_bake_unstaged_run_is_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run(stage=False)
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("the run was not staged (--stage)", v)

    def test_bake_two_exe_paths_are_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run(over={"s9": dict(exepath=STAGE + r"\other\SupermanReturns.exe")})
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("launched from 2 exe paths", v)

    def test_bake_exe_path_outside_the_stage_root_is_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run()
        for p in on:  # every boot from one UNSTAGED path (D:): one path, but not the condition
            pj = os.path.join(p, "drive", "attempt1", "provenance.json")
            doc = json.load(open(pj))
            doc["exePath"] = r"D:\x\SupermanReturns.exe"
            json.dump(doc, open(pj, "w"))
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("an exe path outside the run's stage root", v)

    def test_bake_ab_and_check_rolls_are_neither_arm(self):
        js, on, _r, ab, ck = self.bake_run()
        for extra, what in ((ab, "A/B True"), (ck, "CHECK True")):
            v = self.run_tool([js], on + [extra], design="dcf-stall-v2", allow=self.ALLOW)
            self.assertIn("-> INCONCLUSIVE", v)
            self.assertIn("not an ON boot", v)
            self.assertIn(what, v)

    def test_bake_undeclared_instrument_is_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run()
        v = self.run_tool([js], on, design="dcf-stall-v2")
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("differs beyond the path and dump keys", v)

    def test_bake_foreign_allow_key_is_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run()
        v = self.run_tool([js], on, design="dcf-stall-v2", allow=self.ALLOW + ("SR_NATIVE_SEG_STRESS",))
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("not an observe-only instrument", v)

    def test_bake_allow_key_under_the_old_design_is_inconclusive(self):
        js, on, _ = self.run_set()
        v = self.run_tool([js], on, allow=("SR_NATIVE_BRACKETS",))
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("is dcf-stall-v2's", v)

    def test_bake_set_read_under_the_old_design_is_inconclusive(self):
        js, on, _r, _ab, _ck = self.bake_run()
        v = self.run_tool([js], on)  # dcl-stall-v1 reads the unset key as OFF: none of these is an ON boot there
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("not an ON boot", v)

    def test_the_withdrawn_v1_design_is_not_selectable(self):
        js, on, _r, _ab, _ck = self.bake_run()
        p = subprocess.run([sys.executable, TOOL, "--design", "dcf-stall-v1", "--proof", js, "--on", on[0]],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("invalid choice: 'dcf-stall-v1'", p.stderr)

    def proof(self, run, rolls, verdicts=None, out=None, exe_id="a" * 12, graded=None, gate=None, stage=False,
              plan="devcreate_close", prefix="dcl"):
        """The run's proof JSON (Runner.write_proof's form): rows by umbrella-relative path; verdicts {roll: verdict};
        graded {roll: verdict} = pixel-graded rolls (the runner's PASS / FAIL row with its freeze table); gate = the
        flicker gate's folder (a `gate` row naming it, never its boots); stage: the runner's --stage (the bake's)."""
        rel = lambda p: os.path.relpath(p, prs.UMBRELLA).replace("\\", "/")
        steps = []
        for p in rolls:
            if p in (graded or {}):
                steps.append({"step": os.path.basename(p), "kind": "roll", "verdict": graded[p], "detail": GRADED_DETAIL,
                              "paths": {"roll": rel(p), "freeze_table": rel(p) + "_freeze.txt"}})
            else:
                steps.append({"step": os.path.basename(p), "kind": "roll", "verdict": (verdicts or {}).get(p, "DONE"),
                              "detail": "perf_run exit 0", "paths": {"roll": rel(p)}})
        if gate:
            steps.append({"step": "gate", "kind": "gate", "verdict": "PASS",
                          "detail": "exit=0 | freeze : PASS counted=15 bad=[] world 15 | FLICKER_GATE PASS",
                          "paths": {"gate": rel(gate)}})
        js = os.path.join(self.tmp, run, "%s_proof.json" % prefix)
        os.makedirs(os.path.dirname(js), exist_ok=True)
        doc = {"plan": plan, "prefix": prefix, "out": out or os.path.dirname(js), "exe_id": exe_id,
               "started": "2026-10-14T05:00:00", "steps": steps}
        if stage:
            doc["stage"] = {"stage": STAGE, "stage_source": None}
        with open(js, "w") as f:
            json.dump(doc, f)
        return js

    def run_set(self, run="r1", n_on=29, over=None, n_off=4, n_meter_on=4, exe="a" * 64):
        """A run: a warm-up and n_off OFF rolls, n_on - n_meter_on plain ON boots and n_meter_on meter ON boots (dumps
        kept); over = {index of the ON boot: kwargs}. -> (proof, [ON rolls], [all rolls])."""
        over = over or {}
        rolls, on = [self.roll(run, "warmup", False, exe=exe)], []
        for k in range(n_off):
            rolls.append(self.roll(run, "off%d" % k, False, exe=exe))
        for k in range(n_on):
            kw = dict(dict(exe=exe, dumps=k >= n_on - n_meter_on), **over.get(k, {}))
            p = self.roll(run, "on%d" % k, True, **kw)
            rolls.append(p)
            on.append(p)
        return self.proof(run, rolls), on, rolls

    def run_tool(self, proofs, on, design=None, allow=()):
        cmd = [sys.executable, TOOL]
        if design:
            cmd += ["--design", design]
        for k in allow:
            cmd += ["--allow-key", k]
        for js in proofs:
            cmd += ["--proof", js]
        for p in on:
            cmd += ["--on", p]
        p = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        last = p.stdout.strip().splitlines()[-1]
        verdicts = [v for v in (" -> BOUNDED ", " -> OPEN ", " -> INCONCLUSIVE ") if v in last]
        self.assertEqual(len(verdicts), 1, last)
        if " -> BOUNDED " in last:
            self.assertTrue(last.startswith("BOOT_STALL design=%s on_boots=" % (design or "dcl-stall-v1")), last)
        else:
            self.assertNotIn("BOOT_STALL ", p.stdout)  # the tag is the bounding outcome's alone
        self.assertNotIn("RESOLVED", p.stdout)
        return last

    # -- BOUNDED
    def test_29_clean_on_boots_are_bounded(self):
        js, on, _ = self.run_set()
        v = self.run_tool([js], on)
        self.assertIn("-> BOUNDED no stall event (F / A / W / R / C / H) in 29 ON boots", v)
        self.assertIn("is 9.81%, assuming independent boots with one common probability", v)
        self.assertIn("this event only", v)

    def test_two_runs_on_one_exe_pool(self):
        js1, on1, _ = self.run_set("r1", n_on=15, n_meter_on=0)
        js2, on2, _ = self.run_set("r2", n_on=14, n_meter_on=0)
        self.assertIn("-> BOUNDED", self.run_tool([js1, js2], on1 + on2))
        js3, on3, _ = self.run_set("r3", n_on=14, n_meter_on=0, exe="b" * 64)
        v = self.run_tool([js1, js3], on1 + on3)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("exe hashes", v)

    # -- INCONCLUSIVE: under-counted, design faults
    def test_28_on_boots_are_under_counted(self):
        js, on, _ = self.run_set(n_on=28)
        v = self.run_tool([js], on)
        self.assertIn("-> INCONCLUSIVE (under-counted): 28 ON boots, the design needs 29", v)
        self.assertIn("only below 10.15%", v)

    def test_every_on_boot_must_be_counted(self):
        js, on, _ = self.run_set()
        v = self.run_tool([js], on[:-1])  # one ON boot of the run left out (a stalled boot "discarded")
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("an ON boot of dcl_proof.json that --on does not count", v)
        stray = self.roll("elsewhere", "on_x", True)
        v = self.run_tool([js], on + [stray])  # a roll no declared run records
        self.assertIn("no declared run (--proof) records it", v)

    def test_design_faults_are_inconclusive(self):
        for over, word in (({3: dict(exe="b" * 64)}, "exe hashes"),
                           ({4: dict(engaged=False)}, "devcreate not engaged"),
                           ({5: dict(no_attach=True)}, "unclassifiable: no attach")):
            js, on, _ = self.run_set(over=over)
            v = self.run_tool([js], on)
            self.assertIn("-> INCONCLUSIVE", v, word)
            self.assertIn(word, v)
            shutil.rmtree(os.path.dirname(js))
        js, on, rolls = self.run_set()
        with open(js) as f:
            doc = json.load(f)
        doc["out"] = os.path.join(self.tmp, "another")
        with open(js, "w") as f:
            json.dump(doc, f)
        v = self.run_tool([js], on)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("is not the folder it sits in", v)

    def test_another_runs_identity_is_refused(self):
        # the proof JSON's exe is not the rolls' (a later invocation's JSON over these rolls)
        js, on, rolls = self.run_set()
        self.proof("r1", rolls, exe_id="b" * 12)
        v = self.run_tool([js], on)
        self.assertIn("-> INCONCLUSIVE", v)
        self.assertIn("is not the run's bbbbbbbbbbbb", v)

    def test_meter_rolls_keep_their_dumps(self):
        # the meter ON boots differ only by the dump keys (the world rule's sparse dump): counted, not a design fault
        js, on, _ = self.run_set(n_meter_on=4)
        self.assertIn("-> BOUNDED", self.run_tool([js], on))

    # -- OPEN: the event, never discarded
    def test_intermittent_stall_is_open(self):
        # Astra's counterexample: ON +30 s on ~10% of launches - 3 of 29 boots attach 30 s late
        js, on, _ = self.run_set(over={k: dict(attach_at=31.3, cpup=31.5, hud=113.0) for k in (6, 16, 25)})
        v = self.run_tool([js], on)
        self.assertIn("-> OPEN (kept, never discarded): 3 of 29 ON boots had the stall event", v)
        self.assertIn("A: attach at +31.30 s", v)

    def test_each_event_is_open(self):
        for over, word in (({2: dict(attach_at=6.0, cpup=6.2)}, "A: attach at +6.00 s"),
                           ({2: dict(attach_at=2.6, cpup=1.3)}, "W: the CP waited 1.30 s"),
                           ({2: dict(swaps=2)}, "R: attach after 2 guest VdSwaps"),
                           ({2: dict(result="FAILURE")}, "F: outcome FAILURE"),
                           ({2: dict(extra_lines=[(300.0, "[THREAD_FATAL] tid=9 x")])}, "F: thread fatal in the log"),
                           ({2: dict(cpstart=4.0)}, "C: the CP's first command waited 4.00 s"),
                           ({2: dict(hud=96.0)}, "H: the HUD request at +96.00 s")):
            js, on, _ = self.run_set(over=over)
            v = self.run_tool([js], on)
            self.assertIn("-> OPEN", v, word)
            self.assertIn(word, v)
            shutil.rmtree(os.path.dirname(js))

    def test_runner_voided_attempt_is_counted(self):
        # Astra round 2 (P1), runner to evaluator: native_proof voids a dump-stall attempt (the roll moved to
        # <roll>_void1 and listed in the proof's `voids`, not its rows) and retries it - that ON boot must count, as F
        import argparse
        from datetime import datetime
        import native_proof as npf
        js, on, rolls = self.run_set()
        void = self.roll("r1", "on3_void1", True, extra_lines=[(40.0, "[GPU_HANG] ==== trigger H1 | x")])
        a = argparse.Namespace(plan="devcreate_close", out=os.path.dirname(js), repo=os.path.dirname(js), exe=None,
                               base=None, dry_run=False, only="", skip="")
        rn = npf.Runner(a, npf.plans()["devcreate_close"])
        rn.started, rn.exe_id = datetime(2026, 10, 14, 5, 0, 0), "a" * 12
        rn.results = [{"step": os.path.basename(p), "kind": "roll", "verdict": "DONE", "detail": "",
                       "paths": {"roll": rn.rel(p)}} for p in rolls]
        rn.voids = [{"step": "on3", "roll": rn.rel(void), "evidence": ["hang: [GPU_HANG] ==== trigger H1 | x"]}]
        rn.write_proof()  # the runner's own proof JSON (Runner.write_proof), over the set's
        self.assertEqual(os.path.normcase(rn.path("proof", ".json")), os.path.normcase(js))
        v = self.run_tool([js], on)
        self.assertIn("-> OPEN (kept, never discarded): 1 of 30 ON boots had the stall event", v)
        self.assertIn("F: an attempt voided and retried by the runner", v)

    def test_failed_rows_are_events(self):
        # P1-2 / P1-1: a row ABORT with outcome SUCCESS, and a crash whose row FAILED before its folder existed
        js, on, rolls = self.run_set()
        self.proof("r1", rolls, verdicts={on[7]: "ABORT"})
        v = self.run_tool([js], on)
        self.assertIn("-> OPEN", v)
        self.assertIn("F: the run's row says ABORT", v)
        shutil.rmtree(on[8])
        self.proof("r1", rolls, verdicts={on[8]: "FAIL"})
        v = self.run_tool([js], on)
        self.assertIn("-> OPEN", v)
        self.assertIn("F: the run's row says FAIL", v)


# (what, file, old, new, [BootStall tests the mutant must fail])
MUTANTS = (
    ("N_MIN 28", "boot_stall.py", '"dcl-stall-v1": {"n_min": 29, "baked": False}', '"dcl-stall-v1": {"n_min": 28, "baked": False}',
     ["test_28_on_boots_are_under_counted"]),
    ("no late-attach event", "boot_stall.py", "ATTACH_STALL = 5.0", "ATTACH_STALL = 50.0",
     ["test_intermittent_stall_is_open", "test_each_event_is_open"]),
    ("events not OPEN", "boot_stall.py", "    if events:", "    if False:", ["test_intermittent_stall_is_open"]),
    ("an omitted ON boot accepted", "boot_stall.py", "            if is_on and k not in named:", "            if False:",
     ["test_every_on_boot_must_be_counted"]),
    ("a failed row not an event", "boot_stall.py", "    if v not in ROLL_OK:", "    if False:", ["test_failed_rows_are_events"]),
    ("abort markers ignored", "boot_stall.py", "    what, line = bp.abort_marker(r[\"roll\"])\n    if what:",
     "    what, line = bp.abort_marker(r[\"roll\"])\n    if False:", ["test_each_event_is_open"]),
    ("the run's identity ignored", "boot_stall.py", "        design += pr.identity_problems(doc, rows, list(rows))", "        pass",
     ["test_another_runs_identity_is_refused"]),
    ("the tag on every verdict", "boot_stall.py", 'TAG if verdict == "BOUNDED" else "BOOT_STALL_SET"', "TAG",
     ["test_intermittent_stall_is_open"]),
    ("an approximate bound", "boot_stall.py", "return 1.0 - 0.05 ** (1.0 / n) if n > 0 else 1.0", "return 3.0 / n if n > 0 else 1.0",
     ["test_29_clean_on_boots_are_bounded"]),
    ("voided attempts not counted (round 2 P1)", "boot_stall.py", "            if is_on:\n                voided.append((k, v))",
     "            if False:\n                voided.append((k, v))", ["test_runner_voided_attempt_is_counted"]),
    # the bake's design (native/devcreate-fix): its minimum, the declared instruments, the allow-list
    ("the bake design's minimum", "boot_stall.py", '"dcf-stall-v2": {"n_min": 29, "baked": True}',
     '"dcf-stall-v2": {"n_min": 28, "baked": True}', ["test_bake_28_on_boots_are_under_counted"]),
    ("an undeclared instrument accepted", "boot_stall.py", "    vol = set(bp.VOLATILE) | set(allow)",
     "    vol = set(bp.VOLATILE) | set(INSTRUMENT_KEYS)", ["test_bake_undeclared_instrument_is_inconclusive"]),
    ("a foreign allow key accepted", "boot_stall.py", "        if k not in INSTRUMENT_KEYS:", "        if False:",
     ["test_bake_foreign_allow_key_is_inconclusive"]),
    # Astra round 1 on the branch (the five P2s of the stall criterion)
    ("a graded roll's PASS row read as a failure (v1's bug)", "boot_stall.py", 'ROLL_OK = ("DONE", "PASS")', 'ROLL_OK = ("DONE",)',
     ["test_bake_every_on_boot_is_bounded"]),
    ("no WP1 wait event", "boot_stall.py", '    if isinstance(r.get("C"), (int, float)) and not r["C"] <= U_MAX:', "    if False:",
     ["test_bake_wp1_wait_is_open", "test_each_event_is_open"]),
    ("no late-HUD event", "boot_stall.py", "HUD_STALL = 95.0", "HUD_STALL = 950.0",
     ["test_bake_late_hud_is_open", "test_each_event_is_open"]),
    ("the WP1 line not required", "boot_stall.py", '            need.append(("C", "CP start (WP1) line"))', "            pass",
     ["test_bake_boot_without_wp1_line_is_unclassifiable"]),
    ("the gate's boots not enumerated", "proof_rows.py", '        elif isinstance(row, dict) and row.get("kind") == "gate":',
     "        elif False:", ["test_bake_every_on_boot_is_bounded"]),
    ("a gate roll's exit not read", "proof_rows.py", "            elif ec == 0:", "            elif True:",
     ["test_bake_gate_roll_exit_is_a_failure"]),
    ("retained attempts not counted", "boot_stall.py", "                if is_on:\n                    retained.append((k, (js, row)))",
     "                if False:\n                    retained.append((k, (js, row)))", ["test_bake_retained_attempt_is_counted"]),
    ("the cache condition not required", "boot_stall.py", "            if not st:", "            if False:",
     ["test_bake_unstaged_run_is_inconclusive"]),
    ("the exe paths not checked", "boot_stall.py", '        if len(paths) != 1 or paths == [""]:', "        if False:",
     ["test_bake_two_exe_paths_are_inconclusive"]),
    ("the stage root not checked", "boot_stall.py",
     '        if roots and paths != [""] and not all(any(p.startswith(rt) for rt in roots) for p in paths):', "        if False:",
     ["test_bake_exe_path_outside_the_stage_root_is_inconclusive"]),
)
COPIED = ("boot_stall.py", "boot_pairs.py", "proof_rows.py")


@unittest.skipIf(os.environ.get("BOOT_STALL_TOOL"), "inside a mutant run")
class Mutants(unittest.TestCase):
    def test_every_mutant_breaks_its_test(self):
        texts = {}
        for name in COPIED:
            with open(os.path.join(HERE, name), encoding="utf-8") as f:
                texts[name] = f.read().replace("\r\n", "\n")
        root = os.path.join(REPO, "archive")
        tmp = tempfile.mkdtemp(prefix="boot_stall_mutants_", dir=root)
        try:
            for what, name, old, new, tests in MUTANTS:
                self.assertEqual(texts[name].count(old), 1, "mutant %r: target not exactly once in %s" % (what, name))
                # a mutant of the same byte length written within the same second as the previous copy would be served
                # from the previous copy's .pyc (Python validates size + mtime): no cache between mutants
                shutil.rmtree(os.path.join(tmp, "__pycache__"), ignore_errors=True)
                for other in COPIED:
                    with open(os.path.join(tmp, other), "w", encoding="utf-8") as f:
                        f.write(texts[other].replace(old, new) if other == name else texts[other])
                env = dict(os.environ, BOOT_STALL_TOOL=os.path.join(tmp, "boot_stall.py"))
                p = subprocess.run([sys.executable, "-m", "unittest"] + ["test_boot_stall.BootStall." + t for t in tests],
                                   cwd=HERE, env=env, capture_output=True, text=True)
                self.assertNotEqual(p.returncode, 0, "mutant %r survived %s:\n%s" % (what, tests, p.stderr[-800:]))
                self.assertRegex(p.stderr, r"FAILED \((failures|errors)=\d+", "%s:\n%s" % (what, p.stderr[-800:]))
                self.assertTrue(any(("FAIL: %s " % t) in p.stderr or ("ERROR: %s " % t) in p.stderr for t in tests), what)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
