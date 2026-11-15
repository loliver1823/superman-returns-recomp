#!/usr/bin/env python3
"""xenia_pacing_cmp.py - display.registers' timing reference: our build's guest present pacing against Xenia
Canary's, on the same route and the same scene, from two tools/perf/pacing_probe.py captures
(tools/perf/canary_compare.py --pacing: the route tape, then the pause menu held to the anchor + 300 s).

    python tools/perf/xenia_pacing_cmp.py <xenia capture> <ours capture> [--calibration <second xenia capture>]
                                          [--json out.json] [--pilot]

A capture is a pacing_probe output directory (or a canary_compare run directory holding one in pacing/).

WHAT IS COMPARED. The probe reads, at the entry of every guest present (D3DDeviceSwap_sub_82112050), on both programs
with the same code: the D3D device's vblank count (dev+0x3B14, the vblank ISR source 0), its gate count (dev+0x3B20,
82111E38 once per gated present) and its throttle wait snapshot (dev+0x4E4C); and what the present REQUESTS - the
PresentInterval (dev+0x33A4), the word whose bits 23..29 are the late-release pct (dev+0x2D3C) and the front buffer
(dev+0x3490), the three words GpuPresentEmitCommandBlock_sub_82111EC8 reads inside the present to build the gate's
argument (vblanks << 16 | pct) and the words our native present gate reads (kernel/native_flush.cpp
presentGatePayload): the same device struct in both programs - and the render loop's tick period (FGGameRender's
GameKernelMgr *0x8296D8DC, +0x48 ns: GameKernelMgr_SetTickRate_sub_82720418). The scene is the PAUSE MENU (both
programs at 30 fps there: the docs/CANARY_COMPARISON.md baseline); gameplay is not graded (Xenia is CPU-bound there).

THE REQUEST, per present, read and never inferred: N = the gate's vblanks (82111EC8's map: interval <= 1 -> 1,
2 -> 2, 4 -> 3; none for 0x80000000 IMMEDIATE or a front buffer without a page, which skip the gate, or any other
value); R = the render loop's period in vblanks (period x 60 Hz, a whole number within PERIOD_TOL, else none) - the
loop presents once per tick and never runs faster than its rate; T = max(N, R), the vblanks per present the game asks
for (the console's cap min(the render rate, the vblank rate / N): docs/HIGH_FRAMERATE.md 2.3; the golden PresentInterval
is 1, with the render loop at 30 Hz T is 2 whether N is 1 or 2). A pair of consecutive presents is GRADED against T
when both ends read the same (N, R); a pair with an end whose request does not read is UNREADABLE, one whose ends read
different (N, R) a TRANSITION; both are counted, neither graded.

THE WINDOW (per capture): from the guest's PAUSE_NTH-th (second) request of screens/misc/ScrPauseMenu + WIN_LEAD_S
(3 s: the menu's own loads settle) to the capture's last record - WIN_TAIL_S (1 s); vblanks / gates per present are
deltas between consecutive probe sequence numbers inside it (never across a missing one), the wait one per present.

PRE-REGISTERED for the graded set dxg1 (2026-10-16; fixed from the pilot dxp1 and dxp0's Xenia capture,
archive/dxen/NOTES_dxg1_draft.txt; TOLERANCES_FIXED). The statistical checks bootstrap the graded pairs (vblanks, T)
with tools/perf/pacing_cmp.py's bootstrap (BOOT resamples, fixed seed, one-sided CONF 95% bounds on ours - Xenia;
PASS only inside the margin, FAIL only clearly beyond it, else INCONCLUSIVE), all ONE-SIDED (ours steadier than Xenia
is never a regression):
  * mean_distance_match: |mean(vblanks - T)| ours - Xenia <= +MEAN_DIST_M (0.05 vblanks a present);
  * early_release_match: the share of presents after fewer than T vblanks <= +EARLY_M (0.12);
  * late_match: the share after more than T (a missed vblank) <= +LATE_M (0.13);
  * hitch_rate_match: the share at >= HITCH_V = 6 vblanks <= +HITCH_M (0.002).
  Fixed, per capture: coverage (>= MIN_PAIRS = 3,000 GRADED pairs, <= 1% of the window's sequence numbers missing, no
  duplicate); requested_read (the request reads on all but <= MAX_UNREAD_SHARE = 1% of the window's pairs, with at
  least one graded pair: a capture whose request does not read FAILS - a capture in dxp0 / dxp1's 32-byte format
  among them); gates per present within GATE_TOL (0.02) of 1; the pause held (no other 'screens/' request inside the
  window); the vblank cadence within CADENCE_REL (1%) of 60 Hz. Across: requested_match (the same set of (N, R) over
  the graded pairs on both); the vblank cadence within CADENCE_REL of Xenia's; the max vblanks per present <=
  max(Xenia's, HITCH_V) + MAX_ALLOW_V (2).
  Information only: the vblanks-per-present TVD (Xenia against itself 0.171 in the pilot: the shape is not gradable
  in this scene), the early / late shares against N alone, the pct, the throttle wait (CPU speed), present intervals
  in milliseconds (host scheduling).
  What the rule cannot see, stated: ours regressing to Xenia-like dispersion (e.g. its early share rising to ~10%)
  passes - still timing >= Xenia, the claim; ours' own off-request share is its baseline item, not this check's.
ROUND 2 (Astra round 1, for dxg3; archive/dxen/NOTES_dxg3_draft.txt; the margins above unchanged):
  * THE RELEASE (P1-1). Swap entries alone cannot see the gate: at N = 1 / R = 2 a gate that releases at once would
    leave every statistic above as it is. The probe also reads the gate's own release record at each entry - dev+0x3B18
    (the vblank count at the last release) and dev+0x3B1C (the countdown; > 0 pending, 0 released at a vblank, < 0
    released at the callback, NOW), stored by 82111E38 / the vblank ISR in Xenia and by our host gate mirroring them -
    and releases() rebuilds every gate's release from them (the callback between two entries, its request N, the
    vblank of the entry before it). Fixed, per capture: release_read (the release state read: >= MIN_PAIRS releases
    observed, conflicts and torn reads <= 1%); release_not_early (no release earlier than its request: release -
    previous release >= N, zero tolerance - the gate's contract); release_at_vblank (a release in the vblank of its
    own request's entry, or a NOW release, on <= NOW_MAX = 0.1% of the observed releases: WAIT / NEXT release at a
    vblank after the callback's, and NOW needs line % <= pct, which today's scan-out answers make all but impossible -
    NATIVE_ROADMAP display.registers "Xenia's V_COUNTER": line % <= 8 once per ~3.45 h in ours, V * 100 mod 2^32 <
    5,040 in Xenia). On Xenia's side a release violation is INCONCLUSIVE (the reference does not behave as its code
    says: the instrument or the model is wrong), on ours' a FAIL.
  * THE HOLD TO ITS END (P1-2): held_to_end - the anchor (the guest's first main_hud request) + hold_end (run.json
    pacing.hold_end, HOLD_END_DEFAULT 300 s): the last present and the probe's stop within END_TOL_S (2 s) of it, the
    probe stopped by the runner's stop file (probe_meta stop), no gap above MAX_GAP_S (1 s) between presents from the
    window's start on. A capture whose presents stop before the end FAILS, however many pairs it has.
  * THE SCENE (P2-4): pause_held looks from the pause request itself (its settling lead included) to the last record.
  * THE BOOTSTRAP (P2-5): whole BLOCK_S (10 s) blocks of a capture's graded pairs are resampled (block_boot), not
    pairs: the block length from dxg1 / dxg2's six captures by archive/dxen/dxg_block_analysis.py's rule (the longest
    candidate leaving every capture >= 15 blocks; their early / late SEs rise 1.1-2.1x over the per-pair ones there).
  * THE ENVIRONMENT (P2-6): ours' run.json records its explicit child environment (canary_compare.child_env):
    env_explicit, env_extra (SR_* beyond its gates and the harness roots - none for the baked default).
  * THE NAME (P3-7): pre_calibration_verdict (was composed_verdict) is the comparison's verdict BEFORE the
    calibration gate; verdict is the final one.
VERDICT: any FAIL -> FAIL; else any INCONCLUSIVE -> INCONCLUSIVE; else PASS. CALIBRATION (--calibration): a second
Xenia capture of the same session graded against the first under the same rules; it only gates PASS (a would-be PASS
whose calibration is not a PASS is INCONCLUSIVE, pre_calibration_verdict keeping the comparison's own; a FAIL stays a
FAIL).
The output also carries each roll's identity from canary_compare's run.json (captures.<side>.run: config, exe / Xenia
sha256, the tape's sha256, held, Xenia's flags) and same_tape - information here, checked by the ledger item.
--pilot: information only - the same statistics, no verdict gating (exit 0 when both captures have a window), plus
each capture's split-half statistics and its per-10 s early-release share and mean (against the pilot's inferred
request EARLY_V = 2), the tolerances the pilot rule derives (pilot_rule(): how dxp1 fixed CADENCE_REL and found the
TVD not gradable) and the window the graded captures need (window_s_needed).
Exit 0 = PASS (or a --pilot with data), 1 = FAIL, 2 = a capture without records or without the window, 3 = INCONCLUSIVE.
"""
import argparse
import json
import math
import os
import sys
from collections import Counter

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pacing_cmp as pc  # noqa: E402  (the bootstrap, the TVD)
import pacing_probe as pp  # noqa: E402  (load(): standard library only)

