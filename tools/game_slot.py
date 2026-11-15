#!/usr/bin/env python3
"""game_slot.py - the ONE game slot (a SupermanReturns or Xenia process), shared
by every parallel lane, worktree and chat session.

CLAUDE.md: one game process at a time. Parallel lanes build in their own
worktrees, but rolls are serial. Polling `tasklist` alone races: a lane checks
"no game running", spends minutes rebasing and rebuilding, and then launches
while another lane is mid-roll. This tool adds an atomic lock directory
(archive/game_slot.lock, mkdir is atomic on NTFS) that a lane holds for the whole
Rolls stage, plus optional ordering on workflow journals ("wait until lane X has
N results") with a stall fallback so a dead predecessor never blocks the queue.

    python tools/game_slot.py acquire --owner audio-cmdbuf2 \
        [--after-journal PATH:N ...] [--stall-min 90] [--poll 60] [--max-hours 12]
    python tools/game_slot.py heartbeat --owner audio-cmdbuf2   # during long rolls
    python tools/game_slot.py release --owner audio-cmdbuf2
    python tools/game_slot.py status

acquire blocks until: (1) every --after-journal has >= N '"type":"result"' lines,
or its directory has been idle for --stall-min minutes; (2) no SupermanReturns /
xenia process runs; (3) the lock is free (or stale: no heartbeat for
--stale-min minutes and no game process). It then prints ACQUIRED and exits 0.
Exit 2 = gave up after --max-hours. The lock is never auto-released: always
release in a finally / at the end of the Rolls stage.
"""
import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quiet_window  # noqa: E402  (tools/quiet_window.py: the PC's QUIET_WINDOW sentinel)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# The lock lives in the MAIN repo's archive/ even when run from a worktree copy
# of this script, so every lane sees the same lock.
def git_main_checkout(checkout):
    """The main checkout git knows for this tree: the parent of git's common directory (every worktree of one
    repository shares it; a plain clone is its own main checkout), or None without git."""
    try:
        common = subprocess.run(["git", "-C", checkout, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    return os.path.normpath(os.path.dirname(common)) if common else None


def default_main_root(tools_dir=None):
    """SR_MAIN_ROOT unset: (main checkout, how) for THIS machine, or (None, why not).

    2026-10-18: the default used to be one machine's absolute path on every machine, so `game_slot.py status` run on
    the ROG without SR_MAIN_ROOT read a lock that is not there and printed "lock: free" while a set held the ROG's slot
    (lanes C, D and E each misread it). The main checkout is derived from where this file lives - the umbrella layout
    every machine mirrors: <umbrella>/_worktrees/<lane>/tools/game_slot.py and the public root
    <umbrella>/tools/game_slot.py both mean <umbrella>/_worktrees/dev - when that folder (with its archive/) exists;
    else the main checkout git knows for this tree (a worktree outside the layout, or a plain clone); else nothing,
    and every command refuses (exit 2) instead of guessing a path."""
    checkout = os.path.dirname(os.path.abspath(tools_dir or os.path.dirname(os.path.abspath(__file__))))
    parent = os.path.dirname(checkout)
    cands = []
    if os.path.basename(parent).lower() == "_worktrees":
        cands.append(os.path.join(parent, "dev"))
    cands.append(os.path.join(checkout, "_worktrees", "dev"))
    for c in cands:
        if os.path.isdir(os.path.join(c, "archive")):
            return os.path.normpath(c), "this checkout's umbrella layout"
    main = git_main_checkout(checkout)
    if main:
        return main, "the main checkout git knows for this tree"
    return None, ("SR_MAIN_ROOT is unset, this file (%s) is not inside an umbrella with _worktrees/dev/archive, and "
                  "git names no main checkout for it" % os.path.join(checkout, "tools"))


if os.environ.get("SR_MAIN_ROOT"):
    MAIN_ROOT, MAIN_ROOT_FROM = os.environ["SR_MAIN_ROOT"], "SR_MAIN_ROOT"
else:
    MAIN_ROOT, MAIN_ROOT_FROM = default_main_root()
LOCK = os.path.join(MAIN_ROOT, "archive", "game_slot.lock") if MAIN_ROOT else None
OWNER_FILE = os.path.join(LOCK, "owner.json") if LOCK else None
# 2026-10-03: with 6+ lanes polling, a lane with a slow poll never won the mkdir race.
# `reserve --owner X` makes every OTHER owner's acquire wait while the reservation is
# fresh (--ttl-min, default 45); X's acquire consumes it. Expiry keeps a dead reserver
# from blocking the queue.
NEXT_FILE = os.path.join(MAIN_ROOT, "archive", "game_slot.next.json") if MAIN_ROOT else None
# 2026-10-18: first come, first served. Until now only a reservation ordered the queue; every other waiter raced for
# the mkdir at its own poll, so a fast poller always won (on the ROG lane C waited 14:41-15:31 while B and E took the
# lock in turns). Every waiting acquire holds a ticket here; when the lock frees, the oldest LIVE, READY ticket goes
# first (a reservation still goes before the queue). See ticket_* below.
TICKETS = os.path.join(MAIN_ROOT, "archive", "game_slot.tickets") if MAIN_ROOT else None
# reasons every waiter shares (the lock, a game, a reservation, the queue itself): a waiter blocked ONLY by these is
# "ready" - it would take a free lock - and holds its place; one blocked by its own condition (an --after-journal, its
# RAM floor, builds, a QUIET_WINDOW it may not enter) does not hold up the waiters behind it
SHARED_REASONS = ("game running", "reserved next", "lock held", "held by", "queued behind", "lost the mkdir race")
GAME_NAMES = ("supermanreturns", "xenia")


def now():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def game_procs():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True,
                             text=True, timeout=60).stdout
    except Exception as e:  # tasklist failing must not look like "free"
        return ["<tasklist failed: %s>" % e]
    hits = []
    for line in out.splitlines():
        name = line.split(",")[0].strip('"')
        if any(g in name.lower() for g in GAME_NAMES):
            hits.append(name)
    return hits


def read_owner():
    try:
        with open(OWNER_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def lock_age_min():
    try:
        t = os.path.getmtime(OWNER_FILE)
    except OSError:
        try:
            t = os.path.getmtime(LOCK)
        except OSError:
            return None
    return (time.time() - t) / 60.0


def journal_ok(spec, stall_min):
    path, _, n = spec.rpartition(":")
    n = int(n)
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            cnt = sum(1 for line in f if '"type":"result"' in line)
    except OSError:
        return True, "%s missing -> treated as done" % path
    if cnt >= n:
        return True, "%s results=%d>=%d" % (os.path.basename(os.path.dirname(path)), cnt, n)
    d = os.path.dirname(path)
    newest = max((os.path.getmtime(os.path.join(d, x)) for x in os.listdir(d)), default=0)
    idle = (time.time() - newest) / 60.0
    if idle >= stall_min:
        return True, "%s results=%d<%d but idle %.0f min -> stalled, proceeding" % (
            os.path.basename(d), cnt, n, idle)
    return False, "%s results=%d<%d (idle %.0f min)" % (os.path.basename(d), cnt, n, idle)


def user_idle_min():
    """Minutes since the last keyboard/mouse input in this session (GetLastInputInfo)."""
    try:
        import ctypes

        class LASTINPUTINFO(ctypes.Structure):
            _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]

        lii = LASTINPUTINFO()
        lii.cbSize = ctypes.sizeof(LASTINPUTINFO)
        if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii)):
            return None
        tick = ctypes.windll.kernel32.GetTickCount() & 0xFFFFFFFF
        return ((tick - lii.dwTime) & 0xFFFFFFFF) / 60000.0
    except Exception:
        return None


