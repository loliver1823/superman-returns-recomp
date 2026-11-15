#!/usr/bin/env python3
"""Flicker metric over an every-present dump window (SR_PRESENT_DUMP_EVERY=1).

Contact sheets sample one present in hundreds and cannot show temporal artifacts. This scores
CONSECUTIVE presents: for every triple (a, b, c) and every grid block, b is an "alternation outlier"
when it differs from BOTH neighbours while the neighbours agree with each other (A-B-A: an object or a
texture that is wrong for one present and right again in the next). Smooth camera/object motion gives
d(a,c) ~ d(a,b) + d(b,c) and is not counted.

THE MOTION CHECK (step 6, Fable ruling 26(3); --no-motion-check restores the rule above alone): an edge that the
scene's motion carries THROUGH a block for exactly one present (the PC route's last crosswalk stripe leaving the frame
through block row 8 col 3: road in A, the stripe's corner in B, the stripe gone from the frame in C) also gives
A ~ C != B. Every block the A-B-A rule flags is re-checked against the motion of its neighbourhood, estimated from A
and C ONLY (never from B, so a defect cannot explain itself):
  1. v = the integer displacement (+-MOTION_SEARCH px at 320x180, steps of 2, ties to the smallest |v|) that best
     maps A onto C over the block's 3x3-block neighbourhood (clipped at the frame edge; in-frame pixels only);
  2. the motion is ACCEPTED only if it explains and is evident: MAD_nb(A moved by v, C) < thr AND
     <= MOTION_EVIDENCE x MAD_nb(A, C) AND v != 0 - flat ground carries no evidence, so a defect there keeps its count;
  3. A moved by ceil(v/2) and C by -floor(v/2) predict B; a side whose shifted prediction of the block is not COMPLETELY
     in frame (PREDICT_VALID = 1.0) is undetermined and cannot dismiss (the neighbourhood's estimate in 1. needs only
     MOTION_VALID of its pixels in frame);
  4. the outlier is DISMISSED (listed, not counted) only if a determinable side's prediction matches B AND the motion
     explains most of that side's difference: its predicted block MAD <= thr AND <= MOTION_EVIDENCE x the side's
     static MAD (dab for A, dbc for C) - a weak outlier at a real defect's edge, which a small camera drift shaves from
     just over thr to just under, is not "explained" (V4: the S1 regression's g2 p20107 r7c11, ab2 p18043 r6c5).
     Only flagged blocks are examined, so the check can remove a detection, never add one; thr (6) and the ratio
     (0.35) are unchanged.

Also reports identical consecutive presents (high-fps repeats), the [HIGHFPS_PACE] back= counts
(presents that stepped backwards in sim time = present order), and, when the roll toggled an in-roll
A/B, the outliers split by the A/B state of the pace window each present fell in (file mtime).

usage: flicker_diff.py <roll dir> [--block 20] [--thr 6.0] [--worst out.jpg] [--fail-presents 2] [--no-motion-check]

exit 0 = PASS, 1 = FAIL (or fewer than 3 frames, or an UNREADABLE frame: a truncated / undecodable dump is reported
as "FLICKER UNREADABLE" - no verdict, so a damaged frame can never silently drop or pass a roll; Fable ruling 28 (1)).
An INCOMPLETE inventory is UNREADABLE too (lane C r8 / r9, Astra e7b0cca r7 P1-3, a350efe r8 P1): a gap in the present
numbering, the roll's run.log dump records and the files on disk naming different frames (either way), a dump named
twice, a [PRESENT_DUMP] line that is neither a whole dump record nor the window's mark note, a marked window that is not
exactly its cap presents from the mark, or two files claiming one present that differ - the triples are scored over the
complete, one-to-one inventory or not at all.
"""
import argparse
import json
import datetime as dt
import os
import re
import sys
import traceback

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flicker_rule import CANONICAL, params_record  # noqa: E402  (the rule's one parameter table)
import log_records  # noqa: E402  (the single-record rule the standalone detectors share, lane C r17)

