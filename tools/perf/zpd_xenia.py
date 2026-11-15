#!/usr/bin/env python3
"""zpd_xenia.py - p2.zpd's Xenia oracle (docs/NATIVE_ROADMAP.md P2.12 item 14): our occlusion answers against Xenia's
on the same frames.

A live roll with SR_PM4_TRACE_HOTKEY=1 SR_PM4_TRACE_ZPD=<k> SR_NATIVE_ZPD=1 (native_proof plan zpd_xenia, step
xtrace) records k frames that hold HUD glow queries as Xenia .xtr traces (gfx/pm4_live_trace: exactly the command
stream and memory our renderer consumed) and logs every one of those frames' queries ([NATIVE_ZPD_T] swap= id= slot=
written guest= host=; gfx/native_zpd.cpp). Xenia's own trace-dump (tools/xenia-trace/build_zpd_trace_dump.ps1:
xenia-canary 0bd090dbe + the headless and zpd oracle patches) plays each trace with --occlusion_query=strict: real
host occlusion queries over Xenia's own EDRAM / render-target model, one SR_ZPD_REPORT line per EVENT_WRITE_ZPD it
retires, in stream order. Each END report's zpass is the samples its query's draws passed in Xenia - what the guest's
GetData would read there - and is paired, in order and by slot, with our answer to the same query of the same frame.

    python tools/perf/zpd_xenia.py --roll ROLL_DIR --dump XENIA_TRACE_DUMP.exe --out RESULT.json [--adapter -2]
    python tools/perf/zpd_xenia.py --roll ROLL_DIR --no-run --out RESULT.json        # re-grade the stored logs

The grade (pre-registered, zpd7): PASS when the dump is calibrated (its render of Xenia's own golden trace
454107ED_25023 is within CALIB_MAX_CHANGED % of the golden PNG: the device draws the scene the queries test), at least
--min-paired END reports pair with our answer (same frame, same order, same slot), none is left unpaired, and at least
--min-agree of the pairs agree on visible (> 0) / occluded (0) - the only distinction the HUD glow reads
(HudGlowOcclusionQuery_82304058: 0 -> alpha 127, else full). NO DATA when no traced frame held a query. Exact counts
are recorded beside the classes (Xenia normalizes per scale area, as we do). WARP is not a usable device: the worker's
WARP render of 454107ED_25023 lost Superman and the street (39 % of pixels off) - the dump runs on a GPU.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

SCHEMA = "zpd-xenia/1"
REG_SAMPLE_COUNT_ADDR = 0x2325
T_LINE = re.compile(r"\[NATIVE_ZPD_T\] swap=(\d+) id=(\d+) slot=([0-9A-F]{8}) (\w+) guest=(\d+) host=(\d+)")
X_LINE = re.compile(r"SR_ZPD_REPORT addr=([0-9A-Fa-f]{8}) zpass=(\d+) zfail=(\d+) spec=(\d)")
X_DRAIN = re.compile(r"SR_ZPD_DRAIN left=(\d+)")
XTR_NAME = re.compile(r"live_[0-9A-F]{8}_(\d+)\.xtr$")
COUNTED = ("written", "superseded", "stale")  # lines that carry our computed count (fallback / dead / lost do not)


def slot_of(addr):
    """The 64-byte slot (the END record) of an RB_SAMPLE_COUNT_ADDR value: physical, endian bits and record bit off."""
    return addr & 0x1FFFFFC0


def is_end(addr):
    """END record = the slot itself; BEGIN = slot + 0x20 (gfx/native_zpd.cpp onEvent, Xenia xenos_zpd_report.h)."""
    return (addr & 0x1FFFFFE0) == slot_of(addr)


def xtr_zpd_events(path):
    """[RB_SAMPLE_COUNT_ADDR] of every EVENT_WRITE_ZPD (PM4 type-3 0x5B) in a .xtr, in stream order (a zero address is
    skipped, as Xenia skips it). The register file comes from the trace's register records and type-0 / SET_CONSTANT
    writes, as Xenia's trace player applies them."""
    with open(path, "rb") as f:
        data = f.read()
    regs = {}
    out = []
    pos = 48
    n = len(data)
    while pos + 4 <= n:
        t = struct.unpack_from("<I", data, pos)[0]
        if t in (0, 2):
            pos += 12
        elif t in (1, 3, 5):
            pos += 4
        elif t == 4:
            _, _base, count = struct.unpack_from("<III", data, pos)
            pos += 12
            words = struct.unpack_from(">%dI" % count, data, pos)
            pos += 4 * count
            h = words[0]
            ptype = h >> 30
            if ptype == 0:
                idx, cnt, one = h & 0x7FFF, ((h >> 16) & 0x3FFF) + 1, (h >> 15) & 1
                for k in range(cnt):
                    if 1 + k < len(words):
                        regs[idx if one else idx + k] = words[1 + k]
            elif ptype == 3:
                op = (h >> 8) & 0x7F
                body = words[1:]
                if op == 0x2D and body:  # SET_CONSTANT: type 4 = registers at 0x2000 + index
                    ot = body[0]
                    if (ot >> 16) & 0xFF == 4:
                        for k, v in enumerate(body[1:]):
                            regs[0x2000 + (ot & 0x7FF) + k] = v
                elif op == 0x5B:
                    a = regs.get(REG_SAMPLE_COUNT_ADDR, 0)
                    if a:
                        out.append(a)
        elif t in (6, 7):
            _, _base, _enc, elen, _dlen = struct.unpack_from("<IIIII", data, pos)
            pos += 20 + elen
        elif t == 8:
            _, _enc, elen = struct.unpack_from("<III", data, pos)
            pos += 12 + elen
        elif t == 9:
            pos += 8
        elif t == 10:
            _, first, count, _cb, enc, elen = struct.unpack_from("<IIIIII", data, pos)
            pos += 24
            raw = data[pos:pos + elen]
            pos += elen
            if enc == 0:  # our live traces are uncompressed (gfx/pm4_live_trace.cpp)
                vals = struct.unpack_from("<%dI" % count, raw, 0)
                for k in range(count):
                    regs[first + k] = vals[k]
        elif t == 11:
            _, _comp, _enc, elen = struct.unpack_from("<IIII", data, pos)
            pos += 16 + elen
        else:
            raise ValueError("%s: unknown trace record %d at %d" % (path, t, pos))
    return out