def free_ram_gb():
    """Free physical memory in GB (GlobalMemoryStatusEx), or None if unknown."""
    try:
        import ctypes

        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        m = MEMORYSTATUSEX()
        m.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return None
        return m.ullAvailPhys / (1024.0 ** 3)
    except Exception:
        return None


def read_next():
    """The live reservation {owner, since, ttl_min, note}, or None if absent/expired."""
    try:
        with open(NEXT_FILE, encoding="utf-8") as f:
            r = json.load(f)
        age = (time.time() - os.path.getmtime(NEXT_FILE)) / 60.0
    except Exception:
        return None
    if age >= float(r.get("ttl_min", 45)):
        return None
    r["age_min"] = round(age, 1)
    return r


# A reservation names a label; tools/set_runner.py acquires as <its spec's game_slot.owner>-<12 hex> (a unique owner per
# run, so two runs of one label never nest). That run owner takes its label's reservation. 2026-10-20: lane B reserved
# s4-audit-m1 and its own era set waited 29 min behind it ("reserved next for s4-audit-m1"). The match is exact on
# the label and on set_runner's suffix shape, so "native" never admits "native-b".
RUN_SUFFIX = re.compile(r"-[0-9a-f]{12}$")


def reserved_for(reserved_owner, owner):
    """True when a reservation for `reserved_owner` admits the acquirer `owner`."""
    if not reserved_owner or not owner:
        return False
    return owner == reserved_owner or (RUN_SUFFIX.search(owner) is not None and owner[:-13] == reserved_owner)


