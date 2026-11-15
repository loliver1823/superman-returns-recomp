"""Compare two SDL replay outputs (difftest --audio-sdl-replay) byte for byte: the SDL2 control vs the SDL3 port.

Lane sdl3-audio (RELEASE_PLAN step 3). Each replay dir holds disk.raw (the device's own bytes, float32 interleaved,
starting with the silence the device played while paused), wav.wav (the SR_AUDIO_WAV observer), expected.raw (our
conversion of the frames for the device's channel count) and result.json. The SDL3 side also replays devices with
a channel count that is not ours (4 / 8 ch: SDL's converter); the SDL2 side's counterpart is
--audio-sdl2-convert's sdl2_stream.raw.

    python tools/audio_sdl_compare.py --a <sdl2 dir> --b <sdl3 dir> [--ch 6] [--json out.json]
    python tools/audio_sdl_compare.py --a <sdl2 dir>/sdl2_stream.raw --b <sdl3 dir>/disk.raw --ch 8

Alignment: each stream is trimmed to its first non-zero sample frame and the two are compared over the shorter
length; a trailing run of silence in either one (the device ran after the last frame) is ignored. Verdict EXACT =
every compared byte equal and neither has extra non-silent audio. Otherwise per channel: RMS of each, the gain
(dB, b/a), the max abs difference and the residual level (dB re a). Exit 0 = EXACT.
"""
import argparse
import json
import math
import os
import sys

import numpy as np


def load(path, ch):
    if os.path.isdir(path):
        path = os.path.join(path, "disk.raw")
    raw = np.fromfile(path, dtype="<f4")
    n = (raw.size // ch) * ch
    return path, raw[:n].reshape(-1, ch)


def trim(x):
    nz = np.flatnonzero(np.any(x != 0, axis=1))
    if nz.size == 0:
        return x[:0], 0, 0
    return x[nz[0]:nz[-1] + 1], int(nz[0]), int(x.shape[0] - nz[-1] - 1)


def db(v):
    return 20 * math.log10(v) if v > 0 else float("-inf")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="control (SDL2): a replay dir or a raw file")
    ap.add_argument("--b", required=True, help="port (SDL3): a replay dir or a raw file")
    ap.add_argument("--ch", type=int, default=0, help="channels (default: the result.json device_ch of --b)")
    ap.add_argument("--json", help="write the verdict here")
    a = ap.parse_args()
    ch = a.ch
    if not ch:
        rj = os.path.join(a.b if os.path.isdir(a.b) else os.path.dirname(a.b), "result.json")
        ch = json.load(open(rj))["device_ch"]
    pa, xa = load(a.a, ch)
    pb, xb = load(a.b, ch)
    ta, lead_a, tail_a = trim(xa)
    tb, lead_b, tail_b = trim(xb)
    n = min(ta.shape[0], tb.shape[0])
    same_len = ta.shape[0] == tb.shape[0]
    exact = n > 0 and same_len and ta.tobytes() == tb.tobytes()
    out = {"a": pa, "b": pb, "channels": ch, "frames_a": int(ta.shape[0]), "frames_b": int(tb.shape[0]),
           "lead_silence_a": lead_a, "lead_silence_b": lead_b, "exact": bool(exact), "per_channel": []}
    if n:
        ca, cb = ta[:n].astype(np.float64), tb[:n].astype(np.float64)
        first = np.flatnonzero(np.any(ta[:n] != tb[:n], axis=1))
        out["first_diff_frame"] = int(first[0]) if first.size else None
        tot_a = float(np.sum(ca * ca)) or 1.0
        tot_b = float(np.sum(cb * cb)) or 1.0
        for c in range(ch):
            ra = math.sqrt(float(np.mean(ca[:, c] ** 2)))
            rb = math.sqrt(float(np.mean(cb[:, c] ** 2)))
            res = math.sqrt(float(np.mean((cb[:, c] - ca[:, c]) ** 2)))
            out["per_channel"].append({
                "ch": c, "rms_a_db": round(db(ra), 2), "rms_b_db": round(db(rb), 2),
                "gain_db": round(db(rb) - db(ra), 3) if ra > 0 and rb > 0 else None,
                "share_a_pct": round(100 * float(np.sum(ca[:, c] ** 2)) / tot_a, 2),
                "share_b_pct": round(100 * float(np.sum(cb[:, c] ** 2)) / tot_b, 2),
                "max_abs_diff": float(np.max(np.abs(cb[:, c] - ca[:, c]))),
                "residual_db_re_a": round(db(res) - db(ra), 2) if ra > 0 and res > 0 else None})
    print("a: %s (%d frames, lead %d)" % (pa, out["frames_a"], lead_a))
    print("b: %s (%d frames, lead %d)" % (pb, out["frames_b"], lead_b))
    for p in out["per_channel"]:
        print("  ch%d  rms a %7.2f dB  b %7.2f dB  gain %s  share a %6.2f%% b %6.2f%%  maxdiff %.3g  resid %s" % (
            p["ch"], p["rms_a_db"], p["rms_b_db"], p["gain_db"], p["share_a_pct"], p["share_b_pct"],
            p["max_abs_diff"], p["residual_db_re_a"]))
    print("VERDICT: %s (%d frames compared%s)" % ("EXACT" if exact else "DIFFERENT", n,
                                                  "" if same_len else ", lengths differ"))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    return 0 if exact else 1


if __name__ == "__main__":
    sys.exit(main())
