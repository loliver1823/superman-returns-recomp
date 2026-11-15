#!/usr/bin/env python3
"""pm4_census_eras.py - the WHOLE-RUN, per-era PM4 producer census (iter-467 p2; docs/NATIVE_ROADMAP.md
"Whole-run census (2026-10-09)"). tools/perf/pm4_producers.py does the same for the ingame era of one log.

Reads one or more game logs of a roll made with SR_NATIVE_BRACKETS=1 (each log = one process; a RESTART NOW
relaunch gives two) and, per era, reports PM4 packets per frame by producer (the outermost XDK entry that wrote
them, gfx/pm4_coverage [PM4_COVMAP]; UNSTAMPED / RING_UNSTAMPED = no bracket = an inlined site), mapped to the
P1/P2 plan's buckets, plus ZPD / EXT packets per era with their frame timestamps.

    python tools/perf/pm4_census_eras.py <log> [<log> ...] [--json out.json] [--min-reports 2] [--top N]

Roll it with SR_NATIVE_BRACKETS=1 (+ SR_NATIVE_METER=1 for the cross-check) and no present dumps, e.g. the
player whole run: tools/player/roll_player.ps1 -Gates "SR_NATIVE_BRACKETS=1,SR_NATIVE_METER=1,SR_PRESENT_DUMP=0"
with a scenario copy whose title-visible gates are dwells; the logs are drive/attempt1/run.log (generation 0) and
run_r1.log (the RESTART NOW child). Warworld: perf_run.py --scenario arena keeps the full log in the repo root
(superman_returns_devtool_<stamp>_ours.log).

Buckets (the RE-PLAN P0 work list + the record-interface split, 2026-10-08):

  R12   the S1 draw flush + rows 1+2 (DONE): whatever the two S1 draw flushes 820FC000 / 820FBBF8 still
        emit as PM4 other than the kick / pads (P2T) and the bin masks (P2B row 3): the shader bind (27,
        2B, 3B), LOAD_ALU_CONSTANT 2F, the register runs and fetch constants = S1 declines.
  R467  rows 4+6+7 (native-a, re-proving): the third draw flush EmitDrawIndexedGen 820FB6E8 and its
        callers DrawVerticesUP 820FBBB0 / EndVertices 820FBBA0.
  R9    row 9 (strand B): Resolve 8210C5F8, ClearF 82101A58, Clear 82101998, GpuRingEmitRegisterBlock
        820F5570 (the ClearFullSurface preamble), ClearFullSurface 82100488.
  P2T   P2 transport (row 10): Swap, the present / vblank block, InsertFence, BlockUntilIdle, the
        kick-with-callback, the Lock / invalidate coherency packets, Z-pass begin / end and the replay
        driver, RING_UNSTAMPED (the Z-pass replay's bin selects); inside the draw flushes the segment
        kick (IB 3F, WAIT_REG_MEM 3C, EVENT_WRITE_SHD 58, INTERRUPT 54, coherency r0A, scratch r04/r05)
        and the type-2 NOP pads (o1).
  P2B   P2-B items: row 5 setters (SetViewport / SetScissorRect / SetTexture and the other setters),
        EmitPm4Packet 82113010, row 3 residue (SetPredication 8210D400; bin mask / select 60-63 and the
        tile's r21xx writes inside the draw flushes).
  UNMAPPED  everything else (listed per era with its keys).

Era markers = tools/perf/perf_report.py DEFAULT_ERAS (+ mods, + arena), first match in order per process.
Owner deltas use the [PM4_COVMAP] reports INSIDE the era (first report at/after its start, last at/before its
end; reports come every 256 frames or 20 s), so an era needs two reports; totals and ZPD/EXT use the
per-frame [PM4_COV] F lines (exact).
"""
import argparse
import os
import datetime as dt
import json
import re
import sys
from collections import defaultdict

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
CUM = re.compile(r"\[PM4_COV\] CUM (\S+) frames=(\d+) pk=(\d+) .*? acc=(\d+) unst=(\d+) .*? ops=(\S*) regs=(\S*)")
MAP = re.compile(r"\[PM4_COVMAP\] (\S+) owner=([0-9A-F]+) name=(\S+) pk=(\d+) dw=(\d+) keys=(\S*)")
F = re.compile(r"\[PM4_COV\] F frame=(\d+) pk=(\d+) .*? acc=(\d+) unst=(\d+) ops=(\S*)")
UNST = re.compile(r"\[PM4_COV_UNSTAMPED\] (.*)")
METER = re.compile(r"\[NATIVE_METER_RUN\] periodic ([\d.]+)s frames=(\d+) \| pm4=(\d+)")
HANG = re.compile(r"\[GPU_HANG\] ==== trigger (\S+)")
MOVIE = re.compile(r"\[MOVIE_PACE\] movie#(\d+) (\S+) wall=([\d.]+)s frames=(\d+)")

