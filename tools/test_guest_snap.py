#!/usr/bin/env python3
"""Tests for tools/guest_snap.py (python tools/test_guest_snap.py).

1. Format: a synthetic round trip (full, partial and absent pages), and every corruption the
   parser must refuse (magic, CRC, truncation, unsorted/duplicate table, bad length, schema).
2. The no-zero rule: a read outside the captured pages comes back SHORT; objstate turns it
   into a failed check, never a plausible zero.
3. Equivalence: objstate on a snap written from its own read ledger equals objstate on the
   source image (synthetic world here; the Xenia gameplay golden when present), and the
   multi-reader capture (objstate + save_records) self-verifies.
4. Mutants: plausible wrong readers/writers of the format must each fail the suite.
"""
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import guest_snap as G  # noqa: E402
import havok_snapshot as HS  # noqa: E402
import objstate as O  # noqa: E402
import test_objstate as TO  # noqa: E402


def tmp(suffix=".snap"):
    fd, p = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    return p


class PartialImage(HS.SyntheticImage):
    """A synthetic image where some pages end early (a host-window / dump-range edge)."""

    def __init__(self):
        super().__init__()
        self.limit = {}

    def read(self, va, n):
        out = bytearray()
        for a in range(va, va + n):
            pg = self.pages.get(a >> 12)
            if pg is None or (a & 0xFFF) >= self.limit.get(a >> 12, 4096):
                break
            out.append(pg[a & 0xFFF])
        return bytes(out)


def sample_image():
    img = PartialImage()
    for pg, fill in ((0x82000, 0x11), (0x82001, 0x22), (0x40050, 0x33), (0xE0000, 0x44)):
        img.plant(pg << 12, bytes([fill]) * 4096)
    img.plant(0x82000000, b"MZ")
    img.plant(0x40050000 + 0x10, struct.pack(">I", 0xDEADBEEF))
    img.limit[0xE0000] = 100              # partial page
    return img