def xenia_reports(text):
    """([(addr, zpass, zfail, speculative)] in retire order, drain's left count or None) from a dump's log."""
    reps = [(int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4) == "1") for m in X_LINE.finditer(text)]
    d = X_DRAIN.findall(text)
    return reps, (int(d[-1]) if d else None)


def our_answers(text, swap):
    """Our traced queries of one frame, by id: {id: {slot, count (computed, or None), delivered (what the guest read),
    how (the last outcome)}} in id order."""
    by = {}
    for m in T_LINE.finditer(text):
        if int(m.group(1)) != swap:
            continue
        qid, how, guest = int(m.group(2)), m.group(4), int(m.group(5))
        q = by.setdefault(qid, {"id": qid, "slot": int(m.group(3), 16), "count": None, "host": None,
                                "delivered": None, "how": how})
        q["how"] = how
        if how in COUNTED:
            q["count"], q["host"] = guest, int(m.group(6))
        if how in ("written", "fallback"):
            q["delivered"] = guest
    return [by[k] for k in sorted(by)]


def record_of(addr):
    """The record an RB_SAMPLE_COUNT_ADDR value names: physical, endian bits off, the BEGIN / END record bit kept."""
    return addr & 0x1FFFFFE0


def pair_frame(events, reports, ours):
    """One traced frame: the trace's EVENT_WRITE_ZPD addresses, Xenia's reports (one per event, stream order) and our
    queries -> a per-frame result with the pairs. Every report is checked against its OWN event, the BEGIN / END
    record bit included (Astra merge review: a BEGIN report must never stand in for an END); an END pairs only when its
    report matches. Our side is what the guest read (`delivered`: the written count or strict's stand-in); a count
    that never reached the guest (superseded / stale) is kept as `computed`, a diagnostic, never graded."""
    res = {"events": len(events), "ends": sum(1 for a in events if is_end(a)), "xenia_reports": len(reports),
           "ours": len(ours), "pairs": [], "unpaired": [], "address_mismatch": 0}
    if len(reports) != len(events):
        res["unpaired"].append("Xenia retired %d reports for %d EVENT_WRITE_ZPD" % (len(reports), len(events)))
    xen_ends = []
    for k, a in enumerate(events):
        rep = reports[k] if k < len(reports) else None
        if rep is not None and record_of(rep[0]) != record_of(a):
            res["address_mismatch"] += 1
            res["unpaired"].append({"k": k, "why": "Xenia's report %d is record %08X, the trace's event %08X" % (
                k, record_of(rep[0]), record_of(a))})
            rep = None
        if is_end(a):
            xen_ends.append((a, rep))
    for j, (a, rep) in enumerate(xen_ends):
        q = ours[j] if j < len(ours) else None
        row = {"k": j, "slot": "%08X" % slot_of(a)}
        if q is None or q["slot"] != slot_of(a):
            row["why"] = "no query of ours at this END" if q is None else "our query %d is slot %08X" % (q["id"], q["slot"])
        elif q["delivered"] is None:
            row["why"] = "the guest read no answer of our query %d (%s)" % (q["id"], q["how"])
        elif rep is None:
            row["why"] = "no Xenia report for this END"
        if "why" in row:
            res["unpaired"].append(row)
            continue
        row.update(id=q["id"], ours=q["delivered"], computed=q["count"], how=q["how"], ours_host=q["host"],
                   xenia=rep[1], xenia_zfail=rep[2], agree=(q["delivered"] > 0) == (rep[1] > 0))
        res["pairs"].append(row)
    if len(ours) > len(xen_ends):
        res["unpaired"].append("%d of our traced queries past the trace's %d ENDs" % (len(ours) - len(xen_ends),
                                                                                   len(xen_ends)))
    return res


