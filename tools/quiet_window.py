#!/usr/bin/env python3
"""quiet_window.py - the PC's QUIET_WINDOW sentinel in one place: tools refuse or wait instead of relying on memory.

A timing lane that holds the PC game slot creates `<umbrella>/_worktrees/native/archive/v1_native/QUIET_WINDOW`
("Do nothing on the PC but edit files until this file is gone"). On 2026-10-18 it was broken six times by hand-run
tools (a VM->J: copy, a D: scan, hashing ~512 MB, a cloud sync + Apt dumps, two fetch-verified runs). Tools that do
PC-side bulk work call this module first:

    import quiet_window
    ok, msg = quiet_window.gate("sr_cloud sync", wait_min=0, override_owner=None)   # refuse (wait_min 0) or wait
    if not ok: print(msg, file=sys.stderr); sys.exit(quiet_window.EXIT)
    pause = quiet_window.Pauser("my copy")   # long work: pause()  between files / chunks blocks while a window is open

A start check alone is not enough for long work: a window can open while it runs. Bulk copies call a Pauser between
files and every few MB (tools/cloud/verified_fetch.py, tools/cloud/quiet_copy.py, sr_cloud fetch / sync).

    python tools/quiet_window.py status                    # path, active?, the sentinel's own text
    python tools/quiet_window.py check [--owner <lane>]    # exit 0 clear (or <lane> holds the slot), 75 active
    python tools/quiet_window.py wait [--max-min N]        # exit 0 once clear, 75 at the limit

The override: only the CURRENT PC game-slot holder may act inside a window, and only by naming itself
(override_owner / --owner); anybody else is refused even with an override. The sentinel path: SR_QUIET_WINDOW if set
(tests, another machine), else derived from this file's own umbrella layout (<umbrella>/_worktrees/<lane>/tools or the
public root <umbrella>/tools -> <umbrella>/_worktrees/native/...), else this checkout's own archive/v1_native/
QUIET_WINDOW (a plain clone: it never exists, so nothing is held). On the ROG the derived path
never exists, so Ally work is never held by the PC's window (STEP4_RULES: Ally sets run in parallel with PC rolls).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

EXIT = 75   # EX_TEMPFAIL: "not now" - the caller may retry after the window
HERE = os.path.dirname(os.path.abspath(__file__))


def path(tools_dir=None):
    """The sentinel this machine's tools honour."""
    if os.environ.get("SR_QUIET_WINDOW"):
        return os.environ["SR_QUIET_WINDOW"]
    checkout = os.path.dirname(os.path.abspath(tools_dir or HERE))
    parent = os.path.dirname(checkout)
    worktrees = parent if os.path.basename(parent).lower() == "_worktrees" else os.path.join(checkout, "_worktrees")
    if os.path.isdir(worktrees):
        return os.path.normpath(os.path.join(worktrees, "native", "archive", "v1_native", "QUIET_WINDOW"))
    return os.path.normpath(os.path.join(checkout, "archive", "v1_native", "QUIET_WINDOW"))


def active(p=None):
    return os.path.exists(p or path())


def describe(p=None):
    """The sentinel's own text (its owner and purpose), one line, or '' when there is none."""
    try:
        with open(p or path(), encoding="utf-8", errors="replace") as f:
            return " ".join(f.read(400).split())
    except OSError:
        return ""


def slot_holder():
    """The PC game slot's current owner (tools/game_slot.py's lock), or None when it is free / unreadable."""
    sys.path.insert(0, HERE)
    try:
        import game_slot   # lazy: game_slot imports this module at its top
    except Exception:
        return None
    finally:
        sys.path.remove(HERE)
    if not game_slot.LOCK or not os.path.isdir(game_slot.LOCK):
        return None
    try:
        with open(game_slot.OWNER_FILE, encoding="utf-8") as f:
            return json.load(f).get("owner")
    except Exception:
        return None


def check(what, override_owner=None, p=None, holder=None):
    """(ok, message). ok when no window is open, or when `override_owner` is the current slot holder."""
    p = p or path()
    if not active(p):
        return True, ""
    text = describe(p) or "(empty sentinel)"
    if override_owner:
        h = holder if holder is not None else slot_holder()
        if h and h == override_owner:
            return True, ("QUIET_WINDOW override: %s holds the game slot and runs %s inside its window [%s]"
                          % (override_owner, what, text))
        return False, ("refused: %s - QUIET_WINDOW exists (%s) and the override names %s, but the game slot is held by "
                       "%s: only the slot holder may act inside the window" % (what, text, override_owner, h or "nobody"))
    return False, ("refused: %s - QUIET_WINDOW exists (%s): %s. Do nothing on the PC but edit files until it is gone "
                   "(wait for it, or the slot holder overrides by naming itself)" % (what, p, text))


