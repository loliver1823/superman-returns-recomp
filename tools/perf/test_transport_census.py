#!/usr/bin/env python3
"""Tests for transport_census.py on synthetic run.logs (no game, no archive reads).

    python tools/perf/test_transport_census.py

Each negative control must FAIL its named check: a missing F record, a producer line that does not add up to the
CP's counts, a census whose last report is far from the run's end, a gated roll that still holds a packet under
--strict-zero, a producer the map does not know; and the COVERAGE WITNESS (native/composed-census): a counter that
runs without a frame end (tcmp2 census1), goes dark mid-run, has no witness, or has no CP present count to track is
refused (exit 2, DARK_CENSUS / NO_CENSUS), never read as zero. SR_TCMP2_CENSUS=<folder> (tcmp2_census0/run.log,
tcmp2_census1/run.log) also runs the check on the live tcmp2 pair.
"""
import contextlib
import io
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import transport_census as tc  # noqa: E402

T0 = "2026-10-14 11:00:00.000"


def ts(sec):
    m, s = divmod(sec, 60)
    return "2026-10-14 11:%02d:%06.3f" % (m, s)


def keystr(keys):
    ops = ",".join("%s:%d" % (k, v) for k, v in sorted(keys.items()) if not k.startswith("r") and not k.startswith("o"))
    regs = ",".join("%s:%d/%d" % (k[1:], v, v) for k, v in sorted(keys.items()) if k.startswith("r"))
    return ops, regs


def cp_line(sec, why, swaps):
    """kernel/gpu_command_processor census(): the CP's own present count (swaps=)."""
    return ("[%s] [INFO ] [BOOT] [GPU_CP] %s rptr=6 wptr=6 doorbells=%d packets=0 draws=0 isr=0 (faults 0) swaps=%d "
            "waits=0 (blocked 0) scratch=0 memWrites=0 zpd=0 (fake 0) ext=0 badIB=0 desync=0 | ringless ib=0 bs=0 "
            "desync=0 seg=0" % (ts(sec), why, swaps, swaps))


def write_log(path, frames, per_frame, ingame_frame=10, report_every=10, drop_frame=None, skew_producer=None,
              tail=0, extra_end_seconds=0.0, census=True, late=None, early=None, early_until=0, armed=True, cp=True,
              dark_from=None, record_swaps=False, garble_frame=None, cp_every=10, unsigned_at=(), missed_after=(),
              missed_counted=False, swapblock=False, terminal_tail=False, extra_at=None):
    """frames F records, one per second; per_frame = {producer va: {key: count}} each frame from frame 1 on; a report
    (CUM + ENDS + inner lines) every report_every frames; the ingame marker at ingame_frame. early: producers only in
    frames 1 .. early_until (the opening, before the census interval). The coverage witness (native/composed-census):
    armed = the counter's [PM4_COV] armed line and every report's ENDS line (False: an exe before them); cp = the CP's
    [GPU_CP] census present counts, printed when the CP's count reaches 1-4 or a multiple of cp_every (after the
    frame's F line and report, as the CP thread prints them); dark_from = from that frame on the counter runs without a
    frame end (no F line, no report: tcmp2 census1) while the CP keeps presenting; record_swaps = every frame ended at a
    present with no XE_SWAP packet (the ENDS line's present=). Frame-boundary faults (Astra round 1 on
    native/composed-census): unsigned_at = F frames that end at an XE_SWAP applying no present (no CP present, not in
    the ENDS presents); missed_after = F frames after which one present ends no frame (a CP present; in the ENDS
    presents only with missed_counted); swapblock = SR_NATIVE_SWAP_BLOCK's periodic [NATIVE_FLUSH] swapBlock line;
    terminal_tail = a graceful quit's snapshot after the last frame ([GPU_CP] terminal, the tail frame's line + F,
    the terminal report, then the verify's [GPU_CP] terminal again); extra_at = {frame: {producer va: {key: count}}}:
    packets added to those frames only (merged into the frame's own)."""
    lines = []
    cum = {}
    cumk = {}
    t = 0.0
    st = {"sw": 0, "pr": 0, "xe": 0, "rec": 0, "tails": 0}

    def present(sec, counted=True):
        """the CP applies a present: the counter's presents (unless counted is False: already counted, or never), then
        the CP's own count and its sample (gfx/pm4_engine applySwap: pm4cov::presented, then cp.swap -> onSwap)"""
        st["sw"] += 1
        if counted:
            st["pr"] += 1
        if cp and (st["sw"] <= 4 or st["sw"] % cp_every == 0):
            lines.append(cp_line(sec, "census", st["sw"]))

    def report(why, sec, f):
        tot = sum(cumk.values())
        ops, regs = keystr(cumk)
        lines.append("[%s] [INFO ] [BOOT] [PM4_COV] CUM %s frames=%d pk=%d pk/f mean=1.0 min=0 max=0 t0=0 "
                     "t0dw=0 t3=0 t3dw=0 pred=0 ringIb=0 zero=0 trunc=0 chainTail=0 t1=0 t2=0 t1skip=0 acc=%d "
                     "unst=0 chain=0 setConst=0/0/0/0/0 loadConst=0/0/0/0 snapOverflow=0 ops=%s regs=%s"
                     % (ts(sec), why, f, tot, tot, ops, regs))
        if armed:  # the ENDS line comes with the armed exe
            lines.append("[%s] [INFO ] [BOOT] [PM4_COV] ENDS %s frames=%d xeSwap=%d present=%d presents=%d"
                         % (ts(sec), why, f, st["xe"], st["rec"], st["pr"]))
        for va, keys in sorted(cum.items()):
            k2 = dict(keys)
            if va == skew_producer:
                k2[sorted(k2)[0]] += 1
            ks = ",".join("%s:%d" % (k, v) for k, v in sorted(k2.items()))
            pk = sum(k2.values())
            vaf, name = ("00000000", va) if va in ("UNSTAMPED", "RING_UNSTAMPED") else (va, "fn_" + va)
            lines.append("[%s] [INFO ] [BOOT] [PM4_COVMAP] %s inner=%s name=%s pk=%d dw=%d keys=%s"
                         % (ts(sec), why, vaf, name, pk, pk, ks))
        if census:
            lines.append("[%s] [INFO ] [BOOT] [PM4_COVMAP] %s internalOrigin pk=0" % (ts(sec), why))

    if armed:
        lines.append("[%s] [INFO ] [BOOT] [PM4_COV] armed brackets=%d SR_PM4_COVERAGE=unset SR_NATIVE_BRACKETS=%s "
                     "SR_NATIVE_RECORD=unset SR_NATIVE_DRAW=unset | frames end at an XE_SWAP packet or at a present the "
                     "CP applies without one" % (ts(0.1), 1 if census else 0, "1" if census else "unset"))
    for f in range(1, frames + 1):
        t = float(f)
        if f == ingame_frame:
            lines.append("[%s] [INFO ] [BOOT] [APT_REQ] #17 path='screens/hud/main_hud' holder=1 lr=2" % ts(t - 0.5))
            lines.append("[%s] [INFO ] [BOOT] [PC_CONTROLS] context gameplay (igc type 0) config A invX=0 invY=0 "
                         "mode on-foot camera poll=1" % ts(t - 0.4))
        if swapblock and f % 20 == 0:
            lines.append("[%s] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapBlock calls=%d decl=0 applied=%d"
                         % (ts(t - 0.2), f, f))
        if dark_from is not None and f >= dark_from:
            present(t + 0.01)
            continue
        fk = {}
        now = dict(per_frame)
        if late and f > frames - tail:
            now.update(late)  # producers only in the uncovered tail (after the last report)
        if early and f <= early_until:
            now.update(early)  # producers only in the opening (before the interval's first report)
        if extra_at and f in extra_at:
            for va, keys in extra_at[f].items():
                merged = dict(now.get(va, {}))
                for k, v in keys.items():
                    merged[k] = merged.get(k, 0) + v
                now[va] = merged
        for va, keys in now.items():
            for k, v in keys.items():
                fk[k] = fk.get(k, 0) + v
                cum.setdefault(va, {})
                cum[va][k] = cum[va].get(k, 0) + v
        for k, v in fk.items():
            cumk[k] = cumk.get(k, 0) + v
        pk = sum(fk.values())  # the census's packets include type-2 NOPs (oN keys), the F key lists do not
        ops, regs = keystr(fk)
        # how this frame ends: at an XE_SWAP packet (packet3 ends it, its present follows - or none when unsigned), or
        # at a record's present (presented() counts the present, then ends the frame: the report sees it counted)
        packet = f in unsigned_at or not record_swaps
        if packet:
            st["xe"] += 1
        else:
            st["rec"] += 1
            st["pr"] += 1
        if f == garble_frame:  # a malformed F record (counts as an F line, parses as none)
            lines.append("[%s] [INFO ] [BOOT] [PM4_COV] F frame=%d pk=? (truncated)" % (ts(t), f))
        elif f != drop_frame:
            lines.append("[%s] [INFO ] [BOOT] [PM4_COV] F frame=%d pk=%d t0=0 t0dw=0 t3=0 t3dw=0 pred=0 ringIb=0 zero=0 "
                         "trunc=0 d0=%d dN=0 acc=%d unst=0 ops=%s regs=%s" % (ts(t), f, pk, pk, pk, ops, regs))
        if f % report_every == 0 and f <= frames - tail:
            report("periodic", t, f)
        if f not in unsigned_at:
            present(t + 0.01, counted=packet)  # a record's present was counted before its frame end
        if f in missed_after:  # a present that ends no frame
            present(t + 0.02, counted=missed_counted)
    if terminal_tail:  # the graceful quit's snapshot: the CP parked, its census first, then the drained partial frame
        t += 0.5
        lines.append(cp_line(t, "terminal", st["sw"]))
        f = frames + 1
        lines.append("[%s] [INFO ] [BOOT] [PM4_COV] terminal tail frame=%d pk=0 (the drained work after the last "
                     "XE_SWAP; its F line follows and the CUM frames= below counts it)" % (ts(t), f))
        lines.append("[%s] [INFO ] [BOOT] [PM4_COV] F frame=%d pk=0 t0=0 t0dw=0 t3=0 t3dw=0 pred=0 ringIb=0 zero=0 "
                     "trunc=0 d0=0 dN=0 acc=0 unst=0 ops= regs=" % (ts(t), f))
        report("terminal", t, f)
        lines.append(cp_line(t + 0.1, "terminal", st["sw"]))  # the verify's re-render
    lines.append("[%s] [INFO ] [BOOT] end" % ts(t + extra_end_seconds))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


