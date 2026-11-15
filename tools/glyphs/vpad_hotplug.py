#!/usr/bin/env python3
"""vpad_hotplug.py - plug, press and unplug VIRTUAL pads (ViGEmBus + vgamepad) on a schedule.

The hot-plug test instrument of the iter-464 input lane (docs/INPUT_GLYPHS.md "verification").
It creates real system game controllers - an Xbox 360 pad and a DualShock 4 - that SDL sees
exactly like plugged-in hardware, so the port's hot-plug path runs for real. It never touches
the game process; the game only sees the pads through SDL -> XamInputGetState.

    python tools/glyphs/vpad_hotplug.py --plan quick           # with difftest --host-input-live
    python tools/glyphs/vpad_hotplug.py --plan fe --log <run.log> --mark <out>/marks.txt

A roll opens these pads with SR_HARNESS_VPAD=1 (iter-466: ONLY ViGEm virtual pads, never the owner's
controller; kernel/vpad_filter.hpp). Never use SR_HARNESS_REAL_INPUT=1 for a roll. The DualShock 4
target may need SDL's HIDAPI driver, which vpad-only mode keeps off; the Xbox 360 target works.
Plans are lists of steps; `wait` steps on --log poll the run log for a regex. Every action is
written to stdout and to --mark with a wall-clock stamp, so frames can be matched to actions.
Requires ViGEmBus (installed for tools/xenia_pad.py) and `pip install vgamepad`.
"""
import argparse
import os
import re
import sys
import time

import vgamepad as vg

X = vg.XUSB_BUTTON
D = vg.DS4_BUTTONS
DS = vg.DS4_SPECIAL_BUTTONS
X_BTN = {"A": X.XUSB_GAMEPAD_A, "B": X.XUSB_GAMEPAD_B, "X": X.XUSB_GAMEPAD_X, "Y": X.XUSB_GAMEPAD_Y,
         "START": X.XUSB_GAMEPAD_START, "BACK": X.XUSB_GAMEPAD_BACK, "GUIDE": X.XUSB_GAMEPAD_GUIDE,
         "UP": X.XUSB_GAMEPAD_DPAD_UP, "DOWN": X.XUSB_GAMEPAD_DPAD_DOWN,
         "LEFT": X.XUSB_GAMEPAD_DPAD_LEFT, "RIGHT": X.XUSB_GAMEPAD_DPAD_RIGHT,
         "LB": X.XUSB_GAMEPAD_LEFT_SHOULDER, "RB": X.XUSB_GAMEPAD_RIGHT_SHOULDER}
D_BTN = {"A": D.DS4_BUTTON_CROSS, "B": D.DS4_BUTTON_CIRCLE, "X": D.DS4_BUTTON_SQUARE,
         "Y": D.DS4_BUTTON_TRIANGLE, "START": D.DS4_BUTTON_OPTIONS, "BACK": D.DS4_BUTTON_SHARE,
         "LB": D.DS4_BUTTON_SHOULDER_LEFT, "RB": D.DS4_BUTTON_SHOULDER_RIGHT}

