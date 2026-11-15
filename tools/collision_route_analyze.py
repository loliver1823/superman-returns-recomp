#!/usr/bin/env python3
"""collision_route_analyze.py - offline pass-through detector + classifier for the iter-463 collision
watch (kernel/collision_watch.cpp, SR_COLLISION_WATCH, default ON) and its Xenia twin
(tools/xenia_collision_route.js).

THE QUESTION: the user flew through Midtown buildings on build 60a90bf ("some buildings didn't have
collision"). Did it happen in this log, and if so, WHICH mechanism?

A FLAG is a [COLL_PATH] tick (Superman's mover tick while speed > 20 m/s) whose capsule centre
(entity position after the tick) is more than THRESHOLD metres inside the world AABB of a static
building body (layer 7 by default) while that body is NOT in his contact manifold.
  * body AABBs come from havok_snapshot JSONs (ours_midtown / ours_start / the Xenia golden),
    matched to the run's bodies BY TRANSLATION (0.5 m), because addresses differ per run;
  * THRESHOLD = 4 m, or (known wall depth of that body + 1 m) when the golden-call mover cases
    measured one (--wall-depth; the prep measured walls up to ~7 m inside a body's AABB, so a
    4-6 m flag on a body with no measured depth is LOW confidence);
  * consecutive flagged ticks on one body form an EPISODE.

Each episode is classified with the verifier's decision tree (mover_syn.txt "verify"), first match:
  STREAMING    the body was not in the hkWorld at the flagged tick: removed before he got there,
               added after, or never added this run ([COLL_WORLD] ADD/REMOVE timeline)
  WRITER       a direct position write (TELEPORT / SETPOS / SETXFORM) or a physrep change
               (SET_FILTERINFO, SET_PHYSREP_MODE, DEACTIVATE, phantom layer != 2) around the
               episode: the logged caller lr names the writer
  GATE         the mover did not integrate on a flagged tick (off-camera-skip / keyframed /
               inactive path)
  BROADPHASE   the body is in the world but not in his phantom's overlap list
  MOVER        in the overlap list, integrate path taken, still no contact: a live-state mover
               issue -> dump at that tick and golden-call it; ONLY THEN is H-D (the original
               data) a candidate: compare that one building with Xenia's own shape
  UNKNOWN      world state not established (not layer 7, no event within 300 m, not in the
               overlap list) or overlap list truncated with the body not in the shown part

Zero flags over many minutes of fast flight = not reproducible on this build (the verifier's
"most likely fixed by 04bce60 or dfaa887" branch). The report always states how much fast flight
the log covered, and which run bodies had no snapshot AABB (pass-throughs into those are
invisible to this tool).

USAGE
    python tools/collision_route_analyze.py <superman_returns.log>                # text report
    python tools/collision_route_analyze.py <log> --json out.json                 # + machine-readable
    python tools/collision_route_analyze.py --xenia <xenia_route.jsonl>           # the Xenia side
    python tools/collision_route_analyze.py <log> --layers 3,6,7,13 --threshold 4
    python tools/collision_route_analyze.py --build-wall-depth                    # (re)build the
        per-body wall-depth table from archive/golden_call_mover (classify_mover.py --verbose)
        into archive/v1_invest/collision/route/wall_depth.json (the default --wall-depth)
Tests: python tools/test_collision_route_analyze.py
"""
from __future__ import annotations

import argparse
import ast
import json
import math
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SNAPDIR = os.path.join(ROOT, "archive", "v1_invest", "collision", "snapshots")
DEFAULT_SNAPSHOTS = [os.path.join(SNAPDIR, n) for n in
                     ("ours_midtown.json", "ours_start.json", "xenia_sound_gameplay.json")]
DEFAULT_WALL_DEPTH = os.path.join(ROOT, "archive", "v1_invest", "collision", "route", "wall_depth.json")
MOVER_DIR = os.path.join(ROOT, "archive", "golden_call_mover")

MATCH_TOL = 0.5        # metres, translation match (the route kit's value)
CELL = 64.0            # grid cell for the AABB index
WRITER_WINDOW = (3, 1)  # ticks before / after an episode that count as "around" it
EPISODE_GAP = 3        # flagged ticks of one body closer than this merge into one episode

