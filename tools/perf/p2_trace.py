#!/usr/bin/env python3
"""iter-467 native-a: summarise / compare an SR_P2_TRACE file (kernel/p2_trace.cpp). numpy, streaming-free:
a whole-run trace is ~25M records (1 GB) and loads as one structured array.

    python tools/perf/p2_trace.py <trace.bin> [--presents A-B] [--ab] [--json out.json]
    python tools/perf/p2_trace.py <ring.bin> --compare <ringless.bin>

Record (40 bytes LE): qpc u64, present u32, tid u32, kind u16, aux u16, s u32, c u32, f u32, a u32, b u32.
Kinds: 1 close {f=S written, a=cursor, b=close cursor}; 2 ib {f=wptr0, aux=i, a=phys, b=dwords};
3 bs {f=wptr0, a=value, b=state}; 4/5 wait begin/end {f=F, aux=reason, a=lr | a,b=qpc delta};
6 pending {f=F, aux=result}; 7 query {f=q fence, aux=hr, a=q, b=size}; 8 patch {f=cache fence, aux=result};
9 complete {f=a=value, b=phys}; 10 progress {a=value}; 11 scratch {aux=n, a=value}; 12 isr {aux=mask, a=pc+0,
b=pc+0x10}; 13 present {a=dev+0x34D0, b=fb+0x3C}; 14/15 ring-space begin/end {a=dt at end}; 16/17 rptr-wrap
begin/end; 18 insert-fence {a=S}; 19/20 flush begin/end {a=cursor}.

Checks (the P2.7 invariants that hold on the ring path and must hold on the ringless one):
  - closes: S odd, each close's S == previous + 2 (in close order);
  - C (fb+0) monotone and always <= S (the open id);
  - every COMPLETE value is a closed S, in close order; every closed S completes;
  - IsFencePending answers: TRUE iff (S-F) < (S-C) with the S / C read right after the call ("racy" when the
    answer was TRUE and C has since reached F);
  - per present: BS values follow the Z-pass pattern, IB / close counts.
--ab compares consecutive presents' {IB dwords, BS values, closes} signatures (the frozen-pass identity for
P2-A: even presents one transport, odd the other, under the dense sim freeze); --compare does the same between
two files present by present.
"""
import argparse
import json
import sys
from collections import Counter

import os
import numpy as np

REC = np.dtype([("qpc", "<u8"), ("present", "<u4"), ("tid", "<u4"), ("kind", "<u2"), ("aux", "<u2"),
                ("s", "<u4"), ("c", "<u4"), ("f", "<u4"), ("a", "<u4"), ("b", "<u4")])
HDR = np.dtype([("magic", "<u4"), ("ver", "<u2"), ("rec", "<u2"), ("freq", "<u8"), ("t0", "<u8")])
KINDS = {1: "close", 2: "ib", 3: "bs", 4: "wait_begin", 5: "wait_end", 6: "pending", 7: "query", 8: "patch",
         9: "complete", 10: "progress", 11: "scratch", 12: "isr", 13: "present", 14: "space_begin",
         15: "space_end", 16: "wrap_begin", 17: "wrap_end", 18: "insert_fence", 19: "flush_begin",
         20: "flush_end",
         # 2026-10-10 native-a (the segmentless wedge's CP-side evidence)
         21: "regw", 22: "scratch_skip", 23: "scratch_defer", 24: "cpitem", 25: "seglookup",
         # 2026-10-10 native-a: the whole BlockUntilIdle 820FDCB8 {f = dev+0x2A74 at entry / exit, end: a,b = qpc delta}
         26: "idle_begin", 27: "idle_end",
         # 2026-10-13: SR_NATIVE_SEG_TILE's forced drained point; 2026-10-14: the pacing words at every present
         # {f = dev+0x3B14 vblank counter, a = dev+0x3B20 gate count, b = dev+0x4E4C throttle wait}
         28: "drained", 29: "pacing"}
WAIT_REASONS = {0: "hsio", 3: "swap", 4: "idle", 5: "notbusy", 6: "blockonfence"}
M32 = np.uint32(0xFFFFFFFF)