PAUSE_PATH, PAUSE_NTH = "screens/misc/ScrPauseMenu", 2
WIN_LEAD_S, WIN_TAIL_S = 3.0, 1.0
MIN_PAIRS, MAX_MISSING_SHARE, GATE_TOL, MAX_UNREAD_SHARE = 3000, 0.01, 0.02, 0.01
# the dxg1 margins (one-sided, ours - Xenia), fixed from the pilot (archive/dxen/NOTES_dxg1_draft.txt)
MEAN_DIST_M, EARLY_M, LATE_M, HITCH_M = 0.05, 0.12, 0.13, 0.002
HITCH_V, MAX_ALLOW_V = 6, 2
NOMINAL_HZ = 60.0
CADENCE_REL = 0.01   # the pilot rule's result: its floor (3x dxp1's largest split-half difference is far below)
TOLERANCES_FIXED = True
# the request: 82111EC8's IMMEDIATE interval; how far the render period may sit from a whole number of vblanks
IMMEDIATE, PERIOD_TOL = 0x80000000, 0.05
KEY = 64   # a graded pair's bootstrap value: vblanks * KEY + T (T < KEY)
# round 2 (Astra round 1, archive/dxen/NOTES_dxg3_draft.txt):
# P1-1 the release: a release at its request's own vblank or at the callback (the NOW outcome, which today's scan-out
# answers make all but impossible in both programs: NATIVE_ROADMAP display.registers, "Xenia's V_COUNTER") on at most
# NOW_MAX of a capture's observed releases
NOW_MAX = 0.001
# P1-2 the hold to its scheduled end: the anchor (the main_hud request) + the hold's end (run.json pacing.hold_end,
# canary_compare.PACING_HOLD_END by default); the last present and the probe's stop within END_TOL_S of it, no gap
# between presents above MAX_GAP_S from the window's start to the last one, the probe stopped by the runner's stop file
HOLD_END_DEFAULT, END_TOL_S, MAX_GAP_S = 300.0, 2.0, 1.0
# P2-5 the bootstrap resamples whole BLOCK_S-second blocks of a capture's graded pairs (their temporal dependence kept;
# the block length chosen from dxg1 / dxg2's captures by archive/dxen/dxg_block_analysis.py's rule)
BLOCK_S = 10.0
# --pilot only (how dxp1 fixed the tolerances; information now): each tolerance = max(its floor, PILOT_X x the largest
# split-half difference of the pilot's captures); above its cap the instrument cannot grade the scene
PILOT_X, CADENCE_FLOOR, TVD_FLOOR, CADENCE_CAP, TVD_CAP = 3.0, 0.01, 0.05, 0.02, 0.10
EARLY_V = 2    # --pilot's split halves and bins: the pilot's inferred request
BIN_S = 10.0   # --pilot: the per-bin early-release share and mean
U32 = 1 << 32
GRADED = ("mean_distance_match", "early_release_match", "late_match", "hitch_rate_match")