# Fable 26(3), frozen in the pre-registration (lane C NOTES 2026-10-22): not tuned after any re-validation result
MOTION_SEARCH = 32      # px at 320x180, A -> C (two presents); steps of MOTION_STEP
MOTION_STEP = 2
MOTION_EVIDENCE = 0.5   # the accepted motion at least halves the neighbourhood's A-vs-C difference
MOTION_VALID = 0.5      # the neighbourhood's motion estimate needs this fraction of in-frame pixels
# A side's PREDICTION of the flagged block needs COMPLETE support (every block pixel's shifted source in the frame): a
# partly clipped prediction would be scored on the pixels it covers only, and a defect in the uncovered ones (at the
# frame edge) could be dismissed (Astra, f5e06465 round 1, P1: an edge defect under a 4 px / present pan, bad 1 -> 0).
PREDICT_VALID = 1.0
# Fable 31 (2)(i)-(ii): the READONLY admission diagnostic of a flagged block the check does not dismiss (diag31 lines;
# flicker_gate stores them under span["diag31"], which no pass / fail code reads). SR_FLICKER_DIAG=0 turns it off (the
# on / off identity test).
DIAG31 = os.environ.get("SR_FLICKER_DIAG", "1") != "0"
DIAG_FAULT = os.environ.get("SR_FLICKER_DIAG_FAULT", "0") == "1"   # test seam: the diagnostic's emission fails
# The detector's identity, the first line of every output: flicker_gate grades a stored <d>_flicker.txt as this
# detector's only when it carries this id (a stored output of another detector is re-run, or named when read-only).
# A change to the rule or its constants changes this id (and flicker_gate.DETECTOR with it - test_flicker_diff checks).
DETECTOR = "motion-check-2 (Fable 26(3) + 28; complete-support predictions)"
DETECTOR_STATIC = "static (--no-motion-check: the A-B-A rule alone)"


def one_line(s, n=160):
    """An error text as ONE bounded line (lane C r8, convergence P3: a message with a newline split a record)."""
    return " ".join(str(s).split())[:n]


def frames_of(roll):
    d = os.path.join(roll, "drive", "attempt1", "frames")
    if not os.path.isdir(d):
        d = roll
    out = []
    for f in os.listdir(d):
        m = re.search(r"_p(\d+)_", f)
        if m and f.lower().endswith((".bmp", ".png")):
            out.append((int(m.group(1)), os.path.join(d, f)))
    out.sort()
    return out


# A present dump the run log names (world_floor.DUMP_NAME: the reader's [PRESENT_DUMP] record) and its present number
DUMP_NAME = re.compile(r"\[PRESENT_DUMP\] (.+?_p(\d+)_\d+x\d+\.bmp)(?=\s|$)")   # r16: `.bmp` ends the name


# ... and the one [PRESENT_DUMP] note a span's log carries: the harness mark that opened the window (its first present, cap)
MARK_NOTE = re.compile(r"\[PRESENT_DUMP\] window opened by the harness dump mark at present (\d+) \(cap (\d+)\)\s*$")


def logged_dumps(roll):
    """({file name: [present, number of [PRESENT_DUMP] records naming it]}, [(mark present, cap), ...]) of the roll's
    run.log (a roll laid out as <roll>/drive/attempt1/), or None when the roll has no run log (a bare folder of frames).
    Lane C r9 (Astra a350efe r8 / conv8 P1: a record cut before `.bmp` was read as a note and its present left the
    inventory): EVERY line carrying [PRESENT_DUMP] is a whole dump record (DUMP_NAME) or the window's mark note
    (MARK_NOTE), else it raises. Archive survey 2026-10-23 (19 real span rolls): no other [PRESENT_DUMP] line form."""
    log = os.path.join(roll, "drive", "attempt1", "run.log")
    if not os.path.isfile(log):
        return None
    with open(log, "rb") as fh:
        data = fh.read()
    # r16 (Astra ed02b5a r15 / conv15 P2): the log the inventory comes from is whole - its last line not cut - and strict
    # UTF-8 (an undecodable record is no record)
    if data and not data.endswith(b"\n"):
        raise ValueError("the run log's last line is cut (no final newline)")
    # r24 (Astra dac76f1 r23 P1): the whole log meets the strict gate's STRICT LOG INTEGRITY first - the same rule, one
    # definition (log_records.log_integrity) - so no splice the gate refuses reaches this reader either
    log_records.log_integrity(log, data)
    return dump_records(data.decode("utf-8"))


