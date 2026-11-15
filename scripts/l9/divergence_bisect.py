#!/usr/bin/env python3
"""
L9-C -- divergence bisector (docs/L9_DIFFERENTIAL_ORACLE.md SecC).

Diffs a *golden* guest-execution trace (instrumented Xenia, a working boot) against *ours*
(our recomp under the record-replay scheduler), and reports the FIRST guest-execution
divergence: "both builds ran the same functions up to seq N, then ours did something golden
never does next." That turns "the menu is blank" (unanswerable) into "function X, after this
exact predecessor" -- a crash-class signal with an address, on non-crashing divergence.

Trace format: scripts/l9/trace_format.md  (`<seq> <pc> <r3> <r1> <lr>` per line, or binary).

GRANULARITY-ROBUST BY DEFAULT (this is the v2 core):
  The two oracles record at DIFFERENT granularities. Xenia (A/golden) records ALL guest
  function entries. Our recomp (B/ours) records only indirect/computed/vtable call targets --
  a SUBSET. So strict record-for-record lockstep false-diverges at the very first record where
  the granularities differ. Instead we align by SUBSEQUENCE: if B is a faithful subset of A's
  control flow, B's pc-sequence must appear as a subsequence of A's pc-sequence up to the real
  divergence. The first ours[j] that golden never produces next (within a bounded look-ahead
  window) IS the divergence. Use --strict for old index-lockstep when granularities are known
  to match (e.g. both are full per-function-entry traces, or matched fixtures).

  python scripts/l9/divergence_bisect.py --golden xenia_boot.trace --ours sr_boot.trace
  python scripts/l9/divergence_bisect.py ... --window 4000   # look-ahead bound (loops/cost)
  python scripts/l9/divergence_bisect.py ... --stream        # O(window) RAM; for the 11 GB golden
  python scripts/l9/divergence_bisect.py ... --strict        # old index-lockstep
  python scripts/l9/divergence_bisect.py ... --context 8     # frames of context each side
  python scripts/l9/divergence_bisect.py ... --resolve       # map VAs via va_to_source index

Exit 0 = traces agree (ours is a faithful subsequence of golden); 2 = divergence found; 1 = error.
"""
import argparse
import sqlite3
import struct
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
VA_INDEX_DB = ROOT / ".cache" / "va_index.db"   # built by scripts/va_to_source.py --build

# Canonical binary record (tools/xenia-trace/xenia_trace.h): little-endian
#   guest_pc u32, lr u32, r3 u64, r1 u64, thread_id u32, tag u32  (32 bytes, header "XENTRC01").
_BIN_MAGIC = b"XENTRC01"
_BIN_REC = struct.Struct("<IIQQII")


def parse_trace(path):
    """Parse either the canonical BINARY format (Xenia golden + recomp SR_GUEST_TRACE) or the
    legacy TEXT format. Returns a list of (seq, pc, r3, r1, lr) tuples (seq = record index)."""
    with open(path, "rb") as f:
        head = f.read(8)
        if head == _BIN_MAGIC:
            f.seek(32)  # skip 32-byte header
            recs = []
            i = 0
            while True:
                buf = f.read(32)
                if len(buf) < 32:
                    break
                guest_pc, lr, r3, r1, _tid, _tag = _BIN_REC.unpack(buf)
                recs.append((i, guest_pc, r3, r1, lr))
                i += 1
            return recs
    # text fallback: "<seq> <pc> <r3> <r1> <lr>" per line
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                seq = int(parts[0]); pc = int(parts[1], 16)
            except ValueError:
                continue

            def reg(i):
                if i < len(parts) and parts[i] != "-":
                    try:
                        return int(parts[i], 16)
                    except ValueError:
                        return None
                return None
            recs.append((seq, pc, reg(2), reg(3), reg(4)))
    return recs