ERAS = [
    ("boot", None, None),
    ("intro", "@first_frame", None),
    ("title", r"\[APT_REQ\] #\d+ path='screens/bootflow/ScrPressStart'", None),
    ("fe_menu", r"\[SR_FSCMD\].*claim LoadCreate", None),
    ("mods", r"\[MODLIB\] shown \(FE", None),
    ("loading", r"\[SR_FSCMD\].*answer cppId=\S+ button=0", None),
    ("gameplay", r"\[APT_REQ\].*path='screens/hud/main_hud'", None),
    # EOM (frames >= 2) or a skipped opening FMV's end: perf_report.INGAME_MOVIE / MOVIE_SKIP (iter-467 cutscene
    # skip: the EV187 accept, or the player's first gameplay mode after the HUD request)
    ("ingame", r"(?:\[MOVIE_PACE\] movie#\d+ EOM wall=[\d.]+s frames=(?:[2-9]|[1-9]\d+)\b)"
               r"|(?:\[EV187\] #\d+ slot=\d+ rc=01 ACCEPTED-KILL)"
               r"|(?:\[PC_CONTROLS\] context gameplay \(igc type \d+\) config \S+ invX=\d+ invY=\d+ mode [a-z])",
     "gameplay"),
    # Warworld (dev-tool level skip, tagged): the skip plan's last snapshot = WARobj01 engaged by the game.
    ("arena", r"\[DEVTOOL\] event .*\"label\":\"S\d+_engaged_5\"", None),
]


S1_FLUSH = {"820FC000", "820FBBF8"}
R467 = {"820FB6E8", "820FBBB0", "820FBBA0"}
R9 = {"8210C5F8", "82101A58", "82101998", "820F5570", "82100488"}
P2T_OWNERS = {"82112050", "82111EC8", "820FDD30", "820FDCB8", "820FDE38", "8210DF38", "8210E1A0", "82110840",
              "RING_UNSTAMPED"}
P2T_NAME_HINTS = ("Lock", "Unlock", "Invalidate", "BlockUntil", "Fence", "IsBusy", "Kick", "Swap", "Present",
                  "Query", "Release", "Create", "WaitTiles")
P2B_OWNERS = {"82113010", "8210D400"}
P2B_NAME_HINTS = ("Predication", "Tiling", "SetRenderTarget", "SetDepthStencil")


def _table_setters():
    import re
    out = set()
    try:
        gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kernel", "native_brackets.gen.cpp")
        for line in open(gen, encoding="utf-8"):
            m = re.search(r"\{0x([0-9A-F]{8})u, \"[^\"]*\", kTable,", line)
            if m:
                out.add(m.group(1))
    except OSError:
        pass
    return out


TABLE_SETTERS = _table_setters()
R12_KEYS = {"27", "2B", "3B", "2F"}
P2T_KEYS = {"3F", "3C", "58", "54", "r0A", "r04", "r05", "o1", "46"}
ROW3_KEYS = {"60", "61", "62", "63", "r21"}
BUCKETS = ["R12", "R467", "R9", "P2T", "P2B", "UNMAPPED"]


def classify(owner, name, key):
    if owner in S1_FLUSH:
        if key in R12_KEYS:
            return "R12"
        if key in P2T_KEYS:
            return "P2T"
        if key in ROW3_KEYS:
            return "P2B"
        return "R12"
    if owner in R467:
        return "R467"
    if owner in R9:
        return "R9"
    if any(h in name for h in P2B_NAME_HINTS):
        return "P2B"
    if owner in P2T_OWNERS or any(h in name for h in P2T_NAME_HINTS):
        return "P2T"
    if (owner in P2B_OWNERS or owner in TABLE_SETTERS or "_Set" in name or "D3DDeviceSet" in name
            or any(h in name for h in P2B_NAME_HINTS)):
        return "P2B"
    return "UNMAPPED"


