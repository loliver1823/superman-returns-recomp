#!/usr/bin/env python3
"""freeze_diff.py - iter-466 hfr: the clock-inventory check over SR_DIAG_SIM_FREEZE frames.

    python tools/perf/freeze_diff.py <game log> <present dump dir> --out <dir> [--thresh 8]

While the sim loop is held (SR_DIAG_SIM_FREEZE) the render loop keeps drawing the newest
StateStream frame (t = 0), so every system on the SIM clock - and every render-side reader of the
shown game time - draws the same picture every pass. The log's [SIM_FREEZE_DUMP] lines name the
presents dumped inside each freeze (SR_PRESENT_DUMP_FREEZE=1). For each freeze this compares every
dumped frame with the freeze's first one and reports: frames, the held time between them, the
shown game-time stamp (must be constant), the share of pixels that changed by more than --thresh
(max channel difference), the max difference, and the bounding box of the changed pixels. A
per-freeze sheet (first frame | last frame | changed pixels in red) goes to --out.
"""
import argparse
import glob
import os
import re
import sys
import traceback

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_records  # noqa: E402  (the single-record rule the standalone detectors share, lane C r17)

INVENTORY_TAGS = log_records.INVENTORY_TAGS   # the tags --exact takes inventory records from (r21: log_records')

DUMP = re.compile(r'\[SIM_FREEZE_DUMP\] freeze=(\d+) k=(\d+) present=(\d+) held_ms=(\d+) shown_stamp=(\d+(?:\.\d+)?) s'
                  r'(?: [^\s=]+=\S+)*\s*$')   # r15 (Astra fb6f4c6 r14 P2): the WHOLE record (`s` then key=value fields only)
DUMP_TAG = '[SIM_FREEZE_DUMP]'   # lane C r9 (Astra a350efe conv8): ANY line carrying the tag must be a whole DUMP record
# (archive survey 2026-10-23, 18 real freeze rolls: no other [SIM_FREEZE_DUMP] line form)
BEGIN = re.compile(r'^\[([\d\- :.]+)\].*\[SIM_FREEZE\] #(\d+) begin at ([\d.]+) s: sim time ([\d.]+) s')
BEGIN_ID = re.compile(r'\[SIM_FREEZE\] #(\d+) begin at [\d.]+ s: sim time [\d.]+ s, sim ticks \d+, render ticks \d+\s*$')
# ^ a freeze that began: its WHOLE begin record (r14, Astra 263c082 r13 P2: one cut after `begin at ` was accepted)
# the run log's record of every BMP the exe wrote (world_floor.DUMP_NAME) and the one [PRESENT_DUMP] note it may carry
PDUMP = re.compile(r'\[PRESENT_DUMP\] (.+?_p(\d+)_\d+x\d+\.bmp)(?=\s|$)')   # r16: `.bmp` ends the name (not `.bmpX`)
PMARK = re.compile(r'\[PRESENT_DUMP\] window opened by the harness dump mark at present \d+ \(cap \d+\)\s*$')


