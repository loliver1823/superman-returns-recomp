#!/usr/bin/env python3
"""Tests for tools/collision_route_analyze.py (python tools/test_collision_route_analyze.py).

Synthetic snapshot + synthetic logs, one scenario per branch of the decision tree, plus:
  * a FORMAT-DRIFT test: the printf formats are read out of kernel/collision_watch.cpp,
    instantiated, and must parse (so a C++ format change cannot silently blind the analyzer);
  * a real line captured from the golden-call harness run of the watch;
  * the Xenia JSONL route gives the same verdict as the equivalent log.
"""
from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import collision_route_analyze as A  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# Two buildings (layer 7) and one layer-3 prop. AABB of B1: x -2301..-2262, y 1172..1374, z -375..-355
B1 = {"pos": [-2281.447, 1176.876, -365.411], "aabb": [[-2301.17, 1171.95, -375.39], [-2262.25, 1374.14, -354.79]],
      "name": "Havok_Box01", "layer": 7}
B2 = {"pos": [-2100.0, 1176.876, -365.0], "aabb": [[-2120.0, 1171.95, -385.0], [-2080.0, 1300.0, -345.0]],
      "name": "Havok_Box02", "layer": 7}
B3 = {"pos": [-1900.0, 1176.876, -365.0], "aabb": [[-1910.0, 1171.95, -375.0], [-1890.0, 1200.0, -355.0]],
      "name": "prop", "layer": 3}
CENTRE1 = (-2281.7, 1250.0, -365.1)      # ~9.7 m inside B1 (z faces)
CENTRE2 = (-2100.0, 1250.0, -365.0)      # 20 m inside B2


def snap(bodies):
    out = []
    for i, b in enumerate(bodies):
        out.append({"addr": "0x%08X" % (0x85000000 + 0x100 * i), "layer": b["layer"], "static": True, "pos": b["pos"],
                    "aabb": b["aabb"], "name": b["name"], "shape": {"cls": "hkListShape"}})
    return {"schema": "havok_snapshot/1", "bodies": out}


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def f3(p):
    return "%.3f %.3f %.3f" % tuple(p)


def L_watch():
    return "[x] [INFO ] [BOOT] [COLL_WATCH] enabled (SR_COLLISION_WATCH, default ON, =0 disables; route=0): flat"


def L_world(ev, st, body, pos, t, L=7, dist=50.0, spd=40.0):
    return ("[x] [INFO ] [BOOT] [COLL_WORLD] %s st=%s body=0x%08X world=0x85E2BF40 lock=0 L=%d shapevt=0x8206BE80 "
            "pos=(%s) sup=(1.00 2.00 3.00) spd=%.1f dist=%.1f t=%d ms=%d lr=0x822BE6B4"
            % (ev, st, body, L, f3(pos), spd, dist, t, t * 33))


def L_body(body, pos, t, L=7):
    return "[x] [INFO ] [BOOT] [COLL_BODY] body=0x%08X src=ov pos=(%s) L=%d shapevt=0x8206BE80 t=%d" % (
        body, f3(pos), L, t)


def L_path(t, out, man=(), ov=(), path="integrate", L=2, spd=60.0, ovmore=0):
    def lst(xs, more=0):
        s = ",".join("%08X" % x for x in xs)
        if more:
            s += (",+%d" % more) if s else "+%d" % more
        return "%d[%s]" % (len(xs) + more, s)
    return ("[x] [INFO ] [BOOT] [COLL_PATH] t=%d ms=%d loco=FLY def=8 sub=4 path=%s L=%d in=(%s) out=(%s) "
            "v=(0.00 0.00 -60.00) spd=%.1f man=%s ov=%s" % (t, t * 33, path, L, f3(out), f3(out), spd, lst(man),
                                                              lst(ov, ovmore)))


def L_event(ev, t, detail="lr=0x82345678 -> (1.00 2.00 3.00)"):
    return "[x] [WARN ] [BOOT] [COLL_MOVER] %s %s t=%d" % (ev, detail, t)