def clear_next(owner):
    r = read_next()
    if r and reserved_for(r.get("owner"), owner):
        try:
            os.remove(NEXT_FILE)
        except OSError:
            pass


try:
    import psutil
except ImportError:  # without it the same-owner guard degrades to the iter-466 rule
    psutil = None


def ancestry(levels=8):
    """[[pid, create_time], ...] of this process's parents, nearest first."""
    if psutil is None:
        return []
    try:
        return [[p.pid, round(p.create_time(), 3)] for p in psutil.Process().parents()[:levels]]
    except Exception:
        return []


def _same_proc(pid, ctime):
    try:
        return round(psutil.Process(pid).create_time(), 3) == ctime
    except Exception:
        return False


def holder_batch_alive(cur):
    """iter-467: is the holder's own batch still running OUTSIDE this acquirer's ancestry?

    A tool nested inside the holder's batch shares the batch process, so nothing alive is
    exclusive to the holder: it may nest. A second, independent batch of the same owner sees
    the first batch's process alive and not its own ancestor: it must wait. A lane that holds
    across separate shell calls has dead shells: it may pass (the iter-466 rule)."""
    chain = cur.get("chain") if cur else None
    if not chain or psutil is None:
        return None
    mine = {(pid, ct) for pid, ct in ancestry(16)}
    for pid, ct in chain:
        if (pid, ct) not in mine and _same_proc(pid, ct):
            return pid
    return None


def builds_running():
    """Live build_slot holders, as "owner (N min)" (2026-10-10: builds and rolls take turns on the HDD; see
    build_slot.py). A holder whose pid is dead, or that has held for over 4 h, is not counted."""
    out = []
    for d in glob.glob(os.path.join(MAIN_ROOT, "archive", "build_slot.*")):
        f = os.path.join(d, "owner.json")
        try:
            with open(f, encoding="utf-8") as fh:
                cur = json.load(fh)
            pid = int(cur.get("pid", 0))
            age = (time.time() - os.path.getmtime(f)) / 60.0
        except Exception:
            continue
        if not pid or age > 240 or cur.get("state") == "waiting":
            continue  # a build still waiting for a quiet disk is not using it (and waits for us)
        if psutil is not None:
            try:
                if not psutil.pid_exists(pid):
                    continue
            except Exception:
                pass
        out.append("%s (%.0f min)" % (cur.get("owner"), age))
    return out


def _my_ctime():
    try:
        return round(psutil.Process().create_time(), 3) if psutil is not None else None
    except Exception:
        return None


def ticket_take(owner, poll_s, note=""):
    """A waiting acquire's place in the queue: <TICKETS>/<time_ns>_<pid>.json (the name orders the queue)."""
    os.makedirs(TICKETS, exist_ok=True)
    path = os.path.join(TICKETS, "%020d_%d.json" % (time.time_ns(), os.getpid()))
    ticket_write(path, {"owner": owner, "pid": os.getpid(), "ctime": _my_ctime(), "poll_s": float(poll_s),
                        "since": now(), "note": note, "ready": False})
    return path


def _retry(op, attempts=12, first_s=0.02):
    """Run op() through Windows sharing violations: os.replace / os.remove fail with PermissionError [WinError 5]
    while another process has the file open (a waiter reading the queue, an antivirus scan). 2026-10-18: acquire
    crashed on the first one. Backs off 20 ms, 40 ms, ... (~1.6 s in all); True when op succeeded."""
    for i in range(attempts):
        try:
            op()
            return True
        except PermissionError:
            time.sleep(first_s * (i + 1))
        except FileNotFoundError:
            return False
    return False