def inventory_check(files, freezes, begun, pdumps, dense=True, names=None):
    """Lane C r9 (Astra a350efe r8 / conv8 P1: a freeze dump RECORD removed - its file kept - or a whole freeze's records
    removed passed strict): --exact grades only a COMPLETE frozen-present inventory, reconciled three ways. Raises
    ValueError (FREEZE_EXACT UNREADABLE, no verdict) unless:
      - every dump file on disk is a present some [SIM_FREEZE_DUMP] record names (the records -> files direction is
        _exact's "not on disk" check);
      - the run log's [PRESENT_DUMP] records (every BMP the exe wrote; pdumps, None when the log has none) name exactly
        the recorded presents;
      - (r12) the files those [PRESENT_DUMP] records log writing are exactly the dump files on disk, BY NAME (names: a
        missing dump is never replaced by another file claiming its present number);
      - every freeze that began ([SIM_FREEZE] #N begin at) has dump records;
      - dense (the plain table; SR_PRESENT_DUMP_FREEZE=2 dumps every present of the hold): each freeze's records run
        k = 0..n-1 over consecutive presents, so no interior present can be missing from all three sources at once.
    Archive survey 2026-10-23 (18 real freeze rolls): dense in k and present everywhere; records == [PRESENT_DUMP] wherever
    both are logged; every begin has dumps on an exe that dumps freezes."""
    recorded = {r[1] for rows in freezes.values() for r in rows}
    extra = sorted(set(files) - recorded)
    # (the messages lead with their finding: flicker_gate's freeze_error keeps 200 characters after its own prefix)
    if extra:
        raise ValueError('inventory incomplete: %d dump file(s) no [SIM_FREEZE_DUMP] record names (present %d first)'
                         % (len(extra), extra[0]))
    if pdumps is not None:   # {present: [PRESENT_DUMP] records naming it} (r10, Astra 2453fdf conv9 P1: a set lost a
        twice = sorted(p for p, n in pdumps.items() if n > 1)   # repeated record - one record per BMP written)
        if twice:
            raise ValueError('ambiguous write: present %d has %d [PRESENT_DUMP] records' % (twice[0], pdumps[twice[0]]))
    if pdumps is not None and set(pdumps) != recorded:
        diff = sorted(set(pdumps) ^ recorded)
        raise ValueError('inventory incomplete: [PRESENT_DUMP] and [SIM_FREEZE_DUMP] records disagree on %d present(s) '
                         '(%d first)' % (len(diff), diff[0]))
    if names is not None:   # r12 (Astra 190f7a5 conv11 P1: a missing dump replaced by another file claiming its present):
        logged_names, disk_names = names   # the FILES the exe logged writing are the files on disk, by name
        gone = sorted(logged_names - disk_names)
        if gone:
            raise ValueError('inventory incomplete: %d logged dump file(s) not on disk (%s first)' % (len(gone), gone[0]))
        unnamed = sorted(disk_names - logged_names)
        if unnamed:
            raise ValueError('inventory incomplete: %d dump file(s) on disk the run log does not name (%s first)'
                             % (len(unnamed), unnamed[0]))
    bare = sorted(set(begun) - set(freezes))
    if bare:
        raise ValueError('inventory incomplete: freeze %d began but has no [SIM_FREEZE_DUMP] record' % bare[0])
    if dense:
        for fid in sorted(freezes):
            ks = [r[0] for r in freezes[fid]]
            ps = [r[1] for r in freezes[fid]]
            if ks != list(range(len(ks))) or any(ps[i] != ps[0] + i for i in range(len(ps))):
                raise ValueError('inventory incomplete: freeze %d\'s dump records are not consecutive (k %s; presents %s)'
                                 % (fid, ','.join(map(str, ks))[:40], ','.join(map(str, ps))[:60]))
END = re.compile(r'\[SIM_FREEZE\] #(\d+) end: (\d+) render ticks while held \(([\d.]+)/s\)')
# every [SIM_FREEZE] record's whole forms (archive survey: begin, end, the eye-adaptation / DIAGNOSTIC / shader-time notes),
# lane C r18; r21: log_records.FORMS's - the one definition flicker_gate.RECORD_FORMS takes them from too
SIM_FREEZE_FORMS = [re.compile(r'\[SIM_FREEZE\] ' + p[1:]) for p in log_records.FORMS['SIM_FREEZE']]