def resolve(va, cur):
    """Nearest function start <= va from the va_to_source index -> 'sub_XXXX+off' or '0x..'."""
    if cur is None:
        return f"0x{va:08x}"
    row = cur.execute(
        "SELECT va, name FROM funcs WHERE va <= ? ORDER BY va DESC LIMIT 1", (va,)
    ).fetchone()
    if not row:
        return f"0x{va:08x}"
    base, name = row
    off = va - base
    return f"{name}+0x{off:x}" if off else name


def _h(v):
    return "-" if v is None else f"0x{v:08x}"


def diff_strict(g, o):
    """Old index-lockstep: first i where g[i].pc != o[i].pc. Assumes MATCHED granularity.

    Returns dict describing the result:
      {"kind": "agree"}                       -- identical pcs for min(len) and equal length
      {"kind": "tail", "who":..,"i":..}       -- identical prefix, one ran longer
      {"kind": "diverge", "gi":i, "oi":i}     -- pc mismatch at index i (same i on both sides)
    """
    n = min(len(g), len(o))
    for i in range(n):
        if g[i][1] != o[i][1]:
            return {"kind": "diverge", "gi": i, "oi": i}
    if len(g) == len(o):
        return {"kind": "agree", "n": n}
    who = "golden" if len(g) > len(o) else "ours"
    return {"kind": "tail", "who": who, "i": n}


def diff_subsequence(g, o, window):
    """Granularity-robust alignment. Treats `o` (ours) as a candidate SUBSEQUENCE of `g`
    (golden): for each ours[j], scan forward in golden from the current position `i` (bounded
    by `window`) for a matching pc. If found, sync past it (advance both). If NOT found within
    the window, ours[j] is a pc golden never produces next from here == the divergence.

    Returns dict:
      {"kind": "agree", "matched": M}                  -- every ours record matched in order
      {"kind": "diverge", "oi": j, "gi": i, "synced": [golden-indices...]}
          oi      -- index in `o` (ours) that failed to match -> the frontier
          gi      -- golden index where the search started (last sync point in golden)
          synced  -- list of golden indices that ours matched, in order (the shared spine)

    HEURISTIC LIMITS (honest): this is greedy. On traces with heavy pc repetition (tight loops,
    a hot dispatcher revisited thousands of times) a greedy first-match can sync to the WRONG
    occurrence and then drift. `window` bounds both the cost and the over-advance: too small and
    a legitimately-distant-but-present pc looks like a divergence; too large and a loop lets a
    stale pc match far ahead. There is no backtracking. This LOCALIZES a likely divergence; it
    is not a proof of minimal edit distance. When granularities are known-matched, use --strict.
    """
    i = 0          # pointer into golden
    j = 0          # pointer into ours
    ng = len(g)
    synced = []    # golden indices we matched, in order (shared spine for context output)
    while j < len(o):
        pc = o[j][1]
        # bounded look-ahead from current golden position
        hi = min(ng, i + window)
        found = -1
        k = i
        while k < hi:
            if g[k][1] == pc:
                found = k
                break
            k += 1
        if found < 0:
            return {"kind": "diverge", "oi": j, "gi": i, "synced": synced}
        synced.append(found)
        i = found + 1
        j += 1
    return {"kind": "agree", "matched": len(synced)}


def report_diverge_strict(g, o, gi, oi, cur, context):
    print(f"\n[bisect] DIVERGENCE (strict index-lockstep) at index {gi}:")
    print(f"  golden -> {resolve(g[gi][1], cur)}   (r3={_h(g[gi][2])} r1={_h(g[gi][3])} lr={_h(g[gi][4])})")
    print(f"  ours   -> {resolve(o[oi][1], cur)}   (r3={_h(o[oi][2])} r1={_h(o[oi][3])} lr={_h(o[oi][4])})")
    lo = max(0, gi - context)
    print(f"\n  shared prefix (last {gi - lo} agreeing frames):")
    for i in range(lo, gi):
        print(f"    {g[i][0]:>8}  {resolve(g[i][1], cur)}")
    print("\n  --- divergence ---")
    print("  golden continues:")
    for i in range(gi, min(gi + context, len(g))):
        print(f"    G {g[i][0]:>6}  {resolve(g[i][1], cur)}")
    print("  ours continues:")
    for i in range(oi, min(oi + context, len(o))):
        print(f"    O {o[i][0]:>6}  {resolve(o[i][1], cur)}")
    print("\n[bisect] feed the diverging VA(s) to Ghidra / the L4 call tracer for the surrounding state.")