BASE = {"82112050": {"64": 1, "r48": 1, "o1": 2}, "82111EC8": {"3C": 4, "54": 1, "r05": 5},
        "820FBBF8": {"r44": 3, "r48": 2, "o1": 5}}
TARGET = {"820FBBF8": {"r44": 3, "r48": 2, "o1": 5}}
ZERO = {}


def run(case, fn):
    try:
        fn()
        print("PASS %s" % case)
        return 0
    except AssertionError as e:
        print("FAIL %s: %s" % (case, e))
        return 1
    except Exception as e:  # a field the grader does not write is that case's failure, not the suite's crash
        print("FAIL %s: %s %s" % (case, type(e).__name__, e))
        return 1


def main():
    # synthetic logs under this checkout's archive/ (gitignored; outputs never go to C:)
    root = os.path.join(os.path.dirname(os.path.dirname(HERE)), "archive", "test_transport_census")
    os.makedirs(root, exist_ok=True)
    d = tempfile.mkdtemp(prefix="run_", dir=root)
    fails = 0

    def good():
        p = os.path.join(d, "good.log")
        write_log(p, 60, BASE)
        r = tc.analyse(p)
        assert "error" not in r, r
        c = r["checks"]
        assert c["frames_complete"] and c["window_complete"] and c["covers_run"] and c["reconciled"] \
            and c["pk_reconciled"], c
        assert r["interval"]["A_frames"] == 10 and r["interval"]["B_frames"] == 60, r["interval"]
        pf = {x["producer"]: x["per_frame"] for x in r["producers"]}
        assert abs(pf["82112050"] - 4.0) < 1e-9 and abs(pf["82111EC8"] - 10.0) < 1e-9, pf
        assert abs(r["per_frame"]["pk"] - (4 + 10 + 10)) < 1e-9, r["per_frame"]  # NOP pads count as packets
        ob = {x["producer"]: x["obligation"] for x in r["producers"]}
        assert ob["82112050"] == "p2.transport:SWAP" and ob["82111EC8"] == "p2.transport:GATE", ob
    fails += run("good roll: interval, per-frame producers, map, reconciliation", good)

    def missing_frame():
        p = os.path.join(d, "gap.log")
        write_log(p, 60, BASE, garble_frame=33)
        r = tc.analyse(p)
        c = r["checks"]
        assert r["coverage"]["state"] == "live" and not c["frames_complete"] and not c["window_complete"], c
    fails += run("negative: a malformed F record is INCOMPLETE", missing_frame)

    def dropped_frame():
        p = os.path.join(d, "drop.log")
        write_log(p, 60, BASE, drop_frame=33)
        r = tc.analyse(p)
        # 39 F lines before the CP's swaps=40 line: the census lost a present - no usable census, not an exact count
        assert "error" in r and r["coverage"]["state"] == "dark" and r["coverage"]["first_behind"]["swaps"] == 40, r
    fails += run("negative: a dropped F record (fewer F lines than CP presents) is DARK, no usable census", dropped_frame)

    # THE COVERAGE WITNESS (native/composed-census): tcmp2 census1's shape and its relatives
    def coverage(case, want_state, want_verdict, **kw):
        b, t = os.path.join(d, "cvb_%s.log" % case), os.path.join(d, "cvt_%s.log" % case)
        write_log(b, 60, BASE)
        write_log(t, 60, kw.pop("per_frame", ZERO), **kw)
        j = os.path.join(d, "cv_%s.json" % case)
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tc.main(["--base", b, "--target", t, "--json", j, "--transport-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert out["target"]["coverage"]["state"] == want_state, out["target"]["coverage"]
        assert out["verdict"] == want_verdict and rc == (0 if want_verdict == "PASS" else 2), (rc, out.get("verdict"))
        return out

    def dark_whole():
        out = coverage("dark", "dark", "DARK_CENSUS", dark_from=1)
        cv = out["target"]["coverage"]
        assert cv["armed"] and cv["f_lines"] == 0 and cv["cp_presents"] == 60 and "DARK" in out["unusable"]["target"], cv
        assert "base" not in out["unusable"], out["unusable"]
    fails += run("negative: armed, the CP presenting, no F line ever (tcmp2 census1 on a witnessed exe): DARK_CENSUS, "
                 "never a zero", dark_whole)
    fails += run("negative: the counter going dark mid-run (F lines stop at frame 30, the CP presents to 60): "
                 "DARK_CENSUS, not a short interval", lambda: coverage("dark30", "dark", "DARK_CENSUS", dark_from=30))
    fails += run("negative: no armed line and no F line while the CP presents (tcmp2 census1 itself, an exe before "
                 "the witness): no usable census",
                 lambda: coverage("unwitnessed", "unwitnessed", "NO_CENSUS", armed=False, dark_from=1))
    fails += run("negative: no [GPU_CP] present count: nothing to check the census against (fail closed)",
                 lambda: coverage("nocp", "unchecked", "NO_CENSUS", cp=False))
    fails += run("an exe before the witness whose F lines track every CP present (tcmp2 census0): live, graded",
                 lambda: coverage("legacy", "live", "PASS", armed=False, per_frame=TARGET))

    def record_frames():
        out = coverage("records", "live", "PASS", record_swaps=True, per_frame=TARGET)
        cv = out["target"]["coverage"]
        assert cv["witness"] == "armed" and cv["ends"] == {"why": "periodic", "frames": 60, "xeSwap": 0, "present": 60,
                                                           "presents": 60, "tails_before": 0}, cv
        assert out["checks"]["target_coverage_live"] is True and out["checks"]["base_coverage_live"] is True, out["checks"]
    fails += run("the composed shape fixed: every frame ended at a present record (ENDS present=60, xeSwap=0), the "
                 "F lines tracking the CP: live and graded", record_frames)

    # Astra round 1 on native/composed-census, P2-1: frame ends that are not presents can hide presents that ended no
    # frame. Ten unsigned XE_SWAPs (an F line, no CP present), each followed by a present that ends no frame: 60 F
    # lines, 60 CP presents, equal at every CP sample. F >= swaps passed it (PASS, exit 0); the ENDS accounting refuses.
    pairs = tuple(range(3, 60, 6))  # 3, 9, .. 57: ten pairs
    fails += run("negative (P2-1): ten unsigned XE_SWAPs each paired with a present that ended no frame (the CP count "
                 "equal at every sample): the ENDS accounting is broken - UNACCOUNTED, no usable census",
                 lambda: coverage("p21_pairs", "unaccounted", "NO_CENSUS", per_frame=TARGET, unsigned_at=pairs,
                                  missed_after=pairs))
    fails += run("negative (P2-1): the same pairs on an exe without the ENDS line under SR_NATIVE_SWAP_BLOCK (record "
                 "presents possible): UNACCOUNTED",
                 lambda: coverage("p21_legacy", "unaccounted", "NO_CENSUS", per_frame=TARGET, unsigned_at=pairs,
                                  missed_after=pairs, armed=False, swapblock=True))
    fails += run("negative (P2-1, the literal scenario): ten unsigned XE_SWAPs early, ten presents ending no frame late "
                 "(60 F lines, 60 CP presents at the end): the exact reconciliation finds the surplus at the samples "
                 "between - SURPLUS",
                 lambda: coverage("p21_literal", "surplus", "NO_CENSUS", per_frame=TARGET, armed=False,
                                  unsigned_at=tuple(range(11, 21)), missed_after=tuple(range(41, 51))))
    fails += run("negative (P2-1): one unsigned XE_SWAP alone: SURPLUS at the next CP sample (exact, not >=)",
                 lambda: coverage("p21_one", "surplus", "NO_CENSUS", per_frame=TARGET, unsigned_at=(15,)))
    # the composed shape (record presents) with no CP sample after the unsigned XE_SWAP: report 10 may read one present
    # pending (an XE_SWAP frame end since the report before), report 20 (record frame ends only) must be exact
    fails += run("negative (P2-1): an unsigned XE_SWAP among record presents before any CP sample sees it: the next "
                 "record-only report's exact ENDS accounting refuses it - UNACCOUNTED",
                 lambda: coverage("p21_records", "unaccounted", "NO_CENSUS", per_frame=TARGET, record_swaps=True,
                                  unsigned_at=(5,), cp_every=1000))

    # P2-2: the last CP sample need not cover the final report. 269 F lines, 270 CP presents: present 251 ends no frame;
    # CP samples every 120 (1-4, 120, 240), reports every 10 frames, the final at 260. The census interval ends at the
    # last report a reconciled sample verifies (240): the unverified frames are the tail, never graded
    def p22(case, **kw):
        b, t = os.path.join(d, "p22b_%s.log" % case), os.path.join(d, "p22t_%s.log" % case)
        write_log(b, 269, BASE, cp_every=120)
        write_log(t, 269, TARGET, cp_every=120, missed_after=(250,), **kw)
        j = os.path.join(d, "p22_%s.json" % case)
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tc.main(["--base", b, "--target", t, "--json", j, "--transport-zero"])
        return rc, json.load(open(j, encoding="utf-8"))

    def p22_legacy():
        rc, out = p22("legacy", armed=False)
        tg = out["target"]
        assert tg["coverage"]["state"] == "live" and tg["coverage"]["final_report_frames"] == 260, tg["coverage"]
        assert tg["interval"]["B_frames"] == 240 and tg["coverage"]["verified_report_frames"] == 240, tg["interval"]
        assert tg["checks"]["tail_frames"] == 29 and tg["checks"]["final_tail_frames"] == 9 and \
            tg["checks"]["covers_run"] is True, tg["checks"]
        assert out["base"]["interval"]["B_frames"] == 240 and rc == 0, (rc, out.get("verdict"))
    fails += run("P2-2: the final report (260) after the last CP sample (240) is unverified: the interval ends at "
                 "report 240, the missed frame end at 251 stays in the ungraded tail", p22_legacy)

    def p22_counted():
        rc, out = p22("counted", missed_counted=True, record_swaps=True)
        assert rc == 2 and out["target"]["coverage"]["state"] == "unaccounted", out["target"]["coverage"]
    fails += run("negative P2-2: the composed shape (record presents) with the ENDS line: the present the counter saw "
                 "end no frame breaks the final report's exact accounting: UNACCOUNTED", p22_counted)

    def p22_closing():
        b, t = os.path.join(d, "p22b_close.log"), os.path.join(d, "p22t_close.log")
        write_log(t, 269, TARGET, cp_every=135, missed_after=(250,), armed=False)  # a CP sample at 270 closes it
        r = tc.analyse(t)
        assert r["coverage"]["state"] == "dark" and r["coverage"]["first_behind"]["swaps"] == 270, r["coverage"]
    fails += run("negative P2-2: the same roll with a CP sample after the final report: DARK (269 frame ends, 270 "
                 "presents)", p22_closing)

    def unverified():
        p = os.path.join(d, "unverified.log")
        write_log(p, 9, BASE, report_every=5, cp_every=1000)  # CP samples at 1-4 only, the one report at 5 after them
        r = tc.analyse(p)
        assert "error" in r and r["coverage"]["state"] == "unverified", r
    fails += run("negative: no census report before the last CP sample: UNVERIFIED (fail closed)", unverified)

    def graceful_tail():
        p = os.path.join(d, "graceful.log")
        write_log(p, 60, BASE, terminal_tail=True)
        r = tc.analyse(p)
        assert "error" not in r and r["coverage"]["state"] == "live" and r["coverage"]["tails"] == 1, r
        assert r["interval"]["B_frames"] == 61 and r["checks"]["covers_run"] and r["checks"]["reconciled"], \
            (r["interval"], r["checks"])
    fails += run("a graceful quit's terminal snapshot: the tail frame is accounted (its tail line), the terminal "
                 "report verified by the verify's [GPU_CP] terminal line", graceful_tail)

    def real_tcmp2():
        # the live logs themselves, when this checkout has them (archive/ is local evidence; skipped elsewhere)
        root_ = os.environ.get("SR_TCMP2_CENSUS")
        if not root_:
            print("   (skipped: set SR_TCMP2_CENSUS to the folder holding tcmp2_census0/run.log, tcmp2_census1/run.log)")
            return
        b, t = (os.path.join(root_, n, "run.log") for n in ("tcmp2_census0", "tcmp2_census1"))
        j = os.path.join(d, "tcmp2.json")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tc.main(["--base", b, "--target", t, "--json", j, "--transport-zero"])
        out = json.load(open(j, encoding="utf-8"))
        cb, ct = out["base"]["coverage"], out["target"]["coverage"]
        assert rc == 2 and out["verdict"] == "DARK_CENSUS", (rc, out.get("verdict"))
        assert cb["state"] == "live" and cb["witness"] == "F-track" and cb["cp_lines_behind"] == 0 and \
            cb["cp_lines_ahead"] == 0 and cb["swapblock_lines"] == 0 and cb["cp_lines"] == 45, cb
        assert out["base"]["interval"]["B_frames"] <= cb["cp_presents"], out["base"]["interval"]
        # dark: the counter ran (its [NATIVE_CPTIME] line: a classified packet), no frame ever ended, 5,640 presents
        assert ct["state"] == "dark" and ct["counter_ran"] and ct["f_lines"] == 0 and ct["cp_presents"] == 5640 and \
            ct["swapblock_lines"] == 26, ct
    fails += run("tcmp2 (exe 403bd798be42): census0 live by F-track (no SWAP_BLOCK, 45 / 45 samples exact), census1 "
                 "DARK (the counter ran, 0 F lines, 5,640 CP presents)", real_tcmp2)

    def skew():
        p = os.path.join(d, "skew.log")
        write_log(p, 60, BASE, skew_producer="82111EC8")
        c = tc.analyse(p)["checks"]
        # the skew is in every report, so the delta cancels except where it first appears: A carries it too ->
        # force the skew into B only by writing a second log whose last report differs
        assert c["reconciled"], c  # a constant offset cancels over (A, B]: the check is on the interval delta
    fails += run("a constant producer offset cancels over the interval", skew)

    def skew_b():
        p = os.path.join(d, "skewb.log")
        write_log(p, 60, BASE)
        with open(p, encoding="utf-8") as fh:
            text = fh.read().splitlines()
        # bump the LAST report's inner=82111EC8 key 3C by one: the producers no longer add up to the F lines
        for i in range(len(text) - 1, -1, -1):
            if "inner=82111EC8" in text[i]:
                text[i] = text[i].replace("3C:%d," % (4 * 60), "3C:%d," % (4 * 60 + 1))
                break
        with open(p, "w", encoding="utf-8") as fh:
            fh.write("\n".join(text) + "\n")
        c = tc.analyse(p)["checks"]
        assert not c["reconciled"] and "3C" in c["mismatched"], c
    fails += run("negative: a producer line that does not add up fails reconciliation", skew_b)

    def late_tail():
        p = os.path.join(d, "tail.log")
        write_log(p, 600, BASE, report_every=10, tail=300)
        c = tc.analyse(p)["checks"]
        assert not c["covers_run"], c
    fails += run("negative: a census ending 300 frames before the run's end does not cover it", late_tail)

    def pair_zero():
        b, t = os.path.join(d, "b.log"), os.path.join(d, "t.log")
        write_log(b, 60, BASE)
        write_log(t, 60, TARGET)
        j = os.path.join(d, "pair.json")
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--strict-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == 1 and out["checks"]["target_pm4_cov_zero"] is False, out["checks"]
        assert set(out["removed_per_frame"]) == {"82112050", "82111EC8"}, out["removed_per_frame"]
        assert [x["producer"] for x in out["remaining"]] == ["820FBBF8"], out["remaining"]
    fails += run("negative: --strict-zero fails while a producer remains", pair_zero)

    def pair_zero_pass():
        b, t = os.path.join(d, "b2.log"), os.path.join(d, "t2.log")
        write_log(b, 60, BASE)
        write_log(t, 60, ZERO)
        j = os.path.join(d, "pair2.json")
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--strict-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == 0 and out["verdict"] == "PASS", out["checks"]
    fails += run("--strict-zero passes on an empty target with a complete census", pair_zero_pass)

    def tail_packets():
        b, t = os.path.join(d, "b4.log"), os.path.join(d, "t4.log")
        write_log(b, 60, BASE)
        write_log(t, 65, ZERO, tail=5, late={"82111EC8": {"3C": 4}})
        j = os.path.join(d, "pair4.json")
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--strict-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == 1 and out["checks"]["target_pm4_cov_zero"] is True and out["checks"]["target_tail_zero"] is False, \
            out["checks"]
    fails += run("negative: packets only in the uncovered tail still fail [PM4_COV] 0", tail_packets)

    def tz(case, target, want_rc, want):
        b, t = os.path.join(d, "tzb_%s.log" % case), os.path.join(d, "tzt_%s.log" % case)
        write_log(b, 60, BASE)
        write_log(t, 60, target)
        j = os.path.join(d, "tz_%s.json" % case)
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--transport-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == want_rc, (rc, out["checks"])
        for k, v in want.items():
            assert out["checks"][k] is v, (k, out["checks"])

    fails += run("--transport-zero passes with only the draw residue left",
                 lambda: tz("pass", TARGET, 0, {"target_transport_zero": True, "base_transport_exercised": True}))
    fails += run("negative: --transport-zero fails while a transport producer remains",
                 lambda: tz("gate", {"82111EC8": {"3C": 4}, "820FBBF8": {"r44": 3}}, 1, {"target_transport_zero": False}))
    fails += run("negative: --transport-zero fails on an INTERRUPT inside a draw flush's scope",
                 lambda: tz("sig", {"820FBBF8": {"r44": 3, "54": 1}}, 1, {"target_no_foreign_transport_keys": False}))
    fails += run("negative: --transport-zero fails on an unstamped packet in the interval",
                 lambda: tz("unst", {"UNSTAMPED": {"r44": 1}}, 1, {"target_unattributed_zero": False}))
    fails += run("negative: --transport-zero fails on 820F0048's keys under Swap (devcreate off)",
                 lambda: tz("dc", {"82112050": {"63": 1, "r0F": 1}}, 1, {"target_transport_zero": False}))
    fails += run("a declined resolve's own WAIT_UNTIL (r05) is not a transport signature",
                 lambda: tz("r05", {"8210C5F8": {"r05": 1, "46": 1}}, 0, {"target_no_foreign_transport_keys": True}))

    def fz(case, target, want_rc, want, want_pk, early=None):
        b, t = os.path.join(d, "fzb_%s.log" % case), os.path.join(d, "fzt_%s.log" % case)
        write_log(b, 60, BASE)
        write_log(t, 60, target, early=early, early_until=5)
        j = os.path.join(d, "fz_%s.json" % case)
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--flush-residue-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == want_rc, (rc, out["checks"])
        for k, v in want.items():
            assert out["checks"][k] is v, (k, out["checks"])
        assert out["target"]["flush_residue"]["pk"] == want_pk, out["target"]["flush_residue"]

    fails += run("--flush-residue-zero passes when the S1 flushes leave nothing (flush_residue.pk written as 0)",
                 lambda: fz("pass", ZERO, 0, {"target_flush_residue_zero": True, "base_flush_residue_exercised": True},
                            0))
    fails += run("negative: --flush-residue-zero fails while the S1 flush's guest leaf remains",
                 lambda: fz("leaf", TARGET, 1, {"target_flush_residue_zero": False}, 500))  # 10 a frame, (10, 60]
    fails += run("the construns keys under an S1 flush (r20 / r0A / 3C / r23) are not flush residue",
                 lambda: fz("construns", {"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1}}, 0,
                            {"target_flush_residue_zero": True}, 0))
    fails += run("negative: a declined resolve is flush residue (row 9)",
                 lambda: fz("row9", {"8210C5F8": {"r05": 1, "46": 1}}, 1, {"target_flush_residue_zero": False}, 100))
    # 821075E0's alignment pad (native/flush-residue-2, Astra round 1): construns' only as the census's witnessed key o6
    # (a NOP right before the leaf's run header); every o1 on an S1 line is residue, however few
    fails += run("821075E0's witnessed pad (o6) beside its construns run is not flush residue",
                 lambda: fz("pad", {"820FC000": {"r20": 2, "r0A": 4, "3C": 2, "r23": 2, "o6": 1}}, 0,
                            {"target_flush_residue_zero": True, "target_flush_residue_zero_from_boot": True}, 0))
    fails += run("negative: an unwitnessed NOP (o1) on a construns-only S1 line stays flush residue (o1 <= r23 proves "
                 "nothing: two pads on one run, a stray NOP)",
                 lambda: fz("pad_o1", {"820FC000": {"r20": 2, "r0A": 4, "3C": 2, "r23": 2, "o1": 1}}, 1,
                            {"target_flush_residue_zero": False}, 50))
    fails += run("negative: an S1 leaf's key beside the witnessed pad is residue (the pad stays construns')",
                 lambda: fz("pad_leaf", {"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "o6": 1, "r44": 1}}, 1,
                            {"target_flush_residue_zero": False}, 50))  # r44 only, 1 a frame over (10, 60]
    fails += run("negative: o1 alone on an S1 line stays flush residue",
                 lambda: fz("pad_alone", {"820FBBF8": {"o1": 1}}, 1, {"target_flush_residue_zero": False}, 50))
    # FROM BOOT (Astra round 1): an opening residue before the interval's first report fails, though the interval is 0
    fails += run("negative: an opening residue before the interval (r23:10, o1:20 a frame in frames 1-5) fails from boot",
                 lambda: fz("opening", {"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "o6": 1}}, 1,
                            {"target_flush_residue_zero": True, "target_flush_residue_zero_from_boot": False}, 0,
                            early={"820FBBF8": {"r23": 10, "o1": 20}}))
    fails += run("the opening's witnessed pads (o6 beside r23) pass from boot",
                 lambda: fz("opening_o6", {"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "o6": 1}}, 0,
                            {"target_flush_residue_zero": True, "target_flush_residue_zero_from_boot": True}, 0,
                            early={"820FBBF8": {"r23": 10, "o6": 10}}))

    # p1.row3_construns' zero (native/construns-bake): the SDK body's preamble (3C 1 + r0A 2 a call) on the S1 lines
    cr_base = dict(BASE, **{"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "r44": 3, "o1": 5}})

    def cz(case, target, want_rc, want, base=None, want_residue=None):
        b, t = os.path.join(d, "czb_%s.log" % case), os.path.join(d, "czt_%s.log" % case)
        write_log(b, 60, cr_base if base is None else base)
        write_log(t, 60, target)
        j = os.path.join(d, "cz_%s.json" % case)
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--construns-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == want_rc, (rc, out["checks"])
        for k, v in want.items():
            assert out["checks"][k] is v, (k, out["checks"])
        if want_residue is not None:
            assert out["target_construns_runs_residue_per_frame"] == want_residue, out
        return out

    fails += run("--construns-zero passes when no SDK-body preamble remains on the S1 lines (sync 3 a call in the base)",
                 lambda: cz("pass", {"820FC000": {"r44": 3, "o1": 5}}, 0,
                            {"base_construns_exercised": True, "target_construns_sync_zero": True,
                             "base_tessellated_zero": True, "target_tessellated_zero": True}, want_residue=0.0))
    fails += run("an S1 fallback's r20 / r23 runs without the preamble pass and are reported as residue",
                 lambda: cz("fallback", {"820FC000": {"r20": 1, "r23": 1, "r44": 3}}, 0,
                            {"target_construns_sync_zero": True}, want_residue=2.0))
    fails += run("negative: --construns-zero fails while the SDK body still writes its preamble (820FC000)",
                 lambda: cz("sdk", {"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1}}, 1,
                            {"target_construns_sync_zero": False}))
    fails += run("negative: the preamble under another S1 flush (820FB6E8) fails too",
                 lambda: cz("gen", {"820FB6E8": {"r0A": 2, "3C": 1}}, 1, {"target_construns_sync_zero": False}))
    fails += run("negative: a tessellated draw (820FC478) in the target fails, mapped (not UNMAPPED)",
                 lambda: cz("tess", {"820FC478": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "2B": 1}}, 1,
                            {"target_tessellated_zero": False, "target_construns_sync_zero": True,
                             "target_mapped": True}))
    fails += run("negative: a base without the preamble is not exercised",
                 lambda: cz("unexercised", {"820FC000": {"r44": 3}}, 1, {"base_construns_exercised": False},
                            base=BASE))

    # Astra round 1 on native/construns-bake (P2): the preamble with its attribution lost lands on an unattributed line;
    # the census must fail closed there (both bins), while the pump's ordinary unstamped bin selects still pass
    fails += run("negative: --construns-zero fails on an UNSTAMPED preamble (3C / r0A with the attribution lost)",
                 lambda: cz("unstamped", {"820FC000": {"r44": 3}, "UNSTAMPED": {"3C": 1, "r0A": 2, "r20": 1, "r23": 1}},
                            1, {"target_unattributed_sync_zero": False, "target_construns_sync_zero": True}))
    fails += run("negative: --construns-zero fails on a RING_UNSTAMPED preamble too",
                 lambda: cz("ringunst", {"820FC000": {"r44": 3}, "RING_UNSTAMPED": {"3C": 1, "r0A": 2}}, 1,
                            {"target_unattributed_sync_zero": False}))
    fails += run("the pump's unstamped bin selects (60-63, no 3C / r0A) do not fail --construns-zero",
                 lambda: cz("binsel", {"820FC000": {"r44": 3}, "RING_UNSTAMPED": {"62": 4, "63": 4}}, 0,
                            {"target_unattributed_sync_zero": True, "target_construns_sync_zero": True}))

    def cz_fields():
        p = os.path.join(d, "cz_fields.log")
        write_log(p, 60, cr_base)
        r = tc.analyse(p)
        c = r["construns"]
        # (10, 60]: 50 frames, one SDK-body call a frame: sync 3 a frame, runs 2, construns group 5 (the KEY_SPLIT)
        assert c["sync_pk"] == 150 and c["runs_pk"] == 100 and c["tess_pk"] == 0 and c["pk"] == 250, c
        assert c["unattributed_sync_pk"] == 0, c
        assert abs(c["sdk_calls_per_frame"] - 1.0) < 1e-9 and r["groups"]["construns"] == 5.0, (c, r["groups"])
    fails += run("the construns block: exact signature counts and the KEY_SPLIT group", cz_fields)

    # THE DEFAULT PATH'S PM4 ZERO (native/transport-bake): --pm4-zero, with --allow-devcreate-reemit for a default path
    # that keeps SR_NATIVE_DEVCREATE off (820F0048's 60 / 61 / 62 / 63, r05, r0F under Swap, one each a present)
    reemit = {"82112050": {"60": 1, "61": 1, "62": 1, "63": 1, "r05": 1, "r0F": 1}}

    def pz(case, target, want_rc, want, allow=True, base=None, want_verdict=None, base_shows=None, **kw):
        b, t = os.path.join(d, "pzb_%s.log" % case), os.path.join(d, "pzt_%s.log" % case)
        write_log(b, 60, BASE if base is None else base)
        write_log(t, kw.pop("frames", 60), target, **kw)
        j = os.path.join(d, "pz_%s.json" % case)
        args = ["--base", b, "--target", t, "--json", j, "--pm4-zero"] + (["--allow-devcreate-reemit"] if allow else [])
        if base_shows:
            args += ["--base-shows", base_shows]
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tc.main(args)
        out = json.load(open(j, encoding="utf-8"))
        assert rc == want_rc, (rc, out.get("checks"), out.get("verdict"), out["target"].get("pm4_zero"))
        if want_verdict:
            assert out.get("verdict") == want_verdict, (out.get("verdict"), out.get("checks"))
        for k, v in want.items():
            assert out["checks"][k] is v, (k, out["checks"], out["target"].get("pm4_zero"))
        return out

    def pz_pass():
        out = pz("pass", ZERO, 0, {"base_transport_exercised": True, "target_ingame_frames": True,
                                   "target_pm4_zero_from_ingame": True, "target_pm4_zero_interval": True}, allow=False)
        z = out["target"]["pm4_zero"]
        assert z["ingame_frames"] == 51 and z["ingame_pk"] == 0 and z["interval_other_pk"] == 0 and z["allow"] == [], z
    fails += run("--pm4-zero passes on a target with no packet in gameplay (51 F lines from the ingame marker)", pz_pass)

    def pz_reemit():
        out = pz("reemit", reemit, 0, {"target_pm4_zero_from_ingame": True, "target_pm4_zero_interval": True})
        z = out["target"]["pm4_zero"]
        assert z["reemit_pk"] == 300 and z["reemit_per_frame"] == 6.0 and z["ingame_allowed_pk"] == 306, z
        assert tuple(z["allow"]) == ("60", "61", "62", "63", "r05", "r0F"), z["allow"]
    fails += run("--pm4-zero --allow-devcreate-reemit passes on 820F0048's re-emit alone (6.0 a frame, reported)",
                 pz_reemit)
    fails += run("negative: the re-emit without the allowance fails both views",
                 lambda: pz("reemit_strict", reemit, 1, {"target_pm4_zero_from_ingame": False,
                                                         "target_pm4_zero_interval": False}, allow=False))
    fails += run("negative: tcmp3's remainder (construns' preamble + runs on 820FC000) fails even with the allowance",
                 lambda: pz("construns", dict(reemit, **{"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1}}), 1,
                            {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": False}))
    fails += run("negative: the re-emit's keys on another producer (the pump's bin selects, RING_UNSTAMPED) fail: the "
                 "allowance is 82112050's line only",
                 lambda: pz("binsel", {"RING_UNSTAMPED": {"60": 1, "61": 1, "62": 1, "63": 1}}, 1,
                            {"target_pm4_zero_interval": False, "target_pm4_zero_from_ingame": True}))
    fails += run("negative: two re-emits in one frame fail the from-ingame view (at most one of each key a present)",
                 lambda: pz("twice", {"82112050": {"60": 2, "61": 1, "62": 1, "63": 1, "r05": 1, "r0F": 1}}, 1,
                            {"target_pm4_zero_from_ingame": False}))
    fails += run("negative: a type-2 pad beside the re-emit (no ops / regs key, counted in pk) fails both views",
                 lambda: pz("pad", dict(reemit, **{"82112050": dict(reemit["82112050"], o1=1)}), 1,
                            {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": False}))
    fails += run("negative: a second WAIT_UNTIL in the frame from another producer (BlockUntilIdle's r05) fails both views",
                 lambda: pz("idle_r05", dict(reemit, **{"820FDCB8": {"r05": 1}}), 1,
                            {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": False}))
    fails += run("negative: packets only in the uncovered tail after B fail the from-ingame view",
                 lambda: pz("tail", ZERO, 1, {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": True},
                            frames=65, tail=5, late={"82111EC8": {"3C": 4, "54": 1}}))
    fails += run("negative: packets after the ingame marker but before the interval's first report fail the "
                 "from-ingame view",
                 lambda: pz("opening", ZERO, 1, {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": True},
                            early={"82111EC8": {"54": 1}}, early_until=10))
    fails += run("packets before the ingame marker (boot, the movies) are not gameplay's",
                 lambda: pz("boot", ZERO, 0, {"target_pm4_zero_from_ingame": True, "target_pm4_zero_interval": True},
                            early={"UNSTAMPED": {"r44": 3}}, early_until=9))
    fails += run("negative: a base without the transport group is not exercised",
                 lambda: pz("unexercised", ZERO, 1, {"base_transport_exercised": False}, base=TARGET))

    # THE ALLOWANCE'S ATTRIBUTION (Astra round 1 on native/transport-bake, P2): the allowance was subtracted from every
    # gameplay frame but attributed only inside (A, B] - a BlockUntilIdle r05 before A, or five after B, PASSED. Each
    # negative control here passed the old grader (exit 0). The marker at frame 15 puts gameplay frames 15-20 before A
    # (the report at 20) in the segment (10, 20] that straddles it.
    whole_foreign = {"RING_UNSTAMPED": {"60": 1, "61": 1, "62": 1, "63": 1}, "820FDCB8": {"r05": 1},
                     "UNSTAMPED": {"r0F": 1}}
    fails += run("negative (old grader PASS): a BlockUntilIdle r05 in gameplay before A fails - not a whole re-emit, and "
                 "not 82112050's in a segment whose pre-marker frames hold no r05",
                 lambda: pz("idle_before_a", ZERO, 1, {"target_reemit_whole": False, "target_reemit_attributed": False,
                                                       "target_pm4_zero_interval": True},
                            ingame_frame=15, extra_at={16: {"820FDCB8": {"r05": 1}}}, want_verdict="FAIL"))
    fails += run("negative (old grader PASS): five BlockUntilIdle r05 after B (the uncovered tail) fail - none is a whole "
                 "re-emit",
                 lambda: pz("idle_after_b", ZERO, 1, {"target_reemit_whole": False, "target_pm4_zero_interval": True},
                            frames=65, tail=5, late={"820FDCB8": {"r05": 1}}, want_verdict="FAIL"))
    fails += run("negative (old grader PASS): a whole six-key set from other producers before A fails by attribution "
                 "alone (its shape is the re-emit's)",
                 lambda: pz("forged_before_a", ZERO, 1, {"target_reemit_whole": True, "target_reemit_attributed": False},
                            ingame_frame=15, extra_at={16: whole_foreign}, want_verdict="FAIL"))
    def pz_forged_after_b():
        # B = the last report a CP sample verifies: with the CP sampled every 25 presents the report at 60 follows the
        # last sample (50), so B = 50 and (50, 60] is attributed by the final report alone; a whole foreign set in frame
        # 55, in a frame without the re-emit (the target holds none), passes the shape and fails the attribution
        out = pz("forged_after_b", ZERO, 1, {"target_reemit_whole": True, "target_reemit_attributed": False,
                                            "target_pm4_zero_interval": True},
                 cp_every=25, extra_at={55: whole_foreign}, want_verdict="FAIL")
        z = out["target"]["pm4_zero"]
        assert out["target"]["interval"]["B_frames"] == 50 and z["attributed_through_frames"] == 60, \
            (out["target"]["interval"], z)
        assert z["reemit_foreign_pk"] == 6, z
    fails += run("negative (old grader PASS): a whole six-key set from other producers after B (before the final "
                 "report) fails by attribution", pz_forged_after_b)
    fails += run("negative (old grader PASS): a whole six-key set from other producers after the final report cannot "
                 "be attributed: INCONCLUSIVE (exit 3), never a PASS",
                 lambda: pz("forged_tail", ZERO, 3, {"target_reemit_whole": True, "target_reemit_attributed": True},
                            frames=65, tail=5, late=whole_foreign, want_verdict="INCONCLUSIVE"))

    def pz_tail_reemit():
        out = pz("reemit_tail", reemit, 3, {"target_reemit_whole": True, "target_reemit_attributed": True},
                 frames=65, tail=5, want_verdict="INCONCLUSIVE")
        z = out["target"]["pm4_zero"]
        assert z["unattributed_tail_frames"] == 5 and z["unattributed_tail_reemit_pk"] == 30 and \
            z["reemit_unverified_pk"] == 30 and z["attributed_through_frames"] == 60, z
        assert "graceful-quit" in out["inconclusive"], out["inconclusive"]
    fails += run("the re-emit itself after the final report (a killed roll's tail) is unattributed: INCONCLUSIVE", pz_tail_reemit)

    def pz_graceful():
        out = pz("reemit_graceful", reemit, 0, {"target_reemit_whole": True, "target_reemit_attributed": True},
                 frames=65, tail=5, terminal_tail=True, want_verdict="PASS")
        z = out["target"]["pm4_zero"]
        assert z["unattributed_tail_frames"] == 0 and z["reemit_unverified_pk"] == 0 and \
            z["attributed_through_frames"] == 66, z
    fails += run("the same tail under the graceful quit: the terminal report attributes it - PASS", pz_graceful)
    fails += run("a foreign r05 BEFORE the marker in the segment that straddles it cannot be placed: INCONCLUSIVE",
                 lambda: pz("straddle", reemit, 3, {"target_reemit_attributed": True, "target_reemit_whole": True},
                            ingame_frame=15, extra_at={12: {"820FDCB8": {"r05": 1}}}, want_verdict="INCONCLUSIVE"))
    fails += run("a foreign set in a segment wholly before the marker is not gameplay's: PASS",
                 lambda: pz("pre_segment", reemit, 0, {"target_reemit_attributed": True, "target_reemit_whole": True},
                            ingame_frame=15, extra_at={5: whole_foreign}, want_verdict="PASS"))
    fails += run("negative (old grader PASS): one re-emit key short in a gameplay frame (r0F missing) fails the whole "
                 "re-emit rule",
                 lambda: pz("short", reemit, 1, {"target_reemit_whole": False},
                            extra_at={30: {"82112050": {"r0F": -1}}}, want_verdict="FAIL"))

    def pz_dark():
        b, t = os.path.join(d, "pzb_dark.log"), os.path.join(d, "pzt_dark.log")
        write_log(b, 60, BASE)
        write_log(t, 60, ZERO, dark_from=1)
        j = os.path.join(d, "pz_dark.json")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tc.main(["--base", b, "--target", t, "--json", j, "--pm4-zero", "--allow-devcreate-reemit"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == 2 and out["verdict"] == "DARK_CENSUS" and "checks" not in out, (rc, out.get("verdict"))
    fails += run("negative: a dark target census is no data under --pm4-zero (DARK_CENSUS, exit 2), never a zero", pz_dark)

    def pz_usage():
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                tc.main(["--base", "x", "--target", "y", "--allow-devcreate-reemit"])
        except SystemExit as e:
            assert e.code == 2, e.code
            return
        raise AssertionError("--allow-devcreate-reemit without --pm4-zero was accepted")
    fails += run("negative: --allow-devcreate-reemit alone is a usage error", pz_usage)

    # THE DEVCREATE BAKE'S PAIR (native/devcreate-fix): --pm4-zero --base-shows devcreate - the transport is native in both
    # arms, so the base must show 820F0048's re-emit (all six keys on 82112050's line); the target gets NO allowance
    def dc_pass():
        out = pz("dc_pass", ZERO, 0, {"base_devcreate_exercised": True, "target_ingame_frames": True,
                                      "target_pm4_zero_from_ingame": True, "target_pm4_zero_interval": True},
                 allow=False, base=reemit, base_shows="devcreate", want_verdict="PASS")
        z = out["target"]["pm4_zero"]
        assert "base_transport_exercised" not in out["checks"], out["checks"]
        assert z["allow"] == [] and z["ingame_pk"] == 0 and z["reemit_pk"] == 0 and z["interval_other_pk"] == 0, z
        assert out["base_reemit_keys"] == {k: 50 for k in ("60", "61", "62", "63", "r05", "r0F")}, out["base_reemit_keys"]
    fails += run("--pm4-zero --base-shows devcreate passes: the base shows the whole re-emit, the target no packet with no "
                 "allowance", dc_pass)
    fails += run("negative: a base without the re-emit (the transport group alone) is not exercised under --base-shows devcreate",
                 lambda: pz("dc_unexercised", ZERO, 1, {"base_devcreate_exercised": False}, allow=False, base=BASE,
                            base_shows="devcreate"))
    fails += run("negative: a base with five of the six re-emit keys (r0F missing) is not exercised",
                 lambda: pz("dc_five", ZERO, 1, {"base_devcreate_exercised": False}, allow=False,
                            base={"82112050": {"60": 1, "61": 1, "62": 1, "63": 1, "r05": 1}}, base_shows="devcreate"))
    fails += run("negative: the re-emit in the target fails both views under --base-shows devcreate (nothing let through)",
                 lambda: pz("dc_reemit", reemit, 1, {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": False,
                                                     "base_devcreate_exercised": True}, allow=False, base=reemit,
                            base_shows="devcreate"))

    def dc_usage():
        for args in (["--base", "x", "--target", "y", "--pm4-zero", "--allow-devcreate-reemit", "--base-shows", "devcreate"],
                     ["--base", "x", "--target", "y", "--base-shows", "devcreate"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    tc.main(args)
            except SystemExit as e:
                assert e.code == 2, e.code
                continue
            raise AssertionError("--base-shows devcreate was accepted with an allowance / without --pm4-zero: %s" % args)
    fails += run("negative: --base-shows devcreate with the allowance, or without --pm4-zero, is a usage error", dc_usage)

    # THE ZPD BAKE'S PAIR (native/zpd-bake): --pm4-zero --base-shows zpd - SR_NATIVE_ZPD=0 keeps 82101E08's guest body, so
    # the base must show the HUD glow queries' EVENT_WRITE_ZPD on D3DQueryIssueBeginEnd 82102BC8's line: a producer line
    # is a bracket entry's (the innermost open scope), 82101E08 and the title callers (82304058, 82721118) are none, so
    # the query's packets - RB_MODECONTROL r22, RB_SURFACE_INFO r20, RB_SAMPLE_COUNT_ADDR r23, 5B, one each a BEGIN or an
    # END - sit on 82102BC8's line (s1c1's: keys=5B:2,r20:2,r22:2,r23:2); the target no packet, no allowance
    glow = {"82102BC8": {"r22": 1, "r20": 1, "r23": 1, "5B": 1}}

    def zb_pass():
        out = pz("zb_pass", ZERO, 0, {"base_zpd_exercised": True, "base_mapped": True, "target_ingame_frames": True,
                                      "target_pm4_zero_from_ingame": True, "target_pm4_zero_interval": True},
                 allow=False, base=glow, base_shows="zpd", want_verdict="PASS")
        assert "base_transport_exercised" not in out["checks"] and "base_devcreate_exercised" not in out["checks"], \
            out["checks"]
        assert out["base_zpd"] == {"zpd_key_pk": 50, "group_pk": 200}, out["base_zpd"]
        assert out["target"]["pm4_zero"]["allow"] == [], out["target"]["pm4_zero"]
    fails += run("--pm4-zero --base-shows zpd passes: the base shows the HUD glow queries (5B on 82102BC8, mapped to "
                 "p2.zpd), the target no packet with no allowance", zb_pass)
    fails += run("negative: the queries on another line (82304058, the HUD glow caller's VA - no bracket entry, so no "
                 "census prints it) do not exercise the zpd base: only 82102BC8's line counts",
                 lambda: pz("zb_caller", ZERO, 1, {"base_zpd_exercised": False}, allow=False,
                            base={"82304058": {"r22": 1, "r20": 1, "r23": 1, "5B": 1}}, base_shows="zpd"))

    def zb_s1c1_line():
        # s1c1 (2026-10-16, exe 9ab92ab16131, chapter 18's landing; native/archive/v1_native/s1c1): the line verbatim
        line = ("[2026-10-16 02:56:55.741] [INFO ] [BOOT] [PM4_COVMAP] periodic inner=82102BC8 "
                "name=D3DQueryIssueBeginEnd_sub_82102BC8 pk=8 dw=18 keys=5B:2,r20:2,r22:2,r23:2")
        m = tc.MAP.search(line)
        assert m and m.group(2) == "inner", line
        va = tc.producer_key(m.group(3).upper(), m.group(4))
        keys = tc.parse_keys(m.group(7))
        assert va == tc.ZPD_PRODUCER == "82102BC8", va
        assert tc.PRODUCER_MAP[va][1] == "p2.zpd" and tc.group_of(tc.PRODUCER_MAP[va][1]) == "zpd", tc.PRODUCER_MAP.get(va)
        assert keys == {"5B": 2, "r20": 2, "r22": 2, "r23": 2} and keys.get(tc.ZPD_KEY) == 2, keys
    fails += run("s1c1's real producer line (82102BC8, 5B:2,r20:2,r22:2,r23:2) parses to the zpd producer, mapped to "
                 "p2.zpd (group zpd), its EVENT_WRITE_ZPD count read", zb_s1c1_line)

    def producers_are_entries():
        # A producer line is a bracket entry's (gfx/pm4_coverage.cpp: g_owners = kEntryCount + 2, the innermost scope):
        # every producer a check reads by VA must be an entry of kernel/native_brackets.gen.cpp, or the check reads a
        # line no census prints (zbk1's base check read 82304058's - the HUD glow caller, no entry)
        src = os.path.join(HERE, "..", "..", "kernel", "native_brackets.gen.cpp")
        text = open(src, encoding="utf-8").read()
        # every row of the entry table owns a census line (g_owners = kEntryCount + 2), whatever its kind
        entries = {e.upper() for e in re.findall(r"^\s*\{0x([0-9A-Fa-f]{8})u, \"[^\"]*\", k\w+,", text, re.M)}
        count = re.search(r"const int kEntryCount = (\d+);", text)
        assert count and len(entries) == int(count.group(1)), (len(entries), count and count.group(1))
        read = {"ZPD_PRODUCER": tc.ZPD_PRODUCER, "REEMIT_PRODUCER": tc.REEMIT_PRODUCER,
                "TESS_PRODUCER": tc.TESS_PRODUCER}
        missing = {k: v for k, v in read.items() if v.upper() not in entries}
        assert not missing, missing
        assert "82304058" not in entries and "82721118" not in entries, "a title caller became a bracket entry"
    fails += run("the producers the census checks read (ZPD_PRODUCER, REEMIT_PRODUCER, TESS_PRODUCER) are bracket entries "
                 "(kernel/native_brackets.gen.cpp); the ZPD title callers 82304058 / 82721118 are not",
                 producers_are_entries)
    fails += run("negative: a base without the glow queries (the transport group alone) is not exercised under "
                 "--base-shows zpd",
                 lambda: pz("zb_unexercised", ZERO, 1, {"base_zpd_exercised": False}, allow=False, base=BASE,
                            base_shows="zpd"))
    fails += run("negative: the glow line without its EVENT_WRITE_ZPD (the runs alone) is not exercised",
                 lambda: pz("zb_no5b", ZERO, 1, {"base_zpd_exercised": False}, allow=False,
                            base={"82102BC8": {"r22": 1, "r20": 1, "r23": 1}}, base_shows="zpd"))
    fails += run("negative: the glow queries in the target (a literal-PM4 gap) fail both views under --base-shows zpd",
                 lambda: pz("zb_target", glow, 1, {"target_pm4_zero_from_ingame": False, "target_pm4_zero_interval": False,
                                                   "base_zpd_exercised": True}, allow=False, base=glow,
                            base_shows="zpd"))

    def zb_usage():
        for args in (["--base", "x", "--target", "y", "--pm4-zero", "--allow-devcreate-reemit", "--base-shows", "zpd"],
                     ["--base", "x", "--target", "y", "--base-shows", "zpd"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    tc.main(args)
            except SystemExit as e:
                assert e.code == 2, e.code
                continue
            raise AssertionError("--base-shows zpd was accepted with an allowance / without --pm4-zero: %s" % args)
    fails += run("negative: --base-shows zpd with the allowance, or without --pm4-zero, is a usage error", zb_usage)

    def census_off():
        b, t = os.path.join(d, "b3.log"), os.path.join(d, "t3.log")
        write_log(b, 60, BASE)
        write_log(t, 60, ZERO, census=False)
        j = os.path.join(d, "pair3.json")
        rc = tc.main(["--base", b, "--target", t, "--json", j, "--strict-zero"])
        out = json.load(open(j, encoding="utf-8"))
        assert rc == 2 and out["verdict"] == "NO_CENSUS", out.get("verdict")
    fails += run("negative: a target without the census never reads as zero", census_off)

    def unmapped():
        p = os.path.join(d, "unmapped.log")
        write_log(p, 60, {"8299ABCD": {"r44": 1}})
        c = tc.analyse(p)["checks"]
        assert c["unmapped"] == ["8299ABCD"], c
    fails += run("negative: an unknown producer is reported UNMAPPED", unmapped)

    print("%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