A1, A2, A3 = 0x86001000, 0x86002000, 0x86003000   # run addresses of B1, B2, B3


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.snap = os.path.join(cls.tmp, "snap.json")
        write(cls.snap, json.dumps(snap([B1, B2, B3])))

    def run_lines(self, lines, wall=None, layers=(7,)):
        log = os.path.join(self.tmp, "run.log")
        write(log, "\n".join(lines) + "\n")
        db, _ = A.load_snapshots([self.snap], layers)
        if wall:
            wp = os.path.join(self.tmp, "wall.json")
            write(wp, json.dumps(wall))
            A.apply_wall_depth(db, wp)
        an = A.analyze(A.iter_log(log), db)
        return an, an.episodes()

    def std_world(self):
        """watch on, the ADD stream live (B2 added), B1 added at load."""
        return [L_watch(), L_world("ADD", "added", A1, B1["pos"], 0), L_world("ADD", "added", A2, B2["pos"], 0)]


class Detection(Base):
    def test_in_manifold_is_not_a_flag(self):
        an, eps = self.run_lines(self.std_world() + [L_path(10, CENTRE1, man=[A1], ov=[A1])])
        self.assertEqual(eps, [])

    def test_shallow_is_not_a_flag(self):
        p = (-2281.7, 1250.0, -372.5)   # 2.9 m inside the -z face
        an, eps = self.run_lines(self.std_world() + [L_path(10, p, ov=[A1])])
        self.assertEqual(eps, [])

    def test_slow_ticks_still_checked_and_counted(self):
        an, eps = self.run_lines(self.std_world() + [L_path(10, CENTRE1, ov=[A1], spd=10.0)])
        self.assertEqual(len(eps), 1)
        self.assertEqual(an.fast_ticks, 0)

    def test_low_and_high_confidence(self):
        p5 = (-2281.7, 1250.0, -370.2)  # 5.19 m inside
        _, eps = self.run_lines(self.std_world() + [L_path(10, p5, ov=[A1])])
        self.assertEqual(eps[0]["confidence"], "LOW")
        _, eps = self.run_lines(self.std_world() + [L_path(10, CENTRE1, ov=[A1])])
        self.assertEqual(eps[0]["confidence"], "HIGH")

    def test_wall_depth_raises_threshold(self):
        p7 = (-2281.7, 1250.0, -368.3)  # 7.09 m inside
        wall = [{"pos": B1["pos"], "wall_depth": 6.7}]
        _, eps = self.run_lines(self.std_world() + [L_path(10, p7, ov=[A1])], wall=wall)
        self.assertEqual(eps, [], "7.09 m <= 6.7 + 1 m must not flag")
        _, eps = self.run_lines(self.std_world() + [L_path(10, CENTRE1, ov=[A1])], wall=wall)
        self.assertEqual(len(eps), 1)
        self.assertEqual(eps[0]["confidence"], "HIGH")
        self.assertAlmostEqual(eps[0]["threshold"], 7.7, places=3)

    def test_episode_merge_and_split(self):
        lines = self.std_world() + [L_path(t, CENTRE1, ov=[A1]) for t in (10, 11, 13, 30)]
        _, eps = self.run_lines(lines)
        self.assertEqual([(e["t0"], e["t1"], e["ticks"]) for e in eps], [(10, 13, 3), (30, 30, 1)])

    def test_layer_filter(self):
        p = (-1900.0, 1190.0, -365.0)   # 10 m inside the layer-3 prop
        lines = self.std_world() + [L_path(10, p)]
        _, eps = self.run_lines(lines)
        self.assertEqual(eps, [])
        _, eps = self.run_lines(lines, layers=(3, 7))
        self.assertEqual(len(eps), 1)