def ticket_write(path, info):
    """(Re)write a ticket; never raises. Atomically (a temp file + os.replace, retried through sharing violations);
    when readers keep the file open past the retries, in place (truncate + one write: Python's readers open with
    write sharing, so this cannot be refused) - a reader may then see it empty or partial for an instant, and
    tickets() skips such a ticket for that poll. False only when neither landed (the old content and mtime stay)."""
    data = json.dumps(info)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(data)
    except OSError:
        tmp = None
    if tmp and _retry(lambda: os.replace(tmp, path)):
        return True
    if tmp:
        _retry(lambda: os.remove(tmp), attempts=3)

    def in_place():
        with open(path, "w", encoding="utf-8") as f:
            f.write(data)
    return _retry(in_place, attempts=5)


def ticket_live(path, info, at=None):
    """A ticket counts while its waiter renews it (every poll; stale after 3 polls, at least 2 min) and, where psutil
    can tell, while that exact process (pid + create time) is alive: a killed waiter never blocks the queue."""
    try:
        age = (at or time.time()) - os.path.getmtime(path)
    except OSError:
        return False
    if age > max(3 * float(info.get("poll_s") or 60), 120):
        return False
    if psutil is not None and info.get("pid"):
        if info.get("ctime") is not None:
            return _same_proc(int(info["pid"]), info["ctime"])
        try:
            return psutil.pid_exists(int(info["pid"]))
        except Exception:
            return False
    return True


def tickets(clean=True):
    """[(path, info, live)] oldest first; dead / stale tickets are removed when clean."""
    out = []
    for f in sorted(glob.glob(os.path.join(TICKETS, "*.json"))) if TICKETS else []:
        try:   # read, close, then parse: the file is open as briefly as possible (its writer replaces it)
            with open(f, encoding="utf-8") as fh:
                text = fh.read()
            info = json.loads(text)
            if not isinstance(info, dict):
                continue
        except Exception:   # gone (acquired, expired) or unreadable for a moment: skip it this poll
            continue
        live = ticket_live(f, info)
        if not live and clean:
            try:
                os.remove(f)
            except OSError:
                pass
            continue
        out.append((f, info, live))
    return out


def tickets_ahead(mine):
    """The live, ready tickets older than `mine` (its waiters go first). Older = an earlier name (<time_ns>_<pid>):
    compared by name, so a ticket of ours that is missing for a moment (a failed write, a reader's stale sweep) never
    puts every later waiter in front of us."""
    me = os.path.basename(mine)
    return [info for f, info, live in tickets() if os.path.basename(f) < me and live and info.get("ready")]


def write_owner(owner, note=""):
    with open(OWNER_FILE, "w", encoding="utf-8") as f:
        json.dump({"owner": owner, "since": now(), "heartbeat": now(), "note": note,
                   "cwd": os.getcwd(), "depth": 1, "chain": ancestry()}, f)


def write_owner_dict(cur):
    with open(OWNER_FILE, "w", encoding="utf-8") as f:
        json.dump(cur, f)


def cmd_acquire(a):
    my_ticket = ticket_take(a.owner, a.poll, a.note or "")
    try:
        return _acquire_loop(a, my_ticket)
    finally:
        _retry(lambda: os.remove(my_ticket))   # a reader may hold it open; a leftover expires with this process