def wait(what, max_min=None, poll_s=30.0, override_owner=None, p=None, say=None, sleep=time.sleep, clock=time.time):
    """Block until check() passes -> (True, msg); (False, msg) once max_min minutes have passed (None = forever)."""
    say = say or (lambda m: print(m, file=sys.stderr, flush=True))
    t0 = clock()
    shown = None
    while True:
        ok, msg = check(what, override_owner, p)
        if ok:
            return True, msg
        if max_min is not None and clock() - t0 >= max_min * 60:
            return False, msg + " (waited %.0f min)" % max_min
        if msg != shown:
            say("[%s] waiting: %s" % (time.strftime("%H:%M:%S"), msg))
            shown = msg
        sleep(poll_s)


class Pauser:
    """For PC-side bulk work that is already running (a copy, a fetch): call it between files and every few MB. It
    returns at once while no window is open; while one is, it BLOCKS (re-checking every poll_s) and returns the
    seconds it paused - it never aborts, so the work resumes where it stopped and is verified as usual. The
    override (only the current game-slot holder, naming itself) passes as in check(). Tools keep their deadlines
    out of the paused time with .paused_s. 2026-10-20 (lane C): a fetch-verified that passed its start gate at
    13:59 kept writing J: inside lane D's 14:04 window - one start check is not enough for a long transfer."""

    def __init__(self, what, override_owner=None, p=None, poll_s=15.0, say=None, sleep=None, clock=None):
        self.what, self.override_owner, self.p, self.poll_s = what, override_owner, p, poll_s
        self.say = say or (lambda m: print(m, file=sys.stderr, flush=True))
        # looked up at call time (a test patches quiet_window.time.sleep)
        self.sleep = sleep or (lambda s: time.sleep(s))
        self.clock = clock or (lambda: time.monotonic())
        self.paused_s, self.pauses, self.checks = 0.0, 0, 0

    def __call__(self):
        self.checks += 1
        ok, msg = check(self.what, self.override_owner, self.p)
        if ok:
            return 0.0
        t0 = self.clock()
        self.pauses += 1
        self.say("[%s] PAUSED %s: %s" % (time.strftime("%H:%M:%S"), self.what, msg))
        while not check(self.what, self.override_owner, self.p)[0]:
            self.sleep(self.poll_s)
        dt = self.clock() - t0
        self.paused_s += dt
        self.say("[%s] resumed %s after %.0f s" % (time.strftime("%H:%M:%S"), self.what, dt))
        return dt

    def blocked(self):
        """True while a window is open for this work (the rule of a call, without waiting). A step that is already
        running polls it: quiet_copy kills a range / a tar in flight within ~1 s of the sentinel and redoes it."""
        return not check(self.what, self.override_owner, self.p)[0]

    def record(self):
        return {"checks": self.checks, "pauses": self.pauses, "paused_s": round(self.paused_s, 1)}


def gate(what, wait_min=0.0, override_owner=None, p=None, **kw):
    """The one call a tool makes: wait_min 0 = refuse at once, > 0 = wait up to that many minutes, < 0 = wait forever."""
    if wait_min == 0:
        return check(what, override_owner, p)
    return wait(what, None if wait_min < 0 else wait_min, override_owner=override_owner, p=p, **kw)


def add_args(parser):
    """--wait-quiet MIN / --quiet-override OWNER for a tool's own argparse (sr_cloud's bulk commands)."""
    parser.add_argument("--wait-quiet", type=float, default=0, metavar="MIN",
                        help="while the PC's QUIET_WINDOW exists: wait up to MIN minutes (-1 = as long as it takes) "
                             "instead of refusing at once")
    parser.add_argument("--quiet-override", default="", metavar="OWNER",
                        help="act inside a QUIET_WINDOW - only when OWNER is the current PC game-slot holder")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("status", "check", "wait"))
    ap.add_argument("--what", default="this command")
    ap.add_argument("--owner", default="", help="the caller's slot-owner name (the override)")
    ap.add_argument("--max-min", type=float, default=None)
    ap.add_argument("--poll", type=float, default=30.0)
    a = ap.parse_args()
    if a.cmd == "status":
        p = path()
        print("sentinel: %s\nactive: %s\ntext: %s" % (p, active(p), describe(p) or "-"))
        return 0
    if a.cmd == "check":
        ok, msg = check(a.what, a.owner or None)
    else:
        ok, msg = wait(a.what, a.max_min, a.poll, a.owner or None)
    if msg:
        print(msg, file=sys.stderr)
    return 0 if ok else EXIT


if __name__ == "__main__":
    sys.exit(main())