def log_inventory(lines, exact, pnames=None):
    """The run log's freeze inventory, read line by line (main's loop; the mutation sweep calls it on one line): (the
    dump records by freeze, the begin / end info, the freezes that began, {present: [PRESENT_DUMP] records naming it},
    the names those records log). exact: every line meets the shared single-record rule FIRST, before any filter (r17;
    r24: as flicker_diff.dump_records now does too)."""
    freezes, info = {}, {}
    logged = {}   # present -> the freeze whose dump record names it: one record per present (Astra e7b0cca r7 P1-3)
    begun, pdumps = set(), {}   # r9: the freezes that began; r10: {present: [PRESENT_DUMP] records naming it}
    pnames = set() if pnames is None else pnames
    for line in lines:
        if exact:   # r17 (Astra 5fb5a2c r16 / conv16 P2): a line carrying an inventory tag is ONE whole record
            why = log_records.record_problem(line.rstrip('\r\n'), INVENTORY_TAGS)
            if why:
                raise ValueError('%s: %r' % (why, line.strip()[:160]))
        if '[PRESENT_DUMP]' in line:   # r9: a BMP the exe wrote, or the window's mark note - nothing else
            pm = PDUMP.search(line)
            if pm:
                pdumps[int(pm.group(2))] = pdumps.get(int(pm.group(2)), 0) + 1
                pnames.add(re.split(r'[\\/]', pm.group(1))[-1])
            elif not PMARK.search(line):
                raise ValueError('a [PRESENT_DUMP] record that cannot be read: %r' % line.strip()[:160])
            continue
        # (r19, Astra d26a1a7 r18 / conv18 P2: recognised by its TAG, whatever follows it - a bare `[SIM_FREEZE]` or a
        # tab after it is no whole form either)
        if exact and '[SIM_FREEZE]' in line and not any(rx.search(line) for rx in SIM_FREEZE_FORMS):
            # r18 (Astra 7c872fc r17 / conv17 P2: `[SIM_FREEZE] #2 beg` passed and its begin vanished): every
            # [SIM_FREEZE] record is one of its whole forms (as the strict gate's log_integrity requires)
            raise ValueError('a [SIM_FREEZE] record that cannot be read: %r' % line.strip()[:160])
        if '[SIM_FREEZE] #' in line and ' begin' in line:
            bm = BEGIN_ID.search(line)
            if not bm:   # a begin record cut short never hides that its freeze began (r9)
                raise ValueError('a [SIM_FREEZE] begin record that cannot be read: %r' % line.strip()[:160])
            begun.add(int(bm.group(1)))
        m = DUMP.search(line)
        if m:
            fid, k, pn, ms, st = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5))
            if pn in logged:   # a repeated record would compare a present with itself (a pair that hides nothing)
                raise ValueError('the inventory is not one-to-one: present %d has two [SIM_FREEZE_DUMP] records '
                                 '(freezes %d, %d)' % (pn, logged[pn], fid))
            logged[pn] = fid
            freezes.setdefault(fid, []).append((k, pn, ms, st))
            continue
        if DUMP_TAG in line:
            # a dump record that cannot be read never silently drops its present - whatever field is missing or cut
            # (Astra 6fb621c r6: held_ms=BROKEN; e7b0cca r7 P1-2: a record cut before held_ms= was not recognised)
            raise ValueError('a [SIM_FREEZE_DUMP] record that cannot be read: %r' % line.strip()[:160])
        m = BEGIN.search(line)
        if m:
            info.setdefault(int(m.group(2)), {})['begin'] = (m.group(1), float(m.group(3)), float(m.group(4)))
            continue
        m = END.search(line)
        if m:
            info.setdefault(int(m.group(1)), {})['rps'] = float(m.group(3))
    return freezes, info, begun, pdumps, pnames


def one_line(s, n=160):
    """An error text as ONE bounded line (lane C r8, convergence P3: a message with a newline split the catch-all's
    record across lines)."""
    return ' '.join(str(s).split())[:n]


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


class Unreadable(Exception):
    """A frozen present that cannot be decoded: --exact fails closed (FREEZE_EXACT UNREADABLE, no verdict)."""


def load_frame(path, fid, pn):
    try:
        return load(path)
    except Exception as e:   # PIL: truncated / unidentified / undecodable data, a vanished file, a decoder's own error
        raise Unreadable('freeze %d present %d (%s): %s: %s' % (fid, pn, os.path.basename(path), type(e).__name__,
                                                               one_line(e)))