def _acquire_loop(a, my_ticket):
    deadline = time.time() + a.max_hours * 3600
    last_msg = None
    info = {"owner": a.owner, "pid": os.getpid(), "ctime": _my_ctime(), "poll_s": float(a.poll), "since": now(),
            "note": a.note or "", "ready": False}
    while time.time() < deadline:
        reasons = []
        for spec in a.after_journal or []:
            ok, why = journal_ok(spec, a.stall_min)
            if not ok:
                reasons.append(why)
        procs = game_procs()
        if procs:
            reasons.append("game running: %s" % ",".join(procs))
        # 2026-10-18: inside a QUIET_WINDOW only the holder of the slot (the timing lane that opened it) may acquire,
        # i.e. nest; everybody else waits for the window to close (it was broken six times by hand that day)
        if quiet_window.active():
            holder = read_owner() if os.path.isdir(LOCK) else None
            if not (holder and holder.get("owner") == a.owner):
                reasons.append("QUIET_WINDOW open (%s) - only the slot holder (%s) may acquire inside it"
                               % (quiet_window.describe()[:160] or quiet_window.path(),
                                  holder.get("owner") if holder else "nobody"))
        if a.min_free_gb > 0:
            free = free_ram_gb()
            if free is not None and free < a.min_free_gb:
                reasons.append("free RAM %.1f GB < %.0f GB (a starved roll is not evidence)"
                               % (free, a.min_free_gb))
        if a.user_idle_min > 0:
            idle = user_idle_min()
            if idle is not None and idle < a.user_idle_min:
                reasons.append("user active (idle %.1f < %.0f min)" % (idle, a.user_idle_min))
        nxt = read_next()
        if is_priority(a.owner) and (not nxt or not is_priority(nxt.get("owner"))):
            # the critical path waits in front: a short reservation, renewed every poll, lapses if we die
            with open(NEXT_FILE, "w", encoding="utf-8") as f:
                json.dump({"owner": a.owner, "since": now(), "ttl_min": 10, "note": "priority waiter (auto)"}, f)
            nxt = read_next()
        elif is_priority(a.owner) and nxt and reserved_for(nxt.get("owner"), a.owner):
            os.utime(NEXT_FILE, None)  # renew our own reservation while we wait
        if nxt and not reserved_for(nxt.get("owner"), a.owner):
            reasons.append("reserved next for %s (%.0f min ago)" % (nxt.get("owner"), nxt["age_min"]))
        builds = [] if a.ignore_builds else builds_running()
        if builds:
            reasons.append("build running (the HDD: builds and rolls take turns): %s" % ", ".join(builds))
        cur = read_owner()
        if os.path.isdir(LOCK):
            if cur and cur.get("owner") == a.owner:
                # iter-466: a lane's second batch used to walk straight in while its first batch was
                # still running a game (two game processes at once). Re-entry now waits for the game
                # to exit unless the caller says it is nesting inside its own running batch.
                # iter-467: that still let a second batch in BETWEEN the first batch's games, and its
                # release then freed the first batch's lock (another lane took the slot 90 s before
                # the first batch's flicker step ended). Now a same-owner acquire also waits while the
                # holder's batch process is alive outside this acquirer's ancestry, and every nested
                # pass is counted (depth) so only the outermost release frees the slot.
                other = None if a.reentrant else holder_batch_alive(cur)
                if other:
                    reasons.append("held by %s's own running batch (pid %s) - a second batch of the same "
                                   "owner waits; a tool nested inside that batch passes" % (a.owner, other))
                elif procs and not a.reentrant:
                    reasons.append("held by %s and a game is running (same-owner re-acquire waits; "
                                   "--reentrant to nest inside your own batch)" % a.owner)
                else:
                    cur["depth"] = int(cur.get("depth", 1)) + 1
                    write_owner_dict(cur)
                    clear_next(a.owner)
                    print("ACQUIRED (already held by %s since %s; nested, depth %d)"
                          % (a.owner, cur.get("since"), cur["depth"]))
                    return 0
            age = lock_age_min()
            if age is not None and age >= a.stale_min and not procs:
                print("[%s] breaking stale lock of %s (no heartbeat for %.0f min, no game)"
                      % (now(), cur and cur.get("owner"), age))
                try:
                    os.remove(OWNER_FILE)
                except OSError:
                    pass
                try:
                    os.rmdir(LOCK)
                except OSError:
                    pass
            else:
                reasons.append("lock held by %s (heartbeat %.0f min ago)" % (
                    cur and cur.get("owner"), age or 0))
        # first come, first served: renew this waiter's ticket (ready = blocked by shared reasons only) and wait for
        # every older live ready ticket - unless the slot is reserved for this owner (a reservation goes first)
        info["ready"] = not [r for r in reasons if not r.startswith(SHARED_REASONS)]
        ticket_write(my_ticket, info)
        if not (nxt and reserved_for(nxt.get("owner"), a.owner)):
            ahead = tickets_ahead(my_ticket)
            if ahead:
                reasons.append("queued behind %d earlier waiter(s): %s" % (len(ahead), ", ".join(
                    "%s (since %s)" % (t.get("owner"), t.get("since")) for t in ahead[:4])))
        if not reasons:
            try:
                os.mkdir(LOCK)
            except FileExistsError:
                reasons.append("lost the mkdir race")
            else:
                write_owner(a.owner, a.note or "")
                clear_next(a.owner)
                print("ACQUIRED by %s at %s" % (a.owner, now()))
                return 0
        msg = "; ".join(reasons)
        if msg != last_msg:
            print("[%s] waiting: %s" % (now(), msg), flush=True)
            last_msg = msg
        time.sleep(a.poll)
    print("GAVE UP after %.1f h" % a.max_hours)
    return 2