def probe_dir(path):
    """a pacing_probe output directory, given it or a canary_compare run directory that holds one in pacing/."""
    sub = os.path.join(path, "pacing")
    return sub if os.path.isfile(os.path.join(sub, "pacing.bin")) else path


def capture(path):
    """the window of one capture: its pacing block (pacing_cmp's shape plus the cadence), or {"error": ...}."""
    d = probe_dir(path)
    if not os.path.isfile(os.path.join(d, "pacing.bin")) or not os.path.isfile(os.path.join(d, "probe_meta.json")):
        return {"error": "no pacing.bin / probe_meta.json in %s" % d}
    recs, markers, meta = pp.load(d)
    run = run_identity(os.path.dirname(os.path.abspath(d)))
    out = window(recs, markers, meta, source=d, hold_end=(run or {}).get("hold_end"))
    out["run"] = run
    return out


def run_identity(run_dir):
    """what canary_compare.py --pacing recorded about the roll (run.json beside the capture), for the ledger item's
    checks: the config, the binaries, the tape, the hold, Xenia's flags. None outside a canary_compare run."""
    p = os.path.join(run_dir, "run.json")
    if not os.path.isfile(p):
        return None
    m = json.load(open(p, encoding="utf-8"))
    # round 2 (Astra round 1 P2-6): ours' child environment, built by canary_compare (env_explicit): the SR_* it
    # carried beyond its own gates and the harness roots (env_extra - none for the baked default), the ones removed
    gates = m.get("gates") or {}
    env = m.get("env_sr") or {}
    return {"config": m.get("config"), "side": (m.get("cfg") or {}).get("side"), "exe_sha256": m.get("exe_sha256"),
            "xenia_sha256": m.get("xenia_sha256"), "tape_sha256": m.get("tape_sha256"),
            "held": (m.get("pacing") or {}).get("held", False), "hold_end": (m.get("pacing") or {}).get("hold_end"),
            "xenia_flags": " ".join(m.get("xenia_flags") or []), "scenario": m.get("scenario"),
            "env_explicit": bool(m.get("env_explicit")),
            "env_extra": sorted(k for k in env if k not in gates and k not in HARNESS_ENV),
            "env_removed": sorted(m.get("env_removed") or [])}


# the harness's own roots (tools/ally-runner): read by the roll tools, never by the game
HARNESS_ENV = ("SR_MAIN_ROOT", "SR_PYTHON", "SR_SLOT_MIN_FREE_GB")


def same_tape(caps):
    tapes = [((c.get("run") or {}).get("tape_sha256")) for c in caps]
    return bool(tapes) and None not in tapes and len(set(tapes)) == 1


# ------------------------------------------------------------------------------------------------ the request
def gate_vblanks(word, front):
    """N: the vblanks 82111EC8 puts in the gate's argument (= kernel/native_flush.cpp presentGatePayload's map), or
    (None, why) when the present has no gate (IMMEDIATE, no front buffer) or no defined one."""
    if word is None or front is None:
        return None, "no-field"
    if word == IMMEDIATE:
        return None, "immediate"
    if (front & 0xFFFFF000) == 0:
        return None, "no-front-buffer"
    if word <= 1:
        return 1, None
    if word == 2:
        return 2, None
    if word == 4:
        return 3, None
    return None, "interval-unrecognized"


def render_vblanks(period_ns):
    """R: the render loop's tick period in whole vblanks at NOMINAL_HZ, or (None, why)."""
    if period_ns is None:
        return None, "no-field"
    if period_ns <= 0:
        return None, "no-render-period"
    x = period_ns * NOMINAL_HZ / 1e9
    r = int(round(x))
    if r < 1 or abs(x - r) > PERIOD_TOL:
        return None, "render-period-not-whole"
    return r, None


def request_of(rec):
    """(N, R, pct, why) of one record; why = the first reason its request does not read (None when N and R read)."""
    n, nwhy = gate_vblanks(rec.get("interval"), rec.get("front"))
    r, rwhy = render_vblanks(rec.get("rperiod"))
    pacing = rec.get("pacing")
    return n, r, (None if pacing is None else (pacing >> 23) & 0x7F), nwhy or rwhy


def hist(xs):
    c = Counter(xs)
    return {str(k): c[k] for k in sorted(c, key=lambda x: (x is None, str(x)))}


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= (1 << 31) else x