def dump_records(text):
    """logged_dumps' reading of a run log's text (the mutation sweep calls it line by line)."""
    out, marks = {}, []
    for line in text.split("\n"):
        # r17 (Astra 5fb5a2c r16 / conv16 P2: an interleaved / embedded record after a dump name was ignored): a dump
        # record is ONE whole record, and a dump record carries no mark note. r24 (Astra dac76f1 r23 P1: the literal
        # `[PRESENT_DUMP]` filter ran first, so a mark whose opening `[` a splice took - `... PRESENT_DUMP] window
        # opened ...` - was skipped and the window went unchecked): EVERY line meets the shared rule before any filter
        why = log_records.record_problem(line.rstrip("\r"), ("PRESENT_DUMP",))
        if why:
            raise ValueError("%s: %r" % (why, line.strip()[:160]))
        if "[PRESENT_DUMP]" not in line:
            continue
        if DUMP_NAME.search(line) and "window opened by the harness dump mark" in line:
            raise ValueError("a dump record carrying a mark note: %r" % line.strip()[:160])
        m = DUMP_NAME.search(line.rstrip("\r"))
        if m:
            name = re.split(r"[\\/]", m.group(1))[-1]
            out.setdefault(name, [int(m.group(2)), 0])[1] += 1
            continue
        mk = MARK_NOTE.search(line)
        if mk:
            marks.append((int(mk.group(1)), int(mk.group(2))))
            continue
        raise ValueError("a [PRESENT_DUMP] record that cannot be read: %r" % line.strip()[:160])
    return out, marks


def inventory_problem(roll, fr):
    """Lane C r8 (Astra e7b0cca r7 P1-3: a present missing from disk dropped out of a fresh strict verdict - its
    triples were never scored): the scored frames must be the span's COMPLETE, uniquely identified inventory. -> (frames,
    None) with one frame per present, or (None, why):
      - two or more files claiming one present must decode to the same picture (else which was shown is unknown);
      - the presents are consecutive (a gap is a present the triples would skip);
      - the run log's dump records and the files on disk name the same frames, both directions (r9; a frame deleted at
        an end leaves no gap), none named twice (an ambiguous write), and every [PRESENT_DUMP] line is a whole record;
      - a window the harness mark opened (the log's one mark note) is exactly its cap presents from the mark (r9)."""
    by = {}
    for p, f in fr:
        by.setdefault(p, []).append(f)
    for p in sorted(by):
        fs = by[p]
        if len(fs) > 1:
            first = np.asarray(Image.open(fs[0]).convert("RGB"))
            for f in fs[1:]:
                other = np.asarray(Image.open(f).convert("RGB"))
                if other.shape != first.shape or bool((other != first).any()):
                    return None, "present p%06d is ambiguous: %d files claim it and differ (%s)" % (
                        p, len(fs), ", ".join(os.path.basename(x) for x in fs)[:200])
    uniq = [(p, by[p][0]) for p in sorted(by)]
    pn = [p for p, _ in uniq]
    missing = [pn[i - 1] + 1 for i in range(1, len(pn)) if pn[i] != pn[i - 1] + 1]
    if missing:
        return None, "%d gap(s) in the present numbering (p%06d missing first): the inventory is not complete" % (
            len(missing), missing[0])
    rec = logged_dumps(roll)
    if rec is not None:
        logged, marks = rec
        on_disk = {os.path.basename(f) for _, f in fr}
        # r9: the log's dump records and the files on disk name the SAME frames (both directions); r10 (Astra 2453fdf r9
        # P2): a roll WITH a run log is reconciled even when the log names no dump at all (a bare folder of frames - no
        # run log - is the detector's standalone use and has no records to reconcile)
        twice = sorted(n for n, (_p, k) in logged.items() if k > 1)
        if twice:
            return None, "run.log names %s %d times: an ambiguous write" % (twice[0], logged[twice[0]][1])
        gone = sorted(set(logged) - on_disk)
        if gone:
            return None, "%d dump(s) run.log names are not on disk (%s first)" % (len(gone), gone[0])
        unnamed = sorted(on_disk - set(logged))
        if unnamed:
            return None, "%d frame(s) on disk that run.log does not name (%s first)" % (len(unnamed), unnamed[0])
        # r9: a marked window is exactly the cap presents from the mark (archive survey: every marked real span is)
        if len(marks) > 1:
            return None, "run.log carries %d dump-window marks: the window is ambiguous" % len(marks)
        if marks and (pn[0] != marks[0][0] or len(pn) != marks[0][1]):
            return None, "the window p%06d..p%06d (%d presents) is not the marked one (from p%06d, cap %d)" % (
                pn[0], pn[-1], len(pn), marks[0][0], marks[0][1])
    return uniq, None


