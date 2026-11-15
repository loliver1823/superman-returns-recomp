#!/usr/bin/env python3
"""Tests for tools/verify_run.py (python tools/test_verify_run.py).

verify_run.py had no tests before iter-463. These pin the existing behaviour it must keep
(sheets + checklist for a natural frames dir, exit 1 with no frames) and the dev-tool refusal
added for DEVTOOLS_DESIGN.md 3.3: a RUN_CLASS.json sidecar or a devtool_ name, in the frames dir
or any ancestor, refuses with exit 3 and writes nothing; --allow-devtool writes stamped sheets.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import verify_run as V  # noqa: E402
from PIL import Image  # noqa: E402


def frames(dirpath, n=4):
    os.makedirs(dirpath, exist_ok=True)
    for i in range(n):
        Image.new("RGB", (64, 36), (40 * i, 20, 200 - 40 * i)).save(
            os.path.join(dirpath, "present_cl_000_p%06u_64x36.bmp" % (i * 700)))
    return dirpath


def run(argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = V.main(argv)
    return rc, buf.getvalue()


class VerifyRun(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="verify_run_test_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_natural_dir_unchanged_behaviour(self):
        d = frames(os.path.join(self.tmp, "natural", "frames"))
        out = os.path.join(self.tmp, "sheets")
        rc, text = run([d, "--out", out])
        self.assertEqual(rc, 0, text)
        self.assertTrue(os.path.isfile(os.path.join(out, "sheet_00.png")))
        with open(os.path.join(out, "checklist.txt")) as f:
            cl = f.read()
        self.assertTrue(cl.startswith("VERDICT GATE"))
        self.assertNotIn("DEV-TOOL", cl)
        self.assertIsNone(V.devtool_taint(d))

    def test_no_frames(self):
        d = os.path.join(self.tmp, "empty")
        os.makedirs(d)
        rc, text = run([d, "--out", os.path.join(self.tmp, "o")])
        self.assertEqual(rc, 1)
        self.assertIn("no present_*.bmp", text)

    def test_sidecar_in_dir_refused(self):
        d = frames(os.path.join(self.tmp, "run", "frames"))
        with open(os.path.join(d, "RUN_CLASS.json"), "w") as f:
            json.dump({"run_class": "dev-tool-assisted"}, f)
        out = os.path.join(self.tmp, "sheets")
        rc, text = run([d, "--out", out])
        self.assertEqual(rc, 3)
        self.assertIn("REFUSED", text)
        self.assertFalse(os.path.exists(out))

    def test_sidecar_in_ancestor_refused(self):
        run_dir = os.path.join(self.tmp, "anyname")
        d = frames(os.path.join(run_dir, "drive", "attempt1", "frames"))
        with open(os.path.join(run_dir, "RUN_CLASS.json"), "w") as f:
            f.write("{}")
        rc, _ = run([d, "--out", os.path.join(self.tmp, "o")])
        self.assertEqual(rc, 3)

    def test_devtool_prefix_refused(self):
        for d in (frames(os.path.join(self.tmp, "devtool_20261001-000000_ours")),
                  frames(os.path.join(self.tmp, "devtool_x", "a", "b"))):
            with self.subTest(d=d):
                rc, text = run([d, "--out", os.path.join(self.tmp, "o")])
                self.assertEqual(rc, 3)
                self.assertIn("devtool_", text)

    def test_prefix_must_be_a_prefix(self):
        d = frames(os.path.join(self.tmp, "not_devtool_run"))
        self.assertIsNone(V.devtool_taint(d))

    def _log(self, text):
        p = os.path.join(self.tmp, "superman_returns_run0.log")
        with open(p, "w") as f:
            f.write(text)
        return p

    def test_log_tags_refused(self):
        """Phase 2: the exe's own tags - [RUN_CLASS] other than natural+hud, devtoolActs > 0."""
        d = frames(os.path.join(self.tmp, "plain", "frames"))
        for text, want in (("[RUN_CLASS] dev-tool-assisted tools=levelskip source=env\n", 3),
                           ("[RUN_CLASS] devtool-derived-save marker=x\n", 3),
                           ("[RUN_CLASS] save-seeded label=w1 packages=1\n", 3),
                           ("[GAUGE] 10s | feTick=1 aptPump=2 clipCtorAll=0 clipCtorReal=0 defExec=0 aptReq=0 "
                            "devtools=0x0 devtoolActs=2\n", 3),
                           ("[RUN_CLASS] natural+hud tools=objstate source=env\n", 0),
                           ("[GAUGE] 10s | feTick=1 aptPump=2 clipCtorAll=0 clipCtorReal=0 defExec=0 aptReq=0 "
                            "devtools=0x0 devtoolActs=0\n", 0)):
            with self.subTest(text=text):
                rc, out = run([d, "--out", os.path.join(self.tmp, "o"), "--log", self._log(text)])
                self.assertEqual(rc, want, out)

    def test_log_in_frames_dir_checked(self):
        d = frames(os.path.join(self.tmp, "plain2", "frames"))
        with open(os.path.join(d, "superman_returns_run0.log"), "w") as f:
            f.write("[RUN_CLASS] dev-tool-assisted tools=levelskip source=env\n")
        rc, _ = run([d, "--out", os.path.join(self.tmp, "o")])
        self.assertEqual(rc, 3)

    def test_allow_devtool_stamps(self):
        d = frames(os.path.join(self.tmp, "devtool_y"))
        out = os.path.join(self.tmp, "sheets")
        rc, text = run([d, "--out", out, "--allow-devtool"])
        self.assertEqual(rc, 0)
        self.assertIn("DEV-TOOL-ASSISTED", text)
        with open(os.path.join(out, "checklist.txt")) as f:
            self.assertTrue(f.readline().startswith("DEV-TOOL-ASSISTED RUN"))


if __name__ == "__main__":
    unittest.main(verbosity=1)
