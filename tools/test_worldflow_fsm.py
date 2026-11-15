#!/usr/bin/env python3
"""Tests for tools/worldflow_fsm.py (python tools/test_worldflow_fsm.py).

Three kinds of check, per the tool verification protocol (memory
feedback_tool_verification_protocol.md):
  * independent observers: the script pairing is re-derived here by a DIFFERENT
    method (NUL-separated scan of the first Lua run) and must agree with the tool's
    TOC-offset method; the BaseWorldFlow state count must equal the 44 that
    docs/ENGINE_MAP.md recorded from the engine's own parse (golden dump, iter-350k).
  * structural invariants on real retail data (script name suffix == hex(state id),
    every transition target resolves, every referenced script exists).
  * mutation tests: planted corruptions of each format field the tool relies on must
    raise FormatError (a decoder that silently accepts garbage is the failure mode
    this project has been burned by).
Tests that need the retail disc data skip (not pass) when it is absent.
"""
import os
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import worldflow_fsm as W  # noqa: E402

HAVE = os.path.exists(W.DEFAULT_AST)


def _data():
    return open(W.DEFAULT_AST, "rb").read()


@unittest.skipUnless(HAVE, "retail baseworl.AST not present")
class RetailBaseworl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = _data()
        cls.scripts = W.pair_scripts(cls.d, W.find_script_toc(cls.d))
        cls.graphs = {g["name"]: g for g in (W.fsm_model(n, cls.scripts) | {"H": H}
                                             for H, n in W.find_fsm_docs(cls.d))}
        cls.base = cls.graphs["BaseWorldFlow"]

    def test_counts(self):
        self.assertEqual(len(self.scripts), 217)
        self.assertEqual(len(self.graphs), 17)
        # independent observer: ENGINE_MAP "FE flow FSM" (engine parse, golden dump) = 44 states
        self.assertEqual(len(self.base["states"]), 44)
        self.assertEqual(self.base["start"], "50")

    def test_state_script_names_encode_ids(self):
        for g in self.graphs.values():
            for s in g["states"]:
                self.assertNotIn("id_script_mismatch", s, (g["name"], s["id"]))
                self.assertTrue(s["lua_ok"], (g["name"], s["id"], s["script"]))

    def test_transitions_resolve(self):
        ids = {s["id"] for s in self.base["states"]}
        for s in self.base["states"]:
            for t in s["transitions"]:
                self.assertIn(t["dest"], ids, (s["id"], t))
                self.assertTrue(t["lua_ok"], (s["id"], t))

    def test_nul_scan_agrees_with_toc_offsets(self):
        """Second method: the first TOC run's bodies are NUL-separated in TOC order."""
        recs = W.find_script_toc(self.d)
        first = [r for r in recs if r[1].startswith("TRIGGERFSM_") or r[1].startswith("BASEWORLDFLOWFSM_")]
        start = self.scripts[first[0][1]]["offset"]
        p, n = start, 0
        for _pos, name, ln in first:
            while self.d[p] == 0:
                p += 1
            q = self.d.find(b"\x00", p)
            self.assertEqual(p, self.scripts[name]["offset"], name)
            self.assertEqual(q - p, ln, name)
            p, n = q, n + 1
        self.assertEqual(n, 109)

    def test_chapter_advance_anchors(self):
        by_id = {s["id"]: s for s in self.base["states"]}
        lua = lambda sid: self.scripts[by_id[sid]["script"].upper()]["lua"]  # noqa: E731
        # 105 = ReactivateTriggers: the only natural chapter advance
        self.assertIn('GetDASValue("GoToNextChapter") == 1', lua("105"))
        self.assertIn("AutoSaveGame()", lua("105"))
        # 2023575322 = objective-start teleport: latches GoToNextChapter from the objective flow
        self.assertIn('SetDASValue("GoToNextChapter", GetWorldFlowDASValue(worldflowName, "GoToNextChapter"))',
                      lua("2023575322"))
        self.assertEqual([t["dest"] for t in by_id["2023575322"]["transitions"]], ["107"])
        # 714192539 (post-objective cleanup) -> 105
        self.assertEqual([t["dest"] for t in by_id["714192539"]["transitions"]], ["105"])

    def test_trigger_table(self):
        docs = W.named_docs(self.d)
        rows = W.trigger_table(docs)
        self.assertEqual(len(rows), 57)
        mst1 = [r for r in rows if r["doc"] == "MST01TRIGGERATTRIBUTES"][0]
        self.assertEqual((mst1["objectiveIndex"], mst1["worldflow"], mst1["deps"]), ("0", "MSTobj01", "147"))
        cats = {k: c for k, _v, c in docs["BASEWORLDFLOWATTRIBUTES"]["attrs"]}
        # category bit 16 = saved (AptFrameProcessDisplayItems_sub_82181E28 walks type-bit 16)
        self.assertEqual(cats["GoToNextChapter"], "0")
        self.assertEqual(cats["currentHeroPointChapter"], "17")