PLANS = {
    # A bench check against `build/difftest.exe --host-input-live 40` (no game).
    "quick": [
        ("sleep", 3), ("plug", "x360"), ("sleep", 2), ("press", "x360", "A", 400), ("sleep", 1),
        ("stick", "x360", 32767, 0, 400), ("sleep", 1), ("trigger", "x360", 255, 400), ("sleep", 1),
        ("plug", "ds4"), ("sleep", 3), ("press", "ds4", "B", 400), ("sleep", 2),
        ("unplug", "ds4"), ("sleep", 3), ("press", "x360", "GUIDE", 300), ("sleep", 2),
        ("unplug", "x360"), ("sleep", 2), ("plug", "ds4"), ("sleep", 3), ("unplug", "ds4"), ("sleep", 1),
    ],
    # The front-end hot-plug roll (scenario tools/scenarios/input_hotplug_fe.json runs alongside):
    # title -> START on the Xbox pad -> the storage popup (A prompt) -> a DualShock 4 takes over
    # (PS prompts) -> it is unplugged (user 0 NOT CONNECTED) -> the Xbox pad's Guide button
    # takes player 1 back (360 prompts) -> A on the Xbox pad = START NEW GAME (main_hud).
    "fe": [
        ("wait", r"\[NAT_FE\] #256 natural FE master tick", 240),
        ("plug", "x360"), ("sleep", 3),
        ("press_until", "x360", "START", 300, r"\[APT_SCREEN\] path='main'|\[APT_LOADMOVIE\].*\(\"main\"\)", 90, 6),
        ("wait", r"\[APT_REQ\] #\d+ path='\.\./common/ButtonHints/XBox360/HintB'", 240),
        ("sleep", 14), ("mark", "x360-title-ui"),
        ("press_until", "x360", "START", 300,
         r"XamShowDeviceSelectorUI\(user=|\[APT_REQ\] #\d+ path='screens/misc/ScrGenPop'", 40, 3),
        ("sleep", 10), ("mark", "x360-popup"),
        ("plug", "ds4"), ("sleep", 3), ("press", "ds4", "PS", 300), ("sleep", 10), ("mark", "ds4-popup"),
        ("unplug", "ds4"), ("sleep", 10), ("mark", "ds4-unplugged"),
        ("press", "x360", "GUIDE", 300), ("sleep", 10), ("mark", "x360-back"),
        ("press_until", "x360", "A", 400, r"\[APT_REQ\] #\d+ path='screens/hud/main_hud'", 60, 3),
        ("sleep", 15), ("mark", "after-accept"),
    ],
    # iter-465 polish (scenario tools/scenarios/polish_display_glyphs.json runs alongside): the
    # harness keyboard is player 1 on the title UI; a DualShock 4 is plugged and L1 pressed (L1
    # does nothing on these screens) -> PlayStation prompts; the scenario's ENTER takes player 1
    # back to the keyboard (key caps on the MAIN MENU); L1 again -> PlayStation prompts on the
    # MAIN MENU; the scenario's DOWN takes it back once more. The pad is unplugged only after
    # the keyboard owns player 1 (an unplugged player 1 would be NOT CONNECTED).
    # iter-466 gfx (scenario tools/scenarios/gfxset_settings.json runs alongside): the PC SETTINGS
    # screen driven by an Xbox pad after the keyboard and mouse tours end on the CONTROLS tab:
    # RB wraps to DISPLAY, RB = GRAPHICS, UP x6 to the top row (the tab remembers its last focus),
    # DOWN to ANISOTROPIC FILTERING, A opens its list, DOWN x2 + A picks, Y applies, B leaves.
    "gfxset": [
        ("wait", r"\[PC_SETTINGS\] tab CONTROLS", 900),
        ("sleep", 4), ("plug", "x360"), ("sleep", 4), ("press", "x360", "RB", 250), ("sleep", 3),
        ("press", "x360", "RB", 250), ("mark", "pad-graphics"), ("sleep", 6),
        ("press", "x360", "UP", 200), ("sleep", 0.6), ("press", "x360", "UP", 200), ("sleep", 0.6),
        ("press", "x360", "UP", 200), ("sleep", 0.6), ("press", "x360", "UP", 200), ("sleep", 0.6),
        ("press", "x360", "UP", 200), ("sleep", 0.6), ("press", "x360", "UP", 200), ("sleep", 1),
        ("press", "x360", "DOWN", 200), ("sleep", 1), ("press", "x360", "A", 200), ("mark", "pad-list"),
        ("sleep", 8), ("press", "x360", "DOWN", 200), ("sleep", 0.8), ("press", "x360", "DOWN", 200),
        ("sleep", 0.8), ("press", "x360", "A", 200),
        ("sleep", 3), ("press", "x360", "Y", 250), ("mark", "pad-apply"), ("sleep", 8),
        ("press", "x360", "B", 250), ("mark", "pad-back"), ("sleep", 4), ("unplug", "x360"),
    ],
    "polish": [
        ("wait", r"\[APT_REQ\] #\d+ path='\.\./common/ButtonHints/XBox360/HintB'", 600),
        ("sleep", 18), ("plug", "ds4"), ("sleep", 4), ("press", "ds4", "LB", 400), ("mark", "ds4-title"),
        ("wait", r"\[SR_FSCMD\].*claim LoadCreate", 240),
        ("sleep", 12), ("press", "ds4", "LB", 400), ("mark", "ds4-menu"),
        ("wait", r"\[SR_FSCMD\].*menu button=2", 240),
        ("sleep", 2), ("unplug", "ds4"), ("mark", "ds4-unplugged"),
    ],
}


