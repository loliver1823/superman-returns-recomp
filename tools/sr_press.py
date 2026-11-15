#!/usr/bin/env python3
"""sr_press.py - THE canonical one-shot "press buttons in a running background game" helper
(iter-466 tools consolidation; folds archive/v1_*/press.py, nav.py and v1_smoke/drive.py).

    python tools/sr_press.py --instance <SR_INSTANCE_ID> [--keymap player|legacy]
        [--log superman_returns_<id>.log] [--frames <present dump dir>] [--gap 600] STEP [STEP ...]

STEP
    A B X Y START BACK UP DOWN LEFT RIGHT LB RB LSU LSD LSL LSR RSU RSD RSL RSR [:holdMs]
                       a PAD BUTTON name, translated to the key the active keymap binds to it
    key=<SDL name>[:holdMs]   a raw key (names as in tools/controls/harness_play.py SC: F11, Tab,
                       "Left Shift", Return ...) - use it for gameplay keys of the player keymap
    wait:<sec>         sleep
    await:<regex>[:<sec>]   wait for a NEW --log line matching (since the previous press; default 120 s)
    grep:<regex>       report the last --log line matching (whole log, no wait)
    shot:<name>        save the newest --frames present_*.bmp as archive/sr_press_view/<name>.png

Keymaps (the game reads channel flags bit0; --keymap sets it, otherwise the current bit is used):
  legacy  kernel/host_automation.cpp mapKeysToPad - the sr_drive test map:
          Return=START, Esc/Backspace=BACK, arrows=D-pad, Q/E=LB/RB, Z/Space=A, X=B, C=X, V=Y,
          WASD=left stick, IJKL=right stick.
  player  kernel/pc_controls_bindings.cpp defaultBindings (what a player has) - menus only:
          Return=A (confirm) and START, Esc=B (back), Backspace=BACK, arrows=D-pad, E=X, F=Y.
          LB/RB and sticks have no menu key here: send key=... for gameplay (WASD, mouse = harness_play).
Each press prints the game's poll counter before/after: "NOT DELIVERED" means the game did not
poll the channel during the hold (frozen, or the wrong --instance).

Rules: talks only to the harness keyboard channel Local\\SR_HARNESS_INPUT_<instance> (TOOLKIT 1.8a):
no focus, no SendInput, the real keyboard is untouched. The game must already be running
(launch it with tools/sr_roll.ps1 or tools/sr_drive.ps1 while holding the game slot).
Scripted multi-step input with mouse/read-back assertions: tools/controls/harness_play.py.
"""
import argparse
import glob
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "controls"))
from harness_play import SC, OFF_FLAGS, OFF_KEYS, OFF_POLLS, open_channel  # noqa: E402

# pad button -> key name, per keymap (see the docstring for the sources)
LEGACY = {"START": "RETURN", "BACK": "ESCAPE", "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT",
          "LB": "Q", "RB": "E", "A": "Z", "B": "X", "X": "C", "Y": "V",
          "LSU": "W", "LSD": "S", "LSL": "A", "LSR": "D", "RSU": "I", "RSD": "K", "RSL": "J", "RSR": "L"}
PLAYER = {"START": "RETURN", "A": "RETURN", "B": "ESCAPE", "BACK": "BACKSPACE", "X": "E", "Y": "F",
          "UP": "UP", "DOWN": "DOWN", "LEFT": "LEFT", "RIGHT": "RIGHT"}