def load(path):
    hdr = np.fromfile(path, dtype=HDR, count=1)
    if hdr.size != 1 or int(hdr["magic"][0]) != 0x52543250 or int(hdr["rec"][0]) != REC.itemsize:
        raise SystemExit(f"{path}: not a P2 trace")
    # A soak's trace (4+ GB for 15 min) does not fit in RAM: above 1.5 GB the records are memory-mapped read-only
    # (the summaries only read and select; 2026-10-11, the segmentless bake soak).
    if os.path.getsize(path) > 1500 * 1024 * 1024 or os.environ.get("SR_P2_TRACE_MMAP") == "1":
        recs = np.memmap(path, dtype=REC, mode="r", offset=HDR.itemsize)
    else:
        recs = np.fromfile(path, dtype=REC, offset=HDR.itemsize)
    return int(hdr["freq"][0]), int(hdr["t0"][0]), recs


def moddiff(x):
    """(x[1:] - x[:-1]) mod 2^32 as uint32."""
    x = x.astype(np.uint32)
    return (x[1:] - x[:-1]).astype(np.uint32)


def pct(v, q):
    return float(np.percentile(v, q)) if v.size else 0.0


def summarise(freq, t0, recs, lo=None, hi=None, pacing_window=None):
    out = {"records": int(recs.size)}
    if lo is not None or hi is not None:
        m = np.ones(recs.size, dtype=bool)
        if lo is not None:
            m &= recs["present"] >= lo
        if hi is not None:
            m &= recs["present"] <= hi
        recs = recs[m]
        out["records_in_window"] = int(recs.size)
    if recs.size == 0:
        return out
    kind = recs["kind"]
    kinds, counts = np.unique(kind, return_counts=True)
    out["counts"] = {KINDS.get(int(k), str(int(k))): int(v) for k, v in zip(kinds, counts)}
    presents = np.unique(recs["present"])
    nframes = max(1, presents.size)
    out["presents"] = [int(presents[0]), int(presents[-1])]
    out["per_present"] = {KINDS.get(int(k), str(int(k))): round(float(v) / nframes, 2) for k, v in zip(kinds, counts)}
    tids, tcount = np.unique(recs["tid"], return_counts=True)
    order = np.argsort(-tcount)
    out["threads"] = {str(int(tids[i])): int(tcount[i]) for i in order[:12]}

    # --- closes: the S sequence ---
    closes = recs[kind == 1]
    cs = closes["f"]
    steps = Counter(moddiff(cs).tolist()) if cs.size > 1 else Counter()
    out["close"] = {"n": int(cs.size), "even_S": int(np.count_nonzero((cs & 1) == 0)),
                    "steps": {str(k): v for k, v in steps.most_common(4)}}

    # --- C monotone and <= S ---
    c = recs["c"].astype(np.uint32)
    s = recs["s"].astype(np.uint32)
    back = int(np.count_nonzero(moddiff(c) > 0x7FFFFFFF)) if c.size > 1 else 0
    ahead = int(np.count_nonzero(((s - c).astype(np.uint32)) > 0x7FFFFFFF))
    out["C"] = {"backwards": back, "ahead_of_S": ahead}

    # --- completes are closed S values, in order ---
    comp = recs[kind == 9]["f"]
    closed_set = np.unique(cs)
    out["complete"] = {"n": int(comp.size),
                       "not_a_closed_S": int(np.count_nonzero(~np.isin(comp, closed_set))),
                       "out_of_order": int(np.count_nonzero(moddiff(comp) > 0x7FFFFFFF)) if comp.size > 1 else 0,
                       "closes_not_completed": int(np.count_nonzero(~np.isin(closed_set, np.unique(comp))))}

    # --- pending answers vs the formula ---
    p = recs[kind == 6]
    if p.size:
        ps, pc, pf, res = (p["s"].astype(np.uint32), p["c"].astype(np.uint32), p["f"].astype(np.uint32),
                           p["aux"] != 0)
        formula = ((ps - pf).astype(np.uint32)) < ((ps - pc).astype(np.uint32))
        agree = res == formula
        racy = (~agree) & res & (((pc - pf).astype(np.uint32)) < 0x7FFFFFFF)
        out["pending"] = {"n": int(p.size), "true": int(np.count_nonzero(res)), "agree": int(np.count_nonzero(agree)),
                          "racy": int(np.count_nonzero(racy)),
                          "disagree": int(np.count_nonzero(~agree & ~racy))}
    else:
        out["pending"] = {"n": 0}

    # --- waits by reason ---
    w = recs[kind == 5]
    wsum = {}
    if w.size:
        dt = (w["a"].astype(np.uint64) | (w["b"].astype(np.uint64) << np.uint64(32))).astype(np.float64) / freq * 1e3
        for reason in np.unique(w["aux"]):
            d = dt[w["aux"] == reason]
            wsum[WAIT_REASONS.get(int(reason), str(int(reason)))] = {
                "n": int(d.size), "total_ms": round(float(d.sum()), 1), "p50_ms": round(pct(d, 50), 3),
                "p99_ms": round(pct(d, 99), 3), "max_ms": round(float(d.max()), 3)}
    out["waits"] = wsum
    for ke, name in ((15, "ring_space"), (17, "rptr_wrap")):
        e = recs[kind == ke]
        if e.size:
            d = e["a"].astype(np.float64) / freq * 1e3
            out[name] = {"n": int(d.size), "total_ms": round(float(d.sum()), 1), "p99_ms": round(pct(d, 99), 3),
                         "max_ms": round(float(d.max()), 3), "over_50us": int(np.count_nonzero(d > 0.05))}

    # --- queries / patches ---
    q = recs[kind == 7]
    qk, qc = np.unique(q["aux"], return_counts=True) if q.size else ([], [])
    out["query"] = {"n": int(q.size), "hr": {str(int(k)): int(v) for k, v in zip(qk, qc)}}
    pt = recs[kind == 8]
    out["patch"] = {"n": int(pt.size), "in_place": int(np.count_nonzero(pt["aux"])) if pt.size else 0}

    # --- ring items, BS pattern, scratch, ISR ---
    bs = recs[kind == 3]
    bk, bc = np.unique(bs["a"], return_counts=True) if bs.size else ([], [])
    out["bs"] = {"n": int(bs.size), "per_present": round(float(bs.size) / nframes, 2),
                 "values": {f"{int(k):#010x}": int(v) for k, v in sorted(zip(bk, bc), key=lambda kv: -kv[1])[:6]}}
    ib = recs[kind == 2]
    ik, ic = np.unique(ib["b"], return_counts=True) if ib.size else ([], [])
    out["ib"] = {"n": int(ib.size), "dwords_total": int(ib["b"].astype(np.uint64).sum()) if ib.size else 0,
                 "per_present": round(float(ib.size) / nframes, 2),
                 "size_hist": {str(int(k)): int(v) for k, v in sorted(zip(ik, ic), key=lambda kv: -kv[1])[:6]}}
    sc = recs[kind == 11]
    sk, scc = np.unique(sc["aux"], return_counts=True) if sc.size else ([], [])
    out["scratch"] = {"n": int(sc.size), "by_reg": {str(int(k)): int(v) for k, v in zip(sk, scc)}}
    isr = recs[kind == 12]
    mk, mc = np.unique(isr["aux"], return_counts=True) if isr.size else ([], [])
    out["isr"] = {"n": int(isr.size), "masks": {f"{int(k):#x}": int(v) for k, v in zip(mk, mc)}}

    # --- pacing ---
    pr = recs[kind == 13]
    if pr.size > 2:
        dts = np.diff(pr["qpc"].astype(np.float64)) / freq * 1e3
        out["present"] = {"n": int(pr.size), "p50_ms": round(pct(dts, 50), 3), "p99_ms": round(pct(dts, 99), 3),
                          "fps": round(float(pr.size) / ((float(pr["qpc"][-1]) - float(pr["qpc"][0])) / freq), 1)}

    # --- the pacing words (kind 29, 2026-10-14; p2.transport's `pacing` unknown): per present the vblanks elapsed
    # (dev+0x3B14 delta), the gates counted (dev+0x3B20 delta: 82111E38 runs once per gated present, so the count is
    # frames - 1 on the console path) and the throttle wait snapshot dev+0x4E4C ---
    out["pacing"] = pacing(recs, kind, pacing_window or PACING_TAIL)

    # --- BlockUntilIdle 820FDCB8 whole (kinds 26/27, 2026-10-10): every entry completes; the call minus its own
    # fence wait (reason 4) - the dev+0x2A74 drain spin the forcing lane un-hooks - never longer than a frame ---
    out["idle"] = idle_check(freq, recs)

    # --- segment order (2026-10-13, Astra's re-bake review): registry ids are assigned at the close, 1-based, in
    # order; the CP's FIRST take of every SEGMENT item (cpitem aux 3, f = id low 32) must follow that close order -
    # every segment, the deferred ones (published later by the pump: the seglookup replay hits) included - with no id
    # skipped. The later takes of an id are the guest's own bracket replays. ---
    out["segment_order"] = segment_order(recs, kind)
    return out