def report_diverge_subseq(g, o, res, cur, context):
    oi = res["oi"]      # ours frontier
    gi = res["gi"]      # golden search-start (after last sync)
    synced = res["synced"]
    front_pc = o[oi][1]
    print(f"\n[bisect] DIVERGENCE (subsequence align) at ours record {oi}:")
    print(f"  ours entered {resolve(front_pc, cur)}, which golden never enters within the next "
          f"{context}+ records from the last synced point -- ours took a path golden does not.")
    print(f"  ours   -> {resolve(front_pc, cur)}   (r3={_h(o[oi][2])} r1={_h(o[oi][3])} lr={_h(o[oi][4])})")

    # shared context: the last K golden records that ours matched (the shared spine)
    tail = synced[-context:] if synced else []
    print(f"\n  shared spine (last {len(tail)} synced frames, golden indices):")
    for gidx in tail:
        print(f"    {g[gidx][0]:>8}  {resolve(g[gidx][1], cur)}")

    print("\n  --- divergence ---")
    print("  golden continues (from last sync point):")
    for i in range(gi, min(gi + context, len(g))):
        print(f"    G {g[i][0]:>6}  {resolve(g[i][1], cur)}")
    print("  ours continues (from the frontier):")
    for i in range(oi, min(oi + context, len(o))):
        print(f"    O {o[i][0]:>6}  {resolve(o[i][1], cur)}")
    print("\n[bisect] NOTE: subsequence align is a greedy best-effort localizer, not a proof. On")
    print("[bisect] loop-heavy traces a greedy match can mis-sync; widen/narrow --window or use")
    print("[bisect] --strict when granularities are known-matched. Cross-check the frontier VA.")
    print("[bisect] feed the diverging VA to Ghidra / the L4 call tracer for the surrounding state.")


# ---------------------------------------------------------------------------
# Streaming mode (--stream): O(window) memory, for traces too large to hold in RAM
# (the rich L9-A JIT-prolog golden boot is ~11 GB / 349M records). Records are read
# in 1 MB chunks and code-range-filtered inline; the subsequence matcher keeps only a
# bounded golden look-ahead buffer + a short synced spine, never the whole trace.
# ---------------------------------------------------------------------------

def iter_trace(path, code_min, code_max, thread=None):
    """Stream (raw_idx, pc, r3, r1, lr) records, filtered to [code_min, code_max) and (if `thread`
    is not None) to that thread_id. O(1) memory. Auto-detects binary (XENTRC01) vs text.
    Thread filtering is how you compare main-boot-thread to main-boot-thread: the two oracles
    number threads differently (e.g. Xenia boot=thread 6, our recomp boot=thread 1) and interleave
    workers differently, so an unfiltered diff trips on scheduling noise, not real divergence."""
    with open(path, "rb") as f:
        if f.read(8) == _BIN_MAGIC:
            f.seek(32)                       # past the 32-byte header
            idx, carry = 0, b""
            while True:
                chunk = f.read(1 << 20)      # 1 MB
                if not chunk:
                    break
                data = carry + chunk
                n = len(data) // 32
                for k in range(n):
                    guest_pc, lr, r3, r1, tid, _tag = _BIN_REC.unpack_from(data, k * 32)
                    if (thread is None or tid == thread) and code_min <= guest_pc < code_max:
                        yield (idx, guest_pc, r3, r1, lr)
                    idx += 1
                carry = data[n * 32:]
            return
    # text fallback (re-open in text mode)
    with open(path, encoding="utf-8", errors="replace") as f:
        idx = 0
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                pc = int(parts[1], 16)
            except ValueError:
                continue

            def reg(i):
                if i < len(parts) and parts[i] != "-":
                    try:
                        return int(parts[i], 16)
                    except ValueError:
                        return None
                return None
            if code_min <= pc < code_max:
                yield (idx, pc, reg(2), reg(3), reg(4))
            idx += 1


