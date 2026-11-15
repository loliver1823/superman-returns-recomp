#!/usr/bin/env python3
"""audio_oracle_compare.py - iter-467 sound lane: judge our audio against the Xenia oracle.

Both sides record the SAME capture point: every mixed frame the game hands to
XAudioSubmitRenderDriverFrame (256 samples x 6 ch planar big-endian float, 48 kHz):
  ours   : SR_AUDIO_FRAMES=<path> (kernel/apu/audio_system.cpp, observe-only)
  oracle : tools/xenia_audio_capture.py (Frida hook on guest 0x8211B680 inside Xenia)
Record = u32 'AFR1' | u32 seq | u64 t_us (unix epoch us) | 6144 B frame.

Subcommands
  info    <afr> [--json f]          submit timing (rate, gaps), level per minute, clicks, frame
                                     seams, zero-frame dropouts, channel balance
  compare --oracle X.afr --ours O.afr [--out dir] [--win 10 --hop 5] [--from S --to S]
          For every energetic oracle window: find the best-matching place in ours (multi-band
          log-energy envelope, normalised cross-correlation over the whole run, or near
          --expect-offset), then check the waveform there (2 s chunks, +-40 ms local lag):
          corr (1.0 = same waveform), gain dB (ours/oracle), lag drift (rate error), match
          confidence (best vs runner-up). Unmatched energetic oracle windows = candidate
          MISSING sounds; ours windows never matched = candidate EXTRA sounds.
  spec    <afr> --from S --to S --png f      spectrogram of a span (stereo downmix)

Downmix = Xenia's 6->2 (L = (FL + BL + FC/2)/2.5, R = (FR + BR + FC/2)/2.5, LFE dropped).
Times are seconds from the first frame of each file unless stated.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

RATE = 48000
FS = 256                     # samples per frame
REC = np.dtype([("magic", "<u4"), ("seq", "<u4"), ("t", "<u8"), ("pcm", ">f4", (6, FS))])
HOP = 480                    # 10 ms feature hop
NBANDS = 8
EDGES = [0, 150, 400, 900, 1800, 3500, 6000, 10000, 24000]


class Afr:
    def __init__(self, path):
        n = os.path.getsize(path) // REC.itemsize
        self.path = path
        self.r = np.memmap(path, dtype=REC, mode="r", shape=(n,))
        self.n = n
        bad = np.count_nonzero(self.r["magic"][: min(n, 100000)] != 0x31524641)
        if bad:
            raise SystemExit("%s: %d bad record magics (not an AFR1 file?)" % (path, bad))
        self.t = (self.r["t"].astype(np.int64) - int(self.r["t"][0])) / 1e6 if n else np.zeros(0)
        self.t0_epoch = int(self.r["t"][0]) / 1e6 if n else 0.0

    def frames(self, a, b):
        return np.asarray(self.r["pcm"][a:b], dtype=np.float32)    # (k, 6, 256)

    def stereo(self, a=0, b=None, chunk=20000):
        b = self.n if b is None else b
        out = np.empty(((b - a) * FS, 2), np.float32)
        for s in range(a, b, chunk):
            e = min(b, s + chunk)
            p = self.frames(s, e)
            fc = p[:, 2] * 0.5
            out[(s - a) * FS:(e - a) * FS, 0] = ((p[:, 0] + p[:, 4] + fc) / 2.5).reshape(-1)
            out[(s - a) * FS:(e - a) * FS, 1] = ((p[:, 1] + p[:, 5] + fc) / 2.5).reshape(-1)
        return out

    def mono(self, a=0, b=None):
        s = self.stereo(a, b)
        return s.mean(axis=1)


def sec_to_frame(s):
    return int(round(s * RATE / FS))


# ── info ─────────────────────────────────────────────────────────────────────────────────────
def info(path, js=None, quiet=False):
    A = Afr(path)
    res = {"file": path, "frames": A.n, "content_s": A.n * FS / RATE}
    if A.n < 2:
        print(json.dumps(res))
        return res
    dt = np.diff(A.t)
    res["wall_s"] = float(A.t[-1])
    res["rate_fps_overall"] = float((A.n - 1) / A.t[-1]) if A.t[-1] > 0 else 0.0
    res["interval_ms_p50_p99_max"] = [float(np.percentile(dt, 50) * 1e3), float(np.percentile(dt, 99) * 1e3),
                                      float(dt.max() * 1e3)]
    gaps = np.nonzero(dt > 0.050)[0]
    res["gaps_over_50ms"] = int(len(gaps))
    res["gap_list"] = [[round(float(A.t[i]), 3), round(float(dt[i] * 1e3), 1)] for i in gaps[:40]]
    # per-second submit rate (content-time vs wall: 187.5 frames/s = real time)
    sec = np.floor(A.t).astype(np.int64)
    cnt = np.bincount(sec)
    res["slow_seconds_lt170fps"] = int(np.count_nonzero(cnt[:-1] < 170))
    res["fast_seconds_gt205fps"] = int(np.count_nonzero(cnt[:-1] > 205))
    # level, clicks, seams, dropouts per chunk of ~10 s
    per = []
    ch_energy = np.zeros(6)
    zero_runs = 0
    zero_frames_total = 0
    seam_ratio_list = []
    clip = 0
    nan = 0
    chunk = sec_to_frame(10.0)
    prev_tail = None
    for a in range(0, A.n, chunk):
        b = min(A.n, a + chunk)
        p = A.frames(a, b)
        nan += int(np.count_nonzero(~np.isfinite(p)))
        p = np.nan_to_num(p)
        clip += int(np.count_nonzero(np.abs(p) > 1.0))
        ch_energy += (p.astype(np.float64) ** 2).sum(axis=(0, 2))
        frame_rms = np.sqrt((p.astype(np.float64) ** 2).mean(axis=(1, 2)))
        allzero = np.all(p == 0.0, axis=(1, 2))
        # zero frames between non-silent neighbours = starved voice / dropped buffer
        for i in np.nonzero(allzero)[0]:
            lo = frame_rms[i - 1] if i > 0 else (prev_tail if prev_tail is not None else 0.0)
            hi = frame_rms[i + 1] if i + 1 < len(frame_rms) else 0.0
            if lo > 0.003 and hi > 0.003:
                zero_runs += 1
        zero_frames_total += int(allzero.sum())
        prev_tail = frame_rms[-1]
        st = A.stereo(a, b)
        m = st.mean(axis=1).astype(np.float64)
        rms = float(np.sqrt((m ** 2).mean())) if len(m) else 0.0
        d1 = np.abs(np.diff(st, axis=0))
        # frame seam: jump across a 256-sample boundary vs the median jump inside frames
        idx = np.arange(FS - 1, len(d1), FS)
        inner = np.delete(d1, idx, axis=0)
        if len(idx) and rms > 0.003:
            seam_ratio_list.append(float(d1[idx].mean() / max(1e-9, inner.mean())))
        # clicks: second difference spikes > 12x the local (100 ms) rms of the second difference
        d2 = np.abs(np.diff(m, 2))
        blk = 4800
        nb = len(d2) // blk
        clicks = 0
        if nb:
            d2b = d2[: nb * blk].reshape(nb, blk)
            r = np.sqrt((d2b ** 2).mean(axis=1)) + 1e-9
            clicks = int(np.count_nonzero((d2b.max(axis=1) > 12 * r) & (d2b.max(axis=1) > 0.02)))
        ce = (p.astype(np.float64) ** 2).sum(axis=(0, 2))
        cef = [round(float(x / (ce.sum() + 1e-12)), 3) for x in ce]
        per.append({"t": round(float(A.t[a]), 1), "c": round(a * FS / RATE, 1), "ch": cef, "rms_db": round(20 * math.log10(rms + 1e-9), 1),
                    "peak": round(float(np.abs(st).max()) if len(st) else 0.0, 3), "clicks": clicks})
    tot = ch_energy.sum() + 1e-12
    res["channel_energy_frac"] = [round(float(x / tot), 4) for x in ch_energy]
    res["zero_frames_total"] = zero_frames_total
    res["zero_frames_between_sound"] = zero_runs
    res["seam_ratio_median"] = float(np.median(seam_ratio_list)) if seam_ratio_list else None
    res["seam_ratio_max"] = float(np.max(seam_ratio_list)) if seam_ratio_list else None
    res["clip_samples"] = clip
    res["nonfinite_samples"] = nan
    res["clicks_total"] = int(sum(x["clicks"] for x in per))
    res["silent_10s_chunks"] = int(sum(1 for x in per if x["rms_db"] < -60))
    res["per10s"] = per
    if not quiet:
        brief = {k: v for k, v in res.items() if k not in ("per10s", "gap_list")}
        print(json.dumps(brief, indent=1))
        print("gaps (t, ms):", res["gap_list"][:20])
        print("level per 10 s (t rms_db clicks):")
        line = []
        for x in per:
            line.append("%6.0f %6.1f %3d" % (x["t"], x["rms_db"], x["clicks"]))
            if len(line) == 6:
                print("  " + " | ".join(line))
                line = []
        if line:
            print("  " + " | ".join(line))
    if js:
        json.dump(res, open(js, "w"), indent=1)
    return res


# ── features ─────────────────────────────────────────────────────────────────────────────────
def band_features(A, a_frame=0, b_frame=None):
    """log band energies at a 10 ms hop: (nhop, NBANDS) float32 + per-hop rms."""
    b_frame = A.n if b_frame is None else b_frame
    win = 1024
    freqs = np.fft.rfftfreq(win, 1.0 / RATE)
    bidx = [np.nonzero((freqs >= EDGES[i]) & (freqs < EDGES[i + 1]))[0] for i in range(NBANDS)]
    window = np.hanning(win).astype(np.float32)
    feats = []
    rmss = []
    chunk = sec_to_frame(30.0)
    carry = np.zeros(0, np.float32)
    for a in range(a_frame, b_frame, chunk):
        b = min(b_frame, a + chunk)
        m = np.concatenate([carry, A.mono(a, b)])
        nh = (len(m) - win) // HOP + 1
        if nh <= 0:
            carry = m
            continue
        strides = (m.strides[0] * HOP, m.strides[0])
        fr = np.lib.stride_tricks.as_strided(m, shape=(nh, win), strides=strides)
        spec = np.abs(np.fft.rfft(fr * window, axis=1)) ** 2
        f = np.stack([spec[:, ix].sum(axis=1) for ix in bidx], axis=1)
        feats.append(np.log10(f + 1e-10).astype(np.float32))
        rmss.append(np.sqrt((fr[:, (win - HOP) // 2:(win + HOP) // 2] ** 2).mean(axis=1)).astype(np.float32))
        carry = m[nh * HOP:]
    if not feats:
        return np.zeros((0, NBANDS), np.float32), np.zeros(0, np.float32)
    return np.concatenate(feats), np.concatenate(rmss)


def ncc_multi(hay, needle):
    """normalised cross-correlation of a (n, B) needle over a (N, B) haystack -> (N-n+1,)"""
    from scipy.signal import fftconvolve
    n = len(needle)
    N = len(hay)
    if N < n:
        return np.zeros(0)
    tot = np.zeros(N - n + 1)
    nz = needle - needle.mean(axis=0)
    nn = np.sqrt((nz ** 2).sum()) + 1e-9
    num = np.zeros(N - n + 1)
    s1 = np.zeros(N - n + 1)
    s2 = np.zeros(N - n + 1)
    for b in range(hay.shape[1]):
        h = hay[:, b].astype(np.float64)
        num += fftconvolve(h, nz[::-1, b], mode="valid")
        c1 = np.concatenate([[0], np.cumsum(h)])
        c2 = np.concatenate([[0], np.cumsum(h * h)])
        w1 = c1[n:] - c1[:-n]
        w2 = c2[n:] - c2[:-n]
        s2 += w2 - w1 * w1 / n
    tot = num / (nn * np.sqrt(np.maximum(s2, 1e-9)))
    # iter-467: a haystack window of digital silence has constant log energies (s2 ~ 0), and the
    # cumsum / FFT rounding then made its NCC explode (o13: env_ncc 10.3 on silence, 27 windows
    # matched to it). Such a window cannot match anything.
    tot[s2 < 1e-3 * n] = 0.0
    return np.clip(tot, -1.0, 1.0)


def wave_check(O, U, o_start_s, u_start_s, dur_s, chunk_s=2.0, search_ms=40):
    """waveform correlation of matched spans in 2 s chunks with a local lag search."""
    from scipy.signal import fftconvolve
    out = []
    nchunk = int(dur_s // chunk_s)
    srch = int(search_ms * RATE / 1000)
    for k in range(nchunk):
        os_ = o_start_s + k * chunk_s
        us_ = u_start_s + k * chunk_s
        oa, ob = sec_to_frame(os_), sec_to_frame(os_ + chunk_s)
        ua, ub = sec_to_frame(us_) - (srch // FS + 1), sec_to_frame(us_ + chunk_s) + (srch // FS + 1)
        if oa < 0 or ob > O.n or ua < 0 or ub > U.n:
            continue
        o = O.mono(oa, ob).astype(np.float64)
        u = U.mono(ua, ub).astype(np.float64)
        orms = np.sqrt((o ** 2).mean())
        if orms < 1e-3:
            continue
        num = fftconvolve(u, o[::-1], mode="valid")
        c2 = np.concatenate([[0], np.cumsum(u * u)])
        n = len(o)
        w2 = c2[n:] - c2[:-n]
        corr = num / (np.sqrt((o ** 2).sum()) * np.sqrt(np.maximum(w2, 1e-12)))
        j = int(np.argmax(corr))
        seg = u[j:j + n]
        g = np.sqrt((seg ** 2).mean()) / orms
        out.append({"o_t": round(os_, 2), "corr": round(float(corr[j]), 3),
                    "lag_ms": round(((ua * FS + j) / RATE - us_) * 1000, 1),
                    "gain_db": round(20 * math.log10(g + 1e-9), 2)})
    return out


def compare(a):
    O = Afr(a.oracle)
    U = Afr(a.ours)
    print("oracle %s: %d frames (%.1f s content, %.1f s wall)" % (a.oracle, O.n, O.n * FS / RATE, O.t[-1]))
    print("ours   %s: %d frames (%.1f s content, %.1f s wall)" % (a.ours, U.n, U.n * FS / RATE, U.t[-1]))
    print("features ...", flush=True)
    fo, ro = band_features(O)
    fu, ru = band_features(U)
    win = int(a.win * 100)
    hop = int(a.hop * 100)
    t_from = int(a.__dict__.get("from") or 0) * 100
    t_to = int((a.to or 1e9) * 100)
    rows = []
    matched_u = np.zeros(len(fu), bool)
    for s in range(t_from, min(len(fo) - win, t_to), hop):
        seg = fo[s:s + win]
        level = 20 * np.log10(float(np.sqrt((ro[s:s + win] ** 2).mean())) + 1e-9)
        row = {"o_t": s / 100.0, "o_db": round(level, 1)}
        if level < a.min_db:
            row["state"] = "quiet"
            rows.append(row)
            continue
        if a.expect_offset is not None:
            lo = max(0, s + int(a.expect_offset * 100) - int(a.search * 100))
            hi = min(len(fu), s + win + int(a.expect_offset * 100) + int(a.search * 100))
        else:
            lo, hi = 0, len(fu)
        c = ncc_multi(fu[lo:hi], seg)
        if len(c) == 0:
            row["state"] = "no-ours"
            rows.append(row)
            continue
        j = int(np.argmax(c))
        best = float(c[j])
        # runner-up outside +-2 s of the best
        mask = np.ones(len(c), bool)
        mask[max(0, j - 200):j + 200] = False
        second = float(c[mask].max()) if mask.any() else 0.0
        u_s = (lo + j) / 100.0
        row.update({"u_t": u_s, "off_s": round(u_s - s / 100.0, 2), "env_ncc": round(best, 3),
                    "env_conf": round(best - second, 3),
                    "u_db": round(20 * np.log10(float(np.sqrt((ru[lo + j:lo + j + win] ** 2).mean())) + 1e-9), 1)})
        if best >= a.env_match:
            wc = wave_check(O, U, s / 100.0, u_s, a.win)
            if wc:
                cs = [w["corr"] for w in wc]
                row["wave_corr_med"] = float(np.median(cs))
                row["wave_corr_max"] = float(np.max(cs))
                row["gain_db_med"] = float(np.median([w["gain_db"] for w in wc]))
                lags = [w["lag_ms"] for w in wc if w["corr"] > 0.5]
                row["lag_ms_span"] = [min(lags), max(lags)] if lags else None
            row["state"] = "match"
            matched_u[lo + j:lo + j + win] = True
        else:
            row["state"] = "UNMATCHED"
        rows.append(row)
    # extra sounds: energetic ours spans never matched
    extra = []
    k = 0
    lvl_u = 20 * np.log10(ru + 1e-9)
    while k < len(fu):
        if lvl_u[k] > a.min_db and not matched_u[k]:
            e = k
            while e < len(fu) and lvl_u[e] > a.min_db - 10 and not matched_u[e]:
                e += 1
            if e - k >= 200:
                extra.append([round(k / 100.0, 1), round((e - k) / 100.0, 1)])
            k = e
        k += 1
    res = {"oracle": a.oracle, "ours": a.ours, "win": a.win, "hop": a.hop, "rows": rows, "ours_unmatched_spans": extra}
    print("%8s %6s | %-9s %8s %7s %6s %6s %7s %7s %s" % ("o_t", "o_db", "state", "u_t", "off_s", "env", "conf",
                                                         "wcorr", "gaindB", "lag_ms"))
    for r in rows:
        print("%8.1f %6.1f | %-9s %8s %7s %6s %6s %7s %7s %s" % (
            r["o_t"], r["o_db"], r["state"], r.get("u_t", ""), r.get("off_s", ""), r.get("env_ncc", ""),
            r.get("env_conf", ""), r.get("wave_corr_med", ""), r.get("gain_db_med", ""), r.get("lag_ms_span", "")))
    print("ours energetic spans never matched (start_s, dur_s):", extra[:40])
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        json.dump(res, open(os.path.join(a.out, "compare.json"), "w"), indent=1)
    return res


def read_markers(path):
    """era markers as (epoch_s, label): a Xenia markers.jsonl (tools/xenia_audio_capture.py) or
    our run log ([APT_REQ] / [APT_LOADMOVIE] / [MOVIE...] lines, local-time stamps)."""
    import re
    import time as _t
    out = []
    if path.endswith(".jsonl"):
        for l in open(path, encoding="utf-8", errors="replace"):
            try:
                m = json.loads(l)
            except ValueError:
                continue
            if m.get("t") == "apt" and str(m.get("path", "")).startswith("screens"):
                out.append((m["t_us"] / 1e6, "apt " + m["path"]))
        return out
    rx = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.(\d{3})\].*?\[(APT_REQ|APT_LOADMOVIE)\] (.*)$")
    for l in open(path, encoding="utf-8", errors="replace"):
        if "[APT_REQ]" not in l and "[APT_LOADMOVIE]" not in l:
            continue
        m = rx.match(l)
        if not m:
            continue
        if m.group(3) == "APT_REQ" and "path='screens" not in m.group(4):
            continue
        t = _t.mktime(_t.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")) + int(m.group(2)) / 1000.0
        lab = m.group(4)
        mm = re.search(r"path='([^']+)'", lab)
        out.append((t, ("apt " + mm.group(1)) if mm else ("loadmovie " + lab[:60])))
    return out


def eras(a):
    A = Afr(a.afr)
    for t, lab in read_markers(a.markers):
        print("%9.2f  %s" % (t - A.t0_epoch, lab))


def spec(a):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    files = [a.afr] + (a.vs or [])
    fig, axes = plt.subplots(len(files), 1, figsize=(16, 3.2 * len(files)), squeeze=False)
    froms = [float(x) for x in (a.__dict__["from"] if isinstance(a.__dict__["from"], list) else [a.__dict__["from"]])]
    for i, f in enumerate(files):
        A = Afr(f)
        st = froms[i] if i < len(froms) else froms[0]
        m = A.mono(sec_to_frame(st), min(A.n, sec_to_frame(st + (a.to - froms[0]))))
        ax = axes[i][0]
        ax.specgram(m, NFFT=1024, Fs=RATE, noverlap=512, cmap="magma", vmin=-140, vmax=-20)
        ax.set_title("%s  from %.1f s" % (os.path.basename(f), st))
        ax.set_ylim(0, 16000)
    fig.tight_layout()
    fig.savefig(a.png, dpi=80)
    print("wrote", a.png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("info")
    p.add_argument("afr")
    p.add_argument("--json")
    p = sp.add_parser("compare")
    p.add_argument("--oracle", required=True)
    p.add_argument("--ours", required=True)
    p.add_argument("--out")
    p.add_argument("--win", type=float, default=10.0)
    p.add_argument("--hop", type=float, default=5.0)
    p.add_argument("--from", type=float, default=0.0)
    p.add_argument("--to", type=float)
    p.add_argument("--min-db", type=float, default=-50.0)
    p.add_argument("--env-match", type=float, default=0.6)
    p.add_argument("--expect-offset", type=float)
    p.add_argument("--search", type=float, default=60.0)
    p = sp.add_parser("eras")
    p.add_argument("afr")
    p.add_argument("markers", help="Xenia markers.jsonl or our run.log")
    p = sp.add_parser("spec")
    p.add_argument("afr")
    p.add_argument("--vs", action="append")
    p.add_argument("--from", type=float, nargs="+", default=[0.0])
    p.add_argument("--to", type=float, required=True)
    p.add_argument("--png", required=True)
    a = ap.parse_args()
    if a.cmd == "info":
        info(a.afr, a.json)
    elif a.cmd == "compare":
        compare(a)
    elif a.cmd == "spec":
        spec(a)
    elif a.cmd == "eras":
        eras(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