PACING_TAIL = 2048  # the graded window: the trace's last 2048 presents (the gameplay dwell of perf_run's scenario)


def pacing(recs, kind, tail=PACING_TAIL):
    """The pacing words between consecutive presents (kind 29 records, one per present): vblanks per present =
    dev+0x3B14 delta, gates per present = dev+0x3B20 delta, the throttle wait dev+0x4E4C as snapshotted - over the
    window of present IDS (last - tail, last] plus its first present (boot, movies and menus gate differently: the
    window is the gameplay dwell), with the whole-run vblank mean as information.
    Coverage is counted in present ids, not records (Astra round 1, 2026-10-14): `missing_presents` = the ids in the
    window with no record (100,000 ids missing between two blocks are 100,000, never one gap); a present id recorded
    more than once is `duplicate_ids` (graded apart; the first record of an id is used); deltas are taken only between
    consecutive ids, never across a missing one. The per-present samples are kept for the comparator's distribution and
    tail checks."""
    p = recs[kind == 29]
    res = {"n": int(p.size), "tail": tail}
    if p.size < 3:
        return res
    # the window's last id is the last present the trace saw (its kind-13 present records, independent of kind 29;
    # Astra round 2): pacing records that stop early leave the trailing ids missing, never an earlier healthy window
    seen = recs["present"][kind == 13]
    anchor = int(seen.max()) if seen.size else None
    order = np.argsort(p["present"], kind="stable")
    p = p[order]
    ids = p["present"].astype(np.int64)
    first = np.ones(ids.size, dtype=bool)
    first[1:] = ids[1:] != ids[:-1]
    dup_records = int(np.count_nonzero(~first))
    dup_ids = int(np.unique(ids[~first]).size) if dup_records else 0
    p, ids = p[first], ids[first]
    whole = moddiff(p["f"])[np.diff(ids) == 1].astype(np.int64)
    res["whole_run_vblanks_mean"] = round(float(whole.mean()), 4) if whole.size else 0.0
    last = max(int(ids[-1]), anchor if anchor is not None else int(ids[-1]))
    res["window_anchor"] = "present records" if anchor is not None and anchor >= int(ids[-1]) else "pacing records"
    lo = max(last - tail, int(ids[0]))  # the window's first id (a trace shorter than the window starts at its first)
    inw = ids >= lo
    p, ids = p[inw], ids[inw]
    expected = last - lo + 1
    if ids.size < 2:  # no pacing pair inside the window at all (e.g. the records stopped long before the last present)
        res.update({"pairs": 0, "window_ids": [lo, last], "window_expected": expected,
                    "missing_presents": int(expected - ids.size), "present_gaps": 0, "duplicate_ids": dup_ids,
                    "duplicate_records": dup_records})
        return res
    step = np.diff(ids)
    ok = step == 1
    vbl = moddiff(p["f"])[ok].astype(np.int64)
    gates = moddiff(p["a"])[ok].astype(np.int64)
    wait = p["b"].astype(np.int64)
    vc = Counter(vbl.tolist())
    res.update({
        "pairs": int(ok.sum()), "window_ids": [lo, last], "window_expected": expected,
        "missing_presents": int(expected - ids.size), "present_gaps": int(np.count_nonzero(~ok)),
        "duplicate_ids": dup_ids, "duplicate_records": dup_records,
        "vblanks_per_present": {"mean": round(float(vbl.mean()), 4) if vbl.size else 0.0, "p50": pct(vbl, 50),
                                "p99": pct(vbl, 99), "hist": {str(k): int(v) for k, v in sorted(vc.items())}},
        "gates_per_present": {"mean": round(float(gates.mean()), 4) if gates.size else 0.0,
                              "zero": int(np.count_nonzero(gates == 0)), "over_one": int(np.count_nonzero(gates > 1))},
        "gate_count": {"first": int(p["a"][0]), "last": int(p["a"][-1]), "presents": int(ids[-1] - ids[0])},
        "throttle_wait": {"p50": pct(wait, 50), "p95": pct(wait, 95), "p99": pct(wait, 99), "max": int(wait.max())},
        # the samples the comparator grades (vblanks / gates per consecutive pair, the wait per present)
        "samples": {"vblanks": vbl.tolist(), "gates": gates.tolist(), "wait": wait.tolist()},
    })
    return res


