#!/usr/bin/env python3
"""build_slot.py - cap how many lane builds compile at once (RAM guard).

Parallel lanes each build their own worktree. Every clang job on the generated
guest code takes ~0.5 GB, so six lanes at -j 12 asked for 30+ GB on a 32 GB
machine (2026-10-01: 0.5 GB free, the user's desktop starved). This wraps a build
command in a counting semaphore: at most --capacity builds run together, each
with the -j the caller passes (default use: -j 8 -> ~2 x 8 x 0.5 GB).

    python tools/build_slot.py --owner splice3 -- cmake --build build/ -j 8

Slots are atomic lock directories archive/build_slot.<i> in the MAIN repo (shared
by every worktree). A slot whose owner pid is dead is reclaimed. The slot is
always released when the wrapped command exits; the command's exit code is
returned unchanged.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime


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


MAIN_ROOT = _main_root()
ARCHIVE = os.path.join(MAIN_ROOT, "archive")


def pid_alive(pid):
    try:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return True  # unknown -> assume alive, never steal on a failed check
    return str(pid) in out


def try_take(i, owner):
    d = os.path.join(ARCHIVE, "build_slot.%d" % i)
    f = os.path.join(d, "owner.json")
    try:
        os.mkdir(d)
    except FileExistsError:
        try:
            with open(f, encoding="utf-8") as fh:
                cur = json.load(fh)
            pid = int(cur.get("pid", 0))
        except Exception:
            # owner.json not written yet (a racing taker) or unreadable: leave it,
            # unless the directory is old enough to be an orphan.
            if time.time() - os.path.getmtime(d) < 120:
                return None
            pid = 0
        if pid and pid_alive(pid):
            return None
        try:
            if os.path.exists(f):
                os.remove(f)
            os.rmdir(d)
            os.mkdir(d)
        except OSError:
            return None
    with open(f, "w", encoding="utf-8") as fh:
        json.dump({"owner": owner, "pid": os.getpid(), "state": "waiting",
                   "since": datetime.now().strftime("%Y-%m-%d %H:%M:%S")}, fh)
    return d


def mark_building(d):
    """The slot's build has started on the disk. game_slot.py acquire waits only for builds in this state: a
    build still waiting for a quiet disk must not hold up a roll (2026-10-10: a waiting build and a reserving
    roller each waited for the other until a bound expired)."""
    f = os.path.join(d, "owner.json")
    try:
        with open(f, encoding="utf-8") as fh:
            cur = json.load(fh)
        cur["state"] = "building"
        cur["building_since"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        with open(f, "w", encoding="utf-8") as fh:
            json.dump(cur, fh)
    except Exception:
        pass


def release(d):
    try:
        os.remove(os.path.join(d, "owner.json"))
    except OSError:
        pass
    try:
        os.rmdir(d)
    except OSError:
        pass


# Builds and rolls take turns on the disk (2026-10-10: the repo lives on an HDD; a lane's exe snapshot + harness
# build during a proof roll's boot kept the shader cache from loading for 60 s - no present, GPU watchdog H3, a void
# roll - and a row-3 roll hung in Present under other lanes' builds). A build waits, holding its build slot, while a
# game runs or another lane holds the game slot; game_slot.py acquire waits while a build slot is held. So the two
# alternate: a hold ends -> the waiting builds run -> the next roll acquires. A fresh "next" reservation (someone is
# about to roll) keeps NEW builds from taking a slot. Both waits are bounded (SR_BUILD_QUIET_MAX_MIN, default 120).
GAME_LOCK = os.path.join(ARCHIVE, "game_slot.lock")
GAME_NEXT = os.path.join(ARCHIVE, "game_slot.next.json")
GAME_NAMES = ("supermanreturns", "xenia")


def game_running():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=60).stdout
    except Exception:
        return []
    names = [ln.split(",")[0].strip('"') for ln in out.splitlines()]
    return [n for n in names if any(g in n.lower() for g in GAME_NAMES)]


def my_ancestry():
    try:
        import psutil
        return {(p.pid, round(p.create_time(), 3)) for p in psutil.Process().parents()[:16]}
    except Exception:
        return set()


def same_lane(owner, cur):
    """The game slot holder is this build's own lane: same or prefix owner, same worktree, or our ancestor."""
    other = str(cur.get("owner") or "")
    if other and (other == owner or owner.startswith(other) or other.startswith(owner)):
        return True
    here = os.path.normcase(os.getcwd())
    # Same worktree counts only for a lane's own checkout, never the shared umbrella / _worktrees folders.
    if cur.get("cwd") and os.path.normcase(os.path.abspath(cur["cwd"])) == here and \
            os.path.basename(here) not in ("_worktrees", "superman-returns-recomp"):
        return True
    mine = my_ancestry()
    return any((pid, ct) in mine for pid, ct in (cur.get("chain") or []))