FNUM = r"[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|nan|inf)(?:\(ind\))?"
RE_PATH = re.compile(
    r"\[COLL_PATH\] t=(\d+) ms=(\d+) loco=(\S+) def=(\d+) sub=(\d+) path=(\S+) L=(\d+) "
    r"in=\(([^)]*)\) out=\(([^)]*)\) v=\(([^)]*)\) spd=(\S+) man=(\d+)\[([^\]]*)\] ov=(\d+)\[([^\]]*)\]")
RE_BODY = re.compile(r"\[COLL_BODY\] body=0x([0-9A-Fa-f]+) src=(\w+) pos=\(([^)]*)\) L=(\d+) "
                     r"shapevt=0x([0-9A-Fa-f]+) t=(\d+)")
RE_WORLD = re.compile(
    r"\[COLL_WORLD\] (ADD|REMOVE) st=(\w+) body=0x([0-9A-Fa-f]+) world=0x([0-9A-Fa-f]+) lock=(\d+) L=(\d+) "
    r"shapevt=0x([0-9A-Fa-f]+) pos=\(([^)]*)\) sup=\(([^)]*)\) spd=(\S+) dist=(\S+) t=(\d+) ms=(\d+) "
    r"lr=0x([0-9A-Fa-f]+)")
RE_EVENT = re.compile(r"\[COLL_MOVER\] (TELEPORT|SETPOS|SETXFORM|SET_PHYSREP_MODE|DEACTIVATE|SET_FILTERINFO|"
                      r"NO-INTEGRATE)\b(.*?)\s+t=(\d+)\s*$")
RE_SUMMARY = re.compile(r"\[COLL_MOVER\] 1s .*?dropped=(\d+)")
RE_WATCH = re.compile(r"\[COLL_WATCH\] (enabled|REFUSED)")


def load_json(path):
    with open(path) as f:
        return json.load(f)


def save_json(obj, path):
    with open(path, "w") as f:
        json.dump(obj, f, indent=1)


def fnum(s):
    s = s.strip().lower()
    try:
        return float(s)
    except ValueError:
        return math.nan if "nan" in s else (math.inf if "inf" in s else math.nan)


def vec(s):
    return [fnum(x) for x in s.split()]


def addrs(s):
    """'85D921C0,85D8FD40,+3' -> ([0x85D921C0, 0x85D8FD40], 3)"""
    out, more = [], 0
    for x in s.split(","):
        x = x.strip()
        if not x:
            continue
        if x.startswith("+"):
            more = int(x[1:])
        else:
            out.append(int(x, 16))
    return out, more


# ── records (both sources normalise to these dicts) ───────────────────────────────────────────
def iter_log(path):
    with open(path, errors="replace") as f:
        for line in f:
            if "[COLL_" not in line:
                continue
            m = RE_PATH.search(line)
            if m:
                g = m.groups()
                man, _ = addrs(g[12])
                ov, ovMore = addrs(g[14])
                yield {"k": "path", "t": int(g[0]), "ms": int(g[1]), "loco": g[2], "def": int(g[3]),
                       "sub": int(g[4]), "path": g[5], "L": int(g[6]), "in": vec(g[7]), "out": vec(g[8]),
                       "v": vec(g[9]), "spd": fnum(g[10]), "manN": int(g[11]), "man": man, "ovN": int(g[13]),
                       "ov": ov, "ovTrunc": ovMore > 0}
                continue
            m = RE_BODY.search(line)
            if m:
                g = m.groups()
                yield {"k": "body", "body": int(g[0], 16), "src": g[1], "pos": vec(g[2]), "L": int(g[3]),
                       "shapevt": int(g[4], 16), "t": int(g[5])}
                continue
            m = RE_WORLD.search(line)
            if m:
                g = m.groups()
                yield {"k": "world", "ev": g[0], "st": g[1], "body": int(g[2], 16), "lock": int(g[4]),
                       "L": int(g[5]), "shapevt": int(g[6], 16), "pos": vec(g[7]), "sup": vec(g[8]),
                       "spd": fnum(g[9]), "dist": fnum(g[10]), "t": int(g[11]), "ms": int(g[12]),
                       "lr": int(g[13], 16)}
                continue
            m = RE_EVENT.search(line)
            if m:
                yield {"k": "event", "ev": m.group(1), "detail": m.group(2).strip(), "t": int(m.group(3))}
                continue
            m = RE_SUMMARY.search(line)
            if m:
                yield {"k": "summary", "dropped": int(m.group(1))}
                continue
            m = RE_WATCH.search(line)
            if m:
                yield {"k": "watch", "on": m.group(1) == "enabled"}