def segment_order(recs, kind):
    items = recs[(kind == 24) & (recs["aux"] == 3)]
    # p2.transport BRACKET (SR_NATIVE_BRACKET): a Z-pass bracket's bound segments are applied inside their
    # BRACKET_END item (kind 24 aux 4, once per pass), after the coherency / BRACKET_END segments that carry later ids.
    # They are their own class: first-take order is checked within it, and an id the aux-3 stream skips is not
    # skipped when a BRACKET_END applied it.
    nested = recs[(kind == 24) & (recs["aux"] == 4)]
    res = {"takes": int(items.size), "segments": 0, "replays": 0, "late_first": 0, "skipped": 0,
           "close_published": 0, "pump_published": 0, "bracket_takes": int(nested.size), "bracket_segments": 0,
           "bracket_late_first": 0, "bracket_binds": int(np.count_nonzero((kind == 24) & (recs["aux"] == 5))),
           "tail_bound_unapplied": 0, "tail_bound_ids": [], "tail_bound_gen": None, "tail_bound_rule": "no takes",
           "brackets_open": -1, "bound_unapplied": -1, "published_untaken": -1, "brackets_cut": 0}
    if nested.size:
        nid = nested["f"].astype(np.int64)
        _, nfirst = np.unique(nid, return_index=True)
        nseq = nid[np.sort(nfirst)]
        res["bracket_segments"] = int(nseq.size)
        res["bracket_late_first"] = int(np.count_nonzero(nseq[1:] < np.maximum.accumulate(nseq)[:-1]))
    if not items.size:
        return res
    ids = items["f"].astype(np.int64)
    _, first_idx = np.unique(ids, return_index=True)
    seq = ids[np.sort(first_idx)]
    runmax = np.maximum.accumulate(seq)
    res["segments"] = int(seq.size)
    res["replays"] = int(items.size - seq.size)
    res["late_first"] = int(np.count_nonzero(seq[1:] < runmax[:-1]))
    inorder = seq[runmax == seq]
    gaps = np.diff(inorder)
    res["skipped"] = int(np.sum(gaps[gaps > 1] - 1))
    if nested.size:  # every id strictly inside [first, last] and not in-order lies in a gap: the bound ones are not skips
        bound = np.unique(nested["f"].astype(np.int64))
        inside = bound[(bound > inorder[0]) & (bound < inorder[-1])]
        res["skipped"] -= int(np.setdiff1d(inside, inorder).size)
    tail = tail_bound_unapplied(recs, kind, nested, inorder, seq)
    res.update(tail)
    res["skipped"] -= tail["tail_bound_unapplied"]
    look = recs[(kind == 25)]
    hand = np.unique(look["f"][look["aux"] == 1].astype(np.int64))
    hit = np.unique(look["f"][look["aux"] == 2].astype(np.int64))
    res["close_published"] = int(np.isin(seq, hand).sum())
    res["pump_published"] = int(np.isin(seq, hit).sum())
    # The terminal reconciliation (2026-10-13, Astra: `skipped` sees interior gaps only): at the LAST drained point
    # SR_NATIVE_SEG_TILE forced (kind 28, aux 1 = registered == first-applied), every segment published before it
    # (kind 25 aux 1 / 2) was first taken before it. drain_at_end: that point lies within 1200 presents (~20 s) of the
    # trace's last present, so it covers the run's gameplay, not only its loading.
    dr = recs[kind == 28]
    res["forced_drains"] = int(dr.size)
    res["forced_miss"] = int(np.count_nonzero(dr["aux"] != 1))
    res["terminal_unapplied"], res["last_drain_present"], res["drain_at_end"] = -1, -1, False
    res["max_present"] = int(recs["present"].max()) if recs.size else 0
    if dr.size:
        last = dr[np.argmax(dr["qpc"])]
        q = int(last["qpc"])
        pubm = ((look["aux"] == 1) | (look["aux"] == 2)) & (look["qpc"] <= q)
        published = np.unique(look["f"][pubm].astype(np.int64))
        taken = np.unique(items["f"][items["qpc"] <= q].astype(np.int64))
        res["terminal_unapplied"] = int(np.setdiff1d(published, taken).size)
        res["last_drain_present"] = int(last["present"])
        res["drain_at_end"] = bool(int(last["aux"]) == 1 and res["max_present"] - res["last_drain_present"] <= 1200)
    res.update(bracket_terminal(recs, kind, items, nested, look))
    return res


