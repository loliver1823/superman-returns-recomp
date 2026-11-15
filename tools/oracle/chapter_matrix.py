#!/usr/bin/env python3
"""chapter_matrix.py - step-4 lane A (xenia-oracle): the whole-game matched matrix, ours vs the Xenia oracle, one row
per chapter seed (docs/WHOLE_GAME_FIDELITY.md has the method and the results).

    py -3.11 tools/oracle/chapter_matrix.py xenia  <xenia_seed_pass.py args>     # the Xenia half (PC slot)
    py -3.11 tools/oracle/chapter_matrix.py ours   <ours_seed_pass.py args>      # our half (PC slot)
    py -3.11 tools/oracle/chapter_matrix.py sheets <oracle_sheets.py sheets args>
    py -3.11 tools/oracle/chapter_matrix.py audio  --xenia <pass> --ours <pass> --out <dir> [--seeds a,b] [--win 4 --hop 2]
    py -3.11 tools/oracle/chapter_matrix.py levels --xenia <pass> --ours <pass> --out <dir> [--win 2 --to 120]
    py -3.11 tools/oracle/chapter_matrix.py report --xenia <pass> --ours <pass> --audio <dir> --out <matrix.md>

audio: per seed, tools/audio_oracle_compare.py's compare() (imported, unchanged) of the Xenia AFR (recorded from the
anchor) against ours.afr (the whole roll), searched near the expected offset (ours' AFR position at our own anchor),
then info() of both (level per chunk, channel energy). Gameplay with no input still diverges in timing (traffic, AI,
the idle camera, metro events), so the music / ambience and the level are what match; UNMATCHED windows are
candidates to LOOK at (a missing sound or a different event), never a verdict by themselves (TOOLKIT 1.15).
Disk: AFR reads are ~140 MB (Xenia) + ~400 MB (ours) per seed - run outside rolls (STEP4_RULES: D: is an HDD).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)


def latest(pass_dir, seed, which):
    import oracle_sheets
    r = oracle_sheets.latest_ok(pass_dir, seed, which)
    return r if r else (None, None)


def seeds_of(a):
    import xenia_seed_pass as xsp
    return a.seeds.split(",") if a.seeds else list(xsp.SEEDS)


def ours_anchor_content_s(afr_path, anchor_epoch):
    """ours.afr content seconds at the first record stamped at/after our anchor (records carry epoch us)."""
    import numpy as np
    import audio_oracle_compare as aoc
    U = aoc.Afr(afr_path)
    t = U.r["t"].astype(np.int64) / 1e6
    i = int(np.searchsorted(t, anchor_epoch))
    return i * aoc.FS / aoc.RATE, U


def cmd_audio(a):
    import numpy as np
    import audio_oracle_compare as aoc
    os.makedirs(a.out, exist_ok=True)
    allres = {}
    # --pairs XSEED:OSEED,...: a CROSS-seed check (the retail run of one seed against our run of another, e.g. two
    # runs that drew the same free-roam music cue); default: each seed against itself.
    pairs = [tuple(p.split(":", 1)) for p in a.pairs.split(",") if p.strip()] if a.pairs else         [(s_, s_) for s_ in seeds_of(a)]
    for xs, os_ in pairs:
        seed = xs if xs == os_ else "%s__vs__%s" % (xs, os_)
        xd, xm = latest(a.xenia, xs, "xenia")
        od, om = latest(a.ours, os_, "ours")
        if not xd or not od:
            allres[seed] = {"skip": "missing side (xenia %s, ours %s)" % (bool(xd), bool(od))}
            continue
        xafr, oafr = os.path.join(xd, "xenia.afr"), os.path.join(od, "ours.afr")
        if not (os.path.exists(xafr) and os.path.exists(oafr)) or not om.get("anchor_epoch"):
            allres[seed] = {"skip": "no AFR or no anchor"}
            continue
        off, _U = ours_anchor_content_s(oafr, om["anchor_epoch"])
        out = os.path.join(a.out, seed)
        ns = types.SimpleNamespace(oracle=xafr, ours=oafr, out=out, win=a.win, hop=a.hop, to=a.to, min_db=-50.0,
                                   env_match=0.6, expect_offset=off, search=a.search)
        ns.__dict__["from"] = 0.0
        print("== %s (ours anchor at %.1f s of ours.afr)" % (seed, off), flush=True)
        res = aoc.compare(ns)
        rows = res["rows"]
        en = [r for r in rows if r["state"] != "quiet"]
        m = [r for r in en if r["state"] == "match"]
        wc = [r["wave_corr_med"] for r in m if "wave_corr_med" in r]
        gd = [r["gain_db_med"] for r in m if "gain_db_med" in r]
        lev_x = [r["o_db"] for r in en]
        lev_o = [r["u_db"] for r in m if "u_db" in r]
        s = {"ours_anchor_s": round(off, 2), "windows": len(rows), "energetic": len(en), "matched": len(m),
             "unmatched": len(en) - len(m), "unmatched_at": [r["o_t"] for r in en if r["state"] != "match"],
             "matched_at": [r["o_t"] for r in m],
             "wcorr_med": round(float(np.median(wc)), 3) if wc else None,
             "wcorr_ge_0.9": sum(1 for v in wc if v >= 0.9), "gain_db_med": round(float(np.median(gd)), 2) if gd else None,
             "oracle_db_med": round(float(np.median(lev_x)), 1) if lev_x else None,
             "ours_db_med_matched": round(float(np.median(lev_o)), 1) if lev_o else None,
             "off_s_med": round(float(np.median([r["off_s"] for r in m])), 2) if m else None}
        allres[seed] = s
        print(seed, json.dumps(s), flush=True)
    with open(os.path.join(a.out, a.json_name), "w") as f:
        json.dump(allres, f, indent=1)
    return 0


CH = ("L", "R", "C", "LFE", "Ls", "Rs")


def afr_first_at(path, epoch_s):
    """The index of the first AFR1 record stamped at / after epoch_s, by binary search on single-record reads (a few
    seeks, not the whole file: audio_oracle_compare.Afr() touches every record's page to build its time axis)."""
    import struct
    import audio_oracle_compare as aoc
    rs = aoc.REC.itemsize
    n = os.path.getsize(path) // rs
    want = int(epoch_s * 1e6)
    with open(path, "rb") as f:
        def t_of(i):
            f.seek(i * rs)
            mg, _seq, t = struct.unpack("<IIQ", f.read(16))
            if mg != 0x31524641:
                raise SystemExit("%s: record %d is not AFR1" % (path, i))
            return t
        lo, hi = 0, n
        while lo < hi:
            mid = (lo + hi) // 2
            if t_of(mid) < want:
                lo = mid + 1
            else:
                hi = mid
    return lo, n


def span_levels(path, f0, nfr, win):
    """Per window of `win` s from record f0: the stereo fold-down's RMS dB (audio_oracle_compare.Afr.stereo's matrix)
    and each of the 6 channels' energy (sum of squares). One sequential read of the slice, window by window."""
    import numpy as np
    import audio_oracle_compare as aoc
    rs = aoc.REC.itemsize
    wf = int(round(win * aoc.RATE / aoc.FS))
    n = os.path.getsize(path) // rs
    nfr = max(0, min(nfr, n - f0))
    db, ch = [], np.zeros(6)
    with open(path, "rb") as f:
        f.seek(f0 * rs)
        for _w in range(nfr // wf):
            r = np.fromfile(f, dtype=aoc.REC, count=wf)
            p = r["pcm"].astype(np.float64)                       # (wf, 6, 256)
            ch += (p * p).sum(axis=(0, 2))
            fc = p[:, 2] * 0.5
            l, rr = (p[:, 0] + p[:, 4] + fc) / 2.5, (p[:, 1] + p[:, 5] + fc) / 2.5
            db.append(10 * np.log10(max(float((l * l).sum() + (rr * rr).sum()) / (wf * aoc.FS * 2), 1e-12)))
    return db, ch, (nfr // wf) * wf * aoc.FS / aoc.RATE


def cmd_levels(a):
    """levels: what the two sides' gameplay mixes ARE, independent of matching moments (the game is not deterministic:
    a different music cue or a metro event on one side leaves windows UNMATCHED in `audio` without any defect).
    Per seed over [anchor, anchor + --to] of each side's content time: the window level distribution (median / p10 /
    p90 dB of the stereo fold-down per --win s), the quiet share (windows below -50 dB) and each channel's share of the
    energy (the 5.1 balance). Read only: one sequential slice a side (~140 MB per side per seed - outside rolls)."""
    import numpy as np
    import audio_oracle_compare as aoc
    os.makedirs(a.out, exist_ok=True)
    allres = {}
    nfr = int(round(a.to * aoc.RATE / aoc.FS))
    for seed in seeds_of(a):
        xd, xm = latest(a.xenia, seed, "xenia")
        od, om = latest(a.ours, seed, "ours")
        if not xd or not od:
            allres[seed] = {"skip": "missing side"}
            continue
        xafr, oafr = os.path.join(xd, "xenia.afr"), os.path.join(od, "ours.afr")
        if not (os.path.exists(xafr) and os.path.exists(oafr)) or not om.get("anchor_epoch"):
            allres[seed] = {"skip": "no AFR or no anchor"}
            continue
        i0, _n = afr_first_at(oafr, om["anchor_epoch"])
        off = i0 * aoc.FS / aoc.RATE                               # = ours_anchor_content_s (the audio command's)
        res = {"ours_anchor_s": round(off, 2)}
        for side, path, f0 in (("xenia", xafr, 0), ("ours", oafr, i0)):
            db, ch, span = span_levels(path, f0, nfr, a.win)
            d = np.array(db)
            tot = float(ch.sum()) or 1.0
            res[side] = {"span_s": round(span, 1), "windows": len(db),
                         "db_med": round(float(np.median(d)), 1), "db_p10": round(float(np.percentile(d, 10)), 1),
                         "db_p90": round(float(np.percentile(d, 90)), 1), "quiet_share": round(float((d < -50).mean()), 3),
                         "ch_share": {CH[i]: round(float(ch[i]) / tot, 3) for i in range(6)},
                         "db_per_10s": [round(float(np.mean(d[i:i + int(10 / a.win)])), 1)
                                        for i in range(0, len(d), int(10 / a.win))]}
        x, o = res["xenia"], res["ours"]
        res["delta"] = {"db_med": round(o["db_med"] - x["db_med"], 1), "db_p90": round(o["db_p90"] - x["db_p90"], 1),
                        "ch_share": {c: round(o["ch_share"][c] - x["ch_share"][c], 3) for c in CH}}
        allres[seed] = res
        print(seed, "X med %.1f p90 %.1f | ours med %.1f p90 %.1f | d %+.1f | X ch %s | ours ch %s" % (
            x["db_med"], x["db_p90"], o["db_med"], o["db_p90"], res["delta"]["db_med"],
            "/".join("%.2f" % x["ch_share"][c] for c in CH), "/".join("%.2f" % o["ch_share"][c] for c in CH)), flush=True)
    with open(os.path.join(a.out, "audio_levels.json"), "w") as f:
        json.dump(allres, f, indent=1)
    return 0


def cmd_report(a):
    import xenia_seed_pass as xsp
    au = {}
    if a.audio and os.path.exists(os.path.join(a.audio, "audio_matrix.json")):
        au = json.load(open(os.path.join(a.audio, "audio_matrix.json")))
    rows = ["| seed | Xenia | ours | landing X / ours | anchor s X | audio: energetic / matched / unmatched | wcorr med | gain dB med |",
            "|---|---|---|---|---|---|---|---|"]
    for seed in seeds_of(a):
        xd, xm = latest(a.xenia, seed, "xenia")
        od, om = latest(a.ours, seed, "ours")
        xr = (xm or {}).get("result", {})
        s = au.get(seed, {})
        rows.append("| %s | %s | %s | %s / %s | %s | %s | %s | %s |" % (
            seed, xr.get("verdict", "-"), (om or {}).get("verdict", "-"), xr.get("landing"), (om or {}).get("landing"),
            xr.get("anchor_s"),
            "%s / %s / %s" % (s.get("energetic"), s.get("matched"), s.get("unmatched")) if s and "skip" not in s else s.get("skip", "-"),
            s.get("wcorr_med", "-"), s.get("gain_db_med", "-")))
    txt = "\n".join(rows) + "\n"
    with open(a.out, "w") as f:
        f.write(txt)
    print(txt)
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] in ("xenia", "ours", "sheets"):
        which = sys.argv.pop(1)
        if which == "xenia":
            import xenia_seed_pass as m
        elif which == "ours":
            import ours_seed_pass as m
        else:
            import oracle_sheets as m
            sys.argv.insert(1, "sheets")
        return m.main()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("audio")
    s.add_argument("--xenia", required=True)
    s.add_argument("--ours", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--seeds", default="")
    s.add_argument("--win", type=float, default=4.0)
    s.add_argument("--hop", type=float, default=2.0)
    s.add_argument("--to", type=float, default=120.0)
    s.add_argument("--search", type=float, default=60.0)
    s.add_argument("--pairs", default="", help="XSEED:OSEED,... cross-seed pairs instead of each seed with itself")
    s.add_argument("--json-name", default="audio_matrix.json")
    lv = sub.add_parser("levels")
    lv.add_argument("--xenia", required=True)
    lv.add_argument("--ours", required=True)
    lv.add_argument("--out", required=True)
    lv.add_argument("--seeds", default="")
    lv.add_argument("--win", type=float, default=2.0)
    lv.add_argument("--to", type=float, default=120.0)
    r = sub.add_parser("report")
    r.add_argument("--xenia", required=True)
    r.add_argument("--ours", required=True)
    r.add_argument("--audio", default="")
    r.add_argument("--out", required=True)
    r.add_argument("--seeds", default="")
    a = ap.parse_args()
    return {"audio": cmd_audio, "levels": cmd_levels, "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