def _hex(x):
    return int(x, 16) if isinstance(x, str) else int(x)


def iter_xenia(path):
    """tools/xenia_collision_route.js messages (one JSON object per line)."""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                m = json.loads(line)
            except ValueError:
                continue
            t = m.get("t")
            if t == "path":
                yield {"k": "path", "t": m["tick"], "ms": m.get("ms", 0), "loco": m.get("loco", "?"),
                       "def": m.get("def") or 0, "sub": m.get("sub") or 0, "path": m["path"], "L": m.get("L", 2),
                       "in": m["in"], "out": m["out"], "v": m.get("v", [0, 0, 0]), "spd": m.get("spd", 0.0),
                       "manN": m.get("manN", len(m.get("man", []))), "man": [_hex(a) for a in m.get("man", [])],
                       "ovN": m.get("ovN", len(m.get("ov", []))), "ov": [_hex(a) for a in m.get("ov", [])],
                       "ovTrunc": m.get("ovN", 0) > len(m.get("ov", []))}
            elif t == "body":
                yield {"k": "body", "body": _hex(m["body"]), "src": m.get("src", "?"), "pos": m["pos"],
                       "L": m.get("layer", -1), "shapevt": _hex(m.get("shapevt", 0)), "t": m.get("tick", 0)}
            elif t == "world":
                yield {"k": "world", "ev": m["ev"], "st": m["st"], "body": _hex(m["body"]), "lock": m.get("lock", 0),
                       "L": m.get("layer", -1), "shapevt": _hex(m.get("shapevt", 0)), "pos": m["pos"],
                       "sup": m.get("sup") or [math.nan] * 3, "spd": m.get("spd", math.nan),
                       "dist": m.get("dist") if m.get("dist") is not None else math.nan, "t": m.get("tick", 0),
                       "ms": m.get("ms", 0), "lr": 0}
            elif t == "teleport":
                yield {"k": "event", "ev": "TELEPORT", "detail": "%.3f m" % m.get("d", 0), "t": m["tick"]}
            elif t == "event":
                yield {"k": "event", "ev": m["ev"], "detail": "r4=%s r5=%s" % (m.get("r4"), m.get("r5")),
                       "t": m.get("tick", 0)}
            # no "watch" record: Frida attaches each hook only after Xenia has JIT-compiled the
            # function, i.e. after its first call, so the ADD stream never covers the whole load
            # and "never added" stays UNKNOWN on the Xenia side (only "removed"/"added late" count)