def tail_bound_unapplied(recs, kind, nested, inorder, taken):
    """The final in-flight bracket only (the tcmp1 diagnosis, 2026-10-14; Astra round 1). A run that ends (a forced
    kill) while the CP replays its last BRACKET_END leaves that bracket's later bound segments unapplied: ids no aux-3
    take and no aux-4 apply covers, inside the in-order range, which `skipped` counts. They are the unobserved tail, not
    skips, only when the trace PROVES that execution unfinished and in order up to the cut - otherwise they stay
    `skipped` (fail-closed; a trace without these records, from an exe before them, keeps every gap a skip):
      - the CP's lifecycle (kind 24 aux 6 begin / aux 7 completion {f = segment id, a = steps, b = generation}): the
        LAST begin's generation g has no completion after it;
      - g's plan (kind 24 aux 8 a bin select / aux 9 an application {f = step index, a = value / id, b = g}, traced at
        the publish) is whole: the steps 0 .. a-1 of that begin, each once;
      - after that begin the CP did nothing but g's steps - kind 3 aux 1 bin selects of generation g and kind 24 aux 4
        applications: no take, no other generation's step, no completion - and they are EXACTLY the plan's first k
        steps, k < a. A step left out, reordered or repeated makes them a skip, so a bracket that went on (a later bin
        select) after omitting applications never reads as cut;
      - g's bound set B is its binding records (aux 5).
    The ids counted are B's ids the final execution never applied that `skipped` counted (inside the in-order range, not
    taken by any aux-3 record: `taken`). Every other gap remains a skip."""
    out = {"tail_bound_unapplied": 0, "tail_bound_ids": [], "tail_bound_gen": None}
    aux = recs["aux"]
    if not np.count_nonzero((kind == 24) & (aux == 5)):
        return dict(out, tail_bound_rule="no binding records")
    begins = np.flatnonzero((kind == 24) & (aux == 6))
    if not begins.size or not np.count_nonzero((kind == 24) & ((aux == 8) | (aux == 9))):
        return dict(out, tail_bound_rule="no CP lifecycle / plan records")
    q = recs["qpc"].astype(np.int64)
    idx = np.arange(recs.size)
    last = begins[np.lexsort((begins, q[begins]))[-1]]
    gen, steps = int(recs["b"][last]), int(recs["a"][last])
    out["tail_bound_gen"] = gen
    after = (q > q[last]) | ((q == q[last]) & (idx > last))
    if np.count_nonzero(after & (kind == 24) & (aux == 7) & (recs["b"] == gen)):
        return dict(out, tail_bound_rule="the final bracket completed")
    cp = after & (((kind == 24) & np.isin(aux, (1, 3, 4, 6, 7))) | ((kind == 3) & (aux == 1)))
    sel = np.flatnonzero(cp)
    sel = sel[np.lexsort((sel, q[sel]))]
    done = []
    for r in recs[sel]:
        if int(r["kind"]) == 3 and int(r["b"]) == gen:
            done.append((8, int(r["a"])))
        elif int(r["kind"]) == 24 and int(r["aux"]) == 4:
            done.append((9, int(r["f"])))
        else:
            return dict(out, tail_bound_rule="the CP did other work after the final bracket began")
    pl = recs[(kind == 24) & ((aux == 8) | (aux == 9)) & (recs["b"] == gen)]
    order = pl["f"].astype(np.int64)
    if pl.size != steps or not np.array_equal(np.sort(order), np.arange(steps)):
        return dict(out, tail_bound_rule="the final bracket's plan is not whole")
    pl = pl[np.argsort(order, kind="stable")]
    plan = [(int(r["aux"]), int(r["a"])) for r in pl]
    if len(done) >= steps or done != plan[:len(done)]:
        return dict(out, tail_bound_rule="the final bracket's steps are not its plan's prefix")
    binds = recs[(kind == 24) & (aux == 5) & (recs["b"] == gen)]
    if not binds.size:
        return dict(out, tail_bound_rule="no binding record for the final bracket")
    bset = np.unique(np.concatenate([np.arange(int(r["f"]), int(r["f"]) + int(r["a"]), dtype=np.int64)
                                     for r in binds]))
    applied = np.unique(np.array([v for k, v in done if k == 9], dtype=np.int64))
    missing = np.setdiff1d(bset, applied)
    missing = missing[(missing > inorder[0]) & (missing < inorder[-1])]
    missing = np.setdiff1d(missing, taken)
    out.update(tail_bound_unapplied=int(missing.size), tail_bound_ids=[int(i) for i in missing[:64]],
               tail_bound_rule="the final in-flight bracket")
    return out