# ------------------------------------------------------------------------------------------------ the release
def releases(rows):
    """The gate's releases, from its state at each present's entry (round 2, Astra round 1 P1-1). rows = the window's
    untorn records in present order, (V vblank count, G gate count, L the vblank of the last release, C the countdown,
    N the request at that entry). The gate (82111E38 + the vblank ISR 820FDBA0 source 0; our host gate mirrors both):
    callback g: remaining = L + N - V; > 0 -> C = remaining (WAIT); else C = 1 (NEXT: line % > pct) or, line % <= pct,
    release at once (NOW: L = V, C = remaining <= 0); each vblank decrements a positive C and releases at 0 (L = V).
    So a sample with C > 0 holds release g - 1 (gate G pending), C <= 0 release G; C < 0 = released by NOW. Callback g
    ran between the two entries where G passed g; `before` = the earlier entry's V and N. Every correct release has
    release - previous release >= N (never early) and release > before (WAIT / NEXT release at a vblank AFTER the
    callback's: only NOW releases in the callback's own vblank)."""
    rel, kind = {}, {}
    conflicts = 0
    for V, G, L, C, N in rows:
        c = s32(C)
        g = (G - 1) & 0xFFFFFFFF if c > 0 else G
        if g in rel and rel[g] != L:
            conflicts += 1
            continue
        rel[g] = L
        if c <= 0:
            kind[g] = "now" if c < 0 else kind.get(g, "vblank")
    before = {}
    for (V0, G0, _L0, _C0, N0), (_V1, G1, _L1, _C1, _N1) in zip(rows, rows[1:]):
        d = (G1 - G0) & 0xFFFFFFFF
        if 0 < d <= 4:   # the callbacks that ran between these two entries
            for j in range(1, d + 1):
                before[(G0 + j) & 0xFFFFFFFF] = (V0, N0)
    early = same = now = 0
    intervals, lags = [], []
    for g in sorted(before):
        p = (g - 1) & 0xFFFFFFFF
        V0, N0 = before[g]
        if g not in rel or p not in rel or N0 is None:
            continue
        interval = (rel[g] - rel[p]) & 0xFFFFFFFF
        lag = s32(rel[g] - V0)
        intervals.append(interval)
        lags.append(lag)
        early += interval < N0
        is_now = kind.get(g) == "now"
        now += is_now
        same += lag <= 0 or is_now
    n = len(intervals)
    return {"gates": len(before), "observed": n, "conflicts": conflicts, "early": int(early),
            "same_vblank": int(same), "now": int(now), "same_vblank_share": round(same / n, 6) if n else None,
            "interval_hist": hist(intervals), "lag_hist": hist(lags)}