def cmd_heartbeat(a):
    cur = read_owner()
    if not cur or cur.get("owner") != a.owner:
        print("NOT HELD by %s (owner=%s)" % (a.owner, cur and cur.get("owner")))
        return 1
    cur["heartbeat"] = now()
    with open(OWNER_FILE, "w", encoding="utf-8") as f:
        json.dump(cur, f)
    print("heartbeat ok")
    return 0


def cmd_release(a):
    cur = read_owner()
    if not os.path.isdir(LOCK):
        print("not held")
        return 0
    if cur and cur.get("owner") != a.owner and not a.force:
        print("REFUSED: held by %s, not %s" % (cur.get("owner"), a.owner))
        return 1
    depth = int(cur.get("depth", 1)) if cur else 1
    if depth > 1 and not a.force:
        # iter-467: a nested acquire's release; the outer holder keeps the slot
        cur["depth"] = depth - 1
        write_owner_dict(cur)
        print("RELEASED one nesting level by %s (depth %d -> %d; the outer holder keeps the slot)"
              % (a.owner, depth, depth - 1))
        return 0
    try:
        os.remove(OWNER_FILE)
    except OSError:
        pass
    os.rmdir(LOCK)
    print("RELEASED by %s at %s" % (a.owner, now()))
    return 0


# Owner 2026-10-10: "native goes first" (the critical path). Owners starting with one of these prefixes take the
# next turn ahead of everyone else: a waiting priority acquirer keeps a short self-renewing reservation (the other
# lanes' acquires then wait), and a priority reserve replaces a non-priority reservation. A hold in progress is never
# preempted. SR_SLOT_PRIORITY overrides the prefixes ("" = no priority).
PRIORITY_PREFIXES = tuple(p for p in os.environ.get("SR_SLOT_PRIORITY", "native").split(",") if p)


def is_priority(owner):
    return bool(owner) and any(owner.startswith(p) for p in PRIORITY_PREFIXES)


def cmd_reserve(a):
    cur = read_next()
    if cur and cur.get("owner") != a.owner and not a.force and \
            not (is_priority(a.owner) and not is_priority(cur.get("owner"))):
        print("REFUSED: already reserved for %s (%.0f min ago); one reservation at a time - wait "
              "until it is consumed or expires, or pass --force" % (cur.get("owner"), cur["age_min"]))
        return 1
    with open(NEXT_FILE, "w", encoding="utf-8") as f:
        json.dump({"owner": a.owner, "since": now(), "ttl_min": a.ttl_min, "note": a.note}, f)
    print("RESERVED next for %s (expires in %.0f min unless renewed)" % (a.owner, a.ttl_min))
    return 0


def cmd_unreserve(a):
    r = read_next()
    if r and r.get("owner") != a.owner and not a.force:
        print("REFUSED: reserved for %s, not %s" % (r.get("owner"), a.owner))
        return 1
    try:
        os.remove(NEXT_FILE)
    except OSError:
        pass
    print("reservation cleared")
    return 0