class FormatTest(unittest.TestCase):
    def setUp(self):
        self.img = sample_image()
        self.path = tmp()
        self.info = G.write_snap(self.path, self.img, [0x82000, 0x82001, 0x40050, 0xE0000, 0x70000],
                                 meta={"label": "t"})

    def tearDown(self):
        os.unlink(self.path)

    def test_write_info(self):
        self.assertEqual(self.info["pages"], 4)
        self.assertEqual(self.info["partial"], 1)
        self.assertEqual(self.info["missing"], ["0x70000000"])

    def test_round_trip_bytes(self):
        s = G.SnapImage(self.path)
        for va, n in ((0x82000000, 2), (0x82000FFE, 4), (0x40050010, 4), (0xE0000000, 100)):
            self.assertEqual(s.read(va, n), self.img.read(va, n), hex(va))
        self.assertEqual(s.read(0x82000FFE, 4), b"\x11\x11\x22\x22")     # crosses two captured pages
        self.assertEqual(s.u32(0x40050010), 0xDEADBEEF)                  # havok Image helpers work
        self.assertEqual(s.meta["label"], "t")
        self.assertEqual(s.meta["schema"], G.SCHEMA)

    def test_reads_outside_are_short_never_zero(self):
        s = G.SnapImage(self.path)
        self.assertEqual(s.read(0x70000000, 4), b"")                     # unreadable at capture
        self.assertEqual(s.read(0x50000000, 4), b"")                     # never captured
        self.assertEqual(s.read(0x82001FFE, 4), b"\x22\x22")             # next page not captured
        self.assertEqual(s.read(0xE0000060, 8), b"\x44" * 4)             # partial page ends at 100
        self.assertEqual(s.read(0xE0000064, 1), b"")
        self.assertEqual(s.misses, 5)

    def test_iter_chunks(self):
        s = G.SnapImage(self.path)
        ch = list(s.iter_chunks(0x80000000, 0x90000000))
        self.assertEqual([va for va, _ in ch], [0x82000000, 0x82001000])
        self.assertEqual(s.scan_u32([0xDEADBEEF])[0xDEADBEEF], [0x40050010])

    def test_open_image_dispatch(self):
        for opener in (HS.open_image, O.open_image):
            img = opener("snap:" + self.path)
            self.assertEqual(img.kind, "snap")
            self.assertEqual(img.describe()["pages"], 4)

    def test_pages_of_ranges(self):
        self.assertEqual(G.pages_of_ranges([["0x82000000", "0x82002000"], [0x40050010, 0x40050011],
                                            [5, 5]]), [0x40050, 0x82000, 0x82001])

    def _corrupt(self, fn):
        with open(self.path, "rb") as f:
            blob = bytearray(f.read())
        blob = fn(blob)
        p = tmp()
        with open(p, "wb") as f:
            f.write(blob)
        try:
            with self.assertRaises(G.SnapFormatError):
                G.SnapImage(p)
        finally:
            os.unlink(p)

    @staticmethod
    def _recrc(b):
        b[-4:] = struct.pack("<I", zlib.crc32(bytes(b[:-4])) & 0xFFFFFFFF)
        return b

    def _table_off(self, blob):
        ml = struct.unpack_from("<I", blob, 8)[0]
        return 12 + ml + 4

    def test_corruptions_are_refused(self):
        self._corrupt(lambda b: b"XX" + b[2:])                                   # magic
        self._corrupt(lambda b: b[:-5] + bytes([b[-5] ^ 1]) + b[-4:])            # a data byte (CRC)
        self._corrupt(lambda b: b[:len(b) // 2])                                 # truncated

        def swap(b):                                                             # unsorted table
            t = self._table_off(b)
            e0, e1 = bytes(b[t:t + 8]), bytes(b[t + 8:t + 16])
            b[t:t + 8], b[t + 8:t + 16] = e1, e0
            return self._recrc(b)
        self._corrupt(swap)

        def dup(b):                                                              # duplicate page
            t = self._table_off(b)
            b[t + 8:t + 12] = b[t:t + 4]
            return self._recrc(b)
        self._corrupt(dup)

        def badlen(b):                                                           # length 0
            t = self._table_off(b)
            b[t + 4:t + 8] = struct.pack("<I", 0)
            return self._recrc(b)
        self._corrupt(badlen)

        def schema(b):                                                           # foreign meta
            ml = struct.unpack_from("<I", b, 8)[0]
            meta = json.loads(bytes(b[12:12 + ml]))
            meta["schema"] = "guest_snap/0"
            mj = json.dumps(meta).encode()
            nb = bytearray(b[:8]) + struct.pack("<I", len(mj)) + mj + b[12 + ml:]
            return self._recrc(nb)
        self._corrupt(schema)

        def stray(b):                                                            # trailing junk
            return self._recrc(b[:-4] + b"\0\0\0\0" + b[-4:])
        self._corrupt(stray)


class ObjstateEquivalenceTest(unittest.TestCase):
    """objstate on snap:<file> == objstate on the source, when the snap is its own ledger."""

    def world(self):
        return TO.make_world(status=TO.status_words([147, 2]), triggers=TO.DEFAULT_TRIGGERS)

    def test_synthetic_world_round_trip(self):
        w = self.world()
        a = TO.snap(w)
        led = a.pop("_ledger")
        p = tmp()
        try:
            G.write_snap(p, w.img, G.pages_of_ranges(led))
            b = O.snapshot(O.open_image("snap:" + p), None, TO.SYN_GRAPHS)
            self.assertEqual(b.pop("_ledger"), led)
            a.pop("image")
            b.pop("image")
            self.assertEqual(a, b)
            self.assertEqual(a["checks_failed"], 0)
        finally:
            os.unlink(p)

    def test_missing_page_is_a_failed_check_not_a_zero(self):
        w = self.world()
        a = TO.snap(w)
        led = G.pages_of_ranges(a.pop("_ledger"))
        stat_pg = int(next(x["va"] for x in a["stats"] if x["name"] == "CurrentXP"), 16) >> 12
        p = tmp()
        try:
            G.write_snap(p, w.img, [pg for pg in led if pg != stat_pg])
            snap = O.open_image("snap:" + p)
            b = O.snapshot(snap, None, TO.SYN_GRAPHS)
            self.assertGreater(b["checks_failed"], 0)
            self.assertGreater(snap.misses, 0)
            self.assertNotEqual(b["xp"]["CurrentXP"], 0.0)     # not read as a quiet zero
        finally:
            os.unlink(p)

    def test_recording_capture_covers_every_reader_read(self):
        w = self.world()
        rec = G.RecordingImage(w.img)
        a = O.snapshot(rec, None, TO.SYN_GRAPHS)
        led = G.pages_of_ranges(a.pop("_ledger"))
        self.assertTrue(set(led) <= rec.pages)                   # the wrapper sees at least the ledger
        p = tmp()
        try:
            G.write_snap(p, w.img, sorted(rec.pages))
            snap = O.open_image("snap:" + p)
            b = O.snapshot(snap, None, TO.SYN_GRAPHS)
            b.pop("_ledger")
            a.pop("image")
            b.pop("image")
            self.assertEqual(a, b)
            self.assertEqual(snap.misses, 0)
        finally:
            os.unlink(p)


def _golden(name):
    import golden
    return os.path.isfile(golden.DUMPS.get(name, ""))


@unittest.skipUnless(_golden("sound_gameplay"), "gameplay golden absent")
class GoldenSnapTest(unittest.TestCase):
    """DEVTOOLS_DESIGN 6.2 step 5 on the real oracle image: objstate on snap:<file> equals
    objstate on the full Xenia gameplay golden, and the capture self-verifies with save_records."""

    def test_objstate_on_snap_equals_the_golden(self):
        graphs = O.load_fsm_graphs(O.DEFAULT_AST)
        src = O.open_image("xenia:sound_gameplay")
        a = O.snapshot(src, O.DEFAULT_TRIGGER_DOCS, graphs)
        led = a.pop("_ledger")
        p = tmp()
        try:
            info = G.write_snap(p, src, G.pages_of_ranges(led), meta={"label": "gameplay"})
            self.assertEqual(info["missing"], [])
            snap = O.open_image("snap:" + p)
            b = O.snapshot(snap, O.DEFAULT_TRIGGER_DOCS, graphs)
            self.assertEqual(b.pop("_ledger"), led)
            self.assertEqual(snap.misses, 0)
            a.pop("image")
            b.pop("image")
            self.assertEqual(a, b)
            self.assertEqual(a["checks_failed"], 0)
            self.assertEqual(a["base_fsm"]["state_id"], 107)
            self.assertLess(os.path.getsize(p), 2 << 20)
        finally:
            os.unlink(p)

    def test_multi_reader_capture_self_verifies(self):
        p = tmp()
        try:
            info = G.capture("xenia:sound_gameplay", p)
            self.assertEqual(info["verified_equal"], {"objstate": True, "save_records": True})
            self.assertEqual(info["reads_outside_snap"], 0)
            self.assertEqual(info["missing"], [])
        finally:
            os.unlink(p)


class MutantTest(unittest.TestCase):
    """Plausible wrong format implementations must each fail the suite."""

    SUITE = (FormatTest, ObjstateEquivalenceTest)

    def run_suite(self):
        suite = unittest.TestSuite()
        for cls in self.SUITE:
            suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(cls))
        with open(os.devnull, "w") as devnull:
            res = unittest.TextTestRunner(stream=devnull, verbosity=0).run(suite)
        return len(res.failures) + len(res.errors)

    def test_every_mutant_is_killed(self):
        self.assertEqual(self.run_suite(), 0, "the unmutated suite must pass")
        orig_read, orig_parse = G.SnapImage.read, G.parse_snap

        def zero_fill(self, va, n):                       # the banned behaviour: misses read as 0
            got = orig_read(self, va, n)
            return got + b"\0" * (n - len(got))

        def ignore_len(self, va, n):                      # partial pages padded to 4096
            out = bytearray()
            while n > 0:
                pg = self.pages.get(va >> 12)
                if pg is None:
                    break
                pg = pg + b"\0" * (4096 - len(pg))
                off = va & 0xFFF
                take = min(n, 4096 - off)
                out += pg[off:off + take]
                va, n = va + take, n - take
            return bytes(out)

        def one_page_only(self, va, n):                   # never crosses a page boundary
            pg = self.pages.get(va >> 12)
            return b"" if pg is None else pg[va & 0xFFF:(va & 0xFFF) + n]

        def no_crc(blob):
            return orig_parse(blob[:-4] + struct.pack("<I", zlib.crc32(blob[:-4]) & 0xFFFFFFFF))

        mutants = {"zero_fill": ("read", zero_fill), "ignore_partial_len": ("read", ignore_len),
                   "one_page_only": ("read", one_page_only)}
        survivors = []
        for name, (attr, fn) in mutants.items():
            setattr(G.SnapImage, attr, fn)
            try:
                if self.run_suite() == 0:
                    survivors.append(name)
            finally:
                setattr(G.SnapImage, attr, orig_read)
        G.parse_snap = no_crc
        try:
            if self.run_suite() == 0:
                survivors.append("parser_skips_crc")
        finally:
            G.parse_snap = orig_parse
        orig_write = G.write_snap

        def drop_partial(path, img, pages, meta=None):   # writer drops short pages
            class Full:
                def read(self_, va, n):
                    b = img.read(va, n)
                    return b if len(b) == n else b""

                def describe(self_):
                    return img.describe()
            return orig_write(path, Full(), pages, meta)
        G.write_snap = drop_partial
        try:
            if self.run_suite() == 0:
                survivors.append("writer_drops_partial_pages")
        finally:
            G.write_snap = orig_write
        self.assertEqual(survivors, [])


if __name__ == "__main__":
    unittest.main(verbosity=1)