def frame_for(files, fid, pn):
    """The decoded frozen present pn. The inventory is one-to-one (Astra e7b0cca r7 P1-3): when two or more dump files
    claim the present (a file name's present number), they must decode to the same picture, else the present is
    ambiguous - which one the exe showed is unknown - and --exact fails closed (Unreadable)."""
    paths = files[pn]
    img = load_frame(paths[0], fid, pn)
    for p in paths[1:]:
        other = load_frame(p, fid, pn)
        if other.shape != img.shape or bool((other != img).any()):
            raise Unreadable('freeze %d present %d is ambiguous: %d dump files claim it and differ (%s)'
                             % (fid, pn, len(paths), ', '.join(os.path.basename(q) for q in paths)[:200]))
    return img


def sheet_of(a_img, b_img, mask):
    """The per-freeze sheet: a | b | b with the changed pixels in red."""
    hl = b_img.copy()
    if mask is not None:
        hl[mask] = [255, 0, 0]
    im = Image.fromarray(np.concatenate([a_img, b_img, hl], axis=1).clip(0, 255).astype(np.uint8))
    im.thumbnail((2400, 600))
    return im


def save_sheet(build, path):
    """An evidence image (the per-freeze sheet): building it (build() -> the image) or writing it failing is one note
    line, never an end to the table (Astra e1cca492 r5 P1: a failed save BEFORE a freeze's row left a table without
    that freeze, which graded as a PASS; 6fb621c r6 P2: the construction too)."""
    try:
        build().save(path)
    except Exception as e:
        print('image-error | %s | %s: %s' % (os.path.basename(path), type(e).__name__, one_line(e, 120)))


# test seam (test_parse_boundaries): after this many freeze rows, --exact dies the way a killed process would (no
# FREEZE_EXACT record) - flicker_gate must grade that table as incomplete, never as the rows it got
try:
    FAULT_AFTER = int(os.environ.get('SR_FREEZE_DIFF_FAULT_AFTER', '0') or 0)
except ValueError:
    FAULT_AFTER = 0


def main():
    for stream in (sys.stdout, sys.stderr):   # a path or record the console cannot encode is escaped, never a crash
        try:
            stream.reconfigure(errors='backslashreplace')
        except Exception:
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('dumps')
    ap.add_argument('--out', required=True)
    ap.add_argument('--thresh', type=int, default=8)
    ap.add_argument('--exact', action='store_true',
                    help='flicker gate: every frozen present must be pixel-identical to the one before it '
                         '(any differing pixel is a rendering defect); prints FREEZE_EXACT PASS/FAIL, exit 1 on FAIL')
    ap.add_argument('--allow-maxdiff', type=int, default=0,
                    help='--exact: tolerate per-channel differences up to this (default 0 = exact)')
    ap.add_argument('--ab-control', action='store_true',
                    help='--exact on a per-present A/B roll (SR_AB_PRESENT=1): a consecutive pair p->p+1 (ON<->OFF) '
                         'counts only when its same-transport control p-1->p+1 is exact (the first pair: p->p+2); '
                         'the other pairs are UNSETTLED (the renderer still changing on both transports) and are '
                         'reported, not graded. Adds the static / unsettled columns to the table')
    a = ap.parse_args()
    try:   # fail closed (C2's parse-boundary rule): anything this run cannot read or complete is a named record, no verdict
        return _run(a)
    except Exception as e:   # one named record (where it failed: the innermost frame), no traceback
        tb = traceback.extract_tb(e.__traceback__)
        at = ('%s:%d' % (os.path.basename(tb[-1].filename), tb[-1].lineno)) if tb else '?'
        print('FREEZE_EXACT UNREADABLE: freeze_diff could not complete (%s: %s, at %s) - no verdict (fail closed)'
              % (type(e).__name__, one_line(e), at))
        return 1


