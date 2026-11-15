#!/usr/bin/env python3
"""Find each intro movie's audio inside a game output dump and score it.

iter-463 audio rolls: the game's output (SR_AUDIO_WAV float WAV, or an SDL disk-driver raw
dump) is compared with the movies' own audio decoded offline from the disc (the ASF/XMV files
carved out of DATA/fmvlegal.AST, decoded by stock FFmpeg at 48 kHz). For each reference:

  * lag     - where the movie's audio starts in the dump (seconds from the dump's start);
  * speed   - played-rate / true-rate, from a fit of per-window positions across the movie
              (1.000 = right speed; 0.919 would be 44.1 kHz content played as 48 kHz ...);
  * corr    - median normalised correlation of 0.5 s windows at their best local lag
              (1.0 = same waveform up to gain; ~0 = unrelated), plus the whole-span correlation
              with the fitted speed and no local re-alignment;
  * gaps    - window-to-window jumps of the local lag > 5 ms (dropouts / underrun silence);
  * gain    - least-squares gain of the dump onto the reference over the matched span.
  * found   - MATCH only with >= 3 good (> 0.5) windows and a median window corr >= 0.5; a coarse
              probe hit alone is not a match (negative control: a cutscene the run never played
              passed the 0.3 coarse bar with 0 good windows).

Speeds searched: 0.70 .. 1.15 (step 0.0025) on a 1 s probe, then refined.

Usage:
  python tools/audio_movie_xcorr.py --dump run.wav --ref a.wav --ref b.wav [--json out.json]
  python tools/audio_movie_xcorr.py --raw run.raw --raw-ch 6 --ref ...     (SDL disk driver, f32)
"""
import argparse
import json
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

RATE = 48000
COARSE = 4000


def load_wav(path):
    rate, x = wavfile.read(path, mmap=True)
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1:
        x = x[:, None]
    return rate, x


def mono(x):
    return x.astype(np.float64).sum(axis=1)


def xcorr_norm(hay, needle):
    n = len(needle)
    if len(hay) < n or n == 0:
        return None
    num = signal.fftconvolve(hay, needle[::-1], mode="valid")
    csum = np.concatenate([[0.0], np.cumsum(hay * hay)])
    energy = csum[n:] - csum[:-n]
    den = np.sqrt(np.maximum(energy, 1e-30) * float(np.dot(needle, needle)))
    c = num / den
    c[energy < 1e-9 * n] = 0.0
    return c


