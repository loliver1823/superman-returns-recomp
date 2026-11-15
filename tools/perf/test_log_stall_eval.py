#!/usr/bin/env python3
"""test_log_stall_eval.py - tools/perf/log_stall_eval.py's rule on synthetic run.logs (app/log-async).

    python tools/perf/test_log_stall_eval.py

Each case writes a run.log the way the game does (text mode: \\r\\n; census lines whose bytes= is computed from the
bytes actually before them) and checks the verdict of each check, including the counterexamples each check exists for:
no census line (an old exe), a census bytes= that is off by one line (a lost line), a gap between census lines, an F
frame= skip, a CP interval of 4.85 s (the tcmp1 stall), a pace max over the bound, a producer wait over 100 ms, and an
injected roll without the injected line / with the CP silent during it. Scratch logs go under the checkout's archive/
(never C:).
"""
import datetime
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import log_stall_eval as lse  # noqa: E402

T0 = datetime.datetime(2026, 10, 14, 19, 45, 0)


class Log:
    """A run.log under construction: lines are (seconds since T0, text without the stamp prefix). The census lines
    carry the counters the writer would: waits= / max_wait_ms= from the producer-wait lines before them, plus the
    `hidden` waits (counted, no line: unwitnessed=) and `lost` a case sets."""

    def __init__(self):
        self.raw = b""
        self.waits = 0
        self.max_wait = 0.0
        self.hidden = 0       # waits counted as unwitnessed (no line)
        self.hidden_max = 0.0
        self.lost = 0

    def add(self, sec, text, sev="INFO "):
        t = T0 + datetime.timedelta(seconds=sec)
        line = "[%s.%03d] [%s] [BOOT] %s\n" % (t.strftime("%Y-%m-%d %H:%M:%S"), t.microsecond // 1000, sev, text)
        self.raw += line.encode()

    def wait(self, sec, ms):
        self.waits += 1
        self.max_wait = max(self.max_wait, ms)
        self.add(sec, "[LOG_STALL] producer wait tid=10676 name=- ms=%.1f slices=2 bytes=300 queued=16777000 "
                      "cap=16777216" % ms, "WARN ")

    def census(self, sec, kind="periodic", skew=0):
        before = len(self.raw)  # the raw line bytes so far (the file adds one '\r' per '\n': file_bytes)
        self.add(sec, "[LOG_STALL] census %s t=%.1fs lines=1 bytes=%d written=0 writes=0 slow=0 max_write_ms=0.0 "
                      "waits=%d unwitnessed=%d wait_ms=0.0 max_wait_ms=%.1f peak=0 cap=16777216 lost=%d" % (
                          kind, sec, before + skew, self.waits + self.hidden, self.hidden,
                          max(self.max_wait, self.hidden_max), self.lost), "WARN ")

    def file_bytes(self):
        return self.raw.replace(b"\n", b"\r\n")


def roll(census=True, end=240.0, f_every=0.05, skip_frame=None, gap_at=None, gap_s=0.0, census_every=10.0,
         drop_census_after=None, pace_max=80.0, waits=(), inject=None, cp_silent_in_inject=False, bad_bytes_at=None,
         hidden_wait=None, lost=0, final=False):
    """A gameplay-shaped log: F lines every f_every s (census rolls), pace every 2 s, census every census_every s.
    hidden_wait=(at_s, ms, unwitnessed): a wait the census counts from at_s on with no line (unwitnessed=True: the
    census also says unwitnessed=1; False: it does not - the counters disagree); lost: census lost= from 100 s on;
    final: a final census at the end."""
    lg = Log()
    lg.add(0.0, "[LOG_PROFILE] player")
    t, frame, next_census, next_pace = 0.5, 0, census_every, 2.0
    inj_lo = inj_hi = None
    if inject:
        inj_at, inj_ms = inject
        inj_lo, inj_hi = inj_at - inj_ms / 1000.0, inj_at
    events = []
    while t < end:
        if census:
            silent = cp_silent_in_inject and inj_lo is not None and inj_lo <= t <= inj_hi
            if not silent:
                f = frame + (1 if skip_frame is not None and frame == skip_frame else 0)
                events.append((t, "f", f))
            frame += 1
        t += f_every
        if gap_at is not None and abs(t - gap_at) < f_every / 2:
            t += gap_s
    s = census_every
    while s < end:
        if drop_census_after is None or s < drop_census_after:
            events.append((s, "c", None))
        s += census_every
    p = 2.0
    while p < end:
        events.append((p, "p", None))
        p += 2.0
    for w_at, w_ms in waits:
        events.append((w_at, "w", w_ms))
    if inject:
        events.append((inject[0], "i", inject[1]))
    if hidden_wait:
        events.append((hidden_wait[0], "h", hidden_wait))
    if lost:
        events.append((100.0, "l", lost))
    events.sort(key=lambda e: (e[0], e[1]))
    for sec, kind, v in events:
        if kind == "f":
            lg.add(sec, "[PM4_COV] F frame=%d pk=300 t0=120" % v)
        elif kind == "h":
            if v[2]:
                lg.hidden += 1
            else:
                lg.waits += 1  # counted, but neither a line nor unwitnessed=: the census disagrees with the file
            lg.hidden_max = max(lg.hidden_max, v[1])
        elif kind == "l":
            lg.lost = v
        elif kind == "c":
            lg.census(sec, skew=(1 if bad_bytes_at is not None and abs(sec - bad_bytes_at) < 1e-6 else 0))
        elif kind == "p":
            mx = pace_max if (gap_at is None or abs(sec - gap_at) > 2.0) else max(pace_max, gap_s * 1000.0)
            lg.add(sec, "[HIGHFPS_PACE] w=1 2.00s presents=40 (20.0/s) interval_ms p50=50.00 p95=70.00 p99=75.00 "
                        "max=%.2f | shown_step_ms p50=50.00" % mx)
        elif kind == "w":
            lg.wait(sec, v)
        elif kind == "i":
            lg.add(sec, "[LOG_STALL] write tid=900 name=SR log writer op=file bytes=812 ms=%.1f queued=40960 "
                        "count=1 injected=1" % v, "WARN ")
    lg.add(end, "[PERF] t=%ds the last line" % int(end))
    if final:
        lg.census(end, kind="final")
    return lg


class LogStallEvalTest(unittest.TestCase):
    def setUp(self):
        os.makedirs(os.path.join(REPO, "archive"), exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="lse_", dir=os.path.join(REPO, "archive"))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def grade(self, lg, **kw):
        path = os.path.join(self.dir, "run.log")
        with open(path, "wb") as f:
            f.write(lg.file_bytes())
        return lse.grade(path, **kw)

    def verdicts(self, r):
        return {k: v["verdict"] for k, v in r["checks"].items()}

    def test_clean_census_roll_passes(self):
        r = self.grade(roll(), census=True)
        self.assertEqual(r["verdict"], "PASS", r)
        self.assertEqual(set(r["checks"]), {"witness", "complete", "waits", "cp", "pace"})

    def test_plain_roll_has_no_cp_check(self):
        r = self.grade(roll(census=False), census=False)
        self.assertEqual(r["verdict"], "PASS", r)
        self.assertNotIn("cp", r["checks"])

    def test_no_census_line_is_an_old_exe(self):
        r = self.grade(roll(census_every=1000.0), census=True)
        self.assertEqual(self.verdicts(r)["witness"], "FAIL")
        self.assertIn("not the async writer", r["checks"]["witness"]["evidence"])

    def test_census_gap_fails_witness(self):
        r = self.grade(roll(drop_census_after=100.0), census=True)
        v = self.verdicts(r)
        self.assertEqual(v["complete"], "FAIL")  # the last census is 140 s before the end
        r = self.grade(roll(census_every=30.0), census=True)
        self.assertEqual(self.verdicts(r)["witness"], "FAIL")

    def test_census_bytes_off_by_one_fails_complete(self):
        r = self.grade(roll(bad_bytes_at=120.0), census=True)
        self.assertEqual(self.verdicts(r)["complete"], "FAIL")
        self.assertIn("bytes=", r["checks"]["complete"]["evidence"])

    def test_frame_skip_fails_complete(self):
        r = self.grade(roll(skip_frame=2000), census=True)
        self.assertEqual(self.verdicts(r)["complete"], "FAIL")
        self.assertIn("F frame=", r["checks"]["complete"]["evidence"])

    def test_tcmp1_stall_fails_cp_and_pace(self):
        r = self.grade(roll(gap_at=201.0, gap_s=4.85), census=True)
        v = self.verdicts(r)
        self.assertEqual((v["cp"], v["pace"]), ("FAIL", "FAIL"))
        self.assertIn("max 4900.0 ms", r["checks"]["cp"]["evidence"])

    def test_boot_gap_is_outside_the_window(self):
        r = self.grade(roll(gap_at=20.0, gap_s=5.0), census=True)
        self.assertEqual(self.verdicts(r)["cp"], "PASS")

    def test_pace_over_bound_fails(self):
        r = self.grade(roll(pace_max=1500.0), census=True)
        self.assertEqual(self.verdicts(r)["pace"], "FAIL")

    def test_producer_wait_over_bound_fails(self):
        r = self.grade(roll(waits=[(130.0, 250.0)]), census=True)
        self.assertEqual(self.verdicts(r)["waits"], "FAIL")
        r = self.grade(roll(waits=[(130.0, 40.0)]), census=True)
        self.assertEqual(self.verdicts(r)["waits"], "PASS")

    def test_injected_roll(self):
        r = self.grade(roll(inject=(125.0, 5012.0)), census=True, inject_ms=5000)
        self.assertEqual(r["verdict"], "PASS", r)
        self.assertIn("F lines logged by the CP while the writer slept", r["checks"]["inject"]["evidence"])

    def test_injected_roll_without_the_line_fails(self):
        r = self.grade(roll(), census=True, inject_ms=5000)
        self.assertEqual(self.verdicts(r)["inject"], "FAIL")

    def test_injected_roll_short_stall_fails(self):
        r = self.grade(roll(inject=(125.0, 900.0)), census=True, inject_ms=5000)
        self.assertEqual(self.verdicts(r)["inject"], "FAIL")

    def test_injected_roll_cp_silent_fails(self):
        r = self.grade(roll(inject=(125.0, 5012.0), cp_silent_in_inject=True), census=True, inject_ms=5000)
        v = self.verdicts(r)
        self.assertEqual(v["inject"], "FAIL")
        self.assertEqual(v["cp"], "FAIL")

    def test_injected_roll_with_a_wait_fails(self):
        r = self.grade(roll(inject=(125.0, 5012.0), waits=[(124.0, 30.0)]), census=True, inject_ms=5000)
        self.assertEqual(self.verdicts(r)["inject"], "FAIL")

    # Astra round 1 (P2): the two false passes it reproduced, and their neighbours.
    def test_only_a_final_census_fails_witness(self):
        r = self.grade(roll(census_every=1000.0, final=True), census=True)
        self.assertEqual(self.verdicts(r)["witness"], "FAIL", r)
        self.assertIn("the first 240.0 s after", r["checks"]["witness"]["evidence"])
        self.assertEqual(r["verdict"], "FAIL")

    def test_census_only_from_late_in_the_roll_fails_witness(self):
        r = self.grade(roll(census_every=10.0), census=True)
        self.assertEqual(self.verdicts(r)["witness"], "PASS", r)
        r = self.grade(roll(census_every=120.0, end=130.0), census=True)  # the only census at 120 s of 130
        self.assertEqual(self.verdicts(r)["witness"], "FAIL")
        self.assertIn("the first 120.0 s after", r["checks"]["witness"]["evidence"])

    def test_unwitnessed_long_wait_fails(self):
        r = self.grade(roll(hidden_wait=(130.0, 4850.0, True)), census=True)
        self.assertEqual(self.verdicts(r)["waits"], "FAIL", r)
        self.assertIn("max_wait_ms=4850.0 > 100", r["checks"]["waits"]["evidence"])
        self.assertEqual(r["verdict"], "FAIL")

    def test_unwitnessed_short_wait_passes(self):
        r = self.grade(roll(hidden_wait=(130.0, 40.0, True)), census=True)
        self.assertEqual(self.verdicts(r)["waits"], "PASS", r)

    def test_census_waits_disagreeing_with_the_file_fails(self):
        r = self.grade(roll(hidden_wait=(130.0, 40.0, False)), census=True)
        self.assertEqual(self.verdicts(r)["waits"], "FAIL", r)
        self.assertIn("wait lines before it", r["checks"]["waits"]["evidence"])

    def test_lost_lines_fail_complete(self):
        r = self.grade(roll(lost=2), census=True)
        self.assertEqual(self.verdicts(r)["complete"], "FAIL", r)
        self.assertIn("lost=2", r["checks"]["complete"]["evidence"])

    def test_injected_roll_with_a_counted_wait_fails(self):
        r = self.grade(roll(inject=(125.0, 5012.0), hidden_wait=(124.0, 30.0, True)), census=True, inject_ms=5000)
        self.assertEqual(self.verdicts(r)["inject"], "FAIL", r)
        self.assertIn("waits=1 (want 0)", r["checks"]["inject"]["evidence"])

    def test_proof_mode_and_unreadable(self):
        base = os.path.join(self.dir, "proof")
        for tag, lg in (("census0", roll()), ("plain", roll(census=False)), ("inject", roll(inject=(125.0, 5012.0)))):
            d = os.path.join(base, "lsa_" + tag, "drive", "attempt1")
            os.makedirs(d)
            with open(os.path.join(d, "run.log"), "wb") as f:
                f.write(lg.file_bytes())
        js = os.path.join(self.dir, "out.json")
        self.assertEqual(lse.main(["--proof", base, "--json", js]), 0)
        os.remove(os.path.join(base, "lsa_plain", "drive", "attempt1", "run.log"))
        self.assertEqual(lse.main(["--proof", base]), 2)


if __name__ == "__main__":
    unittest.main()