def _take(it, first, n):
    """Return up to n records starting with `first` then pulling from iterator `it`."""
    out = [first] if first is not None else []
    for _ in range(n - len(out)):
        try:
            out.append(next(it))
        except StopIteration:
            break
    return out


def diff_subsequence_stream(giter, oiter, window, context):
    """Streaming granularity-robust subsequence alignment (see diff_subsequence). Holds only a
    bounded golden look-ahead deque + a context-sized synced spine -> O(window) memory."""
    gbuf = deque()
    spine = deque(maxlen=context)
    matched = 0

    def refill():
        while len(gbuf) < window:
            try:
                gbuf.append(next(giter))
            except StopIteration:
                break

    for orec in oiter:
        refill()
        found = -1
        for k, grec in enumerate(gbuf):
            if grec[1] == orec[1]:
                found = k
                break
        if found < 0:
            golden_next = list(gbuf)[:context]
            return {"kind": "diverge", "frontier": orec, "spine": list(spine),
                    "golden_next": golden_next, "ours_next": _take(oiter, orec, context),
                    "matched": matched}
        last = None
        for _ in range(found + 1):
            last = gbuf.popleft()
        spine.append(last)                   # the matched golden record
        matched += 1
    return {"kind": "agree", "matched": matched}


def diff_strict_stream(giter, oiter, context):
    """Streaming strict index-lockstep (see diff_strict). O(context) memory."""
    spine = deque(maxlen=context)
    n = 0
    for orec in oiter:
        try:
            grec = next(giter)
        except StopIteration:
            return {"kind": "tail", "who": "ours", "frontier": orec, "spine": list(spine), "i": n}
        if grec[1] != orec[1]:
            return {"kind": "diverge", "frontier": orec, "golden_at": grec, "spine": list(spine),
                    "golden_next": _take(giter, grec, context),
                    "ours_next": _take(oiter, orec, context), "i": n}
        spine.append(grec)
        n += 1
    try:
        grec = next(giter)
        return {"kind": "tail", "who": "golden", "frontier": grec, "spine": list(spine), "i": n}
    except StopIteration:
        return {"kind": "agree", "n": n}


def diff_resync_stream(giter, oiter, window):
    """Resync-aware diff. The two boots share most of their flow but with BUBBLES (one side does
    a few records the other doesn't, then they rejoin). diff_subsequence_stream stops at the first
    bubble; this one RESYNCS past each bubble and keeps going, so it finds where the streams
    PERMANENTLY diverge -- for the menu case, where ours stops following golden for good (Xenia
    keeps building the menu, ours doesn't). O(window) memory.

    On mismatch it finds the nearest re-alignment: the (a,b) minimizing a+b with ours[a].pc ==
    golden[b].pc inside the buffered window, records the bubble (golden-only b records, ours-only a
    records), skips both, continues. If no common pc exists in the whole window -> PERMANENT
    divergence (widen --window if you suspect a big legit bubble)."""
    gbuf, obuf = deque(), deque()
    spine = deque(maxlen=16)
    bubbles = []                # (golden_only, ours_only) per bubble
    synced = 0

    def fill(buf, it):
        while len(buf) < window:
            try:
                buf.append(next(it))
            except StopIteration:
                break

    while True:
        fill(gbuf, giter)
        fill(obuf, oiter)
        if not gbuf and not obuf:
            return {"kind": "agree_end", "synced": synced, "bubbles": bubbles}
        if gbuf and obuf and gbuf[0][1] == obuf[0][1]:
            rec = gbuf.popleft(); obuf.popleft()
            spine.append(rec); synced += 1
            continue
        if not gbuf or not obuf:        # one ran out: tail divergence
            who = "ours" if obuf else "golden"
            return {"kind": "tail", "who": who, "synced": synced, "bubbles": bubbles,
                    "spine": list(spine), "ours_next": list(obuf)[:context_default()],
                    "golden_next": list(gbuf)[:context_default()]}
        # divergence: nearest resync (min a+b with obuf[a].pc == gbuf[b].pc)
        gmap = {}
        for b, rec in enumerate(gbuf):
            if rec[1] not in gmap:
                gmap[rec[1]] = b
        best = None
        for a, rec in enumerate(obuf):
            if best is not None and a >= best[0]:
                break                   # a alone already >= best total; can't improve
            b = gmap.get(rec[1])
            if b is not None and (best is None or a + b < best[0]):
                best = (a + b, a, b)
        if best is None:                # no common pc in the window -> permanent split
            return {"kind": "permanent", "synced": synced, "bubbles": bubbles, "spine": list(spine),
                    "golden_next": list(gbuf)[:16], "ours_next": list(obuf)[:16]}
        _, a, b = best
        o_only = [obuf.popleft() for _ in range(a)]
        g_only = [gbuf.popleft() for _ in range(b)]
        bubbles.append((g_only, o_only))