def ts(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def kv(s, sep=":"):
    out = {}
    for p in s.split(","):
        if sep in p:
            k, v = p.split(sep, 1)
            out[k] = int(v.split("/")[0])
    return out


def parse(path):
    groups = []   # [t, frames, pk, acc, unst, ops, {owner: [name, pk, dw, keys]}]
    frames = []   # (t, frame, pk, acc, unst, ops)
    marks = {}
    unst = []
    meter = []
    hangs = []
    movies = []
    first_t = None
    pending = [(e, re.compile(rx), after) for e, rx, after in ERAS if rx and not rx.startswith("@")]
    cur = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "[" not in line:
                continue
            t = ts(line)
            if t is None:
                continue
            if first_t is None:
                first_t = t
            if "[PM4_COV" in line:
                m = F.search(line)
                if m:
                    frames.append((t, int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                                   kv(m.group(5))))
                    continue
                m = CUM.search(line)
                if m:
                    cur = [t, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)),
                           kv(m.group(6)), {}]
                    groups.append(cur)
                    continue
                m = MAP.search(line)
                if m and cur is not None:
                    key = m.group(2) if m.group(2) != "00000000" else m.group(3)  # UNSTAMPED / RING_UNSTAMPED
                    cur[6][key] = [m.group(3), int(m.group(4)), int(m.group(5)), kv(m.group(6))]
                    continue
                m = UNST.search(line)
                if m:
                    unst.append((t, m.group(1)))
                    continue
            m = METER.search(line)
            if m:
                meter.append((t, float(m.group(1)), int(m.group(2)), int(m.group(3))))
            m = HANG.search(line)
            if m:
                hangs.append((t, line.strip()[:300]))
            m = MOVIE.search(line)
            if m:
                movies.append((t, int(m.group(1)), m.group(2), float(m.group(3)), int(m.group(4))))
            for i, (era, rx, after) in enumerate(pending):
                if after and after not in marks:
                    continue
                if rx.search(line):
                    if after and "MOVIE_PACE" in line:
                        w = MOVIE.search(line)
                        if w and t - float(w.group(3)) < marks[after] - 1.0:
                            continue
                    marks[era] = t
                    pending.pop(i)
                    break
    marks["boot"] = first_t
    if frames:
        marks["intro"] = frames[0][0]
    order = [e for e, _, _ in ERAS if e in marks]
    eras = sorted(((e, marks[e]) for e in order), key=lambda x: x[1])
    end_t = max([frames[-1][0] if frames else first_t] + [g[0] for g in groups])
    return dict(path=path, t0=first_t, end=end_t, eras=eras, groups=groups, frames=frames, unst=unst,
                meter=meter, hangs=hangs, movies=movies)