def _run(a):
    try:   # the sheets' folder is evidence: without it each sheet's write fails as one note line, the table still runs
        os.makedirs(a.out, exist_ok=True)
    except Exception as e:   # stderr: the table's first line stays its header (flicker_gate reads the A/B columns there)
        print('image-error | %s | %s: %s' % (os.path.basename(a.out), type(e).__name__, one_line(e, 120)), file=sys.stderr)
    files = {}   # present -> [dump files claiming it] (sorted): two files never silently replace each other (r7 P1-3)
    disk_names, pnames = set(), set()   # r12: every dump file's name on disk; every name a [PRESENT_DUMP] record logs
    for f in glob.glob(os.path.join(a.dumps, '**', 'present_*.bmp'), recursive=True):
        disk_names.add(os.path.basename(f))
        m = re.search(r'_p(\d{6,})_', os.path.basename(f))   # %06d: six digits, more past p999999
        if m:
            files.setdefault(int(m.group(1)), []).append(f)
    for v in files.values():
        v.sort()
    if a.exact:   # r15 (Astra fb6f4c6 r14 P2): a log whose last record is cut (no final newline) is no inventory
        with open(a.log, 'rb') as fh:
            data = fh.read()
        if data and not data.endswith(b'\n'):
            raise ValueError('the run log\'s last line is cut (no final newline)')
        data.decode('utf-8')   # r16 (Astra ed02b5a r15 / conv15 P2): strict UTF-8 - an undecodable record is no record
        # r24 (Astra dac76f1 r23 P1): the whole log meets the strict gate's STRICT LOG INTEGRITY first - the same rule,
        # one definition (log_records.log_integrity) - so no splice the gate refuses reaches this reader either
        log_records.log_integrity(a.log, data)
    with open(a.log, encoding='utf-8', errors='replace') as fh:
        freezes, info, begun, pdumps, pnames = log_inventory(fh, a.exact, pnames)
    if a.exact:   # r9: the complete inventory first (a ValueError here is main's one FREEZE_EXACT UNREADABLE line)
        # r11 (Astra 8d20c07 r10 P1): the [PRESENT_DUMP] records always reconcile - a log with none at all disagrees with
        # every recorded present (the exe writes one per BMP: archive survey, every freeze-dumping roll)
        inventory_check(files, freezes, begun, pdumps, dense=not a.ab_control, names=(pnames, disk_names))
        return exact(a, files, freezes)
    print('freeze | wall_s | sim_s | render/s | frames | held_ms span | stamps | changed%% (>%d) | maxdiff | bbox' % a.thresh)
    for fid in sorted(freezes):
        rows = [r for r in freezes[fid] if r[1] in files]
        inf = info.get(fid, {})
        b = inf.get('begin', ('?', 0.0, 0.0))
        if len(rows) < 2:
            print('%d | %.1f | %.2f | %s | %d | - | - | (too few frames on disk)' % (fid, b[1], b[2], inf.get('rps', '?'), len(rows)))
            continue
        base = load(files[rows[0][1]][0])
        stamps = sorted(set(r[3] for r in rows))
        worst, worst_mask, maxd = 0.0, None, 0
        last = None
        for r in rows[1:]:
            img = load(files[r[1]][0])
            if img.shape != base.shape:
                continue
            d = np.abs(img - base).max(axis=2)
            mask = d > a.thresh
            frac = mask.mean() * 100.0
            maxd = max(maxd, int(d.max()))
            if frac >= worst:
                worst, worst_mask = frac, mask
            last = img
        bbox = '-'
        if worst_mask is not None and worst_mask.any():
            ys, xs = np.nonzero(worst_mask)
            bbox = '%d,%d-%d,%d' % (xs.min(), ys.min(), xs.max(), ys.max())
        print('%d | %.1f | %.2f | %s | %d | %d-%d | %s | %.3f | %d | %s' % (
            fid, b[1], b[2], inf.get('rps', '?'), len(rows), rows[0][2], rows[-1][2],
            ','.join('%.3f' % s for s in stamps), worst, maxd, bbox))
        if last is not None:
            save_sheet(lambda: sheet_of(base, last, worst_mask), os.path.join(a.out, 'freeze_%03d.png' % fid))
    return 0