class Mutations(unittest.TestCase):
    """Planted corruptions must be rejected, not decoded."""

    def _doc(self):
        """A tiny valid bxml doc: <fsm name="G" start_state="1"><states/></fsm>."""
        strs = [b"fsm", b"name", b"G", b"start_state", b"1", b"states"]
        data, ents, off = b"", b"", 0
        for s in strs:
            ents += struct.pack(">III", len(s), off, 0)
            data += s + b"\x00"
            off += len(s) + 1
        data += b"\x00" * (-len(data) % 4)
        nstr = len(strs)
        sdat_off = 0x2C + 12 * nstr
        node_off = sdat_off + len(data)
        attr_off = node_off + 2 * 12
        nodes = struct.pack(">IHHHH", attr_off, 2, 0, 1, 0xFFFF) + struct.pack(">IHHHH", 0, 0, 5, 1, 0xFFFF)
        attrs = struct.pack(">HHHH", 1, 2, 3, 4)
        hdr = struct.pack(">IIIIHHIHHIIII", W.BXML_MAGIC, 0x14, node_off, 0, 2, 0x100, 0x2C,
                          nstr, nstr, sdat_off, len(data), len(data), 1)
        return bytearray(hdr + ents + data + nodes + attrs), node_off

    def test_valid_doc_parses(self):
        doc, _ = self._doc()
        nodes = W.parse_bxml(bytes(doc), 0)
        self.assertEqual([n["tag"] for n in nodes], ["fsm", "states"])
        self.assertEqual(nodes[0]["attrs"], [("name", "G"), ("start_state", "1")])
        self.assertEqual(nodes[0]["children"][0]["tag"], "states")

    def test_bad_magic(self):
        doc, _ = self._doc()
        doc[0] ^= 1
        with self.assertRaises(W.FormatError):
            W.parse_bxml(bytes(doc), 0)

    def test_bad_node_pad(self):
        doc, node_off = self._doc()
        doc[node_off + 11] = 0
        with self.assertRaises(W.FormatError):
            W.parse_bxml(bytes(doc), 0)

    def test_bad_last_descendant(self):
        doc, node_off = self._doc()
        struct.pack_into(">H", doc, node_off + 12 + 8, 0)  # child's span ends before itself
        with self.assertRaises(W.FormatError):
            W.parse_bxml(bytes(doc), 0)

    def test_unterminated_string(self):
        doc, _ = self._doc()
        i = bytes(doc).find(b"fsm\x00")
        doc[i + 3] = ord("X")
        with self.assertRaises(W.FormatError):
            W.parse_bxml(bytes(doc), 0)

    @unittest.skipUnless(HAVE, "retail baseworl.AST not present")
    def test_toc_length_mutation(self):
        for k, delta in ((5, 7), (60, -3), (150, 1)):
            d = bytearray(_data())
            p = W.find_script_toc(bytes(d))[k][0]
            struct.pack_into("<H", d, p + 12, struct.unpack_from("<H", d, p + 12)[0] + delta)
            with self.assertRaises(W.FormatError, msg=(k, delta)):
                W.pair_scripts(bytes(d), W.find_script_toc(bytes(d)))

    @unittest.skipUnless(HAVE, "retail baseworl.AST not present")
    def test_toc_kind_mutation(self):
        d = bytearray(_data())
        p = W.find_script_toc(bytes(d))[5][0]
        name_at = p + 14
        nm = bytes(d[name_at:name_at + 40]).split(b"\x00")[0]
        flipped = nm.replace(b"_S_", b"_T_") if b"_S_" in nm else nm.replace(b"_T_", b"_S_")
        d[name_at:name_at + len(flipped)] = flipped
        with self.assertRaises(W.FormatError):
            W.pair_scripts(bytes(d), W.find_script_toc(bytes(d)))

    @unittest.skipUnless(HAVE, "retail baseworl.AST not present")
    def test_toc_offset_mutation(self):
        d = bytearray(_data())
        recs = W.find_script_toc(bytes(d))
        p = recs[40][0]
        struct.pack_into("<H", d, p + 10, struct.unpack_from("<H", d, p + 10)[0] + 1)  # 16 bytes off
        with self.assertRaises(W.FormatError):
            W.pair_scripts(bytes(d), W.find_script_toc(bytes(d)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
