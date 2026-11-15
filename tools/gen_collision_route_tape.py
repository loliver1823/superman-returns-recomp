#!/usr/bin/env python3
"""gen_collision_route_tape.py - fixed-route input tapes for the collision test (iter-463): from the
gameplay anchor, take off and fly Superman from the spawn into ONE named building, slowly or fast,
then keep pushing into the wall. Writes a v2 .inpr (kernel/input_recorder.cpp: entries are
time-stamped in ms after the anchor = the guest's "screens/hud/main_hud" request), which plays
in OUR build through SR_INPUT_REPLAY and on Xenia through tools/xenia_collision_route.py (the
same anchor, hooked on the same guest function 0x826D2690).

Route (the natural spawn, retail USA, empty save / NEW GAME):
  spawn (-2267.4, 1177.0, -411.6); the spawn camera looks along +z (golden NDC test of the
  sound_gameplay golden: only points on the +z side of Superman are in the frustum).
  Target: Havok_Box01, hkListShape, body translation (-2281.447, 1176.876, -365.411),
  AABB (-2301.17, 1171.95, -375.39)..(-2262.25, 1374.14, -354.79): its -z face is 36 m straight
  ahead of the spawn. The golden-call differential (scripts/difftest/havok/gen_mover_cases.py,
  golden 0x022CBE40 'ax2+' cases) stops the capsule centre at z = -374.81 (0.58 m inside the
  AABB face) against that wall, at 25 and 60 m/s, recomp == interpreter bit-exact.

Timeline (after the anchor; the input pattern follows the user's own tutorial tape
archive/v1_tape3/tutorial.inpr, whose run reached Midtown in flight):
  A tap at 10.7 s (the in-engine scene that waits for input), idle, Y held 1.2 s at --takeoff
  (43.3 s in the user's tape; Y = the movement-mode toggle, INPUT_EVENT_GAME_ACTION_TOGGLE_MOVEMENT_MODE),
  hover 2 s, then the left stick forward (+y) for --push seconds:
    slow: stick at --slow-stick (default 0.45) of full, no triggers;
    fast: stick full and RT held (flight thrust) the whole time.
  Then neutral input. The right stick is never touched, so the camera keeps the spawn heading.
  ASSUMPTIONS to check in the run's [COLL_ROUTE]/[COLL_MOVER] lines: loco=FLY after the takeoff
  (if not, the run is INCONCLUSIVE, not a FAIL) and the heading (+z) at the approach.

    python tools/gen_collision_route_tape.py archive/v1_invest/collision/route/slow.inpr --mode slow
    python tools/gen_collision_route_tape.py archive/v1_invest/collision/route/fast.inpr --mode fast
    python tools/gen_collision_route_tape.py archive/v1_invest/collision/route/sweep.inpr --mode sweep --minutes 8
        (sweep: full stick + RT for N minutes after the takeoff, with seeded steering / pitch pulses.
         MEASURED (iter-463 roll2): the power launch puts him at ~253 m, +ry only climbs and lx does
         not hold a heading, so this flies ABOVE the rooftops - 0.9 of 9.9 min below 300 m)
    python tools/gen_collision_route_tape.py archive/v1_invest/collision/route/dive.inpr --mode dive --minutes 9
        (dive: 13.5 s cycles of hold-Y 1.2 s (ground: power launch; air: power drop), full stick + RT,
         a right-stick yaw pulse (flight follows the camera) and a full -ry pitch-down pulse. MEASURED
         (roll4): 2.5 min of fast flight, all of it below 300 m, 357 fast contact ticks - the tape
         that actually flies fast at buildings. Controls: ENGINE_MAP 6j.1)
    python tools/gen_collision_route_tape.py <out> --dump <tape.inpr>     # print a tape's timeline
"""
import argparse
import os
import struct

MAGIC, VERSION = 0x494E5052, 2
A, Y = 0x1000, 0x8000
STEP_MS = 33


def entry(ms, buttons=0, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0):
    # TimedEntry {u32 ms, u16 afterAnchor, u16 pad, GamepadState <HBBhhhh>}
    return struct.pack("<IHH", ms, 1, 0) + struct.pack("<HBBhhhh", buttons, lt, rt, lx, ly, rx, ry)


def build(mode, takeoff, push, slow_stick, a_at=10.7, total=None):
    t_a0, t_a1 = int(a_at * 1000), int(a_at * 1000) + 220
    t_y0, t_y1 = int(takeoff * 1000), int(takeoff * 1000) + 1200
    t_go = t_y1 + 2000
    t_stop = t_go + int(push * 1000)
    end = int((total or (t_stop / 1000 + 10)) * 1000)
    ly_val = 32767 if mode == "fast" else int(32767 * slow_stick)
    rt_val = 255 if mode == "fast" else 0
    out = []
    for ms in range(0, end, STEP_MS):
        b = lt = rt = lx = ly = 0
        if t_a0 <= ms < t_a1:
            b |= A
        if t_y0 <= ms < t_y1:
            b |= Y
        if t_go <= ms < t_stop:
            ly, rt = ly_val, rt_val
        out.append(entry(ms, b, lt, rt, lx, ly))
    phases = {"A": (t_a0, t_a1), "Y(takeoff)": (t_y0, t_y1), "push": (t_go, t_stop), "end": (end, end)}
    return out, phases