def _main_root():
    """The main checkout (lane worktrees share its archive/, disc and build): SR_MAIN_ROOT, else the
    parent of git's common directory, else this repo."""
    if os.environ.get("SR_MAIN_ROOT"):
        return os.environ["SR_MAIN_ROOT"]
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        common = subprocess.run(["git", "-C", here, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return os.path.abspath(os.path.join(here, "..", "..") if os.path.basename(here) != "tools" else os.path.join(here, ".."))


MAIN = _main_root()


def polls(m):
    return struct.unpack_from("<I", m, OFF_POLLS)[0]


def set_keymap(m, player):
    f = struct.unpack_from("<I", m, OFF_FLAGS)[0]
    f = (f | 1) if player else (f & ~1)
    struct.pack_into("<I", m, OFF_FLAGS, f)
    return f


def press(m, keyname, hold_ms, label):
    sc = SC.get(keyname.upper())
    if sc is None:
        sys.exit("sr_press: unknown key %r (see SC in tools/controls/harness_play.py)" % keyname)
    before = polls(m)
    m[OFF_KEYS + sc] = 1
    try:
        time.sleep(hold_ms / 1000.0)
    finally:
        m[OFF_KEYS + sc] = 0   # key-up is guaranteed: a stuck key poisons everything after
    after = polls(m)
    print("%s (%s) held %d ms: polls %d -> %d%s" % (label, keyname, hold_ms, before, after,
          "" if after > before else "  NOT DELIVERED"), flush=True)


def log_read(path, start):
    try:
        with open(path, "rb") as f:
            f.seek(max(0, start))
            return f.read()
    except OSError:
        return b""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__[__doc__.index("STEP"):])
    ap.add_argument("--instance", required=True, help="the game's SR_INSTANCE_ID")
    ap.add_argument("--keymap", choices=["player", "legacy"], help="set the channel keymap (default: keep)")
    ap.add_argument("--log", help="game log for await:/grep: (default superman_returns_<instance>.log in cwd)")
    ap.add_argument("--frames", help="present-dump dir for shot:")
    ap.add_argument("--view", default=os.path.join(MAIN, "archive", "sr_press_view"), help="where shot: writes")
    ap.add_argument("--gap", type=int, default=600, help="ms after each press (default 600)")
    ap.add_argument("--timeout", type=int, default=30, help="s to wait for the channel")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    log = a.log or "superman_returns_%s.log" % a.instance
    m, pid = open_channel(a.instance, a.timeout)
    if a.keymap:
        print("keymap %s (flags 0x%X), game pid %d" % (a.keymap, set_keymap(m, a.keymap == "player"), pid))
    player = bool(struct.unpack_from("<I", m, OFF_FLAGS)[0] & 1)
    table = PLAYER if player else LEGACY
    mark = os.path.getsize(log) if os.path.exists(log) else 0
    for st in a.steps:
        low = st.lower()
        if low.startswith("wait:"):
            time.sleep(float(st[5:]))
        elif low.startswith(("await:", "grep:")):
            kind, body = st.split(":", 1)
            mt = re.match(r"(.*):(\d+)$", body)
            rx, lim = (mt.group(1), int(mt.group(2))) if mt else (body, 120)
            start, lim = (0, 0) if kind.lower() == "grep" else (mark, lim)
            t0, hit = time.time(), None
            while True:
                hits = re.findall(rb"[^\n]*(?:" + rx.encode() + rb")[^\n]*", log_read(log, start))
                if hits:
                    hit = hits[-1][:300].decode("utf-8", "replace").strip()
                    break
                if time.time() - t0 >= lim:
                    break
                time.sleep(1)
            print("%s %s -> %s" % (kind, rx, hit if hit else "TIMEOUT/NONE"), flush=True)
        elif low.startswith("shot:"):
            if not a.frames:
                sys.exit("sr_press: shot: needs --frames")
            from PIL import Image
            files = sorted(glob.glob(os.path.join(a.frames, "present_*.bmp")), key=os.path.getmtime)
            if not files:
                print("shot: no frames in", a.frames)
                continue
            os.makedirs(a.view, exist_ok=True)
            im = Image.open(files[-1]).convert("RGB")
            im = im.resize((im.width * 6 // 10, im.height * 6 // 10))
            p = os.path.join(a.view, st[5:] + ".png")
            im.save(p)
            print("shot", p, "<-", os.path.basename(files[-1]), flush=True)
        else:
            name, _, hold = st.partition(":")
            hold = int(hold) if hold else 250
            if name.lower().startswith("key="):
                key, label = name[4:], name[4:]
            else:
                key = table.get(name.upper())
                if key is None:
                    sys.exit("sr_press: %s has no key in the %s keymap; send key=<name> instead"
                             % (name, "player" if player else "legacy"))
                label = name.upper()
            mark = os.path.getsize(log) if os.path.exists(log) else mark
            press(m, key, hold, label)
            time.sleep(a.gap / 1000.0)


if __name__ == "__main__":
    main()