def bracket_terminal(recs, kind, items, nested, look):
    """The whole-trace reconciliation a run ended by the graceful quit (tools/graceful-quit: ring cut, CP drained) must
    meet (Astra round 1 on the transport-fix lane): brackets_open = generations the CP began (aux 6) without a completion
    (aux 7) after it; bound_unapplied = bound ids (the binding records, aux 5) no BRACKET_END applied (aux 4), less the
    generations whose END the ring cut dropped (aux 10, brackets_cut: never published, so never applied - tfx2);
    published_untaken = segments published (kind 25 aux 1 / 2) the CP never took (aux 3). -1 each when the trace has no
    such records (an exe before them)."""
    res = {"brackets_open": -1, "bound_unapplied": -1, "published_untaken": -1, "brackets_cut": 0}
    aux, q = recs["aux"], recs["qpc"].astype(np.int64)
    life = np.flatnonzero((kind == 24) & ((aux == 6) | (aux == 7)))
    if life.size:
        open_ = {}
        for i in life[np.lexsort((life, q[life]))]:
            g = int(recs["b"][i])
            if int(aux[i]) == 6:
                open_[g] = open_.get(g, 0) + 1
            elif open_.get(g, 0):
                open_[g] -= 1
        res["brackets_open"] = int(sum(1 for v in open_.values() if v))
    cut = np.unique(recs["b"][(kind == 24) & (aux == 10)].astype(np.int64))
    res["brackets_cut"] = int(cut.size)
    binds = recs[(kind == 24) & (aux == 5)]
    binds = binds[~np.isin(binds["b"].astype(np.int64), cut)]
    if binds.size:
        bset = np.unique(np.concatenate([np.arange(int(r["f"]), int(r["f"]) + int(r["a"]), dtype=np.int64)
                                         for r in binds]))
        res["bound_unapplied"] = int(np.setdiff1d(bset, np.unique(nested["f"].astype(np.int64))).size)
    elif cut.size:
        res["bound_unapplied"] = 0
    pub = look[(look["aux"] == 1) | (look["aux"] == 2)]
    if pub.size:
        res["published_untaken"] = int(np.setdiff1d(np.unique(pub["f"].astype(np.int64)),
                                                    np.unique(items["f"].astype(np.int64))).size)
    return res