# ------------------------------------------------------------------------------------------------ the window
def window(recs, markers, meta, source=None, hold_end=None):
    qpf = float(meta["qpf"])
    if len(recs) < 3:
        why = meta.get("guest_init_error") if not meta.get("guest_init") else None   # the probe's own reason
        return {"error": "fewer than 3 records" + (" (the probe's guest init never succeeded: %s)" % why if why else ""),
                "source": source}
    q = np.array([r["qpc"] for r in recs], dtype=np.int64)
    seq = np.array([r["seq"] for r in recs], dtype=np.int64)
    vbl = np.array([r["vblanks"] for r in recs], dtype=np.int64)
    gat = np.array([r["gates"] for r in recs], dtype=np.int64)
    wai = np.array([r["wait"] for r in recs], dtype=np.int64)
    dev = np.array([r["dev"] for r in recs], dtype=np.int64)
    ri = np.arange(len(recs), dtype=np.int64)   # each kept record's index into recs (its request)
    apts = sorted((int(m["qpc"]), m.get("path")) for m in markers if m.get("t") == "apt" and "qpc" in m)
    pause = [t for t, p in apts if p == PAUSE_PATH]
    out = {"source": source, "side": meta.get("side"), "records": int(q.size), "lost": meta.get("lost"),
           "pause_requests": len(pause)}
    if len(pause) < PAUSE_NTH:
        out["error"] = "the guest requested %s %d time(s), the window needs the %d%s" % (
            PAUSE_PATH, len(pause), PAUSE_NTH, {1: "st", 2: "nd", 3: "rd"}.get(PAUSE_NTH, "th"))
        return out
    q_from = pause[PAUSE_NTH - 1] + int(WIN_LEAD_S * qpf)
    q_to = int(q.max()) - int(WIN_TAIL_S * qpf)
    out["window_s"] = round((q_to - q_from) / qpf, 3)
    # round 2 (Astra round 1 P2-4): the scene from the pause request on - its settling lead included - to the last
    # record: any other screen requested after the PAUSE_NTH-th pause request leaves the pause menu
    out["other_requests_in_window"] = [p for t, p in apts if pause[PAUSE_NTH - 1] < t <= int(q.max())]
    # round 2 (P1-2): the hold to its scheduled end - the anchor (the guest's first main_hud request) + hold_end
    anchors = [t for t, p in apts if p == "screens/hud/main_hud"]
    he = float(hold_end if hold_end is not None else HOLD_END_DEFAULT)
    qs = np.sort(q)
    after = qs[qs >= q_from]
    gap = float(np.diff(after).max()) / qpf if after.size > 1 else None
    clock_end = (meta.get("clock_end") or {}).get("qpc")
    if anchors:
        sched = anchors[0] + int(he * qpf)
        out["held"] = {"hold_end_s": he, "anchor_qpc": anchors[0],
                       "last_present_to_end_s": round((sched - int(qs[-1])) / qpf, 3),
                       "stop_to_end_s": round((sched - int(clock_end)) / qpf, 3) if clock_end is not None else None,
                       "max_gap_s": round(gap, 3) if gap is not None else None, "stop": meta.get("stop")}
    else:
        out["held"] = {"hold_end_s": he, "anchor_qpc": None, "stop": meta.get("stop")}
    h = out["held"]
    h["ok"] = bool(anchors) and h.get("stop") == "stop-file" and h["last_present_to_end_s"] <= END_TOL_S and \
        h["stop_to_end_s"] is not None and h["stop_to_end_s"] <= END_TOL_S and h["max_gap_s"] is not None and \
        h["max_gap_s"] <= MAX_GAP_S
    order = np.argsort(seq, kind="stable")
    q, seq, vbl, gat, wai, dev, ri = q[order], seq[order], vbl[order], gat[order], wai[order], dev[order], ri[order]
    first = np.ones(seq.size, dtype=bool)
    first[1:] = seq[1:] != seq[:-1]
    dup_ids = int(np.unique(seq[~first]).size)
    q, seq, vbl, gat, wai, dev, ri = q[first], seq[first], vbl[first], gat[first], wai[first], dev[first], ri[first]
    inw = (q >= q_from) & (q <= q_to)
    q, seq, vbl, gat, wai, dev, ri = q[inw], seq[inw], vbl[inw], gat[inw], wai[inw], dev[inw], ri[inw]
    out["duplicate_ids"] = dup_ids
    if seq.size < 2:
        out.update({"pairs": 0, "window_expected": 0, "missing_presents": 0})
        out["error"] = "no pair inside the window"
        return out
    expected = int(seq[-1] - seq[0] + 1)
    ok = np.diff(seq) == 1
    dv = (np.diff(vbl) % U32)[ok]
    dg = (np.diff(gat) % U32)[ok]
    iv = (np.diff(q)[ok] * 1000.0 / qpf)
    span = float(q[-1] - q[0]) / qpf
    cadence = float((vbl[-1] - vbl[0]) % U32) / span if span > 0 else 0.0
    vc = {str(int(k)): int(v) for k, v in zip(*np.unique(dv, return_counts=True))}
    # the request of every record in the window, and the pairs graded against it
    req = [request_of(recs[int(i)]) for i in ri]
    g_v, g_t, g_n, g_r, g_q = [], [], [], [], []
    unread = trans = 0
    for j, k in enumerate(np.nonzero(ok)[0]):
        a, b = req[k], req[k + 1]
        if a[3] or b[3]:
            unread += 1
        elif a[:2] != b[:2]:
            trans += 1
        else:
            g_v.append(int(dv[j]))
            g_t.append(max(a[0], a[1]))
            g_n.append(a[0])
            g_r.append(a[1])
            g_q.append(int(q[k + 1]))
    fields = all(k in recs[0] for k in ("interval", "pacing", "front", "rperiod"))
    # round 2 (P1-1): the gate's releases against their requests, from the release state read at each entry
    rfields = all(k in recs[0] for k in ("last", "countdown", "torn"))
    torn = 0
    rows = []
    if rfields:
        for k, i in enumerate(ri):
            r = recs[int(i)]
            if r["torn"]:
                torn += 1
                continue
            rows.append((int(vbl[k]), int(gat[k]), r["last"], r["countdown"], req[k][0]))
    rel = releases(rows) if rows else {"gates": 0, "observed": 0, "conflicts": 0, "early": 0, "same_vblank": 0,
                                       "now": 0, "same_vblank_share": None, "interval_hist": {}, "lag_hist": {}}
    rel.update({"fields": rfields, "torn": torn, "records": int(seq.size)})
    out["release"] = rel
    out.update({
        "pairs": int(ok.sum()), "window_expected": expected, "missing_presents": int(expected - seq.size),
        "devices": [hex(int(x)) for x in np.unique(dev)],
        "cadence_hz": round(cadence, 4), "present_hz": round((seq.size - 1) / span, 4) if span > 0 else 0.0,
        "vblanks_per_present": {"mean": round(float(dv.mean()), 4) if dv.size else 0.0, "hist": vc},
        "gates_per_present": {"mean": round(float(dg.mean()), 4) if dg.size else 0.0,
                              "zero": int((dg == 0).sum()), "over_one": int((dg > 1).sum())},
        "requested": {
            "fields": fields,
            "interval_words": hist(None if recs[int(i)].get("interval") is None else hex(recs[int(i)]["interval"])
                                   for i in ri),
            "n": hist(x[0] for x in req), "r": hist(x[1] for x in req), "pct": hist(x[2] for x in req),
            "render_period_ns": hist(recs[int(i)].get("rperiod") for i in ri),
            "unreadable_why": hist(x[3] for x in req if x[3]),
            "pairs_graded": len(g_v), "pairs_unreadable": unread, "pairs_transition": trans,
            "t_pairs": hist(g_t),
            "nr": [list(x) for x in sorted(set(zip(g_n, g_r)))],   # the (N, R) the graded pairs read
        },
        "interval_ms": {"p50": round(float(np.quantile(iv, 0.5)), 3), "p99": round(float(np.quantile(iv, 0.99)), 3),
                        "max": round(float(iv.max()), 3)} if iv.size else {},
        "throttle_wait": {"p50": float(np.quantile(wai, 0.5)), "p95": float(np.quantile(wai, 0.95)),
                          "max": int(wai.max())},
        "tids": sorted(int(x) for x in np.unique([r["tid"] for r in recs])),
        "samples": {"vblanks": dv.tolist(), "gates": dg.tolist(), "wait": wai.tolist(),
                    "vblank_counts": vbl.tolist(), "qpc": q.tolist(), "pair_qpc": q[1:][ok].tolist(),
                    "graded_vblanks": g_v, "graded_requested": g_t, "graded_interval": g_n, "graded_qpc": g_q},
        "qpf": qpf,
    })
    return out