def load(path, w=320, h=180):
    im = Image.open(path).convert("RGB").resize((w, h), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def block_mad(x, y, bs):
    d = np.abs(x - y).mean(axis=2)
    h, w = d.shape
    d = d[: h - h % bs, : w - w % bs]
    return d.reshape(h // bs, bs, w // bs, bs).mean(axis=(1, 3))


def _shifts():
    """The search displacements (dy, dx), smallest |v| first (so the first minimum found is the smallest |v|)."""
    r = range(-MOTION_SEARCH, MOTION_SEARCH + 1, MOTION_STEP)
    return sorted(((dy, dx) for dy in r for dx in r), key=lambda v: (v[0] * v[0] + v[1] * v[1], v))


SHIFTS = _shifts()


def moved_mad(x, y, dy, dx, ys, xs, valid=MOTION_VALID):
    """MAD over the region (ys, xs) of y between x MOVED by (dy, dx) - out[p] = x[p - (dy, dx)] - and y, over the
    pixels whose source lies in the frame; None when fewer than `valid` of the region's pixels do."""
    h, w = x.shape[:2]
    y0, y1 = max(ys.start, dy), min(ys.stop, h + dy)
    x0, x1 = max(xs.start, dx), min(xs.stop, w + dx)
    if y1 <= y0 or x1 <= x0:
        return None
    n = (y1 - y0) * (x1 - x0)
    if n < valid * (ys.stop - ys.start) * (xs.stop - xs.start):
        return None
    return float(np.abs(x[y0 - dy:y1 - dy, x0 - dx:x1 - dx] - y[y0:y1, x0:x1]).mean())


def side_detail(x, b, dy, dx, ys, xs):
    """Fable 31 (2)(i)-(ii)'s READONLY diagnostic of one side's prediction of the block (ys, xs): its support (the
    fraction of block pixels whose shifted source is in the frame), the compensated and the STATIC MAD over the supported
    pixels, the supported rectangle (block-relative rows / cols) and the per-pixel compensated error (NaN = unsupported)."""
    h, w = x.shape[:2]
    bh, bw = ys.stop - ys.start, xs.stop - xs.start
    y0, y1 = max(ys.start, dy), min(ys.stop, h + dy)
    x0, x1 = max(xs.start, dx), min(xs.stop, w + dx)
    err = np.full((bh, bw), np.nan, np.float32)
    if y1 <= y0 or x1 <= x0:
        return {"support": 0.0, "err": err}
    e = np.abs(x[y0 - dy:y1 - dy, x0 - dx:x1 - dx] - b[y0:y1, x0:x1]).mean(axis=2)
    err[y0 - ys.start:y1 - ys.start, x0 - xs.start:x1 - xs.start] = e
    return {"support": (y1 - y0) * (x1 - x0) / float(bh * bw), "comp": float(e.mean()),
            "static": float(np.abs(x[y0:y1, x0:x1] - b[y0:y1, x0:x1]).mean()),
            "rows": (y0 - ys.start, y1 - ys.start), "cols": (x0 - xs.start, x1 - xs.start), "err": err}


def diag31(a, b, c, blk, ash, csh, thr, bs):
    """Fable 31 (2)(i)-(ii) - READONLY, never pass / fail: for a flagged block the motion check did NOT dismiss, (i) each
    side with support in [0.5, 1) and whether it meets the 28 rule on its supported pixels (compensated <= thr AND <= 0.5
    x its static MAD there), with the unsupported region; (ii) the fully supported side's compensated error split into
    that unsupported (frame-edge) region and the interior. None when neither side is partly supported."""
    sides = {"A": side_detail(a, b, ash[0], ash[1], *blk), "C": side_detail(c, b, csh[0], csh[1], *blk)}
    out = []
    for name, sd in sides.items():
        if not (0.5 <= sd["support"] < 1.0):
            continue
        other = sides["C" if name == "A" else "A"]
        r0, r1 = sd["rows"]
        c0, c1 = sd["cols"]
        unsup = np.isnan(sd["err"])
        d = {"side": name, "support": round(sd["support"], 3), "comp": round(sd["comp"], 2),
             "static": round(sd["static"], 2),
             "meets28": bool(sd["comp"] <= thr and sd["comp"] <= MOTION_EVIDENCE * sd["static"]),
             "unsupported_rows": [[0, r0], [r1, bs]] if (r0 or r1 < bs) else [],
             "unsupported_cols": [[0, c0], [c1, bs]] if (c0 or c1 < bs) else []}
        if other["support"] == 1.0:
            oe = other["err"]
            d["full_side"] = "C" if name == "A" else "A"
            d["full_edge"] = round(float(oe[unsup].mean()), 2)
            d["full_interior"] = round(float(oe[~unsup].mean()), 2)
        out.append(d)
    return out or None


def motion_check(a, b, c, r, k, bs, thr, dab=None, dbc=None):
    """Fable 26(3): is the flagged A-B-A block (row r, col k) explained by its neighbourhood's motion?
    -> (dismissed, info). info: v, the neighbourhood MAD unmoved / moved, the predicted block MADs (None =
    undetermined), and why."""
    h, w = a.shape[:2]
    blk = (slice(r * bs, (r + 1) * bs), slice(k * bs, (k + 1) * bs))
    nb = (slice(max(0, (r - 1) * bs), min(h - h % bs, (r + 2) * bs)),
          slice(max(0, (k - 1) * bs), min(w - w % bs, (k + 2) * bs)))
    m0 = moved_mad(a, c, 0, 0, *nb)
    best, bv = None, (0, 0)
    for dy, dx in SHIFTS:
        m = moved_mad(a, c, dy, dx, *nb)
        if m is not None and (best is None or m < best - 1e-9):
            best, bv = m, (dy, dx)
    info = {"v": bv, "nb0": m0, "nb": best, "ab": None, "bc": None}
    if bv == (0, 0) or best is None or not (best < thr and best <= MOTION_EVIDENCE * m0):
        info["why"] = "motion not evident"
        return False, info
    vy, vx = bv
    ay, ax = vy - vy // 2, vx - vx // 2        # ceil(v / 2): A -> B
    cy, cx = -(vy // 2), -(vx // 2)            # -floor(v / 2): C -> B
    info["ab"] = moved_mad(a, b, ay, ax, *blk, valid=PREDICT_VALID)   # None (undetermined) unless fully supported
    info["bc"] = moved_mad(c, b, cy, cx, *blk, valid=PREDICT_VALID)
    if dab is None:
        dab = moved_mad(a, b, 0, 0, *blk)
    if dbc is None:
        dbc = moved_mad(c, b, 0, 0, *blk)

    def explains(pred, static):   # the prediction matches B AND removes most of that side's static difference
        return pred is not None and pred <= thr and pred <= MOTION_EVIDENCE * static

    dismissed = explains(info["ab"], dab) or explains(info["bc"], dbc)
    info["why"] = "explained by motion" if dismissed else "not explained by motion"
    if not dismissed and DIAG31:   # Fable 31 (2): readonly - recorded, never read by the verdict
        try:   # isolated: a diagnostic failure is recorded, never raised into the detector's loop
            info["diag31"] = diag31(a, b, c, blk, (ay, ax), (cy, cx), thr, bs)
        except Exception as e:
            info["diag31"] = [{"error": "%s: %s" % (type(e).__name__, e)}]
    return dismissed, info


def pace_windows(roll):
    """[(end_time, ab_mask, back)] from the run log's [HIGHFPS_PACE] lines."""
    log = os.path.join(roll, "drive", "attempt1", "run.log")
    out = []
    if not os.path.isfile(log):
        return out
    rx = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[HIGHFPS_PACE\] w=(\d+).*?back=(\d+).*?(?:ab=0x([0-9A-Fa-f]+))?\s*$")
    with open(log, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "[HIGHFPS_PACE] w=" not in line:
                continue
            m = rx.match(line.rstrip("\n"))
            if not m:
                continue
            try:   # informational (the A/B tallies): a line that cannot be read is skipped, never an exception
                t = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
                out.append((t, int(m.group(4), 16) if m.group(4) else None, int(m.group(3))))
            except Exception:
                continue
    return out


# The rule's parameters live in ONE table, flicker_rule.CANONICAL (this file's argument defaults and flicker_gate's fresh
# run both read it): the detector's identity is DETECTOR only at exactly those values (Astra f5e06465 r3 P1: an output made
# with another --thr / --ratio / --block / --motion / --fail-presents carried the canonical id and could be reused into a
# strict PASS). Any other setting prints a distinct, non-canonical identity.
def identity_of(a):
    """The FLICKER DETECTOR line's id for parsed arguments `a`."""
    off = {k: getattr(a, k) for k, v in CANONICAL.items() if getattr(a, k) != v}
    if a.no_motion_check and not off:
        return DETECTOR_STATIC
    if not off:
        return DETECTOR
    return "noncanonical (%s; %s base)" % (", ".join("%s=%s" % kv for kv in sorted(off.items())),
                                           "static" if a.no_motion_check else DETECTOR.split(" ")[0])


def main():
    for stream in (sys.stdout, sys.stderr):   # a path or record the console cannot encode is escaped, never a crash
        try:
            stream.reconfigure(errors="backslashreplace")
        except Exception:
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("roll")
    ap.add_argument("--block", type=int, default=CANONICAL["block"])
    ap.add_argument("--thr", type=float, default=CANONICAL["thr"])
    ap.add_argument("--ratio", type=float, default=CANONICAL["ratio"])
    ap.add_argument("--worst", default=None)
    ap.add_argument("--fail-presents", type=int, default=CANONICAL["fail_presents"],
                    help="verdict: FAIL when this many presents (or more) carry an A-B-A outlier (default 2)")
    ap.add_argument("--motion", type=float, default=CANONICAL["motion"],
                    help="a present whose two neighbours disagree (block MAD > thr) on more than this fraction of the "
                         "frame is a whole-frame camera / cut event, reported as 'motion' and not counted: the defect "
                         "class (an object or draw wrong for ONE present) leaves the neighbours agreeing (default 0.25)")
    ap.add_argument("--no-motion-check", action="store_true",
                    help="the A-B-A rule alone, without Fable 26(3)'s motion re-check of each flagged block (the A/B)")
    ap.add_argument("--run-id", default=None,
                    help="flicker_gate's fresh-run stamp (C2 2026-10-22): echoed as `FLICKER RUN: <id>` so the gate can "
                         "prove the output it grades came from THIS invocation; provenance only, no rule parameter")
    a = ap.parse_args()
    try:   # fail closed (C2's parse-boundary rule): anything this run cannot read or complete is a named record, no verdict
        return _run(a)
    except Exception as e:   # one named record (where it failed: the innermost frame), no traceback
        tb = traceback.extract_tb(e.__traceback__)
        at = ("%s:%d" % (os.path.basename(tb[-1].filename), tb[-1].lineno)) if tb else "?"
        print("FLICKER UNREADABLE: flicker_diff could not complete (%s: %s, at %s) - no verdict (fail closed)"
              % (type(e).__name__, one_line(e), at))
        return 1


def _run(a):
    print("FLICKER DETECTOR: %s" % identity_of(a))
    print("FLICKER PARAMS: %s" % json.dumps(params_record(vars(a), not a.no_motion_check), sort_keys=True))
    if a.run_id is not None:
        print("FLICKER RUN: %s" % a.run_id)
    try:   # C2's parse-boundary rule (Astra r5): a roll whose frames cannot be listed fails closed, named
        fr = frames_of(a.roll)
    except Exception as e:
        print("FLICKER UNREADABLE: the roll's frames cannot be listed (%s: %s) - no verdict (fail closed)"
              % (type(e).__name__, one_line(e, 200)))
        return 1
    if len(fr) < 3:
        print("flicker_diff: %s: %d frames - nothing to score" % (a.roll, len(fr)))
        return 1
    try:   # lane C r8 (Astra e7b0cca r7 P1-3): the scored frames are the span's complete, one-to-one inventory
        fr, why = inventory_problem(a.roll, fr)
    except Exception as e:
        fr, why = None, "the inventory cannot be read (%s: %s)" % (type(e).__name__, one_line(e, 200))
    if why:
        print("FLICKER UNREADABLE: %s - no verdict (fail closed)" % why)
        return 1
    pn = [p for p, _ in fr]
    gaps = sum(1 for i in range(1, len(pn)) if pn[i] != pn[i - 1] + 1)   # 0: inventory_problem refuses a gap
    # Fable 28 (1): FAIL CLOSED on a damaged frame (archive/v1_native/fz3 crashed both detectors on a truncated BMP):
    # every frame is decoded first; any that cannot be is listed and the roll gets no FLICKER PASS / FAIL line at all,
    # only the FLICKER UNREADABLE line (flicker_gate's grade_span makes that a named span FAIL, never a PASS).
    imgs, unreadable = [], []
    for p, f in fr:
        try:
            imgs.append(load(f))
        except Exception as e:   # PIL: truncated / unidentified / undecodable data, a decompression bomb, a decoder's own error
            unreadable.append((p, f, e))
    if unreadable:
        for p, f, e in unreadable[:10]:
            print("  unreadable frame p%06d %s: %s" % (p, os.path.basename(f), one_line(e, 200)))
        print("FLICKER UNREADABLE: %d of %d frames could not be decoded - no verdict (fail closed)"
              % (len(unreadable), len(fr)))
        return 1
    # the dump times and [HIGHFPS_PACE] windows feed only the A/B tallies and the worst list's ab= column (information):
    # a failure there is one note line and scoring runs (Astra r5: a malformed pace line suppressed the verdict)
    try:
        pw = pace_windows(a.roll)
    except Exception as e:
        pw = []
        print("  [HIGHFPS_PACE] unreadable (%s: %s) - not attributed" % (type(e).__name__, one_line(e, 120)))
    try:
        mt = [os.path.getmtime(f) for _, f in fr]
    except Exception as e:
        mt, pw = [0.0] * len(fr), []   # no times: nothing is attributed to a pace window
        print("  dump times unreadable (%s: %s) - the A/B tallies are not attributed" % (type(e).__name__,
                                                                                         one_line(e, 120)))

    def ab_at(t):
        for te, ab, _ in pw:
            if te >= t:
                return ab
        return None

    same = 0
    d1 = [block_mad(imgs[i], imgs[i + 1], a.block) for i in range(len(imgs) - 1)]
    for i in range(len(imgs) - 1):
        if float(d1[i].max()) < 0.5:
            same += 1
    # per present b = frame i+1: (counted blocks, i, max lo-diff of the counted, motion, flagged blocks, dismissed)
    per = []
    by_ab = {}
    motion_hits = 0
    explained = []   # (p#, row, col, info)
    diags = []       # Fable 31 (2): flagged, not dismissed, a side partly supported - (p#, row, col, info); readonly
    for i in range(len(imgs) - 2):
        dab, dbc = d1[i], d1[i + 1]
        dac = block_mad(imgs[i], imgs[i + 2], a.block)
        lo = np.minimum(dab, dbc)
        out = (dab > a.thr) & (dbc > a.thr) & (dac < a.ratio * lo)
        n = int(out.sum())
        motion = bool(n) and float((dac > a.thr).mean()) > a.motion
        if motion:
            motion_hits += 1
        kept = out.copy()
        if n and not motion and not a.no_motion_check:
            for r, k in zip(*np.nonzero(out)):
                gone, info = motion_check(imgs[i], imgs[i + 1], imgs[i + 2], int(r), int(k), a.block, a.thr,
                                          float(dab[r, k]), float(dbc[r, k]))
                if gone:
                    kept[r, k] = False
                    explained.append((pn[i + 1], int(r), int(k), info))
                elif info.get("diag31"):
                    diags.append((pn[i + 1], int(r), int(k), info))
        nk = int(kept.sum())
        per.append((0 if motion else nk, i, float(lo[kept].max()) if nk else 0.0, motion, n, n - nk))
        k = ab_at(mt[i + 1])
        s = by_ab.setdefault(k, [0, 0, 0])
        s[0] += 1
        if not motion:
            s[1] += nk
            s[2] += 1 if nk else 0
    nb = per and (imgs[0].shape[0] // a.block) * (imgs[0].shape[1] // a.block)
    tot = sum(p[0] for p in per)
    hit = sum(1 for p in per if p[0])
    print("flicker_diff %s: presents p%d..p%d (%d files, %d gaps in numbering), %d blocks/frame of %dpx (320x180)"
          % (a.roll, pn[0], pn[-1], len(fr), gaps, nb, a.block))
    print("  identical consecutive presents: %d of %d pairs" % (same, len(fr) - 1))
    print("  A-B-A outliers (thr %.1f, ratio %.2f): %d blocks in %d of %d presents (%.2f blocks/100 presents); "
          "%d presents with outliers inside whole-frame motion (neighbours disagree on > %.0f%% of the frame) not counted"
          % (a.thr, a.ratio, tot, hit, len(per), 100.0 * tot / max(1, len(per)), motion_hits, 100.0 * a.motion))
    if a.no_motion_check:
        print("  motion check: OFF (--no-motion-check: the A-B-A rule alone)")
    else:
        print("  motion check (Fable 26(3)): %d flagged blocks in %d presents explained by their neighbourhood's motion "
              "(not counted; search +-%d px, evidence %.2f, valid %.2f)"
              % (len(explained), len({e[0] for e in explained}), MOTION_SEARCH, MOTION_EVIDENCE, MOTION_VALID))
        for p, r, k, info in explained:
            print("    explained  p%06d  r%dc%d  v=(%+d,%+d)  neighbourhood %.1f -> %.1f  predicted ab %s bc %s"
                  % (p, r, k, info["v"][0], info["v"][1], info["nb0"], info["nb"],
                     "-" if info["ab"] is None else "%.1f" % info["ab"], "-" if info["bc"] is None else "%.1f" % info["bc"]))
        for p, r, k, info in diags:   # Fable 31 (2)(i)-(ii): READONLY, one JSON object per line; never in the verdict
            try:   # isolated end to end (Astra r3 P2): a failure here is one note line, the verdict below always prints
                if DIAG_FAULT:
                    raise OSError("SR_FLICKER_DIAG_FAULT: an injected diagnostic-emission failure (test seam)")
                print("    diag31  p%06d  r%dc%d  %s" % (p, r, k, json.dumps(
                    {"v": list(info["v"]), "nb": [round(info["nb0"], 2), round(info["nb"], 2)], "sides": info["diag31"]},
                    sort_keys=True)))
            except Exception as e:
                try:
                    print("    diag31-error  p%06d  r%dc%d  %s: %s" % (p, r, k, type(e).__name__, one_line(e, 120)))
                except Exception:
                    pass
    if pw:
        print("  [HIGHFPS_PACE] windows=%d back(sum)=%d" % (len(pw), sum(b for _, _, b in pw)))
    if len(by_ab) > 1 or (len(by_ab) == 1 and None not in by_ab):
        for k in sorted(by_ab, key=lambda v: -1 if v is None else v):
            s = by_ab[k]
            print("  ab=%s: presents=%d outlier blocks=%d presents-with-outliers=%d (%.2f/100)"
                  % ("?" if k is None else "0x%X" % k, s[0], s[1], s[2], 100.0 * s[1] / max(1, s[0])))
    worst = sorted(per, key=lambda p: (p[0], p[4]), reverse=True)[:12]
    print("  worst presents (blocks, p#, max lo-diff; 'motion' = not counted, 'explained' = dismissed by the motion check):")
    for n, i, mx, motion, raw, gone in worst:
        if raw == 0:
            break
        print("    %3d  p%06d  %.1f  ab=%s%s%s" % (raw, pn[i + 1], mx, ab_at(mt[i + 1]), "  motion" if motion else "",
                                              "  explained %d" % gone if gone else ""))
    if a.worst and worst and worst[0][4]:
        try:   # evidence (Astra r5; e7b0cca r7 P2: the WHOLE image - its assembly too): a failure is one note line,
            rows = []   # the verdict below always prints
            for n, i, _, motion, raw, gone in worst[:4]:
                if raw == 0:
                    break
                rows.append(np.concatenate([imgs[i], imgs[i + 1], imgs[i + 2]], axis=1))
            Image.fromarray(np.concatenate(rows, axis=0).astype(np.uint8)).save(a.worst, quality=88)
            print("  worst triples -> %s" % a.worst)
        except Exception as e:
            print("  worst triples not written (%s: %s)" % (type(e).__name__, one_line(e, 120)))
    verdict = "FAIL" if hit >= a.fail_presents else "PASS"
    print("FLICKER %s: %d of %d presents carry an A-B-A outlier (fail at %d)" % (verdict, hit, len(per), a.fail_presents))
    return 1 if verdict == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
