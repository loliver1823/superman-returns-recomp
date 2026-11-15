#!/usr/bin/env python3
"""xenia_pad_tape.py - play a v2 .inpr input tape (tools/gen_collision_route_tape.py, or a recorded
SR_INPUT_RECORD tape) on a virtual Xbox 360 pad for Xenia, anchored like our build: playback
starts when --anchor-file appears (written by tools/xenia_collision_route.py when the guest
requests "screens/hud/main_hud", the same event kernel/fe_flow_watch.cpp anchors our tapes on).

Before the anchor it plays an optional timed button schedule (--pre) to get through the front
end (tools/XENIA_HARNESS.md flow: PRESS START ~115 s after launch, then "Select a gamer profile"
-> DOWN, A). Buttons, triggers and both sticks are applied exactly as the tape's GamepadState.
Needs vgamepad + ViGEmBus (the Python that has vgamepad; here Python 3.11). Start it BEFORE
Xenia so Xenia enumerates the pad.

    python tools/xenia_pad_tape.py --tape archive/v1_invest/collision/route/slow.inpr \\
        --anchor-file archive/v1_invest/collision/route/xenia_slow/anchor.txt \\
        --pre "120:START,140:START,160:DOWN,162:A,200:A" [--log <file>]
"""
import argparse
import os
import struct
import time

import vgamepad as vg

BTN_BITS = [0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200,
            0x1000, 0x2000, 0x4000, 0x8000]
NAMES = {"UP": 0x0001, "DOWN": 0x0002, "LEFT": 0x0004, "RIGHT": 0x0008, "START": 0x0010, "BACK": 0x0020,
         "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000}


def load_tape(path):
    d = open(path, "rb").read()
    mag, ver, _n, _ = struct.unpack_from("<IIII", d, 0)
    if mag != 0x494E5052 or ver != 2:
        raise SystemExit("%s: not a v2 INPR tape" % path)
    out = []
    for i in range(16, len(d) - 19, 20):
        ms, aa, _p, b, lt, rt, lx, ly, rx, ry = struct.unpack_from("<IHHHBBhhhh", d, i)
        if aa:
            out.append((ms, b, lt, rt, lx, ly, rx, ry))
    return out


def apply(gp, st):
    _ms, b, lt, rt, lx, ly, rx, ry = st
    for bit in BTN_BITS:
        btn = vg.XUSB_BUTTON(bit)
        if b & bit:
            gp.press_button(button=btn)
        else:
            gp.release_button(button=btn)
    gp.left_trigger(value=lt)
    gp.right_trigger(value=rt)
    gp.left_joystick(x_value=lx, y_value=ly)
    gp.right_joystick(x_value=rx, y_value=ry)
    gp.update()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tape", required=True)
    ap.add_argument("--anchor-file", required=True)
    ap.add_argument("--pre", default="", help="comma list sec:BUTTON[+BUTTON] after this script starts, until the anchor")
    ap.add_argument("--log", default=None)
    ap.add_argument("--max-wait", type=float, default=1200.0, help="s to wait for the anchor")
    ap.add_argument("--cmd-file", default=None,
                    help="iter-467: before the anchor, also press the tokens written to this file "
                         "(xenia_pad.py syntax: A, @START, DOWN+A ...; the file is deleted once read)")
    a = ap.parse_args()
    tape = load_tape(a.tape)
    logf = open(a.log or (os.path.splitext(a.anchor_file)[0] + "_pad.log"), "a")

    def log(m):
        logf.write("%.3f %s\n" % (time.time(), m))
        logf.flush()

    gp = vg.VX360Gamepad()
    gp.update()
    log("pad created; tape %s: %d entries, %.1f s" % (a.tape, len(tape), tape[-1][0] / 1000 if tape else 0))
    pre = []
    for tok in filter(None, (x.strip() for x in a.pre.split(","))):
        t, names = tok.split(":")
        pre.append((float(t), sum(NAMES[n] for n in names.upper().split("+"))))
    t0 = time.time()
    while not os.path.exists(a.anchor_file):
        now = time.time() - t0
        while pre and pre[0][0] <= now:
            _t, bits = pre.pop(0)
            apply(gp, (0, bits, 0, 0, 0, 0, 0, 0))
            time.sleep(0.25)
            apply(gp, (0, 0, 0, 0, 0, 0, 0, 0))
            log("pre %04X at %.1f s" % (bits, now))
        if a.cmd_file and os.path.exists(a.cmd_file):
            try:
                txt = open(a.cmd_file).read().strip()
                os.remove(a.cmd_file)
            except OSError:
                txt = ""
            for tok in txt.split():
                names = [n for n in tok.lstrip("@").upper().split("+") if n in NAMES]
                if not names:
                    continue
                apply(gp, (0, sum(NAMES[n] for n in names), 0, 0, 0, 0, 0, 0))
                time.sleep(0.45 if tok.startswith("@") else 0.12)
                apply(gp, (0, 0, 0, 0, 0, 0, 0, 0))
                time.sleep(0.45)
                log("cmd %s at %.1f s" % ("+".join(names), now))
        if now > a.max_wait:
            log("no anchor after %.0f s - giving up" % now)
            return 2
        time.sleep(0.05)
    ta = time.time()
    log("anchor seen (%.1f s after start) - playing the tape" % (ta - t0))
    last = None
    for st in tape:
        delay = ta + st[0] / 1000.0 - time.time()
        if delay > 0:
            time.sleep(delay)
        key = st[1:]
        if key != last:
            apply(gp, st)
            log("t=%.2f btn=%04X LT=%d RT=%d L=(%d,%d) R=(%d,%d)" % ((st[0] / 1000.0,) + st[1:]))
            last = key
    apply(gp, (0, 0, 0, 0, 0, 0, 0, 0))
    log("tape done; neutral")
    time.sleep(30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