# ------------------------------------------------------------------------------------------------ the rule
def cadence_of(p):
    return p["cadence_hz"]


def graded_of(p):
    """(vblanks, T, qpc) of a capture's graded pairs."""
    s = p["samples"]
    return (np.asarray(s.get("graded_vblanks") or [], dtype=np.int64),
            np.asarray(s.get("graded_requested") or [], dtype=np.int64),
            np.asarray(s.get("graded_qpc") or [], dtype=np.int64))


def _off(vals):
    return vals // KEY - vals % KEY


# the statistics of the graded pairs, on the bootstrap's values vblanks * KEY + T; d = (resamples, values) counts,
# n = each resample's size (a block resample's size varies)
KSTATS = {
    "mean_distance_match": lambda vals, d, n: np.abs((d * _off(vals)[None, :]).sum(axis=1) / n),
    "early_release_match": lambda vals, d, n: d[:, _off(vals) < 0].sum(axis=1) / n,
    "late_match": lambda vals, d, n: d[:, _off(vals) > 0].sum(axis=1) / n,
    "hitch_rate_match": lambda vals, d, n: d[:, vals // KEY >= HITCH_V].sum(axis=1) / n,
}


def block_boot(keys, qpc, qpf, rng, stats, block_s=None):
    """Round 2 (Astra round 1 P2-5): the bootstrap of a capture's graded pairs by whole time blocks - the pairs of each
    block_s-second block (by the pair's QPC) stay together, the blocks are drawn with replacement (BOOT resamples of as
    many blocks as the capture has), so a capture's correlated bursts widen the bounds as they should. block_s 0 = the
    independent per-pair resample (pacing_cmp.boot's, every pair its own block)."""
    block_s = BLOCK_S if block_s is None else block_s
    vals, inv = np.unique(keys, return_inverse=True)
    if block_s and qpc.size == keys.size:
        blk = ((qpc - qpc.min()) / (block_s * qpf)).astype(np.int64)
    else:
        blk = np.arange(keys.size, dtype=np.int64)
    m = np.zeros((int(blk.max()) + 1, vals.size), dtype=np.float64)
    np.add.at(m, (blk, inv), 1.0)
    m = m[m.sum(axis=1) > 0]
    nb, sizes = m.shape[0], m.sum(axis=1)
    out = {k: [] for k in stats}
    for lo in range(0, pc.BOOT, pc.CHUNK):
        w = rng.multinomial(nb, np.full(nb, 1.0 / nb), size=min(pc.CHUNK, pc.BOOT - lo)).astype(np.float64)
        d, n = w @ m, w @ sizes
        for k, f in stats.items():
            out[k].append(f(vals, d, n))
    return {k: np.concatenate(v) for k, v in out.items()}, nb
MARGINS = {"mean_distance_match": MEAN_DIST_M, "early_release_match": EARLY_M, "late_match": LATE_M,
           "hitch_rate_match": HITCH_M}


def point(v, t):
    o = v - t
    return {"mean_distance_match": abs(float(o.mean())), "early_release_match": float((o < 0).mean()),
            "late_match": float((o > 0).mean()), "hitch_rate_match": float((v >= HITCH_V).mean())}


def against_interval(p):
    """information: the graded pairs against N alone (the interval without the render loop) and the mean."""
    s = p["samples"]
    v = np.asarray(s.get("graded_vblanks") or [], dtype=np.int64)
    n = np.asarray(s.get("graded_interval") or [], dtype=np.int64)
    if not v.size:
        return None
    return {"early": round(float((v < n).mean()), 6), "late": round(float((v > n).mean()), 6),
            "mean": round(float(v.mean()), 6)}


def compare(control, target, seed=pc.SEED, block_s=None):
    """Xenia (control) against ours (target), or Xenia against a second Xenia capture (the calibration)."""
    rng = np.random.default_rng(seed)
    cv = np.asarray(control["samples"]["vblanks"], dtype=np.int64)
    tv = np.asarray(target["samples"]["vblanks"], dtype=np.int64)
    cgv, cgt, cgq = graded_of(control)
    tgv, tgt, tgq = graded_of(target)
    states, stats = {}, {}

    def fixed(name, ok):
        states[name] = "PASS" if ok else "FAIL"

    # --- per capture (fixed): the hold to its end, coverage, the request read, one gate per present, the pause held,
    # the cadence; the release against the request (round 2: on Xenia's side a violation means the reference does not
    # behave as the gate's code says - the instrument or the model is wrong: INCONCLUSIVE, never a FAIL of ours) ---
    for side, p in (("xenia", control), (target.get("role", "ours"), target)):
        xen = side.startswith("xenia")
        fixed(side + "_held_to_end", bool((p.get("held") or {}).get("ok")))
        rl = p.get("release") or {}
        obs = rl.get("observed", 0)
        stats[side + "_release"] = {k: rl.get(k) for k in ("gates", "observed", "conflicts", "torn", "early",
                                                           "same_vblank", "now", "same_vblank_share")}
        fixed(side + "_release_read", bool(rl.get("fields")) and obs >= MIN_PAIRS and
              rl.get("conflicts", obs) <= MAX_UNREAD_SHARE * obs and
              rl.get("torn", 0) <= MAX_UNREAD_SHARE * max(1, rl.get("records", 0)))
        not_early = obs > 0 and rl.get("early", 1) == 0
        at_vblank = obs > 0 and rl.get("same_vblank", obs) <= NOW_MAX * obs
        states[side + "_release_not_early"] = "PASS" if not_early else "INCONCLUSIVE" if xen else "FAIL"
        states[side + "_release_at_vblank"] = "PASS" if at_vblank else "INCONCLUSIVE" if xen else "FAIL"
        expected = p.get("window_expected", 0)
        rq = p.get("requested") or {}
        pairs, graded = p.get("pairs", 0), rq.get("pairs_graded", 0)
        fixed(side + "_coverage", graded >= MIN_PAIRS and expected > 0 and
              p.get("missing_presents", expected) <= MAX_MISSING_SHARE * expected)
        fixed(side + "_requested_read", bool(rq.get("fields")) and pairs > 0 and graded > 0 and
              rq.get("pairs_unreadable", pairs) <= MAX_UNREAD_SHARE * pairs)
        fixed(side + "_no_duplicate_ids", p.get("duplicate_ids", 1) == 0)
        fixed(side + "_one_gate_per_present", abs(p["gates_per_present"]["mean"] - 1.0) <= GATE_TOL)
        fixed(side + "_pause_held", not p.get("other_requests_in_window"))
        fixed(side + "_cadence_nominal", abs(cadence_of(p) - NOMINAL_HZ) <= CADENCE_REL * NOMINAL_HZ)
    # --- across (fixed): the same request, the cadence against Xenia's, severity ---
    c_nr = (control.get("requested") or {}).get("nr") or []
    t_nr = (target.get("requested") or {}).get("nr") or []
    stats["requested_nr"] = [c_nr, t_nr]
    fixed("requested_match", bool(c_nr) and c_nr == t_nr)
    c_hz, t_hz = cadence_of(control), cadence_of(target)
    stats["cadence_hz"] = [c_hz, t_hz]
    stats["cadence_rel_diff"] = round((t_hz - c_hz) / c_hz, 6) if c_hz else None
    fixed("cadence_match", c_hz > 0 and abs(t_hz - c_hz) <= CADENCE_REL * c_hz)
    cmax, tmax = (int(cv.max()) if cv.size else 0), (int(tv.max()) if tv.size else 0)
    stats["vblanks_max"] = [cmax, tmax]
    stats["vblanks_hitches"] = [int((cv >= HITCH_V).sum()), int((tv >= HITCH_V).sum())]
    fixed("vblanks_max_match", cv.size > 0 and tv.size > 0 and tmax <= max(cmax, HITCH_V) + MAX_ALLOW_V)
    # --- statistical, one-sided: the graded pairs against T ---
    if cgv.size and tgv.size:
        ck, tk = cgv * KEY + cgt, tgv * KEY + tgt
        (bc, cnb), (bt, tnb) = (block_boot(ck, cgq, float(control["qpf"]), rng, KSTATS, block_s),
                                block_boot(tk, tgq, float(target["qpf"]), rng, KSTATS, block_s))
        stats["blocks"] = [cnb, tnb]
        pcn, ptg = point(cgv, cgt), point(tgv, tgt)
        for name, margin in MARGINS.items():
            diff = bt[name] - bc[name]
            lo, hi = float(np.quantile(diff, 1.0 - pc.CONF)), float(np.quantile(diff, pc.CONF))
            se = float(diff.std())
            states[name] = "FAIL" if lo > margin else "PASS" if hi <= margin else "INCONCLUSIVE"
            stats[name] = {"diff": round(ptg[name] - pcn[name], 6), "lower": round(lo, 6), "upper": round(hi, 6),
                           "margin": margin, "se": round(se, 6), "values": [round(pcn[name], 6), round(ptg[name], 6)],
                           "presents_needed": int(math.ceil(ck.size * (2 * pc.Z * se / margin) ** 2))}
    else:
        for name in GRADED:   # a side with no graded pair: its request did not read (requested_read FAILS too)
            states[name] = "FAIL"
            stats[name] = {"error": "no graded pair (xenia %d, %s %d)" % (cgv.size, target.get("role", "ours"),
                                                                           tgv.size)}

    vals = set(states.values())
    verdict = "FAIL" if "FAIL" in vals else "INCONCLUSIVE" if "INCONCLUSIVE" in vals else "PASS"
    needed = [s["presents_needed"] for s in stats.values() if isinstance(s, dict) and s.get("presents_needed")]
    return {"verdict": verdict, "states": states, "checks": {k: v == "PASS" for k, v in states.items()},
            "stats": stats, "presents_needed": max(needed) if needed else None,
            "pairs": [int(cv.size), int(tv.size)], "pairs_graded": [int(cgv.size), int(tgv.size)],
            "info": {"present_hz": [control.get("present_hz"), target.get("present_hz")],
                     "interval_ms": [control.get("interval_ms"), target.get("interval_ms")],
                     "throttle_wait": [control.get("throttle_wait"), target.get("throttle_wait")],
                     "vblanks_hist": [control["vblanks_per_present"]["hist"], target["vblanks_per_present"]["hist"]],
                     "vblanks_tvd": round(pc.tvd(cv, tv), 4) if cv.size and tv.size else None,
                     "requested": [control.get("requested"), target.get("requested")],
                     "against_interval": [against_interval(control), against_interval(target)],
                     "window_s": [control.get("window_s"), target.get("window_s")],
                     "held": [control.get("held"), target.get("held")],
                     "release": [control.get("release"), target.get("release")]},
            "tolerances": {"cadence_rel": CADENCE_REL, "mean_dist_m": MEAN_DIST_M, "early_m": EARLY_M,
                           "late_m": LATE_M, "hitch_m": HITCH_M, "max_unread_share": MAX_UNREAD_SHARE,
                           "now_max": NOW_MAX, "end_tol_s": END_TOL_S, "max_gap_s": MAX_GAP_S,
                           "block_s": BLOCK_S if block_s is None else block_s, "fixed": TOLERANCES_FIXED}}


def judge(control, target, calibration=None):
    out = compare(control, target)
    if calibration is not None:
        cal = compare(control, dict(calibration, role="xenia2"))
        out["calibration"] = {k: cal[k] for k in ("verdict", "states", "checks", "stats", "presents_needed")}
        if out["verdict"] == "PASS" and cal["verdict"] != "PASS":
            # round 2 (Astra round 1 P3-7): the comparison's own verdict before the calibration gate - never the final
            out["pre_calibration_verdict"] = out["verdict"]
            out["verdict"] = "INCONCLUSIVE"
    return out


# ------------------------------------------------------------------------------------------------ --pilot
def halves(p):
    """one capture's split-half statistics: its first half of pairs against its second."""
    v = np.asarray(p["samples"]["vblanks"], dtype=np.int64)
    vc = np.asarray(p["samples"]["vblank_counts"], dtype=np.int64)
    qq = np.asarray(p["samples"]["qpc"], dtype=np.int64)
    h = v.size // 2
    m = vc.size // 2
    hz = []
    for a, b in ((0, m), (m, vc.size - 1)):
        span = (qq[b] - qq[a]) / p["qpf"]
        hz.append(float((vc[b] - vc[a]) % U32) / span if span > 0 else 0.0)
    # the per-10 s early-release share and mean: whether the presents' phase against the vblank clock is locked (a
    # constant level or bursts, the run's luck) or sweeps (a steady mixture, e.g. a vblank clock slightly off 60 Hz)
    pq = np.asarray(p["samples"].get("pair_qpc") or [], dtype=np.int64)
    bins = {"early": [], "mean": []}
    if pq.size == v.size and v.size:
        idx = ((pq - pq[0]) / (BIN_S * p["qpf"])).astype(np.int64)
        for b in range(int(idx.max()) + 1):
            sel = v[idx == b]
            if sel.size >= 100:
                bins["early"].append(round(float((sel < EARLY_V).mean()), 4))
                bins["mean"].append(round(float(sel.mean()), 4))
    return {"tvd": round(pc.tvd(v[:h], v[h:]), 4), "mean_diff": round(float(v[h:].mean() - v[:h].mean()), 4),
            "early_diff": round(float((v[h:] < EARLY_V).mean() - (v[:h] < EARLY_V).mean()), 5),
            "cadence_hz": [round(hz[0], 4), round(hz[1], 4)],
            "cadence_rel_diff": round((hz[1] - hz[0]) / hz[0], 6) if hz[0] else None,
            "bins_10s": bins}


def pilot_rule(caps, verdict):
    """The tolerances the pilot fixes: max(floor, PILOT_X x the largest split-half difference over the captures),
    the TVD rounded up to 0.01, the cadence to 0.1%; above a cap the scene is not gradable with this instrument.
    And the hold the graded captures need: the statistical checks' presents_needed at the slower present rate, plus
    the window's lead and tail."""
    hs = [halves(c) for c in caps]
    tvd = math.ceil(max(TVD_FLOOR, PILOT_X * max(h["tvd"] for h in hs)) * 100 - 1e-9) / 100.0
    cad = math.ceil(max(CADENCE_FLOOR, PILOT_X * max(abs(h["cadence_rel_diff"] or 0.0) for h in hs)) * 1000 - 1e-9) / 1000.0
    rate = min(c.get("present_hz") or 0.0 for c in caps)
    need = verdict.get("presents_needed")
    return hs, {"vblanks_tvd": tvd, "cadence_rel": cad, "within_caps": tvd <= TVD_CAP and cad <= CADENCE_CAP,
                "presents_needed": need, "pairs_min": MIN_PAIRS,
                "window_s_needed": round(max(need or 0, MIN_PAIRS) / rate, 1) if rate > 0 else None,
                "rule": "max(floor, %.0fx the largest split-half difference); floors TVD %.2f, cadence %.1f%%; caps "
                        "TVD %.2f, cadence %.1f%%" % (PILOT_X, TVD_FLOOR, CADENCE_FLOOR * 100, TVD_CAP,
                                                     CADENCE_CAP * 100)}


def strip(p):
    return {k: v for k, v in p.items() if k != "samples"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("xenia")
    ap.add_argument("ours")
    ap.add_argument("--calibration", help="a second Xenia capture from the same session")
    ap.add_argument("--json")
    ap.add_argument("--pilot", action="store_true", help="information only: no verdict gating; the pilot rule")
    a = ap.parse_args(argv)
    c, t = capture(a.xenia), capture(a.ours)
    k = capture(a.calibration) if a.calibration else None
    bad = [(n, p) for n, p in (("xenia", c), ("ours", t), ("calibration", k)) if p is not None and "error" in p]
    if bad:
        out = {"verdict": "NO_DATA", "errors": {n: p["error"] for n, p in bad},
               "captures": {n: strip(p) for n, p in (("xenia", c), ("ours", t), ("calibration", k)) if p is not None}}
        rc = 2
    else:
        out = judge(c, t, k)
        out["captures"] = {n: strip(p) for n, p in (("xenia", c), ("ours", t), ("calibration", k)) if p is not None}
        # the rolls' identity (information here; the ledger item checks it): one tape on every capture
        out["same_tape"] = same_tape([p for p in (c, t, k) if p is not None])
        if a.pilot:
            hs, rule = pilot_rule([p for p in (c, t, k) if p is not None], out)
            out["pilot"] = {"information_only": True, "split_halves": hs, "proposed_tolerances": rule}
            rc = 0
        else:
            rc = {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 3}[out["verdict"]]
    print(json.dumps(out, indent=1))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