MIN_STATIC_PAIRS = 20   # --ab-control: a freeze needs this many static pairs (half the 39-pair window) to be graded
SETTLE_MAXDIFF = 8      # ... and an excluded pair's control above this (the gate's spike tolerance) means the frame MOVED


def exact(a, files, freezes):
    """--exact, failing closed: a frozen present that cannot be decoded ends the table with FREEZE_EXACT UNREADABLE (no
    verdict, exit 1); flicker_gate grades a fresh table only when it ends with its FREEZE_EXACT record and the rows add up
    to the record's frame / pair counts (C2's parse-boundary rule, Astra e1cca492 r5 P1). files: present -> its dump
    file(s) (a path, or the list _run builds: several files claiming one present must agree, frame_for)."""
    files = {p: ([v] if isinstance(v, str) else list(v)) for p, v in files.items()}
    try:
        return _exact(a, files, freezes)
    except Unreadable as e:
        print('FREEZE_EXACT UNREADABLE: %s - no verdict (fail closed)' % one_line(e, 400))
        return 1


def _exact(a, files, freezes):
    """Consecutive frozen presents compared exactly. A present that differs from the one before it
    while the sim is held is a rendering defect (a dropped / wrong draw, stale data, a race): the
    frozen game draws the same picture every pass (docs/HIGH_FRAMERATE.md section 6).
    --ab-control (the per-present A/B): the pair p->p+1 compares ON with OFF; its same-transport control
    p-1->p+1 (p->p+2 for a freeze's first pair) says whether the frame is static at all. A pair whose control
    differs sits on a frame the renderer is still changing on BOTH transports (a settling exposure / mip /
    temporal term inside the hold: s1b_abps freeze 11, r467b_abp freeze 6, 2026-10-09) and cannot judge the
    A/B; it is counted and reported as UNSETTLED with the largest control difference, never graded as the A/B.
    A differing static pair is a finding whatever else the window did. A freeze with fewer than MIN_STATIC_PAIRS
    static pairs, or with an excluded pair whose control exceeds SETTLE_MAXDIFF (a settling transient changes
    values by LSBs; a dropped draw or a black flip changes them by hundreds: the frame moved), is NO DATA, not
    PASS. The trailing static run is reported as information."""
    ctl = a.ab_control
    bad, unsettled_ids, frames, pairs = [], [], 0, 0
    tot_static = tot_unsettled = 0
    worst_ctrl_all = None
    hdr = 'freeze | frames | pairs | differing pairs | worst pair (presents) | pixels changed | maxdiff | bbox'
    if ctl:
        hdr += (' | static pairs | differing static | worst static pair (presents) | pixels (static) | maxdiff (static)'
                ' | unsettled pairs | worst control (presents) | pixels (control) | maxdiff (control) | trailing static pairs')
    print(hdr)
    done = 0
    for fid in sorted(freezes):
        # the log's dump records are the inventory: a logged frozen present that is not on disk is never silently
        # left out of the comparison (Astra 6fb621c r6: reconcile the expected dumps with the processed ones)
        gone = [r[1] for r in freezes[fid] if r[1] not in files]
        if gone:
            raise Unreadable('freeze %d: %d logged dump(s) not on disk (present %d first)' % (fid, len(gone), gone[0]))
        rows = sorted((r for r in freezes[fid] if r[1] in files), key=lambda r: r[1])
        if len(rows) < 2:   # lane C r10 (Astra 2453fdf r9 P1: a corrupt singleton was skipped unread, the table PASSED):
            # a freeze with no pair to compare is an incomplete inventory, never a freeze left out (archive survey: the
            # smallest real freeze holds 19 presents)
            raise Unreadable('freeze %d has %d frozen present(s): no pair to compare' % (fid, len(rows)))
        if FAULT_AFTER and done >= FAULT_AFTER:   # the seam: die like a killed process (the rows so far, no record)
            sys.stdout.flush()
            os._exit(1)
        done += 1
        stamps = set(r[3] for r in rows)
        prev2, prev = None, frame_for(files, fid, rows[0][1])
        frames += len(rows)
        ndiff, worst = 0, None
        nstatic, ndiff_static, worst_static = 0, 0, None   # --ab-control
        nunsettled, worst_ctrl, trailing = 0, None, 0   # trailing = the static run that ends the window (it settled)
        first_pair = None
        # every differing pair, for flicker_gate's movie rule (a movie excuses only the pairs inside its window):
        # printed after the freeze's row as `pair | freeze | present a | present b | pixels | maxdiff`
        diff_rows = []
        # Astra round 2 on native/zpd-bake (P2-2): EVERY pair this table compared, after the freeze's row, as
        # `compared | freeze | present a | present b | static|unsettled|plain` (--ab-control: static = graded, its control
        # exact; unsettled = excluded) - so tools/perf/zpd_activity.py counts the presents the grade actually compared
        cmp_rows = []

        def classify(pair, nc, cpres, cmax, img, m):
            nonlocal nstatic, ndiff_static, worst_static, nunsettled, worst_ctrl, trailing
            cmp_rows.append('compared | %d | %d | %d | %s' % (fid, pair[0], pair[1], 'static' if nc == 0 else 'unsettled'))
            if nc == 0:
                nstatic += 1
                trailing += 1
                if pair[2]:
                    ndiff_static += 1
                    if worst_static is None or pair[2] > worst_static[2]:
                        worst_static = pair
            else:
                nunsettled += 1
                trailing = 0
                if worst_ctrl is None or nc > worst_ctrl[2]:
                    worst_ctrl = (cpres[0], cpres[1], nc, cmax)

        for i in range(1, len(rows)):
            img = frame_for(files, fid, rows[i][1])
            pairs += 1
            if img.shape != prev.shape:
                ndiff += 1
                diff_rows.append('pair | %d | %d | %d | -1 | 255' % (fid, rows[i - 1][1], rows[i][1]))
                # a frozen present whose SIZE changed differs everywhere: it is the freeze's worst pair (lane C, r6: it
                # used to count in ndiff while the row printed 0 differing pairs and the freeze was never marked)
                cand = (1 << 30, rows[i - 1][1], rows[i][1], 255, 'size %dx%d->%dx%d' % (
                    prev.shape[1], prev.shape[0], img.shape[1], img.shape[0]))
                if worst is None or cand[0] > worst[0]:
                    worst = cand
                prev2, prev = prev, img
                continue
            d = np.abs(img - prev).max(axis=2)
            m = d > a.allow_maxdiff
            n = int(m.sum())
            pair = (rows[i - 1][1], rows[i][1], n, int(d.max()) if n else 0)
            if not ctl:
                cmp_rows.append('compared | %d | %d | %d | plain' % (fid, pair[0], pair[1]))
            if n:
                ndiff += 1
                diff_rows.append('pair | %d | %d | %d | %d | %d' % (fid, pair[0], pair[1], n, pair[3]))
                ys, xs = np.nonzero(m)
                cand = (n, rows[i - 1][1], rows[i][1], int(d.max()), '%d,%d-%d,%d' % (xs.min(), ys.min(), xs.max(), ys.max()))
                if worst is None or n > worst[0]:
                    worst = cand
                    save_sheet(lambda: sheet_of(prev, img, m), os.path.join(a.out, 'freeze_exact_%03d.png' % fid))
            if ctl:
                if i >= 2 and prev2 is not None and prev2.shape == img.shape:
                    dc = np.abs(img - prev2).max(axis=2)
                    nc = int((dc > a.allow_maxdiff).sum())
                    cmax = int(dc.max()) if nc else 0
                    cpres = (rows[i - 2][1], rows[i][1])
                    classify(pair, nc, cpres, cmax, img, m)
                    if i == 2 and first_pair is not None:  # the first pair's control is this same p0->p2 difference
                        classify(first_pair, nc, cpres, cmax, img, m)
                elif i == 1:
                    first_pair = pair
                    if len(rows) == 2:  # no third frame: no control, the pair is unsettled by construction
                        classify(pair, 1, (rows[0][1], rows[1][1]), 0, img, m)
            prev2, prev = prev, img
        if len(stamps) > 1:
            print('%d | %d | %d | (shown stamp moved: %s - not a frozen window)' % (fid, len(rows), len(rows) - 1,
                                                                                   ','.join('%.3f' % s for s in sorted(stamps))))
            continue
        base = ('%d | %d | %d | %d | p%d->p%d | %d | %d | %s' % (fid, len(rows), len(rows) - 1, ndiff, worst[1], worst[2],
                                                               worst[0], worst[3], worst[4])) if worst else \
               ('%d | %d | %d | 0 | - | 0 | 0 | -' % (fid, len(rows), len(rows) - 1))
        if not ctl:
            if worst:
                bad.append(fid)
            print(base)
            for row in diff_rows + cmp_rows:
                print(row)
            continue
        tot_static += nstatic
        tot_unsettled += nunsettled
        if worst_ctrl and (worst_ctrl_all is None or worst_ctrl[2] > worst_ctrl_all[2]):
            worst_ctrl_all = (fid,) + worst_ctrl
        ws = ('p%d->p%d | %d | %d' % (worst_static[0], worst_static[1], worst_static[2], worst_static[3])) if worst_static else '- | 0 | 0'
        wc = ('p%d->p%d | %d | %d' % (worst_ctrl[0], worst_ctrl[1], worst_ctrl[2], worst_ctrl[3])) if worst_ctrl else '- | 0 | 0'
        print('%s | %d | %d | %s | %d | %s | %d' % (base, nstatic, ndiff_static, ws, nunsettled, wc, trailing))
        for row in diff_rows + cmp_rows:
            print(row)
        if worst_static:  # a differing static pair is a finding whatever else the window did
            bad.append(fid)
        elif nstatic < MIN_STATIC_PAIRS or (worst_ctrl and worst_ctrl[3] > SETTLE_MAXDIFF):
            unsettled_ids.append(fid)
    if ctl:
        verdict = 'FAIL' if bad else ('PASS' if pairs and tot_static else 'NO DATA')
        print('FREEZE_EXACT %s: %d freezes with a differing STATIC pair (%s) | %d frozen presents, %d consecutive pairs, '
              'allow-maxdiff %d | A/B control: %d static pairs, %d unsettled (excluded), %d freezes NO DATA (below %d '
              'static pairs or moved: a control above maxdiff %d) (%s), worst control %s'
              % (verdict, len(bad), ','.join(map(str, bad)) or '-', frames, pairs, a.allow_maxdiff, tot_static,
                 tot_unsettled, len(unsettled_ids), MIN_STATIC_PAIRS, SETTLE_MAXDIFF, ','.join(map(str, unsettled_ids)) or '-',
                 ('freeze %d p%d->p%d %d px at maxdiff %d' % worst_ctrl_all) if worst_ctrl_all else '-'))
        return 1 if bad or not pairs else 0
    verdict = 'FAIL' if bad else ('PASS' if pairs else 'NO DATA')
    print('FREEZE_EXACT %s: %d freezes with a differing present (%s) | %d frozen presents, %d consecutive pairs, '
          'allow-maxdiff %d' % (verdict, len(bad), ','.join(map(str, bad)) or '-', frames, pairs, a.allow_maxdiff))
    return 1 if bad or not pairs else 0


if __name__ == '__main__':
    sys.exit(main())
