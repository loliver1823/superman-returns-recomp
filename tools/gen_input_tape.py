#!/usr/bin/env python3
"""Generate .inpr input-replay tapes for SR_INPUT_REPLAY (kernel/input_recorder.cpp).

Format: FileHeader{magic 'INPR' LE, version 1, frameCount, 0} + N x GamepadState
(<HBBhhhh> = wButtons, LT, RT, LX, LY, RX, RY — host byte order, 12 B/entry).
One entry per XamInputGetState user-0 poll; measured poll rate ~29/s (one per
30 fps frame; [INPUT] log lines at calls 1/100/1000 calibrate any run).

Patterns (iter-456 menu-text verification, both proven live):
  start  — neutral 450 polls (~15 s), then 10-poll START press every 150 polls
           (~0.33 s press / ~5 s period) until 30000, neutral tail. Skips the
           skippable intro movie, enters the title, parks on the storage dialog.
  menu   — same START era to poll 3600, then [DOWN x8, 82 neutral, A x8, 202
           neutral] 300-poll (~10 s) cycles: walks CONTINUE -> the LOAD A GAME?
           dialog -> the save-slot picker -> back to title, in a loop.

Buttons (XINPUT): DPAD_UP 0x0001 DOWN 0x0002 LEFT 0x0004 RIGHT 0x0008
START 0x0010 BACK 0x0020 A 0x1000 B 0x2000 X 0x4000 Y 0x8000.
"""
import argparse
import struct

MAGIC, VERSION = 0x494E5052, 1
START, A, DOWN = 0x0010, 0x1000, 0x0002


def pattern_start(i: int) -> int:
    if i < 450 or i >= 30000:
        return 0
    return START if (i - 450) % 150 < 10 else 0


def pattern_menu(i: int) -> int:
    if i < 450:
        return 0
    if i < 3600:
        return START if (i - 450) % 150 < 10 else 0
    if i < 30000:
        ph = (i - 3600) % 300
        if ph < 8:
            return DOWN
        if 90 <= ph < 98:
            return A
        return 0
    return 0


PATTERNS = {"start": pattern_start, "menu": pattern_menu}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", help="output .inpr path")
    ap.add_argument("--pattern", choices=sorted(PATTERNS), default="start")
    ap.add_argument("--entries", type=int, default=36000,
                    help="tape length in polls (36000 ~= 20 min at 30 Hz)")
    args = ap.parse_args()

    fn = PATTERNS[args.pattern]
    with open(args.out, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, VERSION, args.entries, 0))
        for i in range(args.entries):
            f.write(struct.pack("<HBBhhhh", fn(i), 0, 0, 0, 0, 0, 0))
    pressed = sum(1 for i in range(args.entries) if fn(i))
    print(f"{args.out}: {args.entries} entries, {pressed} pressed polls "
          f"({args.pattern})")


if __name__ == "__main__":
    main()