def idle_check(freq, recs, frame_ms=1000.0 / 60.0):
    """Per thread, in record order: kinds 26 (begin) / 27 (end, a|b<<32 = QPC duration) and the reason-4 fence waits
    (kind 5) inside them. -> entries, completed, open_at_end, max_ms, spin_max_ms, spin_over_frame, a74_nonzero_exit."""
    kind = recs["kind"]
    m = (kind == 26) | (kind == 27) | ((kind == 5) & (recs["aux"] == 4))
    sel = recs[m]
    res = {"entries": 0, "completed": 0, "open_at_end": 0, "max_ms": 0.0, "spin_max_ms": 0.0, "spin_over_frame": 0,
           "a74_nonzero_exit": 0}
    open_, fence = {}, {}
    for r in sel:
        k, tid = int(r["kind"]), int(r["tid"])
        ms = (int(r["a"]) | (int(r["b"]) << 32)) * 1000.0 / freq
        if k == 26:
            res["entries"] += 1
            open_[tid] = True
            fence[tid] = 0.0
        elif k == 5:
            if tid in open_:
                fence[tid] += ms
        else:
            res["completed"] += 1
            open_.pop(tid, None)
            spin = ms - fence.pop(tid, 0.0)
            res["max_ms"] = max(res["max_ms"], round(ms, 3))
            res["spin_max_ms"] = max(res["spin_max_ms"], round(spin, 3))
            res["spin_over_frame"] += int(spin > frame_ms)
            res["a74_nonzero_exit"] += int(int(r["f"]) != 0)
    res["open_at_end"] = len(open_)
    return res