class Pads:
    def __init__(self):
        self.p = {}

    def plug(self, kind):
        if kind in self.p:
            return
        self.p[kind] = vg.VX360Gamepad() if kind == "x360" else vg.VDS4Gamepad()
        self.p[kind].update()

    def unplug(self, kind):
        pad = self.p.pop(kind, None)
        if pad is not None:
            del pad  # vgamepad removes the device when the target is freed

    def press(self, kind, btn, ms):
        pad = self.p[kind]
        if kind == "ds4" and btn == "PS":
            pad.press_special_button(special_button=DS.DS4_SPECIAL_BUTTON_PS)
            pad.update(); time.sleep(ms / 1000.0)
            pad.release_special_button(special_button=DS.DS4_SPECIAL_BUTTON_PS)
            pad.update()
            return
        b = (X_BTN if kind == "x360" else D_BTN)[btn]
        pad.press_button(button=b); pad.update(); time.sleep(ms / 1000.0)
        pad.release_button(button=b); pad.update()

    def stick(self, kind, x, y, ms):
        pad = self.p[kind]
        if kind == "x360":
            pad.left_joystick(x_value=x, y_value=y)
        else:
            pad.left_joystick(x_value=128 + x // 258, y_value=128 - y // 258)
        pad.update(); time.sleep(ms / 1000.0)
        if kind == "x360":
            pad.left_joystick(x_value=0, y_value=0)
        else:
            pad.left_joystick(x_value=128, y_value=128)
        pad.update()

    def trigger(self, kind, v, ms):
        pad = self.p[kind]
        pad.right_trigger(value=v); pad.update(); time.sleep(ms / 1000.0)
        pad.right_trigger(value=0); pad.update()


class LogTail:
    def __init__(self, path):
        self.path, self.pos, self.buf = path, 0, ""

    def find(self, rx, timeout):
        end = time.time() + timeout
        pat = re.compile(rx)
        while time.time() < end:
            if self.path and os.path.isfile(self.path):
                with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(self.pos)
                    chunk = f.read()
                    self.pos = f.tell()
                self.buf = (self.buf + chunk)[-200000:]
                for line in self.buf.splitlines():
                    if pat.search(line):
                        self.buf = ""
                        return line
            time.sleep(0.25)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", default="quick", choices=sorted(PLANS))
    ap.add_argument("--log", help="run log to watch (wait / press_until steps)")
    ap.add_argument("--log-glob", help="directory to find the newest run.log in (sr_drive -Out)")
    ap.add_argument("--mark", help="append action marks here")
    a = ap.parse_args()
    marks = open(a.mark, "a", encoding="utf-8") if a.mark else None

    def note(msg):
        line = f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {msg}"
        print(line, flush=True)
        if marks:
            marks.write(line + "\n"); marks.flush()

    def log_path():
        if a.log:
            return a.log
        if a.log_glob:
            best = None
            for root, _d, files in os.walk(a.log_glob):
                for fn in files:
                    if fn == "run.log":
                        p = os.path.join(root, fn)
                        if best is None or os.path.getmtime(p) > os.path.getmtime(best):
                            best = p
            return best
        return None

    pads = Pads()
    tail = LogTail(None)
    rc = 0
    try:
        for step in PLANS[a.plan]:
            op = step[0]
            if tail.path is None:
                tail.path = log_path()
            if op == "sleep":
                time.sleep(step[1])
            elif op == "plug":
                pads.plug(step[1]); note(f"plug {step[1]}")
            elif op == "unplug":
                pads.unplug(step[1]); note(f"unplug {step[1]}")
            elif op == "press":
                note(f"press {step[1]} {step[2]} {step[3]}ms"); pads.press(step[1], step[2], step[3])
            elif op == "stick":
                note(f"stick {step[1]} {step[2]},{step[3]}"); pads.stick(step[1], step[2], step[3], step[4])
            elif op == "trigger":
                note(f"trigger {step[1]} {step[2]}"); pads.trigger(step[1], step[2], step[3])
            elif op == "mark":
                note(f"MARK {step[1]}")
            elif op == "wait":
                while tail.path is None:
                    time.sleep(1); tail.path = log_path()
                hit = tail.find(step[1], step[2])
                note(f"wait {'HIT' if hit else 'TIMEOUT'} /{step[1]}/")
                if not hit:
                    rc = 1
                    break
            elif op == "press_until":
                _, kind, btn, ms, rx, timeout, tries = step
                hit = None
                for i in range(tries):
                    note(f"press {kind} {btn} {ms}ms (try {i + 1}/{tries})")
                    pads.press(kind, btn, ms)
                    hit = tail.find(rx, timeout / tries)
                    if hit:
                        break
                note(f"press_until {'HIT' if hit else 'TIMEOUT'} /{rx}/")
                if not hit:
                    rc = 1
    finally:
        for k in list(pads.p):
            pads.unplug(k)
            note(f"unplug {k} (end)")
    sys.exit(rc)


if __name__ == "__main__":
    main()