class DecisionTree(Base):
    def cls_of(self, lines):
        _, eps = self.run_lines(lines)
        self.assertEqual(len(eps), 1, eps)
        return eps[0]

    def test_streaming_removed_early(self):
        e = self.cls_of(self.std_world() + [L_world("REMOVE", "removed", A1, B1["pos"], 5, dist=250.0),
                                             L_path(10, CENTRE1)])
        self.assertEqual(e["class"], "STREAMING")
        self.assertIn("removed at t=5", e["notes"][0])

    def test_streaming_added_late(self):
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0), L_path(10, CENTRE1),
                 L_world("ADD", "added", A1, B1["pos"], 12, dist=3.0)]
        e = self.cls_of(lines)
        self.assertEqual(e["class"], "STREAMING")
        self.assertIn("added only at t=12", e["notes"][0])

    def test_add_logged_on_the_same_tick_counts_as_after(self):
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0), L_world("ADD", "added", A1, B1["pos"], 10),
                 L_path(10, CENTRE1)]
        self.assertEqual(self.cls_of(lines)["class"], "STREAMING")

    def test_streaming_never_added(self):
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0), L_path(10, CENTRE1)]
        e = self.cls_of(lines)
        self.assertEqual(e["class"], "STREAMING")
        self.assertIn("never added", e["notes"][0])

    def test_never_added_without_add_stream_is_unknown(self):
        e = self.cls_of([L_watch(), L_path(10, CENTRE1)])
        self.assertEqual(e["class"], "UNKNOWN")

    def test_never_added_with_dropped_lines_is_unknown(self):
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0),
                 "[x] [INFO ] [BOOT] [COLL_MOVER] 1s t=5 calls=30 dropped=7 selfUs=1.0", L_path(10, CENTRE1)]
        self.assertEqual(self.cls_of(lines)["class"], "UNKNOWN")

    def test_writer_teleport(self):
        e = self.cls_of(self.std_world() + [L_event("TELEPORT", 9, "12.000 m before mover: last-out (0 0 0) -> in (1 1 1)"),
                                             L_path(10, CENTRE1, ov=[A1])])
        self.assertEqual(e["class"], "WRITER")
        self.assertTrue(any("TELEPORT" in n for n in e["notes"]))

    def test_writer_setxform_after_window_is_ignored(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A1]), L_event("SETXFORM", 20)])
        self.assertEqual(e["class"], "MOVER")

    def test_writer_layer(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A1], L=11)])
        self.assertEqual(e["class"], "WRITER")

    def test_gate(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A1], path="off-camera-skip")])
        self.assertEqual(e["class"], "GATE")

    def test_broadphase(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A2])])
        self.assertEqual(e["class"], "BROADPHASE")

    def test_truncated_overlap_list_is_unknown(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A2], ovmore=5)])
        self.assertEqual(e["class"], "UNKNOWN")

    def test_mover(self):
        e = self.cls_of(self.std_world() + [L_path(10, CENTRE1, ov=[A1])])
        self.assertEqual(e["class"], "MOVER")
        self.assertTrue(any("H-D" in n for n in e["notes"]))

    def test_in_overlap_list_overrides_missing_add(self):
        # no ADD for B1 but it is in the overlap list: it IS in the world -> not STREAMING
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0), L_body(A1, B1["pos"], 9), L_path(10, CENTRE1, ov=[A1])]
        self.assertEqual(self.cls_of(lines)["class"], "MOVER")

    def test_address_reuse_rebinds(self):
        # A1 first names B2, then (after its removal) is reused for B1; the manifold entry A1 at t=10 is B1
        lines = [L_watch(), L_world("ADD", "added", A2, B2["pos"], 0), L_world("ADD", "added", A1, B2["pos"], 0),
                 L_world("REMOVE", "removed", A1, B2["pos"], 3), L_world("ADD", "added", A1, B1["pos"], 4),
                 L_path(10, CENTRE1, man=[A1], ov=[A1])]
        _, eps = self.run_lines(lines)
        self.assertEqual(eps, [])


