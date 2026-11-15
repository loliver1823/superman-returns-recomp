#!/usr/bin/env python3
"""test_native_meter.py - the native meter's parsers know every column the kernel prints (iter-467 native-a).

    python tools/perf/test_native_meter.py

Synthetic [NATIVE_METER] lines in the kernel's format (kernel/native_meter.cpp) through tools/perf/native_meter.py
and tools/perf/meter_delta.py: the display-register column `dispreg` (RELEASE_PLAN C1's fourth class of emulated
hardware work) is parsed, summed per frame, carried into the verdict, and read by meter_delta's window parser (the
one tools/native_ledger.py uses); an unwired column prints "-" and is never read as 0; and the Python column list
equals the kernel's kName list, read from the source, so a column added on one side cannot silently vanish from
the verdict on the other. Exit code = failed checks.
"""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import native_meter as nm  # noqa: E402
import meter_delta as md  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


def win(w, secs, frames, avgmax, totals):
    """One [NATIVE_METER] window line as native_meter.cpp prints it."""
    return ("[2026-10-10 01:00:%02d.000] [INFO ] [BOOT] [NATIVE_METER] w=%d %.2fs frames=%d | per_frame avg/max%s | window%s"
            % (w, w, secs, frames, avgmax, totals))


def main():
    # 1. The kernel's column list (kName in native_meter.cpp) is what the tools parse.
    src = open(os.path.join(REPO, "kernel", "native_meter.cpp"), encoding="utf-8").read()
    m = re.search(r"kName\[kCols\] = \{([^}]*)\}", src)
    kernel_cols = re.findall(r'"(\w+)"', m.group(1)) if m else []
    check(kernel_cols == nm.COLS, "native_meter.py COLS == kernel kName %s" % kernel_cols)
    check("dispreg" in nm.COLS and nm.COLS.index("dispreg") == nm.COLS.index("nrec") + 1,
          "dispreg sits after nrec (before shm) in the parsed columns")
    # mem.resources step 1 (release/mem-meter): shm = the shared-memory page model's page validations.
    check("shm" in nm.COLS and nm.COLS.index("shm") == nm.COLS.index("dispreg") + 1 and nm.COLS[-1] == "xma",
          "shm sits after dispreg, before the pulled xma, in the parsed columns")
    check(set(md.SHOW) >= {"pm4", "dispreg", "shm"}, "meter_delta prints dispreg and shm")
    hdr = open(os.path.join(REPO, "kernel", "native_meter.hpp"), encoding="utf-8").read()
    check("kDisplayReg" in hdr and re.search(r"enum Id[^}]*kNativeRec, kDisplayReg, kShm, kIds", hdr) is not None,
          "kDisplayReg then kShm are the Ids after kNativeRec (the column order the static_asserts pin)")

    # 2. Parse + summarise: dispreg and shm counted per frame, into the verdict; nrec never.
    lines = [
        "[2026-10-10 01:00:00.000] [INFO ] [BOOT] [GPU_CP] command processor thread up",
        win(1, 2.0, 100, " pm4=10.0/20 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=0.5/2 shm=20.0/40 xma=0.0/0",
            " pm4=1000 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=50 shm=2000 xma=0"),
        win(2, 2.0, 100, " pm4=0.0/0 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=1.5/2 shm=0.0/0 xma=0.0/0",
            " pm4=0 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=150 shm=0 xma=0"),
        "[2026-10-10 01:00:04.000] [INFO ] [BOOT] [NATIVE_METER_RUN] periodic 4.0s frames=200 | pm4=1000 edram_res=0 "
        "edram_xfer=0 edram_load=0 nrec=10000 dispreg=200 shm=2000 xma=0 | per_frame pm4=5.0 edram_res=0.0 edram_xfer=0.0 "
        "edram_load=0.0 nrec=50.0 dispreg=1.0 shm=10.0 xma=0.0",
    ]
    with tempfile.TemporaryDirectory() as td:
        log = os.path.join(td, "run.log")
        open(log, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        rows, run, krows, krun = nm.parse_log(log)
        check(len(rows) == 2, "two window lines parsed")
        s = nm.summarize(rows, None, krows)
        check(s["frames"] == 200, "frames summed over the windows (200)")
        check(s["per"]["dispreg"] == (1.0, 2), "dispreg = 200 answers / 200 frames = 1.0 per frame, max 2")
        check(s["per"]["shm"] == (10.0, 40), "shm = 2000 page validations / 200 frames = 10.0 per frame, max 40")
        check(s["per"]["pm4"] == (5.0, 20), "pm4 5.0 per frame, max 20")
        check(s["nonzero"] == ["pm4", "dispreg", "shm"], "the verdict names pm4, dispreg and shm, never nrec: %s" % s["nonzero"])
        check(s["verdict"].startswith("NOT NATIVE: pm4, dispreg, shm"), "verdict: " + s["verdict"])
        out = "\n".join(nm.report_lines(log))
        check("dispreg=1.0/frame (max 2)" in out and "shm=10.0/frame (max 40)" in out and "run: [NATIVE_METER_RUN]" in out,
              "report prints the columns and the run line")

        # 3. All zeros but nrec -> NATIVE; an unwired dispreg ('-') is reported unwired, not 0.
        zero = [win(1, 2.0, 100, " pm4=0.0/0 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=0.0/0 shm=0.0/0 xma=0.0/0",
                    " pm4=0 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=0 shm=0 xma=0")]
        open(log, "w", encoding="utf-8").write("\n".join(zero) + "\n")
        s = nm.summarize(nm.parse_log(log)[0])
        check(s["verdict"] == "NATIVE (all wired counters 0)", "all wired counters 0 -> NATIVE: " + s["verdict"])
        unw = [win(1, 2.0, 100, " pm4=0.0/0 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=- shm=0.0/0 xma=0.0/0",
                   " pm4=0 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=- shm=0 xma=0")]
        open(log, "w", encoding="utf-8").write("\n".join(unw) + "\n")
        s = nm.summarize(nm.parse_log(log)[0])
        check(s["per"]["dispreg"] is None and "dispreg" in s["unwired"] and "dispreg" not in s["nonzero"],
              "an unwired dispreg ('-') is listed unwired and never read as 0")
        check("unwired: dispreg" in s["verdict"], "verdict names the unwired column: " + s["verdict"])

        # 4. meter_delta (the ledger's reader): a roll dir without an ingame marker uses the whole run.
        roll = os.path.join(td, "roll")
        os.makedirs(os.path.join(roll, "drive", "attempt1"))
        os.makedirs(os.path.join(roll, "perf"))
        open(os.path.join(roll, "drive", "attempt1", "run.log"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
        start, mrows = md.windows(roll)
        check(start is None and len(mrows) == 2, "meter_delta reads both windows (no ingame marker -> whole run)")
        frames, per = md.summarize(mrows)
        check(frames == 200 and abs(per.get("dispreg", -1) - 1.0) < 1e-9, "meter_delta per-frame dispreg = 1.0")
        check(abs(per.get("shm", -1) - 10.0) < 1e-9, "meter_delta per-frame shm = 10.0 (the ledger's shm meter item reads this)")

        # 5. meter_delta (Astra round 1 on release/mem-meter): a column missing from one selected window is not a
        # measurement - never 0 over the whole run. Window 1 carries shm=0, window 2 (an exe before the column, or an
        # unwired '-') does not: shm must be ABSENT from the summary (the ledger then says 'meter field shm absent'),
        # while the columns every window carries are still summed; the kind rows do not count as meter windows.
        partial = [
            win(1, 2.0, 100, " pm4=10.0/20 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=0.5/2 shm=0.0/0 xma=0.0/0",
                " pm4=1000 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=50 shm=0 xma=0"),
            "[2026-10-10 01:00:02.000] [INFO ] [BOOT] [NATIVE_METER_KINDS] w=1 frames=100 | pm4 per_frame by kind: t0=1.0 t1=0.0",
            win(2, 2.0, 100, " pm4=0.0/0 edram_res=0.0/0 edram_xfer=0.0/0 edram_load=0.0/0 nrec=50.0/60 dispreg=1.5/2 xma=0.0/0",
                " pm4=0 edram_res=0 edram_xfer=0 edram_load=0 nrec=5000 dispreg=150 xma=0"),
        ]
        open(os.path.join(roll, "drive", "attempt1", "run.log"), "w", encoding="utf-8").write("\n".join(partial) + "\n")
        start, prows = md.windows(roll)
        pframes, pper = md.summarize(prows)
        check(pframes == 200 and "shm" not in pper, "a column missing from one window is absent from the summary, not 0: %s" % sorted(pper))
        check(abs(pper.get("pm4", -1) - 5.0) < 1e-9 and abs(pper.get("dispreg", -1) - 1.0) < 1e-9,
              "the columns every window carries are still summed (pm4 5.0, dispreg 1.0)")
        check(abs(pper.get("kind_t0", -1) - 0.5) < 1e-9, "the kind rows are summed on their own (t0 100 over 200 frames = 0.5)")
        check(abs(per.get("pm4", -1) - 5.0) < 1e-9 and abs(per.get("nrec", -1) - 50.0) < 1e-9, "meter_delta pm4 5.0, nrec 50.0")

    print("native meter test: %d failed" % FAILS)
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