def stretch(x, speed):
    """What `x` looks like when played at `speed` (speed<1 = slower = longer)."""
    if abs(speed - 1.0) < 1e-9:
        return x
    newlen = int(round(len(x) / speed))
    return signal.resample(x, newlen) if len(x) < 400000 else np.interp(
        np.arange(newlen) * speed, np.arange(len(x)), x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump")
    ap.add_argument("--raw")
    ap.add_argument("--raw-ch", type=int, default=6)
    ap.add_argument("--ref", action="append", required=True)
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.dump:
        rate, d = load_wav(a.dump)
    elif a.raw:
        rate = RATE
        d = np.fromfile(a.raw, dtype="<f4")
        d = d[: len(d) // a.raw_ch * a.raw_ch].reshape(-1, a.raw_ch)
    else:
        sys.exit("need --dump or --raw")
    if rate != RATE:
        sys.exit("dump rate %d != %d" % (rate, RATE))
    dm = mono(d)
    dur = len(dm) / RATE
    rms = float(np.sqrt(np.mean(dm * dm))) if len(dm) else 0.0
    sec = [float(np.sqrt(np.mean(dm[i:i + RATE] ** 2))) for i in range(0, len(dm) - RATE + 1, RATE)]
    nonsilent = sum(1 for v in sec if v > 1e-3)
    chrms = [float(np.sqrt(np.mean(d[:, c].astype(np.float64) ** 2))) for c in range(d.shape[1])]
    dmc = signal.resample_poly(dm, 1, RATE // COARSE)
    out = dict(dump=a.dump or a.raw, seconds=dur, channels=int(d.shape[1]), rms=rms,
               channel_rms=chrms, nonsilent_seconds=nonsilent, rms_per_second=sec, movies=[])
    print("dump %s: %.1f s, %d ch, mono rms %.5f, non-silent (>1e-3) seconds %d/%d, ch rms %s"
          % (out["dump"], dur, d.shape[1], rms, nonsilent, len(sec),
             " ".join("%.4f" % v for v in chrms)))
    speeds = np.arange(0.70, 1.15001, 0.0025)
    for rp in a.ref:
        rr, r = load_wav(rp)
        if rr != RATE:
            sys.exit("ref rate %d != %d: %s" % (rr, RATE, rp))
        rm = mono(r)
        name = rp.replace("\\", "/").split("/")[-1]
        res = dict(ref=rp, ref_seconds=len(rm) / RATE, found=False)
        # probe: the loudest 1 s in the first 8 s of the reference.
        best_t0, best_e = 0, -1.0
        for t0 in range(0, max(1, min(len(rm) - RATE, 8 * RATE)), RATE // 4):
            e = float(np.dot(rm[t0:t0 + RATE], rm[t0:t0 + RATE]))
            if e > best_e:
                best_t0, best_e = t0, e
        probe = rm[best_t0:best_t0 + RATE]
        best = (-1.0, 1.0, 0)
        for s in speeds:
            ps = signal.resample_poly(stretch(probe, s), 1, RATE // COARSE)
            c = xcorr_norm(dmc, ps)
            if c is None:
                continue
            i = int(np.argmax(c))
            if c[i] > best[0]:
                best = (float(c[i]), float(s), i * (RATE // COARSE))
        coarse_corr, s0, ppos = best
        if coarse_corr < 0.3:
            res.update(coarse_corr=coarse_corr)
            out["movies"].append(res)
            print("  %-34s NOT FOUND (best coarse corr %.3f at speed %.4f)" % (name, coarse_corr, s0))
            continue
        start = ppos - int(round(best_t0 / s0))
        # windows: 0.5 s of the reference, placed at start + off/s0 + the shift so far, local
        # search +-60 ms. When the local search fails, a wide search (+-1.5 s, accept >= 0.8)
        # re-acquires the waveform, so a dropout longer than 60 ms shows up as a gap instead of
        # every later window failing (review: a 190 ms dropout 13 s into the title movie of
        # r_review_logfix1 read "gaps 0" with 25/62 good windows).
        win, search, wide = RATE // 2, int(RATE * 0.06), int(RATE * 1.5)
        pts = []
        shift = 0
        for off in range(0, len(rm) - win, win):
            seg = rm[off:off + win]
            if np.dot(seg, seg) < 1e-5 * win:
                continue
            segs = stretch(seg, s0)
            pred = start + int(round(off / s0)) + shift
            lo, hi = pred - search, pred + len(segs) + search
            if lo < 0 or hi > len(dm):
                continue
            c = xcorr_norm(dm[lo:hi], segs)
            j = int(np.argmax(c))
            pos, cj = lo + j, float(c[j])
            if cj < 0.5:
                wlo, whi = max(0, pred - wide), min(len(dm), pred + len(segs) + wide)
                cw = xcorr_norm(dm[wlo:whi], segs)
                if cw is not None and len(cw):
                    k = int(np.argmax(cw))
                    if cw[k] >= 0.8:
                        pos, cj = wlo + k, float(cw[k])
            if cj > 0.5:
                shift = pos - (start + int(round(off / s0)))
            pts.append((off, pos, cj))
        good = [p for p in pts if p[2] > 0.5]
        speed = None
        gaps = 0
        gap_ms = 0.0
        if len(good) >= 3:
            offs = np.array([p[0] for p in good], float)
            pos = np.array([p[1] for p in good], float)
            # Steps between consecutive good windows: a step that deviates > 5 ms from the
            # coarse-speed expectation is a gap (dropout / inserted silence / skip); the speed is
            # fitted over the remaining steps only, so one gap does not bend it.
            doff, dpos = np.diff(offs), np.diff(pos)
            dev = dpos - doff / s0
            jump = np.abs(dev) > RATE * 0.005
            gaps = int(np.sum(jump))
            gap_ms = float(np.sum(np.abs(dev[jump])) * 1000.0 / RATE)
            if np.sum(~jump) >= 2 and np.sum(dpos[~jump]) > 0:
                speed = float(np.sum(doff[~jump]) / np.sum(dpos[~jump]))
        med = float(np.median([p[2] for p in pts])) if pts else 0.0
        # whole-span corr at the fitted speed, no local re-alignment.
        sp = speed or s0
        full = stretch(rm, sp)
        n = min(len(full), len(dm) - start)
        span_corr = gain = 0.0
        if n > 0 and start >= 0:
            A, B = dm[start:start + n], full[:n]
            den = np.sqrt(np.dot(A, A) * np.dot(B, B))
            span_corr = float(np.dot(A, B) / den) if den > 0 else 0.0
            gain = float(np.dot(A, B) / np.dot(B, B)) if np.dot(B, B) > 0 else 0.0
        # A coarse hit alone is not a match: the negative control (archive/v1_audio/review/negctl,
        # cutscenes from fmv.AST that the intro never plays) cleared the 0.3 coarse bar with 0/169
        # good windows and median window corr 0.09. "found" needs >= 3 good windows (the speed fit)
        # and a median window corr >= 0.5; the right movie scores 0.9999-1.000 there.
        matched = speed is not None and med >= 0.5
        res.update(found=matched, lag_s=start / RATE, coarse_corr=coarse_corr, coarse_speed=s0,
                   speed=speed, median_window_corr=med, windows_good=len(good), windows=len(pts),
                   span_corr=span_corr, gain=gain, gaps=gaps, gap_ms=gap_ms,
                   played_seconds=n / RATE)
        out["movies"].append(res)
        print("  %-34s %s lag %8.3f s  speed %s  win-corr(med) %.3f  good %d/%d  span-corr %.3f  "
              "gain %.3f  gaps %d (%.0f ms)"
              % (name, "MATCH   " if matched else "NO MATCH", res["lag_s"],
                 ("%.4f" % speed) if speed else "n/a", med, len(good),
                 len(pts), span_corr, gain, gaps, gap_ms))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