def per_present_signature(recs, lo=None, hi=None):
    """present -> (tuple of IB dwords, tuple of BS values, closes), in record order."""
    m = np.isin(recs["kind"], (1, 2, 3))
    if lo is not None:
        m &= recs["present"] >= lo
    if hi is not None:
        m &= recs["present"] <= hi
    sub = recs[m]
    if sub.size == 0:
        return {}
    order = np.argsort(sub["present"], kind="stable")
    sub = sub[order]
    bounds = np.flatnonzero(np.diff(sub["present"])) + 1
    sig = {}
    for chunk in np.split(sub, bounds):
        k = chunk["kind"]
        sig[int(chunk["present"][0])] = (tuple(chunk["b"][k == 2].tolist()), tuple(chunk["a"][k == 3].tolist()),
                                         int(np.count_nonzero(k == 1)))
    return sig


def ab(recs, lo, hi):
    sig = per_present_signature(recs, lo, hi)
    presents = sorted(sig)
    same = diff = 0
    examples = []
    for p, q in zip(presents, presents[1:]):
        if q != p + 1:
            continue
        if sig[p] == sig[q]:
            same += 1
        else:
            diff += 1
            if len(examples) < 5:
                examples.append({"presents": [p, q], "ib": [len(sig[p][0]), len(sig[q][0])],
                                 "ib_dwords": [sum(sig[p][0]), sum(sig[q][0])],
                                 "bs": [[hex(v) for v in sig[p][1]], [hex(v) for v in sig[q][1]]],
                                 "closes": [sig[p][2], sig[q][2]]})
    return {"pairs_identical": same, "pairs_different": diff, "examples": examples}


def compare(recs_a, recs_b, lo, hi):
    sa, sb = per_present_signature(recs_a, lo, hi), per_present_signature(recs_b, lo, hi)
    common = sorted(set(sa) & set(sb))
    same = sum(1 for p in common if sa[p] == sb[p])
    return {"presents_common": len(common), "identical": same, "different": len(common) - same}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--presents", help="A-B present window")
    ap.add_argument("--ab", action="store_true", help="consecutive-present signature identity")
    ap.add_argument("--compare", help="second trace: per-present signature identity")
    ap.add_argument("--json", help="write the summary here")
    ap.add_argument("--pacing-window", type=int, default=PACING_TAIL,
                    help="the pacing block's window in present ids (default %d; transport_composed sizes its own)"
                         % PACING_TAIL)
    args = ap.parse_args()
    lo = hi = None
    if args.presents:
        a, _, b = args.presents.partition("-")
        lo, hi = (int(a) if a else None), (int(b) if b else None)
    freq, t0, recs = load(args.trace)
    out = summarise(freq, t0, recs, lo, hi, args.pacing_window)
    if args.ab:
        out["ab"] = ab(recs, lo, hi)
    if args.compare:
        _, _, recs_b = load(args.compare)
        out["compare"] = compare(recs, recs_b, lo, hi)
    # the console copy leaves out the pacing samples (thousands of numbers); the --json file keeps them
    shown = dict(out, pacing={k: v for k, v in out["pacing"].items() if k != "samples"}) if "pacing" in out else out
    print(json.dumps(shown, indent=1))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