def build_sweep(takeoff, minutes, a_at=10.7, seed=463):
    """iter-463 roll 2: fast flight AT buildings for `minutes` after the takeoff - full forward stick
    + RT the whole time, with steering pulses (left stick X, 0.4-1.6 s, both signs) and pitch pulses
    (right stick Y, small, alternating) every 3-7 s, so the route mixes head-on, oblique and grazing
    wall contacts and keeps changing altitude. Deterministic (seeded)."""
    import random
    rnd = random.Random(seed)
    t_a0, t_a1 = int(a_at * 1000), int(a_at * 1000) + 220
    t_y0, t_y1 = int(takeoff * 1000), int(takeoff * 1000) + 1200
    t_go = t_y1 + 2000
    t_stop = t_go + int(minutes * 60000)
    end = t_stop + 10000
    pulses = []          # (start, end, lx, ry)
    t = t_go + 4000
    pitch_sign = -1
    while t < t_stop:
        dur = rnd.randint(400, 1600)
        lx = rnd.choice((-1, 1)) * rnd.randint(12000, 30000)
        ry = 0
        if rnd.random() < 0.5:
            ry = pitch_sign * rnd.randint(6000, 14000)
            pitch_sign = -pitch_sign
        pulses.append((t, t + dur, lx, ry))
        t += dur + rnd.randint(3000, 7000)
    out, pi = [], 0
    for ms in range(0, end, STEP_MS):
        b = lx = ly = ry = rt = 0
        if t_a0 <= ms < t_a1:
            b |= A
        if t_y0 <= ms < t_y1:
            b |= Y
        if t_go <= ms < t_stop:
            ly, rt = 32767, 255
            while pi < len(pulses) and pulses[pi][1] <= ms:
                pi += 1
            if pi < len(pulses) and pulses[pi][0] <= ms < pulses[pi][1]:
                lx, ry = pulses[pi][2], pulses[pi][3]
        out.append(entry(ms, b, 0, rt, lx, ly, 0, ry))
    phases = {"A": (t_a0, t_a1), "Y(takeoff)": (t_y0, t_y1), "sweep": (t_go, t_stop), "end": (end, end)}
    return out, phases


def build_dive(minutes, a_at=10.7, seed=4634, start=46.0):
    """iter-463 roll 4: fast flight at building height (see the docstring)."""
    import random
    rnd = random.Random(seed)
    t = int(start * 1000)
    t_end = t + int(minutes * 60000)
    ev = []
    while t < t_end:
        ev.append((t, t + 1200, "b", Y))
        f0 = t + 2000
        ev.append((f0, t + 12000, "ly", 32767))
        ev.append((f0, t + 12000, "rt", 255))
        r0 = f0 + rnd.randint(0, 1500)
        ev.append((r0, r0 + rnd.randint(500, 2000), "rx", rnd.choice((-1, 1)) * rnd.choice((8000, 16000, 26000))))
        d0 = t + 4000
        ev.append((d0, d0 + rnd.randint(600, 1600), "ry", -32767))
        t += 13500
    end = t + 3000
    t_a0, t_a1 = int(a_at * 1000), int(a_at * 1000) + 220
    out = []
    for ms in range(0, end, STEP_MS):
        f = {"b": A if t_a0 <= ms < t_a1 else 0, "lt": 0, "rt": 0, "lx": 0, "ly": 0, "rx": 0, "ry": 0}
        for s0, e0, key, v in ev:
            if s0 <= ms < e0:
                f[key] = (f[key] | v) if key == "b" else v
        out.append(entry(ms, f["b"], f["lt"], f["rt"], f["lx"], f["ly"], f["rx"], f["ry"]))
    return out, {"A": (t_a0, t_a1), "dive cycles": (int(start * 1000), t), "end": (end, end)}


def dump(path):
    d = open(path, "rb").read()
    mag, ver, n, _ = struct.unpack_from("<IIII", d, 0)
    print("magic %08X version %d entries %d" % (mag, ver, (len(d) - 16) // 20))
    prev = None
    for i in range(16, len(d) - 19, 20):
        ms, aa, _p, b, lt, rt, lx, ly, rx, ry = struct.unpack_from("<IHHHBBhhhh", d, i)
        key = (aa, b, lt > 30, rt > 30, lx // 4096, ly // 4096, rx // 4096, ry // 4096)
        if key != prev:
            print("%8.2fs anchor=%d btn=%04X LT=%3d RT=%3d L=(%6d,%6d) R=(%6d,%6d)" % (ms / 1000, aa, b, lt, rt, lx, ly, rx, ry))
            prev = key


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--mode", choices=["slow", "fast", "sweep", "dive"], default="slow")
    ap.add_argument("--minutes", type=float, default=8.0, help="sweep/dive: minutes of flight")
    ap.add_argument("--takeoff", type=float, default=43.3, help="s after the anchor (Y hold starts)")
    ap.add_argument("--a-at", type=float, default=10.7, help="s after the anchor (A tap)")
    ap.add_argument("--push", type=float, default=12.0, help="s of forward stick (approach + push into the wall)")
    ap.add_argument("--slow-stick", type=float, default=0.45)
    ap.add_argument("--dump", help="print this tape's timeline and exit")
    a = ap.parse_args()
    if a.dump:
        dump(a.dump)
        return
    if a.mode == "sweep":
        ents, phases = build_sweep(a.takeoff, a.minutes, a.a_at)
    elif a.mode == "dive":
        ents, phases = build_dive(a.minutes, a.a_at)
    else:
        ents, phases = build(a.mode, a.takeoff, a.push, a.slow_stick, a.a_at)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, VERSION, len(ents), 0))
        f.write(b"".join(ents))
    print("%s: %d entries (%s) %s" % (a.out, len(ents), a.mode,
                                       ", ".join("%s %.1f-%.1fs" % (k, v[0] / 1000, v[1] / 1000) for k, v in phases.items())))


if __name__ == "__main__":
    main()