# ── the body AABB database ─────────────────────────────────────────────────────────────────────
class BodyDB:
    def __init__(self, layers):
        self.layers = set(layers)
        self.bodies = []     # {pos, lo, hi, layer, name, cls, src}
        self.grid = {}

    def _cells(self, lo, hi):
        for cx in range(int(math.floor(lo[0] / CELL)), int(math.floor(hi[0] / CELL)) + 1):
            for cz in range(int(math.floor(lo[2] / CELL)), int(math.floor(hi[2] / CELL)) + 1):
                yield (cx, cz)

    def add(self, pos, lo, hi, layer, name, cls, src):
        for b in self.near_translation(pos):
            if b["layer"] == layer and b["cls"] == cls:
                b["src"].add(src)
                return b
        b = {"idx": len(self.bodies), "pos": list(pos), "lo": list(lo), "hi": list(hi), "layer": layer,
             "name": name, "cls": cls, "src": {src}, "wall": None}
        self.bodies.append(b)
        for c in self._cells(lo, hi):
            self.grid.setdefault(c, []).append(b)
        # translation lookups use the translation's own cell as well
        self.grid.setdefault(("t",) + self._tcell(pos), []).append(b)
        return b

    def _tcell(self, p):
        return (int(math.floor(p[0] / CELL)), int(math.floor(p[2] / CELL)))

    def near_translation(self, p):
        if any(math.isnan(x) for x in p):
            return []
        cx, cz = self._tcell(p)
        out = []
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for b in self.grid.get(("t", cx + dx, cz + dz), []):
                    if all(abs(b["pos"][i] - p[i]) <= MATCH_TOL for i in range(3)):
                        out.append(b)
        return out

    def match(self, p):
        c = self.near_translation(p)
        return c[0] if c else None

    def containing(self, p):
        if any(math.isnan(x) for x in p):
            return []
        cell = (int(math.floor(p[0] / CELL)), int(math.floor(p[2] / CELL)))
        out = []
        for b in self.grid.get(cell, []):
            d = depth_in(p, b["lo"], b["hi"])
            if d > 0:
                out.append((b, d))
        return out


def depth_in(p, lo, hi):
    """>0 inside: distance from p to the nearest AABB face."""
    return min(min(p[i] - lo[i], hi[i] - p[i]) for i in range(3))


def load_snapshots(paths, layers):
    db = BodyDB(layers)
    used = []
    for path in paths:
        if not os.path.exists(path):
            continue
        snap = load_json(path)
        src = os.path.basename(path)
        n = 0
        for b in snap.get("bodies", []):
            if b.get("layer") not in db.layers or not b.get("static") or not b.get("aabb") or not b.get("pos"):
                continue
            db.add(b["pos"], b["aabb"][0], b["aabb"][1], b["layer"], b.get("name"),
                   (b.get("shape") or {}).get("cls"), src)
            n += 1
        used.append((src, n))
    return db, used


def apply_wall_depth(db, path):
    if not path or not os.path.exists(path):
        return 0
    n = 0
    for e in load_json(path):
        b = db.match(e["pos"])
        if b is not None:
            b["wall"] = max(b["wall"] or 0.0, float(e["wall_depth"]))
            n += 1
    return n


