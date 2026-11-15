#!/usr/bin/env python3
"""iter-464 controls lane: summarise a move_probe.py timeline (docs/PC_CONTROLS.md 9).

    python tools/controls/analyze_timeline.py <probe.jsonl> [--play play.jsonl] [--pad-log pad_log.txt]

Segments the samples at the probe's `mark` lines (Xenia) or at harness_play's steps (--play, our
build: every step whose name starts with a letter label in the script is a boundary), and prints
per segment: Superman's horizontal displacement split into his FORWARD / RIGHT at the segment's
start (right-handed, y up, local +Z forward: right = (-fz, fx)), the peak horizontal speed from
consecutive positions, the vertical change, and the sequence of mover states (class/def/slot).
An evade shows as a burst sideways or backwards with the mover switching to ANIM; a block as
ANIM with ~no displacement.
"""
import argparse
import json
import math


def facing(q):
    """Local +Z rotated by the unit quaternion q = (x, y, z, w), on the ground plane.
    (golden sound_gameplay: q = (0, 0.435, 0, 0.900) -> (0.783, 0.621))"""
    x, y, z, w = q
    fx = 2 * (w * y + x * z)
    fz = 1 - 2 * (x * x + y * y)
    n = math.hypot(fx, fz) or 1.0
    return fx / n, fz / n


def load(path):
    rows = []
    with open(path) as f:
        for ln in f:
            try:
                rows.append(json.loads(ln))
            except ValueError:
                pass
    return rows


def summarise(label, samples):
    s = [r for r in samples if r.get("pos") and r.get("q")]
    if len(s) < 2:
        print("%-52s  (no samples)" % label[:52])
        return
    p0, p1 = s[0]["pos"], s[-1]["pos"]
    fx, fz = facing(s[0]["q"])
    rx, rz = -fz, fx
    dx, dz = p1[0] - p0[0], p1[2] - p0[2]
    fwd, rgt = dx * fx + dz * fz, dx * rx + dz * rz
    vmax = 0.0
    for a, b in zip(s, s[1:]):
        dt = (b["t"] - a["t"]) / 1000.0
        if dt > 0:
            vmax = max(vmax, math.hypot(b["pos"][0] - a["pos"][0], b["pos"][2] - a["pos"][2]) / dt)
    states, last = [], None
    for r in s:
        st = "%s/%s%s" % (r.get("mover"), r.get("def"), ("/s%d" % r["slot"]) if r.get("slot", -1) >= 0 else "")
        if st != last:
            states.append(st)
            last = st
    print("%-52s fwd %+6.2f m  right %+6.2f m  dy %+6.2f  vmax %5.1f m/s  ctx %s  %s" % (
        label[:52], fwd, rgt, p1[1] - p0[1], vmax, s[-1].get("ctx"), " > ".join(states[:8])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("probe")
    ap.add_argument("--play", help="harness_play.py --out jsonl (our build): segment at its steps")
    a = ap.parse_args()
    rows = load(a.probe)
    samples = [r for r in rows if "t" in r and "mark" not in r and "pad" not in r]
    if a.play:
        bounds = [(r["epoch_ms"], r["step"]) for r in load(a.play) if "epoch_ms" in r and "step" in r
                  and not r["step"].startswith("watch")]
    else:
        # probe marks carry the probe's wall clock: map to epoch via the first sample
        t0 = samples[0]["t"] - samples[0].get("wall", 0) * 1000 if samples else 0
        bounds = [(int(t0 + r["wall"] * 1000), r["mark"]) for r in rows if "mark" in r and "wall" in r]
    bounds.sort()
    for i, (t, label) in enumerate(bounds):
        t_end = bounds[i + 1][0] if i + 1 < len(bounds) else float("inf")
        summarise(label, [r for r in samples if t <= r["t"] < t_end])


if __name__ == "__main__":
    main()