def context_default():
    return 16


def report_resync(res, cur):
    bubbles = res.get("bubbles", [])
    print(f"\n[bisect] resync diff: synced {res['synced']} records through {len(bubbles)} bubble(s)")
    # show the last few bubbles for the pattern
    for g_only, o_only in bubbles[-6:]:
        gpc = resolve(g_only[0][1], cur) if g_only else "(none)"
        opc = resolve(o_only[0][1], cur) if o_only else "(none)"
        print(f"   bubble: golden+{len(g_only):<3} (@{gpc})   ours+{len(o_only):<3} (@{opc})")
    if res["kind"] == "agree_end":
        print("[bisect] streams re-synced to the end -- NO permanent divergence "
              "(ours follows golden throughout, only benign bubbles).")
        return 0
    print("\n[bisect] *** PERMANENT DIVERGENCE -- ours stops following golden here ***")
    sp = res.get("spine", [])
    if sp:
        print(f"  last synced: seq {sp[-1][0]}  {resolve(sp[-1][1], cur)}")
    if res["kind"] == "tail":
        print(f"  '{res['who']}' continued alone after the last sync (the other stream ended).")
    print("  golden continues (it keeps going -- e.g. building the menu):")
    for rec in res.get("golden_next", []):
        print(f"    G {rec[0]:>10}  {resolve(rec[1], cur)}")
    print("  ours continues (where ours goes instead):")
    for rec in res.get("ours_next", []):
        print(f"    O {rec[0]:>10}  {resolve(rec[1], cur)}")
    print("\n[bisect] the ours-side frontier VA is the menu-divergence root -- RE that function + its caller.")
    return 2


def report_diverge_stream(res, cur, context, strict):
    front = res["frontier"]
    mode = "strict index-lockstep" if strict else "subsequence align"
    print(f"\n[bisect] DIVERGENCE ({mode}, streaming) at ours record {front[0]}:")
    if strict and res.get("golden_at"):
        print(f"  golden -> {resolve(res['golden_at'][1], cur)}   "
              f"(r3={_h(res['golden_at'][2])} r1={_h(res['golden_at'][3])} lr={_h(res['golden_at'][4])})")
    else:
        print(f"  ours entered {resolve(front[1], cur)}, which golden never enters within the "
              f"look-ahead window from the last synced point -- ours took a path golden does not.")
    print(f"  ours   -> {resolve(front[1], cur)}   (r3={_h(front[2])} r1={_h(front[3])} lr={_h(front[4])})")
    spine = res.get("spine", [])
    print(f"\n  shared spine (last {len(spine)} synced frames):")
    for rec in spine:
        print(f"    {rec[0]:>10}  {resolve(rec[1], cur)}")
    print("\n  --- divergence ---")
    print("  golden continues:")
    for rec in res.get("golden_next", []):
        print(f"    G {rec[0]:>8}  {resolve(rec[1], cur)}")
    print("  ours continues:")
    for rec in res.get("ours_next", []):
        print(f"    O {rec[0]:>8}  {resolve(rec[1], cur)}")
    print("\n[bisect] feed the diverging VA to Ghidra / the L4 call tracer for the surrounding state.")