def grade(frames, min_paired, min_agree, calibration=None):
    """calibration: calibrate()'s result, or None (the offline tests: the pairing alone is graded)."""
    pairs = [p for f in frames for p in f["pairs"]]
    unpaired = sum(len(f["unpaired"]) for f in frames)
    agree = sum(1 for p in pairs if p["agree"])
    r = {"schema": SCHEMA, "frames": len(frames), "paired": len(pairs), "agree": agree,
         "mismatch": len(pairs) - agree, "unpaired": unpaired,
         "agree_rate": round(agree / len(pairs), 4) if pairs else None,
         "both_zero": sum(1 for p in pairs if not p["ours"] and not p["xenia"]),
         "both_nonzero": sum(1 for p in pairs if p["ours"] and p["xenia"]),
         "ours_zero_xenia_nonzero": sum(1 for p in pairs if not p["ours"] and p["xenia"]),
         "ours_nonzero_xenia_zero": sum(1 for p in pairs if p["ours"] and not p["xenia"]),
         "exact": sum(1 for p in pairs if p["ours"] == p["xenia"]),
         "rule": "PASS: paired >= %d, unpaired 0, agree_rate >= %s on visible (> 0) / occluded (0); NO DATA: no traced "
                 "frame held a query" % (min_paired, min_agree)}
    if calibration is not None:
        r["calibration"] = calibration
    # every frame's dump ran in THIS run's fresh directory, exited 0 and drained every report (Astra merge review: a
    # failed launch must not pass on an earlier run's logs)
    unrun = ["%s (rc %s, drain left %s)" % (f.get("swap"), f.get("dump_rc"), f.get("drain_left")) for f in frames
             if f.get("dump_rc") != 0 or f.get("drain_left") != 0]
    r["unrun"] = unrun
    if calibration is not None and not calibration.get("ok"):
        r["verdict"] = "FAIL"
        r["why"] = "uncalibrated: Xenia's render of its golden trace is %s%% off (> %s%%, or unreadable; rc %s)" % (
            calibration.get("changed_pct"), CALIB_MAX_CHANGED, calibration.get("dump_rc"))
    elif unrun:
        r["verdict"], r["why"] = "FAIL", "the dump did not run to a drained end on frame(s) " + ", ".join(unrun)
    elif not any(f["ends"] for f in frames) or not any(f["ours"] for f in frames):
        r["verdict"], r["why"] = "NO DATA", "no traced frame held an occlusion query"
    elif len(pairs) < min_paired or unpaired or (r["agree_rate"] or 0) < min_agree:
        r["verdict"] = "FAIL"
        r["why"] = "; ".join(w for w in (
            "paired %d < %d" % (len(pairs), min_paired) if len(pairs) < min_paired else "",
            "unpaired %d" % unpaired if unpaired else "",
            "agree %s < %s" % (r["agree_rate"], min_agree) if (r["agree_rate"] or 0) < min_agree else "") if w)
    else:
        r["verdict"], r["why"] = "PASS", "%d of %d pairs agree" % (agree, len(pairs))
    return r


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def dump_args(dump, xtr, outdir, adapter):
    log = os.path.join(outdir, os.path.splitext(os.path.basename(xtr))[0] + ".xenia.log")
    # Only flags the trace-dump defines: an unknown one (--mute, --apu, --hid: xenia_main's) is an error message box
    # and an immediate exit. The first-run disclaimer box blocks forever in a session without a desktop unless
    # HKCU\SOFTWARE\Xenia XEFLAGS = 1 (tools/xenia-trace/build_zpd_trace_dump.ps1 sets it on the worker).
    cmd = [dump, "--async_shader_compilation=false", "--occlusion_query=strict",
           "--trace_dump_path=" + outdir, "--log_file=" + log]
    if adapter is not None:
        cmd.append("--d3d12_adapter=%d" % adapter)
    return cmd + [xtr], log