class Formats(unittest.TestCase):
    def test_nan_tolerant(self):
        self.assertTrue(A.math.isnan(A.fnum("-nan(ind)")))
        self.assertTrue(A.math.isnan(A.fnum("nan")))
        self.assertEqual(A.vec("1.000 -nan(ind) 3.5")[2], 3.5)

    def test_cpp_formats_parse(self):
        """Instantiate the printf formats in kernel/collision_watch.cpp and parse them."""
        with open(os.path.join(ROOT, "kernel", "collision_watch.cpp"), encoding="utf-8") as f:
            src = f.read()
        fmts = {}
        for m in re.finditer(r'"(\[COLL_(PATH|BODY|WORLD|MOVER)\][^"]*)"((?:\s*"[^"]*")*)', src):
            full = m.group(1) + "".join(re.findall(r'"([^"]*)"', m.group(3)))
            fmts.setdefault(m.group(2), []).append(full)

        def inst(f):
            f = f.replace("0x%08X", "0x86001000").replace("%08X", "86001000").replace("%02X", "0F")
            f = re.sub(r"%\.\df", "12.500", f)
            f = f.replace("%u", "7").replace("%s", "WORD")
            return f
        path = inst(fmts["PATH"][0]).replace("man=WORD", "man=1[86001000]").replace("ov=WORD", "ov=2[86001000,+1]")
        r = next(A.iter_log(self._write(path)))
        self.assertEqual((r["k"], r["t"], r["man"], r["ov"], r["ovTrunc"]), ("path", 7, [0x86001000], [0x86001000], True))
        r = next(A.iter_log(self._write(inst(fmts["BODY"][0]))))
        self.assertEqual((r["k"], r["body"], r["L"]), ("body", 0x86001000, 7))
        w = inst(fmts["WORLD"][0]).replace("] WORD st=WORD", "] ADD st=added")
        r = next(A.iter_log(self._write(w)))
        self.assertEqual((r["k"], r["ev"], r["st"], r["t"], r["dist"]), ("world", "ADD", "added", 7, 12.5))
        evs = [f for f in fmts["MOVER"] if re.match(r"\[COLL_MOVER\] (TELEPORT|SETPOS|SETXFORM|SET_PHYSREP_MODE|"
                                                     r"DEACTIVATE|SET_FILTERINFO|NO-INTEGRATE)", f)]
        self.assertGreaterEqual(len(evs), 7)
        for f in evs:
            r = next(A.iter_log(self._write(inst(f))), None)
            self.assertIsNotNone(r, f)
            self.assertEqual((r["k"], r["t"]), ("event", 7), f)
        s = [f for f in fmts["MOVER"] if f.startswith("[COLL_MOVER] 1s")][0]
        r = next(A.iter_log(self._write(inst(s))))
        self.assertEqual((r["k"], r["dropped"]), ("summary", 7))

    def test_real_harness_line(self):
        """Lines emitted by the built watch on the golden-call harness (ours_midtown image)."""
        lines = ["[2026-10-01 07:34:24.407] [INFO ] [BOOT] [COLL_BODY] body=0x85C782E0 src=ov pos=(-2297.141 1176.876 "
                 "-765.107) L=7 shapevt=0x8206BE80 t=11",
                 "[2026-10-01 07:34:24.408] [INFO ] [BOOT] [COLL_PATH] t=11 ms=105 loco=FLY def=8 sub=4 path=integrate L=2 "
                 "in=(-2278.987 1185.540 -754.917) out=(-2278.785 1185.326 -754.221) v=(6.04 -6.44 20.85) spd=22.6 "
                 "man=0[] ov=3[85D921C0,85D8FD40,85C782E0]"]
        recs = list(A.iter_log(self._write("\n".join(lines))))
        self.assertEqual([r["k"] for r in recs], ["body", "path"])
        self.assertEqual(recs[1]["ov"], [0x85D921C0, 0x85D8FD40, 0x85C782E0])
        self.assertAlmostEqual(recs[1]["spd"], 22.6)

    def _write(self, text):
        p = os.path.join(tempfile.mkdtemp(), "l.log")
        write(p, text + "\n")
        return p


class Xenia(Base):
    def test_jsonl_matches_log(self):
        recs = [{"t": "attached", "guest": "0x82596508"},
                {"t": "world", "ev": "ADD", "st": "added", "body": "0x%x" % A2, "layer": 7, "pos": B2["pos"], "tick": 0},
                {"t": "world", "ev": "REMOVE", "st": "removed", "body": "0x%x" % A1, "layer": 7, "pos": B1["pos"],
                 "tick": 5, "dist": 250.0, "spd": 40.0},
                {"t": "path", "tick": 10, "path": "integrate", "L": 2, "in": list(CENTRE1), "out": list(CENTRE1),
                 "spd": 60.0, "man": [], "ov": [], "ovN": 0, "manN": 0}]
        p = os.path.join(self.tmp, "x.jsonl")
        write(p, "\n".join(json.dumps(r) for r in recs) + "\n")
        db, _ = A.load_snapshots([self.snap], (7,))
        eps = A.analyze(A.iter_xenia(p), db).episodes()
        self.assertEqual([e["class"] for e in eps], ["STREAMING"])


class Mutations(Base):
    """The suite must notice a broken tolerance and a broken event ordering."""

    def test_match_tol_mutation(self):
        good = A.MATCH_TOL
        try:
            A.MATCH_TOL = 1e-9
            lines = self.std_world() + [L_body(A1, [B1["pos"][0] + 0.2, B1["pos"][1], B1["pos"][2]], 9),
                                        L_path(10, CENTRE1, man=[A1], ov=[A1])]
            _, eps = self.run_lines(lines)
            self.assertNotEqual(eps, [], "with a zero tolerance the manifold body no longer binds -> false flag")
        finally:
            A.MATCH_TOL = good
        _, eps = self.run_lines(self.std_world() + [L_body(A1, [B1["pos"][0] + 0.2, B1["pos"][1], B1["pos"][2]], 9),
                                                    L_path(10, CENTRE1, man=[A1], ov=[A1])])
        self.assertEqual(eps, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