def run_stream(args, cur):
    """--stream entry point: never loads a whole trace into RAM."""
    thr = ""
    if args.golden_thread is not None or args.ours_thread is not None:
        thr = f"  [golden thread={args.golden_thread}, ours thread={args.ours_thread}]"
    anc = f"  [anchor=0x{args.anchor_va:08x}]" if args.anchor_va is not None else ""
    print(f"[bisect] mode: {'strict' if args.strict else f'subsequence (window={args.window})'} "
          f"(STREAMING, O({'context' if args.strict else 'window'}) memory){thr}{anc}")
    giter = iter_trace(args.golden, args.code_min, args.code_max, args.golden_thread)
    oiter = iter_trace(args.ours, args.code_min, args.code_max, args.ours_thread)
    if args.anchor_va is not None:
        # Align both streams at the first entry of the anchor VA. The two boots start at different
        # points (different thread-entry thunks / capture offsets), so full-granularity comparison
        # from record 0 trips on startup noise. Anchoring at a known-common function (e.g. the FMUI
        # init point the indirect-granularity run identified) compares from the same logical phase.
        def skip_until(it, va):
            for rec in it:
                if rec[1] == va:
                    yield rec
                    break
            yield from it
        giter = skip_until(giter, args.anchor_va)
        oiter = skip_until(oiter, args.anchor_va)
    if args.resync:
        return report_resync(diff_resync_stream(giter, oiter, args.window), cur)
    if args.strict:
        res = diff_strict_stream(giter, oiter, args.context)
    else:
        res = diff_subsequence_stream(giter, oiter, args.window, args.context)
    if res["kind"] == "agree":
        if args.strict:
            print(f"[bisect] PASS - traces agree for all {res['n']} records (no divergence)")
        else:
            print(f"[bisect] PASS - ours is a faithful subsequence of golden "
                  f"({res['matched']} ours records matched in order; no divergence)")
        return 0
    if res["kind"] == "tail":
        print(f"\n[bisect] DIVERGENCE: identical for {res['i']} records, then '{res['who']}' "
              f"continued and the other stopped.")
        print(f"  first extra ({res['who']}): seq={res['frontier'][0]} {resolve(res['frontier'][1], cur)}")
        return 2
    report_diverge_stream(res, cur, args.context, args.strict)
    return 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", required=True, help="instrumented-Xenia trace (working boot)")
    ap.add_argument("--ours", required=True, help="our recomp replay trace")
    ap.add_argument("--context", type=int, default=6, help="frames of context to show each side")
    ap.add_argument("--resolve", action="store_true", help="resolve VAs via the va_to_source index")
    ap.add_argument("--strict", action="store_true",
                    help="use strict index-lockstep (assumes MATCHED granularity); default is "
                         "granularity-robust subsequence alignment")
    ap.add_argument("--window", type=int, default=2000,
                    help="subsequence look-ahead bound (default 2000): how far to scan golden for "
                         "each ours pc before declaring a divergence. Bounds cost + over-advance.")
    ap.add_argument("--stream", action="store_true",
                    help="O(window) streaming mode -- never loads a whole trace into RAM. Required "
                         "for the rich L9-A golden boot (~11 GB). Reports record indices, not counts.")
    ap.add_argument("--golden-thread", type=int, default=None,
                    help="filter golden to this thread_id (Xenia boot thread is usually 6). Compare "
                         "main-thread-to-main-thread to avoid worker-interleaving noise. Needs --stream.")
    ap.add_argument("--ours-thread", type=int, default=None,
                    help="filter ours to this thread_id (our recomp boot thread is usually 1). Needs --stream.")
    ap.add_argument("--anchor-va", type=lambda x: int(x, 0), default=None,
                    help="align both streams at the first entry of this VA before diffing (the two "
                         "boots start at different points; anchor at a known-common function). Needs --stream.")
    ap.add_argument("--resync", action="store_true",
                    help="resync-aware diff: skip past benign bubbles (where the streams rejoin) and "
                         "report the PERMANENT divergence -- where ours stops following golden for good "
                         "(the menu-killer). Needs --stream. Widen --window if a real bubble is too big.")
    ap.add_argument("--code-min", type=lambda x: int(x, 0), default=0x82000000,
                    help="drop records with pc below this (default 0x82000000) -- strips non-code "
                         "dispatch targets (D3D/allocator stubs) our emitter logs but Xenia won't")
    ap.add_argument("--code-max", type=lambda x: int(x, 0), default=0x83000000,
                    help="drop records with pc at/above this (default 0x83000000)")
    args = ap.parse_args()

    cur = None
    if args.resolve and VA_INDEX_DB.exists():
        cur = sqlite3.connect(VA_INDEX_DB).cursor()
    elif args.resolve:
        print(f"[bisect] (no va index at {VA_INDEX_DB}; run scripts/va_to_source.py --build) -- showing raw VAs")

    if args.stream:
        try:
            return run_stream(args, cur)
        except FileNotFoundError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

    try:
        g = parse_trace(args.golden)
        o = parse_trace(args.ours)
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    def in_code(recs):
        return [r for r in recs if args.code_min <= r[1] < args.code_max]
    gn, on = len(g), len(o)
    g, o = in_code(g), in_code(o)
    print(f"[bisect] golden={len(g)} records (dropped {gn - len(g)} non-code)  "
          f"ours={len(o)} records (dropped {on - len(o)} non-code)")

    mode = "strict index-lockstep" if args.strict else f"subsequence align (window={args.window})"
    print(f"[bisect] mode: {mode}")
    if args.strict:
        print("[bisect] NOTE: --strict assumes MATCHED granularity. Xenia (A) traces all function "
              "entries; our emitter (B) traces only indirect/computed calls -- a subset. If they are "
              "NOT matched, strict lockstep will false-diverge at the first granularity gap. Drop "
              "--strict for the granularity-robust default.")
    else:
        print("[bisect] NOTE: subsequence align treats ours as a faithful SUBSET of golden's control "
              "flow (Xenia logs all entries; we log only indirect/computed calls). It is a greedy "
              "best-effort localizer -- not a proof. On loop-heavy traces a greedy match can mis-sync; "
              "the --window bound caps that. Use --strict when granularities are known-matched.")

    if args.strict:
        res = diff_strict(g, o)
        if res["kind"] == "agree":
            print(f"[bisect] PASS - traces agree for all {res['n']} records (no divergence)")
            return 0
        if res["kind"] == "tail":
            who, i = res["who"], res["i"]
            print(f"\n[bisect] DIVERGENCE: identical for {i} records, then '{who}' continued and the other stopped.")
            longer = g if who == "golden" else o
            if i < len(longer):
                print(f"  first extra ({who}): seq={longer[i][0]} {resolve(longer[i][1], cur)}")
            return 2
        report_diverge_strict(g, o, res["gi"], res["oi"], cur, args.context)
        return 2

    # default: granularity-robust subsequence alignment
    res = diff_subsequence(g, o, args.window)
    if res["kind"] == "agree":
        print(f"[bisect] PASS - ours is a faithful subsequence of golden "
              f"({res['matched']} of {len(o)} ours records matched in order; no divergence)")
        return 0
    report_diverge_subseq(g, o, res, cur, args.context)
    return 2


if __name__ == "__main__":
    sys.exit(main())
