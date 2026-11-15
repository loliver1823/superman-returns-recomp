#!/usr/bin/env python3
"""Tests for tools/save_records.py (python tools/test_save_records.py).

Independence: the save bytes are produced here by a separate model of the game's writers
(WriterModel below, transcribed from the IDA decompiles named in save_records.py), and the
CRC and slot checksum are re-implemented from the PowerPC disassembly (64-bit register
arithmetic, slw with a 6-bit shift amount, stw of the low word). The tool's decoder must
recover the model's state; the tool's encoder must reproduce the bytes. Mutation checks at
the end prove the independent checks can fail.
"""
from __future__ import annotations

import os
import random
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_records as T  # noqa: E402

# A decrypted retail PE image (local data, never in git); the test that reads it skips without it.
RETAIL_PE = os.environ.get("SR_RETAIL_PE", "retail_pe_decrypted.bin")
# First 8 entries of the CRC table at 0x8292E2F0, read from IDA (retail default.xex).
IDA_CRC_TABLE_HEAD = [0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
                      0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005]


# ------------------------------------------------------------------ independent ports ----
def ppc_crc(data, table):
    """sub_826424F8, instruction by instruction (rotrwi/rotlwi assembly of the seed, rlwinm
    10,22,29 index, slwi 8, or, lwzx, xor; not r3)."""
    if len(data) < 4:
        return 0
    r10 = ((data[0] >> 8) | (data[0] << 24)) & 0xFFFFFFFF      # rotrwi r10, b0, 8
    r8 = ((data[1] >> 16) | (data[1] << 16)) & 0xFFFFFFFF      # rotrwi r8, b1, 16
    r10 |= r8
    r8 = (data[2] << 8) & 0xFFFFFFFF                           # rotlwi r8, b2, 8
    r8 |= r10
    r11 = (~(data[3] | r8)) & 0xFFFFFFFF
    for b in data[4:]:
        r6 = ((r11 << 10 | r11 >> 22) & 0xFFFFFFFF) & 0x3FC    # rlwinm r6, r11, 10, 22, 29
        r11 = ((r11 << 8) & 0xFFFFFFFF) | b
        r11 ^= table[r6 // 4]
    return (~r11) & 0xFFFFFFFF


def ppc_slot_checksum(part2, count, slot):
    """AptDisplayListColumnChecksum_sub_821773E8 disassembly: r11 = count*slot+1, r5 =
    (slot+1)*count, r8 = 3*count + r11; per step r31 = ((r11&3)+1)*8, r30 = ((r8&3)+1)*8;
    slw: amount = low 6 bits, >= 32 -> 0; add into a 64-bit r7; stw stores the low word."""
    r11 = count * slot + 1
    r5 = (slot + 1) * count
    r8 = 3 * count + r11
    r7 = 0
    while r11 < r5:
        r4 = struct.unpack_from(">I", part2, 16 * r11 + 8)[0]
        r3 = struct.unpack_from(">I", part2, 16 * r8 + 8)[0]
        r31 = ((r11 & 3) + 1) << 3
        r30 = ((r8 & 3) + 1) << 3
        slw = lambda v, n: 0 if (n & 0x3F) >= 32 else (v << (n & 0x3F)) & 0xFFFFFFFF  # noqa: E731
        r7 = (r7 + slw(r4, r31) + slw(r3, r30)) & 0xFFFFFFFFFFFFFFFF
        r11 += 1
        r8 += 1
    return r7 & 0xFFFFFFFF


def jhash(name):
    """Str_Hash31_sub_8263C508 (independent copy)."""
    h = 0
    for ch in name:
        c = ord(ch)
        h = (h * 31 + (c - 256 if c >= 128 else c)) % (1 << 32)
    return h


def fnv(name):
    """FNV-1 (AptResolvePathString_sub_826CD770; independent copy)."""
    h = 0x811C9DC5
    for ch in name:
        h = ((h * 0x01000193) % (1 << 32)) ^ ord(ch)
    return h


def bitwise_crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = (c << 1) ^ (0x04C11DB7 if c & 0x80000000 else 0)
            c &= 0xFFFFFFFF
        t.append(c)
    return t


class WriterModel:
    """The game's writers over a model state, into a buffer of 6*count records whose non-dword-0
    bytes start as stale garbage (AptEnsureDisplayListBuffer zeroes dword 0 only)."""

    def __init__(self, schema, rnd):
        self.s, self.rnd = schema, rnd

    def state(self):
        r, s = self.rnd, self.s
        return {"music": r.uniform(0, 1), "sfx": r.uniform(0, 1),
                "pads": [(r.randint(0, 2), r.randint(0, 1), r.randint(0, 1), r.randint(0, 1)) for _ in range(s.pads)],
                "stats": {n: float(r.randint(0, 99999)) for n in s.stats},
                "entities": {n: r.randint(0, 1) for n in s.entities},
                "metro": r.randint(1, 25000),
                "das": {n: (r.uniform(0, 4) if t == 1 else r.getrandbits(32) if t == 5 else r.randint(0, 300))
                        for n, t in s.das}}

    def write_region(self, buf, start, st):
        """Cursor starts at record 1 of the region (a1[26] = count*slot + 1)."""
        cur = [start + 1]

        def rec(i):
            return 16 * i
        # SaveRecords_WriteOptions_sub_82165088
        struct.pack_into(">f", buf, rec(cur[0]) + 8, st["music"])
        struct.pack_into(">f", buf, rec(cur[0]) + 24, st["sfx"])
        cur[0] += 2
        for scheme, x, y, vib in st["pads"]:
            struct.pack_into(">I", buf, rec(cur[0]) + 8, scheme)
            buf[rec(cur[0]) + 24] = x
            buf[rec(cur[0]) + 40] = y
            buf[rec(cur[0]) + 56] = vib
            cur[0] += 4
        # sub_82165198: key then double, per flag&4 stat
        for n in self.s.stats:
            struct.pack_into(">I", buf, rec(cur[0]), jhash(n))
            struct.pack_into(">d", buf, rec(cur[0]) + 8, st["stats"][n])
            cur[0] += 1
        # sub_82165250: key then byte +35
        for n in self.s.entities:
            struct.pack_into(">I", buf, rec(cur[0]), jhash(n))
            buf[rec(cur[0]) + 8] = st["entities"][n]
            cur[0] += 1
        # sub_82186F70
        struct.pack_into(">I", buf, rec(cur[0]) + 8, st["metro"])
        cur[0] += 1
        # AptStoreDisplayObjectValue_sub_82177310: key, then std of an 8-byte stack slot whose
        # first word is the value and second word is stale stack
        for n, t in self.s.das:
            v = st["das"][n]
            struct.pack_into(">I", buf, rec(cur[0]), fnv(n))
            first = struct.pack(">f", v) if t == 1 else struct.pack(">I", v & 0xFFFFFFFF)
            buf[rec(cur[0]) + 8: rec(cur[0]) + 12] = first
            buf[rec(cur[0]) + 12: rec(cur[0]) + 16] = self.rnd.randbytes(4)
            cur[0] += 1
        assert cur[0] == start + self.s.count

    def save_file(self, slots):
        s = self.s
        count = s.count
        part2 = bytearray(self.rnd.randbytes(6 * 16 * count))
        for i in range(6 * count):
            part2[16 * i: 16 * i + 4] = bytes(4)
        part1 = bytearray(self.rnd.randbytes(88))
        part1[:16] = bytes(16)
        for k in range(3):
            struct.pack_into(">f", part1, 16 + 24 * k, -1.0)
            struct.pack_into(">d", part1, 16 + 24 * k + 8, -1.0)
            struct.pack_into(">I", part1, 16 + 24 * k + 16, 0xFFFFFFFF)
        states = {}
        for k in slots:
            live, ckp = self.state(), self.state()
            states[k] = (live, ckp)
            self.write_region(part2, (k + 3) * count, ckp)   # SaveSnapshot copies the capture
            self.write_region(part2, k * count, live)
            struct.pack_into(">f", part1, 16 + 24 * k, 0.5)
            struct.pack_into(">d", part1, 16 + 24 * k + 8, 1234.5)
            struct.pack_into(">I", part1, 16 + 24 * k + 16, 7)
            hdr = bytearray(self.rnd.randbytes(16))            # stack struct {0, junk, chk, junk}
            struct.pack_into(">I", hdr, 0, 0)
            struct.pack_into(">I", hdr, 8, ppc_slot_checksum(part2, count, k))
            part2[16 * k * count: 16 * k * count + 16] = hdr
        table = bitwise_crc_table()
        h = struct.pack(">6I", 0x4D433032, 28 + 88 + len(part2), 88, len(part2),
                        ppc_crc(bytes(part1), table), ppc_crc(bytes(part2), table))
        h += struct.pack(">I", ppc_crc(h, table))
        return h + bytes(part1) + bytes(part2), states


def small_schema():
    return T.Schema(4, ["TotalPlayTime", "CurrentXP", "KittensFound"], ["ComboLevelOne", "StaminaInfinite"],
                    [("objectiveStatus0", 5), ("objectiveStatus4", 5), ("objectiveFound0", 5),
                     ("currentHeroPointChapter", 0), ("ME01Weight", 1)], source="test")


# ------------------------------------------------------------------ tests ----------------
class CrcTest(unittest.TestCase):
    def test_table_matches_ida(self):
        self.assertEqual(T.CRC_TABLE[:8], IDA_CRC_TABLE_HEAD)
        self.assertEqual(T.CRC_TABLE, bitwise_crc_table())

    @unittest.skipUnless(os.path.exists(RETAIL_PE), "retail PE image not present")
    def test_table_matches_retail_image(self):
        img = open(RETAIL_PE, "rb").read()
        o = 0x8292E2F0 - 0x82000000
        self.assertEqual(list(struct.unpack(">256I", img[o:o + 1024])), T.CRC_TABLE)

    def test_crc_equals_ppc_port(self):
        rnd = random.Random(1)
        table = bitwise_crc_table()
        for n in (0, 3, 4, 5, 24, 88, 1000):
            d = rnd.randbytes(n)
            self.assertEqual(T.mcm_crc(d), ppc_crc(d, table), n)


class ChecksumTest(unittest.TestCase):
    def test_slot_checksum_equals_ppc_port(self):
        rnd = random.Random(2)
        for count in (5, 12, 207):
            p2 = rnd.randbytes(6 * 16 * count)
            for s in range(3):
                self.assertEqual(T.slot_checksum(p2, count, s), ppc_slot_checksum(p2, count, s))


class DecodeTest(unittest.TestCase):
    def setUp(self):
        self.schema = small_schema()
        self.data, self.states = WriterModel(self.schema, random.Random(3)).save_file([0, 2])

    def test_container_and_checksums(self):
        d = T.decode(self.data, self.schema)
        self.assertTrue(all(v for k, v in d["container"].items() if k.endswith("_ok")), d["container"])
        self.assertEqual([s["empty"] for s in d["slots"]], [False, True, False])
        self.assertTrue(d["slots"][0]["checksum_ok"] and d["slots"][2]["checksum_ok"])

    def test_fields_recovered(self):
        d = T.decode(self.data, self.schema)
        for k in (0, 2):
            for region, st in (("live", self.states[k][0]), ("checkpoint", self.states[k][1])):
                f = d["slots"][k][region]
                self.assertEqual(f["key_mismatches"], [])
                self.assertAlmostEqual(f["music"], st["music"], places=6)
                self.assertEqual(f["MetropolisHealth"], st["metro"])
                self.assertEqual(f["stat"], st["stats"])
                self.assertEqual(f["entity"], st["entities"])
                for p, (sc, x, y, v) in enumerate(st["pads"]):
                    self.assertEqual(f["pads"][p], {"scheme": sc, "xinv": x, "yinv": y, "vibration": v})
                for n, t in self.schema.das:
                    if t == 1:
                        self.assertAlmostEqual(f["das"][n], st["das"][n], places=5)
                    elif t == 0:
                        self.assertEqual(f["das"][n], st["das"][n])
                    else:
                        self.assertEqual(f["das"][n], st["das"][n] & 0xFFFFFFFF)

    def test_roundtrip_byte_identical(self):
        d = T.decode(self.data, self.schema)
        self.assertEqual(T.encode_into(self.data, d, self.schema), self.data)

    def test_objective_bits(self):
        d = T.decode(self.data, self.schema)
        f = d["slots"][0]["live"]
        f["das"]["objectiveStatus0"] = 1 << 5
        f["das"]["objectiveStatus4"] = 1 << 19
        again = T.decode(T.encode_into(self.data, d, self.schema), self.schema)
        self.assertEqual(again["slots"][0]["live"]["objectivesWon"], [5, 147])
        self.assertTrue(again["slots"][0]["checksum_ok"])
        self.assertTrue(again["container"]["crc2_ok"])

    def test_written_byte_corruption_is_detected(self):
        b = bytearray(self.data)
        # 2nd stat record (index 20, shift 8) byte +11: written and inside the checksummed bits.
        # (Index 19 would NOT do: 8*((19&3)+1) = 32, and slw by 32 gives 0, so the game's slot
        # checksum ignores every record whose index is 3 mod 4.)
        off = 28 + 88 + 16 * (3 + 4 * 4 + 1) + 11
        b[off] ^= 0x40
        d = T.decode(bytes(b), self.schema)
        self.assertFalse(d["container"]["crc2_ok"])
        self.assertFalse(d["slots"][0]["checksum_ok"])

    def test_stale_byte_is_semantically_invisible(self):
        b = bytearray(self.data)
        off = 28 + 88 + 16 * (3 + 4 * 4) + 4           # +4 of a stat record: never written
        b[off] ^= 0xFF
        d = T.decode(bytes(b), self.schema)
        self.assertFalse(d["container"]["crc2_ok"])     # the file CRC covers every byte
        self.assertTrue(d["slots"][0]["checksum_ok"])   # the slot checksum reads +8 only
        self.assertEqual(T.semantic_bytes(bytes(b), self.schema), T.semantic_bytes(self.data, self.schema))

    def test_stale_byte_invisible_after_the_game_rebuilds_the_container(self):
        # A real second save of the same state has different stale bytes AND CRCs recomputed
        # over them (SaveMcm2_BeginWrite_sub_8263EEB0). semantic_bytes must not see either.
        _h, p1, p2, _c = T.split(self.data)
        p2 = bytearray(p2)
        p2[16 * (3 + 4 * 4) + 4] ^= 0xFF                 # stat record +4: never written
        p2[16 * (self.schema.count - 1) + 12] ^= 0xFF    # DAS record low word: stack garbage
        p1 = bytearray(p1)
        p1[16 + 4] ^= 0xFF                               # slot descriptor +4: never written
        again = T.build(bytes(p1), bytes(p2))
        d = T.decode(again, self.schema)
        self.assertTrue(all(v for k, v in d["container"].items() if k.endswith("_ok")))
        self.assertNotEqual(again[0x10:0x1C], self.data[0x10:0x1C])
        self.assertEqual(T.semantic_bytes(again, self.schema), T.semantic_bytes(self.data, self.schema))

    def test_checksum_blind_spot_is_real(self):
        b = bytearray(self.data)
        off = 28 + 88 + 16 * (3 + 4 * 4) + 9            # record 19: shift 32 -> excluded
        b[off] ^= 0x40
        d = T.decode(bytes(b), self.schema)
        self.assertFalse(d["container"]["crc2_ok"])
        self.assertTrue(d["slots"][0]["checksum_ok"])

    def test_key_mismatch_reported(self):
        b = bytearray(self.data)
        off = 28 + 88 + 16 * (3 + 4 * 4)               # first stat key
        b[off] ^= 1
        d = T.decode(bytes(b), self.schema)
        self.assertTrue(d["slots"][0]["live"]["key_mismatches"])

    def test_no_xuid_field(self):
        d = T.decode(self.data, self.schema)
        self.assertEqual(d["xuid_bound_fields"], [])
        d.pop("xuid_bound_fields")
        self.assertNotIn("xuid", repr(d).lower())

    def test_inferred_layout_without_schema(self):
        d = T.decode(self.data, None)
        self.assertIn("inferred", d["schema_note"])
        self.assertEqual(d["slots"][0]["live"]["MetropolisHealth"], self.states[0][0]["metro"])

    def test_tool_synth_roundtrip(self):
        data = T.synth(self.schema, seed=9, slots=(1,))
        d = T.decode(data, self.schema)
        self.assertTrue(d["slots"][1]["checksum_ok"])
        self.assertEqual(T.encode_into(data, d, self.schema), data)

    def test_cli_verify_and_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            root = os.path.join(td, "0009000000000001", "454107ED", "00000001", "Superman Returns")
            os.makedirs(root)
            p = os.path.join(root, "Superman Returns")
            with open(p, "wb") as fh:
                fh.write(self.data)
            sp = os.path.join(td, "schema.json")
            self.schema.dump(sp, {"test": True})
            self.assertEqual(T.main(["verify", td, "--schema", sp]), 0)
            self.assertEqual(T.main(["roundtrip", p, "--schema", sp]), 0)


class SchemaTest(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(T.SCHEMA_DEFAULT), "committed schema missing")
    def test_committed_schema(self):
        s = T.Schema.load()
        self.assertEqual(s.count, 4 * s.pads + 3 + len(s.das) + len(s.entities) + len(s.stats) + 1)
        self.assertEqual(s.count, 207)
        keys = [s.expected_key(k, n) for k, n, _t in s.kinds if k in ("stat", "entity", "das")]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn(("objectiveStatus4", 5), s.das)
        self.assertEqual(T.fnv1("objectiveStatus4"), 0x17E63150)     # key seen in the golden dump
        self.assertEqual(T.java_hash("TotalPlayTime"), 0xA12E8365)

    @unittest.skipUnless(os.path.exists(T.SCHEMA_DEFAULT), "committed schema missing")
    def test_retail_schema_model_file(self):
        s = T.Schema.load()
        data, st = WriterModel(s, random.Random(6)).save_file([0, 1, 2])
        self.assertEqual(len(data), 28 + 88 + 96 * 207)                 # 19988 bytes
        d = T.decode(data, s)
        self.assertTrue(all(sl["checksum_ok"] for sl in d["slots"]))
        self.assertEqual(d["slots"][1]["live"]["stat"]["CurrentXP"], st[1][0]["stats"]["CurrentXP"])
        self.assertEqual(T.encode_into(data, d, s), data)


class SynthImage:
    def __init__(self):
        self.mem = {}

    def plant(self, va, data):
        for i, b in enumerate(data):
            self.mem[va + i] = b

    def read(self, va, n):
        out = bytearray()
        for a in range(va, va + n):
            if a not in self.mem:
                break
            out.append(self.mem[a])
        return bytes(out)


class ImageTest(unittest.TestCase):
    def test_schema_and_file_from_image(self):
        sch = small_schema()
        data, _st = WriterModel(sch, random.Random(4)).save_file([1])
        _h, p1, p2, _c = T.split(data)
        img = SynthImage()
        w = lambda va, v: img.plant(va, struct.pack(">I", v))  # noqa: E731
        s = lambda va, t: img.plant(va, t.encode() + b"\0")    # noqa: E731
        # save manager
        mgr, buf, cap = 0x40000000, 0x40100000, 0x40200000
        w(0x8296D8D4, mgr)
        w(mgr, cap)
        img.plant(mgr + 8, p1)
        w(mgr + 96, buf)
        w(mgr + 104, 0)
        img.plant(mgr + 108, b"\0")
        img.plant(buf, p2)
        img.plant(cap, bytes(16 * sch.count))
        # pads
        w(0x8296D884, 0x41000000)
        w(0x41000000 + 28, 4)
        # stats: one extra non-flag4 stat must be skipped
        st, arr = 0x42000000, 0x42001000
        w(0x8296DF70, st)
        w(st, arr)
        names = sch.stats[:1] + ["PreviousXP"] + sch.stats[1:]
        w(st + 4, len(names))
        w(st + 16, len(sch.stats))
        for i, n in enumerate(names):
            e = 0x42100000 + 0x100 * i
            w(arr + 4 * i, e)
            w(e + 28, e + 0x80)
            s(e + 0x80, n)
            img.plant(e + 34, bytes([1 if n == "PreviousXP" else 7]))
        en, earr = 0x43000000, 0x43001000
        w(0x8296DF6C, en)
        w(en + 4, earr)
        w(en + 8, len(sch.entities))
        for i, n in enumerate(sch.entities):
            e = 0x43100000 + 0x100 * i
            w(earr + 4 * i, e)
            w(e + 8, e + 0x80)
            s(e + 0x80, n)
        # flows: current flow index 0 -> name "BaseWorldFlow"; slot 0 -> doc with node vector
        fl, names_tab, slots, slot0, doc, vec = 0x44000000, 0x44001000, 0x44002000, 0x44003000, 0x44004000, 0x44005000
        w(0x8296DF74, fl)
        w(fl + 8, names_tab)
        w(fl + 32, slots)
        w(fl + 44, 0)
        w(names_tab + 4, 0x44006000)
        s(0x44006000, "BaseWorldFlow")
        w(slots, slot0)
        for j in range(1, 8):
            w(slots + 4 * j, 0)
        s(slot0 + 12, "BaseWorldFlow")
        w(slot0 + 104, doc)
        w(doc + 88, vec)
        w(doc + 92, vec + 4 * len(sch.das))
        # atom map: header at m+4, root at m+12; build a right-leaning chain
        m = 0x45000000
        w(0x829A23B8, 0x45100000)
        w(0x45100000, m)
        nodes = []
        for i, (n, t) in enumerate(sch.das):
            nd = 0x46000000 + 0x100 * i
            w(vec + 4 * i, nd)
            w(nd, T.fnv1(n))
            w(nd + 8, (t << 17) | 0x01000001)
            an = 0x47000000 + 0x100 * i
            nodes.append(an)
            w(an + 16, T.fnv1(n))
            w(an + 28, an + 0x80)
            s(an + 0x80, n)
        for i, an in enumerate(nodes):
            w(an, nodes[i + 1] if i + 1 < len(nodes) else 0)
            w(an + 4, 0)
        w(m + 12, nodes[0])
        got, info = T.schema_from_image(img)
        self.assertEqual((got.pads, got.stats, got.entities, got.das), (sch.pads, sch.stats, sch.entities, sch.das))
        f, capture, info = T.file_from_image(img, got)
        self.assertEqual(f, data)
        self.assertEqual(len(capture), 16 * sch.count)


class MutationTest(unittest.TestCase):
    """Each mutant of the tool must be caught by the independent checks above."""

    def _decode_ok(self):
        sch = small_schema()
        data, _st = WriterModel(sch, random.Random(5)).save_file([0])
        d = T.decode(data, sch)
        return d["container"]["crc1_ok"] and d["container"]["crc2_ok"] and d["slots"][0]["checksum_ok"] \
            and T.encode_into(data, d, sch) == data and d["slots"][0]["live"]["key_mismatches"] == []

    def _mutant(self, name, fn):
        orig = getattr(T, name)
        setattr(T, name, fn)
        try:
            return self._decode_ok()
        finally:
            setattr(T, name, orig)

    def test_baseline(self):
        self.assertTrue(self._decode_ok())

    def test_crc_without_seed_complement(self):
        def bad(data):
            crc = struct.unpack_from(">I", data, 0)[0] if len(data) >= 4 else 0
            for b in data[4:]:
                crc = (((crc << 8) & 0xFFFFFFFF) | b) ^ T.CRC_TABLE[crc >> 24]
            return ~crc & 0xFFFFFFFF
        self.assertFalse(self._mutant("mcm_crc", bad))

    def test_checksum_without_checkpoint(self):
        def bad(p2, count, slot):
            acc = 0
            for a in range(count * slot + 1, count * (slot + 1)):
                s1 = 8 * ((a & 3) + 1)
                acc += (struct.unpack_from(">I", p2, 16 * a + 8)[0] << s1 if s1 < 32 else 0) & 0xFFFFFFFF
            return acc & 0xFFFFFFFF
        self.assertFalse(self._mutant("slot_checksum", bad))

    def test_checksum_shift_off_by_one(self):
        def bad(p2, count, slot):
            acc = 0
            for a in range(count * slot + 1, count * (slot + 1)):
                for idx in (a, a + 3 * count):
                    sh = 8 * (idx & 3)
                    acc += (struct.unpack_from(">I", p2, 16 * idx + 8)[0] << sh) & 0xFFFFFFFF
            return acc & 0xFFFFFFFF
        self.assertFalse(self._mutant("slot_checksum", bad))

    def test_signed_vs_unsigned_hash(self):
        # the Java hash matters only for keys; a wrong hash function must surface as key mismatches
        self.assertFalse(self._mutant("java_hash", lambda s: T.fnv1(s)))


if __name__ == "__main__":
    unittest.main(verbosity=1)