def disk_busy(owner):
    """Why a build must wait now (None = the disk is ours)."""
    procs = game_running()
    if procs:
        return "a game is running (%s)" % ",".join(procs)
    f = os.path.join(GAME_LOCK, "owner.json")
    try:
        with open(f, encoding="utf-8") as fh:
            cur = json.load(fh)
        age = (time.time() - os.path.getmtime(f)) / 60.0
    except Exception:
        return None
    if age < 60 and not same_lane(owner, cur):
        return "the game slot is held by %s (its batch may start a game at any moment)" % cur.get("owner")
    return None


def reserved_for_other(owner):
    try:
        with open(GAME_NEXT, encoding="utf-8") as fh:
            r = json.load(fh)
        age = (time.time() - os.path.getmtime(GAME_NEXT)) / 60.0
    except Exception:
        return None
    if age < float(r.get("ttl_min", 45)) and not same_lane(owner, {"owner": r.get("owner")}):
        return r.get("owner")
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--owner", required=True)
    p.add_argument("--capacity", type=int, default=int(os.environ.get("SR_BUILD_SLOTS", "2")))
    p.add_argument("--poll", type=float, default=15)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    a = p.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        p.error("no command given (put it after --)")
    # RAM guard (2026-10-02: 2 builds x -j 8 = 16 clang jobs ~8 GB on a 32 GB box that
    # also runs the game, the editor and 10 agents): clamp any -j to SR_BUILD_JOBS.
    cap = int(os.environ.get("SR_BUILD_JOBS", "4"))
    out, i = [], 0
    while i < len(cmd):
        t = cmd[i]
        if t == "-j" and i + 1 < len(cmd) and cmd[i + 1].isdigit():
            out += ["-j", str(min(int(cmd[i + 1]), cap))]
            i += 2
            continue
        if t.startswith("-j") and t[2:].isdigit():
            t = "-j%d" % min(int(t[2:]), cap)
        out.append(t)
        i += 1
    if not any(t == "-j" or (t.startswith("-j") and t[2:].isdigit()) for t in out) and \
            any("cmake" in t.lower() or "ninja" in t.lower() for t in out[:1]):
        out += ["-j", str(cap)]
    cmd = out
    quiet_max = float(os.environ.get("SR_BUILD_QUIET_MAX_MIN", "120")) * 60
    held, waited, said = None, 0.0, None
    while held is None:
        rsv = reserved_for_other(a.owner) if waited < quiet_max else None
        if rsv is None:
            for i in range(a.capacity):
                held = try_take(i, a.owner)
                if held:
                    break
        if held is None:
            why = ("the game slot is reserved next for %s (a roll is about to start)" % rsv) if rsv else \
                "one of %d build slots" % a.capacity
            if why != said:
                print("[build_slot] %s waiting: %s" % (a.owner, why), flush=True)
                said = why
            time.sleep(a.poll)
            waited += a.poll
    try:
        said, qw = None, 0.0
        while True:
            why = disk_busy(a.owner)
            if why is None:
                break
            if qw >= quiet_max:
                print("[build_slot] %s WARNING: waited %.0f min for a quiet disk; building anyway (%s - a roll now "
                      "is contended)" % (a.owner, qw / 60, why), flush=True)
                break
            if why != said:
                print("[build_slot] %s holds %s, waiting for a quiet disk: %s" % (
                    a.owner, os.path.basename(held), why), flush=True)
                said = why
            time.sleep(a.poll)
            qw += a.poll
        waited += qw
        print("[build_slot] %s took %s after %.0f s: %s" % (a.owner, os.path.basename(held), waited, " ".join(cmd)),
              flush=True)
        # Builds yield the CPU to the game (2026-10-10: a row-3 proof roll hung 3.15 s in Present under 11.6 cores
        # of other lanes' builds and was voided). Below-normal priority is inherited by ninja / clang / the regen,
        # so a proof roll and the owner's desktop keep the CPU; SR_BUILD_PRIORITY=normal opts out.
        flags = 0
        if os.name == "nt" and os.environ.get("SR_BUILD_PRIORITY", "below").lower() != "normal":
            flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS
        mark_building(held)
        rc = subprocess.call(cmd, shell=(os.name == "nt" and len(cmd) == 1), creationflags=flags)
    finally:
        release(held)
    print("[build_slot] %s released %s (exit %d)" % (a.owner, os.path.basename(held), rc), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