# ── analysis ──────────────────────────────────────────────────────────────────────────────────
class Analysis:
    def __init__(self, db, threshold=4.0, high=6.0):
        self.db, self.threshold, self.high = db, threshold, high
        self.addr_pos = {}       # run body addr -> latest translation seen
        self.addr_db = {}        # run body addr -> db body (by translation)
        self.hist = {}           # db idx -> [(t, "added"/"removed", dist, spd, addr, lr)]
        self.events = []         # writer/physrep events (t, ev, detail)
        self.flags = []          # per flagged tick
        self.watch_on = False
        self.world_seen_before_watch = False
        self.dropped = 0
        self.l7_adds = 0         # layer-7 "added" lines seen: proof the ADD stream is live
        self.fast_ticks = 0
        self.fast_ms = [None, None]
        self.path_ticks = 0
        self.unmapped_l7 = {}    # addr -> pos (L7 world bodies with no snapshot AABB)
        self.layer_nonstd = []   # ticks with phantom layer != 2

    def _bind(self, addr, pos):
        if pos is None or any(math.isnan(x) for x in pos):
            return self.addr_db.get(addr)
        self.addr_pos[addr] = pos
        b = self.db.match(pos)
        if b is None:
            self.addr_db.pop(addr, None)
        else:
            self.addr_db[addr] = b
        return b

    def feed(self, r):
        k = r["k"]
        if k == "watch":
            self.watch_on = self.watch_on or r["on"]
        elif k == "summary":
            self.dropped += r["dropped"]
        elif k == "body":
            self._bind(r["body"], r["pos"])
        elif k == "world":
            if not self.watch_on:
                self.world_seen_before_watch = True
            b = self._bind(r["body"], r["pos"])
            if r["L"] == 7 and r["st"] == "added":
                self.l7_adds += 1
            if r["st"] in ("added", "removed"):
                if b is not None:
                    self.hist.setdefault(b["idx"], []).append(
                        (r["t"], r["st"], r["dist"], r["spd"], r["body"], r["lr"]))
                elif r["L"] == 7 and r["st"] == "added":
                    self.unmapped_l7[r["body"]] = r["pos"]
                elif r["st"] == "removed":
                    self.unmapped_l7.pop(r["body"], None)
        elif k == "event":
            self.events.append((r["t"], r["ev"], r["detail"]))
        elif k == "path":
            self.path_ticks += 1
            if r["spd"] > 20.0:
                self.fast_ticks += 1
                if self.fast_ms[0] is None:
                    self.fast_ms[0] = r["ms"]
                self.fast_ms[1] = r["ms"]
            if r["L"] != 2:
                self.layer_nonstd.append(r["t"])
            man = {self.addr_db.get(a, {}).get("idx") for a in r["man"]} - {None}
            ov = {self.addr_db.get(a, {}).get("idx") for a in r["ov"]} - {None}
            for b, d in self.db.containing(r["out"]):
                thr = self.threshold
                if b["wall"] is not None:
                    thr = max(thr, b["wall"] + 1.0)
                if d <= thr or b["idx"] in man:
                    continue
                self.flags.append({"t": r["t"], "ms": r["ms"], "idx": b["idx"], "depth": d, "thr": thr,
                                   "path": r["path"], "L": r["L"], "loco": r["loco"], "spd": r["spd"],
                                   "out": r["out"], "in_ov": b["idx"] in ov, "ovTrunc": r["ovTrunc"],
                                   "ovN": r["ovN"], "manN": r["manN"]})

    # the world state of db body idx at tick t
    def world_state(self, idx, t):
        h = self.hist.get(idx, [])
        # an event logged with t=N happened AFTER mover tick N moved him (the watch bumps its tick
        # counter after the guest mover returns), so it is "before" tick t only when N < t
        before = [e for e in h if e[0] < t]
        after = [e for e in h if e[0] >= t]
        if before:
            last = before[-1]
            return ("in" if last[1] == "added" else "removed", last, after[0] if after else None)
        return ("never", None, after[0] if after else None)

    def episodes(self):
        by = {}
        for f in self.flags:
            by.setdefault(f["idx"], []).append(f)
        eps = []
        for idx, fl in by.items():
            fl.sort(key=lambda f: f["t"])
            cur = [fl[0]]
            for f in fl[1:]:
                if f["t"] - cur[-1]["t"] <= EPISODE_GAP:
                    cur.append(f)
                else:
                    eps.append(self.classify(idx, cur))
                    cur = [f]
            eps.append(self.classify(idx, cur))
        eps.sort(key=lambda e: e["t0"])
        return eps

    def classify(self, idx, fl):
        b = self.db.bodies[idx]
        t0, t1 = fl[0]["t"], fl[-1]["t"]
        maxd = max(f["depth"] for f in fl)
        notes = []
        state, last, nxt = self.world_state(idx, t0)
        in_ov = any(f["in_ov"] for f in fl)
        trunc = any(f["ovTrunc"] and not f["in_ov"] for f in fl)
        wev = [e for e in self.events
               if t0 - WRITER_WINDOW[0] <= e[0] <= t1 + WRITER_WINDOW[1] and e[1] != "NO-INTEGRATE"]
        layers = sorted({f["L"] for f in fl if f["L"] != 2})
        paths = sorted({f["path"] for f in fl if f["path"] != "integrate"})
        coverage = (self.watch_on and self.l7_adds > 0 and not self.world_seen_before_watch
                    and self.dropped == 0)
        if in_ov:
            state_eff = "in"
        else:
            state_eff = state
        if state_eff == "removed":
            cls = "STREAMING"
            notes.append("removed at t=%d (dist %.1f m, speed %.1f m/s) before he reached it at t=%d"
                         % (last[0], last[2], last[3], t0))
        elif state_eff == "never" and nxt is not None:
            cls = "STREAMING"
            notes.append("added only at t=%d (dist %.1f m, speed %.1f m/s), after he was inside it at t=%d"
                         % (nxt[0], nxt[2], nxt[3], t0))
        elif state_eff == "never" and b["layer"] == 7 and coverage:
            cls = "STREAMING"
            notes.append("never added to the hkWorld in this run (every layer-7 ADD is logged)")
        elif wev or layers:
            cls = "WRITER"
        elif paths:
            cls = "GATE"
        elif state_eff == "in" and not in_ov and not trunc:
            cls = "BROADPHASE"
        elif in_ov and not paths:
            cls = "MOVER"
            notes.append("next: dump at t=%d and golden-call it; only then H-D (compare this building with "
                         "Xenia's own shape)" % t0)
        else:
            cls = "UNKNOWN"
            if state_eff == "never":
                notes.append("world state not established (%s)" %
                             ("not layer 7: ADD/REMOVE only logged within 300 m" if b["layer"] != 7 else
                              "log coverage incomplete: watch enabled late or lines dropped"))
            if trunc:
                notes.append("overlap list truncated in the log and the body not in its shown part")
        if wev:
            notes.append("events: " + "; ".join("t=%d %s %s" % e for e in wev[:6]))
        if layers:
            notes.append("phantom layer %s on flagged ticks (Superman's normal layer is 2)" % layers)
        if paths:
            notes.append("mover path %s on flagged ticks" % paths)
        if cls != "STREAMING" and state == "in" and last is not None:
            notes.append("in world since t=%d" % last[0])
        conf = "HIGH" if (maxd > self.high or b["wall"] is not None) else "LOW"
        if conf == "LOW":
            notes.append("LOW confidence: depth %.2f m <= %.0f m and no measured wall depth for this body "
                         "(walls sit up to ~7 m inside some AABBs)" % (maxd, self.high))
        return {"class": cls, "confidence": conf, "t0": t0, "t1": t1, "ticks": len(fl), "max_depth": round(maxd, 3),
                "threshold": fl[0]["thr"], "body": {"name": b["name"], "cls": b["cls"], "layer": b["layer"],
                                                    "pos": b["pos"], "aabb": [b["lo"], b["hi"]],
                                                    "wall_depth": b["wall"], "snapshots": sorted(b["src"])},
                "speed": round(max(f["spd"] for f in fl), 1), "loco": sorted({f["loco"] for f in fl}),
                "in_overlap_list": in_ov, "world_state": state_eff, "first_out": fl[0]["out"], "notes": notes}