def era_census(run, i, min_reports=2):
    era, t0 = run["eras"][i]
    t1 = run["eras"][i + 1][1] if i + 1 < len(run["eras"]) else run["end"] + 1
    fr = [f for f in run["frames"] if t0 <= f[0] < t1]
    res = dict(era=era, t0=t0 - run["t0"], dur=t1 - t0 if i + 1 < len(run["eras"]) else run["end"] - t0,
               frames=len(fr))
    if fr:
        res["pk_per_frame"] = sum(f[2] for f in fr) / len(fr)
        res["unst_per_frame"] = sum(f[4] for f in fr) / len(fr)
        res["acc_per_frame"] = sum(f[3] for f in fr) / len(fr)
        zpd = [(f[0] - run["t0"], f[1], f[5].get("5B", 0), f[5].get("5A", 0)) for f in fr
               if f[5].get("5B") or f[5].get("5A")]
        res["zpd_total"] = sum(z[2] for z in zpd)
        res["ext_total"] = sum(z[3] for z in zpd)
        res["zpd_frames"] = len(zpd)
        res["zpd_first_last"] = [zpd[0], zpd[-1]] if zpd else []
        ops = defaultdict(int)
        for f in fr:
            for k, v in f[5].items():
                ops[k] += v
        res["ops_per_frame"] = {k: v / len(fr) for k, v in sorted(ops.items(), key=lambda kv: -kv[1])}
    inside = [g for g in run["groups"] if t0 <= g[0] <= t1 and g[6]]
    if len(inside) >= min_reports:
        a, b = inside[0], inside[-1]
        nf = b[1] - a[1]
        res["owner_window"] = [a[0] - run["t0"], b[0] - run["t0"], nf]
        rows = []
        if nf > 0:
            for o, (name, pk, dw, keys) in b[6].items():
                pa = a[6].get(o, [name, 0, 0, {}])
                dpk, ddw = pk - pa[1], dw - pa[2]
                if dpk <= 0:
                    continue
                dk = {k: (v - pa[3].get(k, 0)) / nf for k, v in keys.items() if v - pa[3].get(k, 0) > 0}
                rows.append(dict(owner=o, name=name, pk=dpk / nf, dw=ddw / nf, keys=dk))
        rows.sort(key=lambda r: -r["pk"])
        res["owners"] = rows
        res["owner_pk_per_frame"] = sum(r["pk"] for r in rows)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--min-reports", type=int, default=2)
    ap.add_argument("--top", type=int, default=0, help="owners listed per era (0 = buckets only)")
    a = ap.parse_args()
    out = []
    for p in a.logs:
        run = parse(p)
        print("== %s" % p)
        print("   eras: " + ", ".join("%s@%.1fs" % (e, t - run["t0"]) for e, t in run["eras"]))
        if run["groups"]:
            g = run["groups"][0]
            u = {o: v for o, v in g[6].items() if "UNSTAMPED" in o}
            print("   first report (frame %d, includes the boot work before the first present): pk=%d unst=%d %s" % (
                g[1], g[2], g[4], " ".join("%s pk=%d keys=%s" % (o, v[1], ",".join("%s:%d" % kv for kv in
                                                                       sorted(v[3].items()))) for o, v in u.items())))
        print("   GPU_HANG triggers: %d" % len(run["hangs"]))
        for t, l in run["hangs"]:
            print("     %.1fs %s" % (t - run["t0"], l[:200]))
        eras = []
        for i in range(len(run["eras"])):
            r = era_census(run, i, a.min_reports)
            eras.append(r)
            print("  -- era %-9s t0=%7.1fs dur=%6.1fs frames=%6d pk/f=%8.1f unst/f=%6.1f zpd=%s ext=%s" % (
                r["era"], r["t0"], r["dur"], r["frames"], r.get("pk_per_frame", 0), r.get("unst_per_frame", 0),
                r.get("zpd_total", 0), r.get("ext_total", 0)))
            if "owners" in r:
                tot = defaultdict(float)
                unm = defaultdict(lambda: [0.0, {}])
                for row in r["owners"]:
                    for k, v in row["keys"].items():
                        b = classify(row["owner"], row["name"], k)
                        tot[b] += v
                        if b == "UNMAPPED":
                            unm[(row["owner"], row["name"])][0] += v
                            unm[(row["owner"], row["name"])][1][k] = v
                r["buckets"] = dict(tot)
                print("     buckets: " + "  ".join("%s %.1f" % (b, tot[b]) for b in BUCKETS))
                for (o, n), (v, ks) in sorted(unm.items(), key=lambda kv: -kv[1][0]):
                    if v >= 0.05:
                        print("     UNMAPPED %-14s %-44s %8.2f pk/f  %s" % (o, n[:44], v, ", ".join(
                            "%s %.2f" % kv for kv in sorted(ks.items(), key=lambda kv: -kv[1])[:8])))
                w = r["owner_window"]
                print("     owners over reports %.1f-%.1fs (%d frames), sum %.1f pk/f" % (w[0], w[1], w[2],
                                                                                    r["owner_pk_per_frame"]))
                for row in r["owners"][:a.top]:
                    top = sorted(row["keys"].items(), key=lambda kv: -kv[1])[:6]
                    print("       %-9s %-52s %8.1f pk/f %8.1f dw/f  %s" % (
                        row["owner"], row["name"][:52], row["pk"], row["dw"],
                        ", ".join("%s %.1f" % (k, v) for k, v in top)))
        out.append(dict(path=p, eras=eras, hangs=[(t - run["t0"], l) for t, l in run["hangs"]],
                        unstamped=[(t - run["t0"], l) for t, l in run["unst"]],
                        movies=[(m[0] - run["t0"],) + tuple(m[1:]) for m in run["movies"]],
                        meter=[(m[0] - run["t0"],) + tuple(m[1:]) for m in run["meter"]]))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
