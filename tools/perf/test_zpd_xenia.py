#!/usr/bin/env python3
"""test_zpd_xenia.py - p2.zpd's Xenia oracle (tools/perf/zpd_xenia.py) on synthetic frames, offline: no game, no Xenia.

    python tools/perf/test_zpd_xenia.py

A .xtr written in Xenia's trace format (the records gfx/pm4_live_trace.cpp writes: a register write of
RB_SAMPLE_COUNT_ADDR, then EVENT_WRITE_ZPD, per BEGIN / END), Xenia's SR_ZPD_REPORT log for it, and our
[NATIVE_ZPD_T] lines: the pairs (frame, order, slot), the visible / occluded agreement, and every way a pair is
refused (a slot that differs, our query with no count of its own, a report Xenia never retired, a frame of ours with
more queries than the trace) must come out as the grade says. Exit code = failed checks.
"""
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import zpd_xenia as zx  # noqa: E402

FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


def packet(words):
    """A PacketStart record (type 4: base, count, the dwords big-endian as guest memory holds them) + PacketEnd."""
    return struct.pack("<III", 4, 0x1000, len(words)) + struct.pack(">%dI" % len(words), *words) + struct.pack("<I", 5)


def xtr(path, addrs, frames=1):
    """A trace whose frame issues one EVENT_WRITE_ZPD per address (type-0 write of 0x2325, then type-3 0x5B), then a
    swap event. frames > 1 (zpdx3, SR_PM4_TRACE_FRAMES): the events spread over that many frames of one trace, a swap
    packet and a swap event between them, as gfx/pm4_live_trace writes them."""
    body = bytearray(struct.pack("<I", 1) + b"0" * 40 + struct.pack("<I", 0x454107ED))
    body += struct.pack("<IIIIII", 10, 0x2000, 2, 0, 0, 8) + struct.pack("<II", 0x14000500, 0)  # register file
    per = 2 * max(1, -(-(len(addrs) // 2) // frames))  # whole BEGIN / END pairs per frame
    for i, a in enumerate(addrs):
        if i and i % per == 0 and i // per < frames:
            body += packet([0xC0006400, 0x00000000])  # XE_SWAP
            body += struct.pack("<II", 9, 0)          # the frame's swap event
        body += packet([0x00002325, a])           # type 0: RB_SAMPLE_COUNT_ADDR = a
        body += packet([0xC0005B00, 0x00000016])  # type 3 EVENT_WRITE_ZPD, one dword (the initiator)
        body += packet([0xC0002200, 0x00000000])  # an unrelated type-3 packet (DRAW_INDX header, ignored)
    body += struct.pack("<II", 9, 0)              # swap
    with open(path, "wb") as f:
        f.write(bytes(body))


SLOTS = [0x1E3CE000 + 0x40 * i for i in range(30)]


def frame(td, swap, counts_ours, counts_xenia, slot_shift=None, ours_how=None, drop_report=None, extra_ours=0,
          swap_records=False, drain_left=0, fallback_all=False, rc=0, dump="out_dump", frames=1):
    """Write one traced frame: the .xtr, Xenia's log, the dump directory's exec.json record (rc), and our lines;
    -> our run.log text. swap_records: every Xenia report names the OTHER record of its slot (BEGIN <-> END).
    fallback_all: every query of ours delivered strict's stand-in 1, its own count (counts_ours) superseded."""
    n = len(counts_ours)
    addrs = []
    for i in range(n):
        addrs += [SLOTS[i] + 0x20, SLOTS[i]]      # BEGIN, END
    xdir = os.path.join(td, "roll_xtr")
    os.makedirs(xdir, exist_ok=True)
    xtr(os.path.join(xdir, "live_454107ED_%d.xtr" % swap), addrs, frames)
    dump = os.path.join(td, dump)
    os.makedirs(dump, exist_ok=True)
    lines = []
    for k, a in enumerate(addrs):
        if drop_report is not None and k == drop_report:
            continue
        z = 1234 if a & 0x20 else counts_xenia[k // 2]  # a BEGIN's delta is the draws before it (not compared)
        lines.append("i> 00001234 SR_ZPD_REPORT addr=%08X zpass=%d zfail=0 spec=0" % (a ^ 0x20 if swap_records else a, z))
    lines.append("i> 00001234 SR_ZPD_DRAIN left=%d mode=strict" % drain_left)
    with open(os.path.join(dump, "live_454107ED_%d.xenia.log" % swap), "w") as f:
        f.write("\n".join(lines) + "\n")
    # the record read and written here directly (not through zx.read_exec): archive/zpd/old_code_check.py runs these
    # cases against the grader before the review fixes, which had no exec.json
    ep = os.path.join(dump, getattr(zx, "EXEC", "exec.json"))
    rec = json.load(open(ep, encoding="utf-8")) if os.path.exists(ep) else {"frames": {}, "calibration": None}
    rec["frames"]["live_454107ED_%d.xtr" % swap] = rc
    with open(ep, "w", encoding="utf-8") as f:
        json.dump(rec, f)
    ours = []
    pre = "[2026-10-15 00:00:00.000] [INFO ] [BOOT] [NATIVE_ZPD_T] swap=%d id=%d slot=%08X %s guest=%d host=%d"
    for i in range(n + extra_ours):
        slot = SLOTS[i] if i < n else SLOTS[-1]
        if slot_shift is not None and i == slot_shift:
            slot += 0x40 * 7
        how = (ours_how or {}).get(i, "written")
        c = counts_ours[i] if i < n else 3
        if fallback_all:
            ours.append(pre % (swap, 100 + i, slot, "fallback", 1, 0))
            ours.append(pre % (swap, 100 + i, slot, "superseded", c, c * 9))
            continue
        ours.append(pre % (swap, 100 + i, slot, how, c if how != "dead" else 0, c * 9))
    return "\n".join(ours) + "\n"


def run(case, **kw):
    with tempfile.TemporaryDirectory() as td:
        roll = os.path.join(td, "roll")
        os.makedirs(os.path.join(roll, "drive", "attempt1"))
        text = ""
        for swap, ours, xen, extra in case:
            text += frame(td, swap, ours, xen, **dict(kw, **extra))
        with open(zx.run_log_of(roll), "w") as f:
            f.write("[2026-10-15 00:00:00.000] [INFO ] [BOOT] [NATIVE_ZPD_T] swap=99 id=1 slot=00000040 written "
                    "guest=1 host=9\n" + text)   # another frame's line: never paired
        return zx.evaluate(roll, roll + "_xtr", os.path.join(td, "out_dump"), run=False)


def main():
    check(zx.is_end(0x1E3CE000) and not zx.is_end(0x1E3CE020) and zx.slot_of(0x1E3CE023) == 0x1E3CE000,
          "END = the slot itself, BEGIN = slot + 0x20 (endian bits ignored)")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "live_454107ED_7.xtr")
        xtr(p, [0x1E3CE020, 0x1E3CE000, 0x1E3CE060, 0x1E3CE040])
        check(zx.xtr_zpd_events(p) == [0x1E3CE020, 0x1E3CE000, 0x1E3CE060, 0x1E3CE040],
              "the trace's EVENT_WRITE_ZPD addresses in stream order (the register written by type-0 packets)")
    good = [0, 4, 0, 7, 0] * 5                    # 25 queries: 15 occluded, 10 visible
    r = run([(7001, good, good, {})])
    check(r["verdict"] == "PASS" and r["paired"] == 25 and r["agree"] == 25 and r["unpaired"] == 0 and
          r["both_zero"] == 15 and r["both_nonzero"] == 10 and r["exact"] == 25,
          "25 matching answers in one frame: PASS (15 both occluded, 10 both visible) - %s" % r["why"])
    xen = [c if c else 3 for c in good]           # Xenia sees every marker (the HUD clear hypothesis)
    r = run([(7001, good, xen, {})])
    check(r["verdict"] == "FAIL" and r["ours_zero_xenia_nonzero"] == 15 and r["agree_rate"] == 0.4,
          "Xenia visible where we answer 0 (a clear before the query): FAIL, ours 0 / Xenia > 0 = 15 - %s" % r["why"])
    one = list(good)
    one[3] = 0                                    # one edge marker: 1 of 50 differs
    r = run([(7001, good, good, {}), (8001, good, one, {})])
    check(r["verdict"] == "PASS" and r["paired"] == 50 and r["mismatch"] == 1 and r["agree_rate"] == 0.98 and
          r["mismatches"][0]["swap"] == 8001 and r["mismatches"][0]["k"] == 3,
          "two frames, 1 of 50 differing: agree 0.98, PASS, the mismatch named (frame 8001, END 3)")
    # zpdx3: one trace of several frames (SR_PM4_TRACE_FRAMES; the oracle dump plays them all), swap events between
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "live_454107ED_9.xtr")
        xtr(p, [0x1E3CE020, 0x1E3CE000, 0x1E3CE060, 0x1E3CE040, 0x1E3CE0A0, 0x1E3CE080], frames=3)
        raw = open(p, "rb").read()
        check(zx.xtr_zpd_events(p) == [0x1E3CE020, 0x1E3CE000, 0x1E3CE060, 0x1E3CE040, 0x1E3CE0A0, 0x1E3CE080] and
              raw.count(struct.pack("<II", 9, 0)) == 3,
              "a 3-frame trace: every frame's EVENT_WRITE_ZPD in stream order across the swap events")
    r = run([(7001, good, good, {"frames": 4})])
    check(r["verdict"] == "PASS" and r["paired"] == 25 and r["unpaired"] == 0,
          "25 answers over the 4 frames of one trace: all paired, PASS - %s" % r["why"])
    r = run([(7001, good[:10], good[:10], {})])
    check(r["verdict"] == "FAIL" and "paired 10 < 20" in r["why"], "10 pairs: FAIL (fewer than 20)")
    r = run([(7001, good, good, {"slot_shift": 4})])
    check(r["verdict"] == "FAIL" and r["unpaired"] >= 1 and any("slot" in str(u["row"]) for u in r["unpaired_rows"]),
          "our query with another slot than the trace's END: unpaired, FAIL")
    r = run([(7001, good, good, {"ours_how": {2: "dead"}})])
    check(r["verdict"] == "FAIL" and any("the guest read no answer" in str(u["row"]) for u in r["unpaired_rows"]),
          "our query whose answer never reached the guest (dead): unpaired, FAIL")
    r = run([(7001, good, good, {"ours_how": {2: "superseded"}})])
    check(r["verdict"] == "FAIL" and any("the guest read no answer" in str(u["row"]) for u in r["unpaired_rows"]),
          "a count that was computed but superseded never reached the guest: unpaired, never graded (Astra P2)")
    # Astra merge review P2: every guest got strict's stand-in 1 while the computed counts (0) matched Xenia's 0 - the
    # old grader passed 20/20 on the counts the guest never read
    zeros = [0] * 25
    r = run([(7001, zeros, zeros, {"fallback_all": True})])
    check(r["verdict"] == "FAIL" and r["paired"] == 25 and r["ours_nonzero_xenia_zero"] == 25 and
          all(p.get("computed") == 0 for f in r["frames_detail"] for p in f["pairs"]),
          "the guest-visible answer is graded: fallback 1 everywhere vs Xenia 0 FAILS (the computed 0s kept as "
          "diagnostics) - %s" % r["why"])
    r = run([(7001, good, good, {"drop_report": 5})])
    check(r["verdict"] == "FAIL" and r["unpaired"] >= 1, "a report Xenia never retired: unpaired, FAIL")
    # Astra merge review P2: BEGIN reports must not stand in for END ones (swapping every report's record PASSED)
    r = run([(7001, good, good, {"swap_records": True})])
    check(r["verdict"] == "FAIL" and r["paired"] == 0 and r["frames_detail"][0]["address_mismatch"] == 50,
          "every report checked against its own event, BEGIN / END bit kept: swapped records FAIL (50 mismatches)")
    r = run([(7001, good, good, {"drain_left": 3})])
    check(r["verdict"] == "FAIL" and "drain left 3" in r["why"], "a dump that did not drain every report: FAIL")
    r = run([(7001, good, good, {"rc": 1})])
    check(r["verdict"] == "FAIL" and "rc 1" in r["why"], "a dump that exited 1: FAIL, its log not graded")
    r = run([(7001, good, good, {"extra_ours": 2})])
    check(r["verdict"] == "FAIL" and any("past the trace" in str(u["row"]) for u in r["unpaired_rows"]),
          "more traced queries of ours than the trace's ENDs: FAIL")
    r = run([(7001, [], [], {})])
    check(r["verdict"] == "NO DATA", "a traced frame without a query: NO DATA, never PASS")
    # the calibration (the dump draws Xenia's own golden frame as the golden PNG shows it) gates every verdict
    frames = [dict(swap=1, ends=25, ours=25, unpaired=[], dump_rc=0, drain_left=0,
                   pairs=[dict(k=i, ours=c, xenia=c, agree=True) for i, c in enumerate(good)])]
    r = zx.grade(frames, 20, 0.98, {"ok": False, "changed_pct": 39.28})
    check(r["verdict"] == "FAIL" and r["why"].startswith("uncalibrated") and r["calibration"]["changed_pct"] == 39.28,
          "an uncalibrated dump (WARP: 39 % off its golden frame) fails whatever the pairs say")
    r = zx.grade(frames, 20, 0.98, {"ok": True, "changed_pct": 0.0})
    check(r["verdict"] == "PASS" and r["calibration"]["ok"], "a calibrated dump with 25 agreeing pairs: PASS")
    r = zx.grade(frames, 20, 0.98, {"ok": None, "changed_pct": None})
    check(r["verdict"] == "FAIL", "a calibration that could not be read (no PIL, no render) is not a calibration")
    try:
        from PIL import Image
        have_pil = True
    except ImportError:
        have_pil = False
    if have_pil:
        with tempfile.TemporaryDirectory() as td:
            a, b, c = (os.path.join(td, n) for n in ("a.png", "b.png", "c.png"))
            Image.new("RGB", (10, 10), (100, 100, 100)).save(a)
            im = Image.new("RGB", (10, 10), (100, 100, 100))
            im.putpixel((0, 0), (200, 100, 100))
            im.save(b)
            Image.new("RGB", (5, 5)).save(c)
            check(zx.png_changed(a, a) == 0.0 and zx.png_changed(a, b) == 1.0 and zx.png_changed(a, c) == 100.0 and
                  zx.png_changed(a, os.path.join(td, "none.png")) is None,
                  "png_changed: 0 % identical, 1 pixel of 100 off = 1 %, another size 100 %, unreadable None")
    else:
        print("  --  png_changed not checked here (no PIL)")
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "res.json")
        roll = os.path.join(td, "roll")
        os.makedirs(os.path.join(roll, "drive", "attempt1"))
        open(zx.run_log_of(roll), "w").write(frame(td, 7001, good, good))
        os.replace(os.path.join(td, "out_dump"), os.path.join(td, "res_dump"))
        rc = zx.main(["--roll", roll, "--no-run", "--out", out])
        d = json.load(open(out, encoding="utf-8"))
        check(rc == 0 and d["verdict"] == "PASS" and d["schema"] == zx.SCHEMA,
              "the CLI grades stored logs (--no-run) into the result JSON (exit 0 on PASS)")
        # Astra merge review P2: a run whose every launch fails must not pass on those stored logs - its output
        # directory is fresh (the old one moved aside), so nothing it did not write is graded
        r = zx.evaluate(roll, roll + "_xtr", os.path.join(td, "res_dump"), dump=os.path.join(td, "no_such_dump.exe"),
                        run=True)
        check(r["verdict"] == "FAIL" and "FileNotFoundError" in r["why"] and
              os.path.isdir(os.path.join(td, "res_dump.prev1")) and
              not os.path.exists(os.path.join(td, "res_dump", "live_454107ED_7001.xenia.log")),
              "a run whose launch fails FAILS: the earlier logs moved aside (res_dump.prev1), never graded - %s"
              % r["why"][:80])
        r = zx.evaluate(roll, roll + "_xtr", os.path.join(td, "res_dump"), dump=os.path.join(td, "no_such_dump.exe"),
                        run=True, golden=(os.path.join(td, "g.xtr"), os.path.join(td, "g.png")))
        check(r["verdict"] == "FAIL" and not r["calibration"]["ok"] and r["calibration"]["changed_pct"] is None,
              "... and its calibration is not a calibration (no run of its own)")
    print("zpd xenia test: %d failed" % FAILS)
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