def analyze(records, db, threshold=4.0):
    a = Analysis(db, threshold)
    for r in records:
        a.feed(r)
    return a


def report(a, eps, out=sys.stdout):
    secs = ((a.fast_ms[1] - a.fast_ms[0]) / 1000.0) if a.fast_ms[0] is not None else 0.0
    p = lambda *x: print(*x, file=out)  # noqa: E731
    p("collision_route_analyze: %d path ticks, %d fast (> 20 m/s) spanning %.0f s; watch banner %s; "
      "dropped lines %d" % (a.path_ticks, a.fast_ticks, secs, "seen" if a.watch_on else "NOT seen", a.dropped))
    p("  AABB db: %d bodies; run bodies bound to it: %d; layer-7 world bodies with NO snapshot AABB: %d "
      "(pass-throughs into those are invisible here)" % (len(a.db.bodies), len(a.addr_db), len(a.unmapped_l7)))
    if not eps:
        p("RESULT: 0 flags." + (" Not reproduced in %.0f s of fast flight." % secs if a.fast_ticks else
                                " No fast flight in this log - nothing tested."))
        return
    counts = {}
    for e in eps:
        counts[e["class"]] = counts.get(e["class"], 0) + 1
    p("RESULT: %d episode(s): %s" % (len(eps), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    for e in eps:
        b = e["body"]
        p("- %s [%s] t=%d..%d (%d ticks) depth %.2f m (thr %.1f) speed %.1f loco %s ov=%s world=%s"
          % (e["class"], e["confidence"], e["t0"], e["t1"], e["ticks"], e["max_depth"], e["threshold"],
             e["speed"], "/".join(e["loco"]), int(e["in_overlap_list"]), e["world_state"]))
        p("    body %s %s L%d pos (%.2f %.2f %.2f) aabb %s wall %s [%s]"
          % (b["name"], b["cls"], b["layer"], b["pos"][0], b["pos"][1], b["pos"][2], b["aabb"], b["wall_depth"],
             ",".join(b["snapshots"])))
        for n in e["notes"]:
            p("    " + n)


# ── wall-depth table from the golden-call mover cases ─────────────────────────────────────────
RE_CASE = re.compile(r"^(\w+) (\w+) (0x[0-9A-Fa-f]+) (\S+) v(\d+) (ax\d[+-])\s+(\S+)\s+(\{.*?\})")


def build_wall_depth(out_path=DEFAULT_WALL_DEPTH, mover_dir=MOVER_DIR):
    classify = os.path.join(ROOT, "scripts", "difftest", "havok", "classify_mover.py")
    snaps = {"golden": os.path.join(SNAPDIR, "xenia_sound_gameplay.json"),
             "midtown": os.path.join(SNAPDIR, "ours_midtown.json")}
    pos_of = {}
    for key, sp in snaps.items():
        pos_of[key] = {b["addr"].lower(): b for b in load_json(sp)["bodies"]}
    depth = {}
    for key in ("golden", "midtown"):
        for s in ("mv", "fly"):
            script = os.path.join(mover_dir, "%s_%s.script" % (s, key))
            outp = os.path.join(mover_dir, "out", "%s_%s.recomp.out" % (s, key))
            if not (os.path.exists(script) and os.path.exists(outp)):
                continue
            txt = subprocess.run([sys.executable, classify, script, outp, "--verbose"], capture_output=True,
                                 text=True).stdout
            for line in txt.splitlines():
                m = RE_CASE.match(line.strip())
                if not m or m.group(7) not in ("INSET-STOP", "BLOCKED"):
                    continue
                info = ast.literal_eval(m.group(8))
                if not info.get("contact"):
                    continue
                b = pos_of[key].get(m.group(3).lower())
                if not b or not b.get("pos"):
                    continue
                k = tuple(round(x, 2) for x in b["pos"])
                cur = depth.get(k)
                if cur is None or info["pen_final"] > cur["wall_depth"]:
                    depth[k] = {"pos": b["pos"], "wall_depth": info["pen_final"], "name": b.get("name"),
                                "source": "%s_%s %s" % (s, key, m.group(3))}
    rows = sorted(depth.values(), key=lambda r: r["pos"])
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    save_json(rows, out_path)
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", help="our run log (superman_returns*.log)")
    ap.add_argument("--xenia", help="xenia_collision_route.js JSONL instead of our log")
    ap.add_argument("--snapshots", default=",".join(DEFAULT_SNAPSHOTS))
    ap.add_argument("--layers", default="7")
    ap.add_argument("--threshold", type=float, default=4.0)
    ap.add_argument("--wall-depth", default=DEFAULT_WALL_DEPTH)
    ap.add_argument("--json")
    ap.add_argument("--build-wall-depth", action="store_true")
    a = ap.parse_args(argv)
    if a.build_wall_depth:
        rows = build_wall_depth(a.wall_depth)
        print("wall depth: %d bodies -> %s (max %.2f m)" % (len(rows), a.wall_depth,
                                                              max([r["wall_depth"] for r in rows] or [0])))
        if not a.log and not a.xenia:
            return 0
    if not a.log and not a.xenia:
        ap.error("a log (or --xenia) is required")
    db, used = load_snapshots([s for s in a.snapshots.split(",") if s], [int(x) for x in a.layers.split(",")])
    nw = apply_wall_depth(db, a.wall_depth)
    print("snapshots: %s; wall depths applied: %d" % (", ".join("%s %d" % u for u in used), nw))
    recs = iter_xenia(a.xenia) if a.xenia else iter_log(a.log)
    an = analyze(recs, db, a.threshold)
    eps = an.episodes()
    report(an, eps)
    if a.json:
        save_json({"episodes": eps, "fast_ticks": an.fast_ticks, "path_ticks": an.path_ticks,
                   "unmapped_layer7": len(an.unmapped_l7), "dropped": an.dropped}, a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