def run_log_of(roll):
    return os.path.join(roll, "drive", "attempt1", "run.log")


CALIB_MAX_CHANGED = 1.0  # % of pixels more than 16 levels off Xenia's golden render of its own trace


def png_changed(a_png, b_png, level=16):
    """% of pixels whose largest channel difference exceeds `level` (PIL), or None when either image is unreadable."""
    try:
        from PIL import Image, ImageChops
        a = Image.open(a_png).convert("RGB")
        b = Image.open(b_png).convert("RGB")
    except Exception:  # noqa: BLE001 - no PIL / no image: uncalibrated, never calibrated
        return None
    if a.size != b.size:
        return 100.0
    h = ImageChops.difference(a, b).convert("L").histogram()
    return round(100.0 * sum(h[level + 1:]) / max(1, sum(h)), 3)


EXEC = "exec.json"  # the dump runs of THIS output directory: {"frames": {xtr name: rc}, "calibration": rc}


def fresh_dir(path):
    """A fresh output directory for one run: an existing one (an earlier run's logs and PNGs) is moved aside as
    <path>.prev<k>, never read again (Astra merge review: a failed launch passed on old artifacts)."""
    if os.path.exists(path):
        k = 1
        while os.path.exists("%s.prev%d" % (path, k)):
            k += 1
        os.rename(path, "%s.prev%d" % (path, k))
    os.makedirs(path)


def run_dump(dump, xtr, outdir, adapter, timeout):
    """One dump run -> its exit code, or the launch error / timeout as a string (never 0)."""
    cmd, _log = dump_args(dump, xtr, outdir, adapter)
    try:
        return subprocess.run(cmd, cwd=os.path.dirname(dump) or None, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=timeout).returncode
    except (OSError, ValueError, subprocess.TimeoutExpired) as x:
        return "%s: %s" % (type(x).__name__, x)