def cmd_status(a):
    cur = read_owner()
    print("lock: %s" % (json.dumps(cur) if os.path.isdir(LOCK) else "free"))
    print("next: %s" % (json.dumps(read_next()) if read_next() else "none"))
    print("age_min: %s" % (None if lock_age_min() is None else round(lock_age_min(), 1)))
    print("game procs: %s" % (game_procs() or "none"))
    print("builds: %s" % (", ".join(builds_running()) or "none"))
    q = tickets(clean=False)
    print("queue: %s" % ("; ".join("%d. %s since %s%s%s" % (i + 1, t.get("owner"), t.get("since"),
                                                            "" if t.get("ready") else " (waiting for its own condition)",
                                                            "" if live else " (stale)")
                                   for i, (_, t, live) in enumerate(q)) or "empty"))
    print("main: %s (%s)" % (MAIN_ROOT, MAIN_ROOT_FROM))
    print("quiet_window: %s" % (("OPEN - " + (quiet_window.describe() or quiet_window.path()))
                                 if quiet_window.active() else "none (%s)" % quiet_window.path()))
    return 0


def threshold(s):
    """argparse type of acquire's waits (GB, minutes): a finite number >= 0, 0 = off. float() takes 'nan' and 'inf', and
    a NaN fails every `> 0` test, so SR_SLOT_MIN_FREE_GB=NaN would turn the free-RAM floor off silently (tools/ally-runner
    review P2): a value that is not finite or is negative is an error (exit 2), never a disabled check."""
    try:
        v = float(s)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("not a number: %r" % (s,))
    if not math.isfinite(v) or v < 0:
        raise argparse.ArgumentTypeError("must be a finite number >= 0 (0 = off): %r" % (s,))
    return v


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("acquire")
    q.add_argument("--owner", required=True)
    q.add_argument("--reentrant", action="store_true",
                   help="nest inside your OWN running batch (otherwise a same-owner re-acquire waits for the game to exit)")
    q.add_argument("--after-journal", action="append", metavar="PATH:N")
    q.add_argument("--stall-min", type=float, default=90)
    q.add_argument("--stale-min", type=float, default=60)  # heartbeats come every ~20 min; an orphan (killed agent) must not hold the slot for hours
    q.add_argument("--poll", type=float, default=60)
    q.add_argument("--max-hours", type=float, default=12)
    q.add_argument("--note", default="")
    q.add_argument("--ignore-builds", action="store_true",
                   help="do not wait for running builds (a replay-only or non-timing batch; see build_slot.py)")
    # USER 2026-10-01: rolls yanked the game window to the foreground while the user
    # worked. iter-464 bgruns made automated runs focus-free (docs/TOOLKIT.md 1.8a:
    # background window, harness keyboard, no console, background Xenia), so the
    # default no longer waits (0 = off). Pass a value, or set SR_SLOT_USER_IDLE_MIN,
    # to start a roll batch only after the user has been idle that long anyway.
    q.add_argument("--user-idle-min", type=threshold,
                   default=os.environ.get("SR_SLOT_USER_IDLE_MIN", "0"))
    # 2026-10-02: another session's runaway job starved the machine (commit ~105 GB) while
    # rolls ran; a starved roll's hangs/timeouts are not evidence. Wait for free RAM.
    # SR_SLOT_MIN_FREE_GB is the machine's floor (the Ally: 6); a string default goes through `threshold` too.
    q.add_argument("--min-free-gb", type=threshold,
                   default=os.environ.get("SR_SLOT_MIN_FREE_GB", "8"))
    h = sub.add_parser("heartbeat")
    h.add_argument("--owner", required=True)
    r = sub.add_parser("release")
    r.add_argument("--owner", required=True)
    r.add_argument("--force", action="store_true")
    s = sub.add_parser("reserve")
    s.add_argument("--owner", required=True)
    s.add_argument("--ttl-min", type=float, default=45)
    s.add_argument("--note", default="")
    s.add_argument("--force", action="store_true")
    u = sub.add_parser("unreserve")
    u.add_argument("--owner", required=True)
    u.add_argument("--force", action="store_true")
    sub.add_parser("status")
    a = p.parse_args()
    if not MAIN_ROOT:   # never fall back to another machine's path: "lock: free" from a lock that is not there
        print("game_slot: refused - no main checkout: %s. Set SR_MAIN_ROOT (the ROG: call sr-env.cmd)." % MAIN_ROOT_FROM,
              file=sys.stderr)
        return 2
    return {"acquire": cmd_acquire, "heartbeat": cmd_heartbeat, "release": cmd_release,
            "reserve": cmd_reserve, "unreserve": cmd_unreserve, "status": cmd_status}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