def read_exec(outdir):
    try:
        with open(os.path.join(outdir, EXEC), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def calibrate(dump, golden_xtr, golden_png, outdir, adapter, run=True, timeout=600, rc=None):
    """The oracle's own check (zpd7): this dump, on this device, renders Xenia's golden gameplay trace as the golden
    PNG shows it (the oracle build's frame of 454107ED_25023, Metropolis with the HUD) - at most CALIB_MAX_CHANGED % of
    pixels more than 16 levels off. Xenia on WARP drew that frame without Superman or the street (39 % off, the worker
    2026-10-15): a device that cannot draw the scene cannot answer its occlusion queries. The render must come from
    this run's own dump (rc 0, in the fresh directory): rc = the stored exit code when grading again (run=False)."""
    cdir = os.path.join(outdir, "calibration")
    os.makedirs(cdir, exist_ok=True)
    c = {"golden_xtr": golden_xtr, "golden_png": golden_png, "max_changed_pct": CALIB_MAX_CHANGED}
    c["dump_rc"] = run_dump(dump, golden_xtr, cdir, adapter, timeout) if run else rc
    render = os.path.join(cdir, os.path.splitext(os.path.basename(golden_xtr))[0] + ".png")
    c["render"] = render
    c["changed_pct"] = png_changed(render, golden_png) if os.path.exists(render) and c["dump_rc"] == 0 else None
    c["ok"] = c["dump_rc"] == 0 and c["changed_pct"] is not None and c["changed_pct"] <= CALIB_MAX_CHANGED
    return c


def evaluate(roll, xtr_dir, outdir, dump=None, adapter=None, run=True, min_paired=20, min_agree=0.98, timeout=600,
             golden=None):
    """The whole oracle for one roll -> the result dict (also the per-frame detail). golden = (xtr, png): the
    calibration (calibrate) - required for a PASS whenever given. run=True: a FRESH outdir (fresh_dir), every dump run
    recorded in its exec.json; run=False grades that directory again from its own exec.json - a frame (or the
    calibration) without a recorded exit 0 there is not graded as run."""
    log = run_log_of(roll)
    text = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
    traces = sorted((int(XTR_NAME.search(n).group(1)), os.path.join(xtr_dir, n))
                    for n in (os.listdir(xtr_dir) if os.path.isdir(xtr_dir) else []) if XTR_NAME.search(n))
    if run:
        fresh_dir(outdir)
        rec = {"frames": {}, "calibration": None}
    else:
        os.makedirs(outdir, exist_ok=True)
        rec = read_exec(outdir)
    frames = []
    for swap, xtr in traces:
        _cmd, xlog = dump_args(dump or "xenia-gpu-d3d12-trace-dump.exe", xtr, outdir, adapter)
        name = os.path.basename(xtr)
        f = {"swap": swap, "xtr": xtr, "xenia_log": xlog,
             "xenia_png": os.path.join(outdir, os.path.splitext(name)[0] + ".png")}
        if run:
            rec["frames"][name] = run_dump(dump or "xenia-gpu-d3d12-trace-dump.exe", xtr, outdir, adapter, timeout)
        f["dump_rc"] = (rec.get("frames") or {}).get(name)
        xtext = open(xlog, "rb").read().decode("utf-8", "replace") if os.path.exists(xlog) and f["dump_rc"] == 0 else ""
        reports, left = xenia_reports(xtext)
        f["drain_left"] = left
        f.update(pair_frame(xtr_zpd_events(xtr), reports, our_answers(text, swap)))
        frames.append(f)
    calib = None
    if golden:
        calib = calibrate(dump, golden[0], golden[1], outdir, adapter, run, timeout, rc=rec.get("calibration"))
        rec["calibration"] = calib["dump_rc"]
    if run:
        with open(os.path.join(outdir, EXEC), "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1)
    r = grade(frames, min_paired, min_agree, calib)
    if dump and os.path.exists(dump):
        r["dump"], r["dump_sha256"] = dump, sha256(dump)
    r["roll"], r["xtr_dir"] = roll, xtr_dir
    r["frame_summary"] = [{k: f.get(k) for k in ("swap", "events", "ends", "xenia_reports", "ours", "drain_left",
                                                   "dump_rc", "address_mismatch")} | {
        "pairs": len(f["pairs"]), "agree": sum(1 for p in f["pairs"] if p["agree"]), "unpaired": len(f["unpaired"])}
        for f in frames]
    r["mismatches"] = [dict(p, swap=f["swap"]) for f in frames for p in f["pairs"] if not p["agree"]][:200]
    r["unpaired_rows"] = [{"swap": f["swap"], "row": u} for f in frames for u in f["unpaired"]][:200]
    r["frames_detail"] = frames
    return r


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--roll", required=True, help="the xtrace roll directory (drive/attempt1/run.log)")
    ap.add_argument("--xtr-dir", help="the roll's traces (default <roll>_xtr)")
    ap.add_argument("--dump", help="xenia-gpu-d3d12-trace-dump.exe (tools/xenia-trace/build_zpd_trace_dump.ps1)")
    ap.add_argument("--adapter", type=int, help="Xenia --d3d12_adapter (-2 = WARP, for a machine without a GPU)")
    ap.add_argument("--out", required=True, help="the result JSON (the Xenia logs and PNGs go beside it)")
    ap.add_argument("--no-run", action="store_true", help="grade the stored Xenia logs again, run nothing")
    ap.add_argument("--min-paired", type=int, default=20)
    ap.add_argument("--min-agree", type=float, default=0.98)
    ap.add_argument("--golden-xtr", help="the calibration: Xenia's golden trace (454107ED_25023.xtr)")
    ap.add_argument("--golden-png", help="... and its golden render")
    a = ap.parse_args(argv)
    outdir = os.path.splitext(a.out)[0] + "_dump"
    r = evaluate(a.roll, a.xtr_dir or a.roll.rstrip("\\/") + "_xtr", outdir, a.dump, a.adapter, not a.no_run,
                 a.min_paired, a.min_agree, golden=(a.golden_xtr, a.golden_png) if a.golden_xtr else None)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(r, f, indent=1)
    print("%s: %s (%s) - %d frames, %d pairs, %d agree, %d unpaired; ours 0 / Xenia > 0: %d, ours > 0 / Xenia 0: %d" % (
        a.out, r["verdict"], r["why"], r["frames"], r["paired"], r["agree"], r["unpaired"],
        r["ours_zero_xenia_nonzero"], r["ours_nonzero_xenia_zero"]))
    return 0 if r["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
