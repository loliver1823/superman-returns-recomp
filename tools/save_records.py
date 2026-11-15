#!/usr/bin/env python3
"""save_records.py - decode, verify, diff and round-trip the game's save file
(rmcsave:\\Superman Returns, one file per profile; host path
<content root>/<xuid>/454107ED/00000001/Superman Returns/Superman Returns).

Every layout fact below is from IDA on the retail default.xex (iter-463); no real save has
been decoded yet (none exists: see docs/SAVES.md "Save file format").

FILE = MCM2 container (writer sub_8263EEB0 / sub_8263F030, reader sub_8263F8D8):
  +0x00  be u32 'MC02' (0x4D433032; `lis 0x4D43; ori 0x3032` at 0x8263EF48 - iter-465 save lane,
         read off the first real save; the iter-463 'MCM2' constant was a misreading)
  +0x04  be u32 total size = 28 + len1 + len2
  +0x08  be u32 len1 = 88           (sub_82165000)
  +0x0C  be u32 len2 = 96 * count   (sub_82181DF8)
  +0x10  be u32 crc(part1)  +0x14 be u32 crc(part2)  +0x18 be u32 crc(header bytes 0..23)
  +0x1C  part1: 88 bytes = save manager mgr+8..mgr+96 (mgr = *0x8296D8D4)
  +0x74  part2: 6*count 16-byte records = the manager's slot buffer mgr+96 (AptEnsureDisplayListBuffer)
  crc = sub_826424F8: MSB-first CRC-32 (poly 0x04C11DB7, table 0x8292E2F0) seeded with the
        complement of the first 4 bytes big-endian, then crc=((crc<<8)|b)^T[crc>>24]; result ~crc.

PART1 (88 bytes): 16 bytes zeroed at buffer creation (sub_825F4C20(mgr+8,0,16)), then three
  24-byte slot descriptors (ChapterProgressEntryFill_sub_8218E410; -1.0 = empty slot):
  +0 f32 story progress (HeroPointChapterProgressFraction_sub_821870A0), +4 unwritten,
  +8 f64 TotalPlayTime stat, +16 u32 currentHeroPointChapter, +20 unwritten.

PART2: count = 4*pads + 3 + D + E + S + 1 (AptComputeDisplayListCount_sub_821770E0).
  Region r (0..5) = records [r*count, (r+1)*count). Regions 0..2 = the three save slots as of the
  manual save (SaveMgr_WriteSlot = sub_8218F7D0); regions 3..5 = the same slot's chapter-start
  checkpoint (a copy of the AutoSaveGame capture buffer, AptDisplayListSaveSnapshot_sub_82177280),
  which LoadAutoSaveGame (sub_8218E578) restores on Retry.
  Record {be u32 key, 4 bytes unwritten, 8-byte value}; only the bytes named below are written;
  the rest is stale heap memory (the buffers are allocated with only dword 0 of each record zeroed).
    rec 0          header: +8 u32 slot checksum (AptDisplayListColumnChecksum_sub_821773E8)
    rec 1, 2       +8 f32 music, sfx level                         (SaveRecords_WriteOptions_sub_82165088)
    per pad p      +8 u32 control scheme; +8 u8 X-invert; +8 u8 Y-invert; +8 u8 vibration
    S stats        key = Java hash (h*31+c) of the stat name; +8 f64 value (flag&4 stats, sub_82165198)
    E entities     key = Java hash of the entity name; +8 u8 byte +35 (unlocked)     (sub_82165250)
    metro          +8 u32 max(health, maxHealth*MetroHealthSaveThreshold)            (sub_82186F70)
    D DAS          key = FNV-1 of the BaseWorldFlow attribute name; +8 u32 value (int/bitfield) or
                   f32 (float); objectiveStatus4 bits 13/18/20 := status of 34/12/44  (sub_82177310,
                   PackObjectiveStatusBits_sub_82165638)
  Slot checksum over live records i=1..count-1 (absolute index a) and the checkpoint records
  (index a+3*count): sum of (be u32 at +8) << (8*((a&3)+1)), 32-bit wrap, a shift of 32 gives 0
  (PowerPC slw).

No record is XUID-bound: none of the five writers reads a profile id; options are per input slot.

Usage:
  python tools/save_records.py decode  FILE|CONTENT_ROOT [--schema S] [--json OUT]
  python tools/save_records.py verify  FILE|CONTENT_ROOT
  python tools/save_records.py diff    A B            # semantic: stale bytes ignored
  python tools/save_records.py roundtrip FILE         # decode -> re-encode -> byte-identical?
  python tools/save_records.py from-image SPEC [--out FILE]   # SPEC xenia:<dump> | ours:<dump>
  python tools/save_records.py schema --image SPEC [--out data/saves/save_schema_retail.json]
  python tools/save_records.py synth --out FILE [--seed N]    # synthetic test file
"""
from __future__ import annotations

import argparse
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SCHEMA_DEFAULT = os.path.join(REPO, "data", "saves", "save_schema_retail.json")
SAVE_REL = os.path.join("454107ED", "00000001", "Superman Returns", "Superman Returns")

MCM2 = 0x4D433032  # 'MC02' (the container keeps its historical name MCM2 in this tool)
HDR = 28
PART1 = 88
SLOTS = 3
REGIONS = 6
REC = 16

# ------------------------------------------------------------------ hashes / crc -----
def java_hash(s):
    """Str_Hash31_sub_8263C508: h = h*31 + (signed char)c."""
    h = 0
    for c in s.encode("latin1"):
        c = c - 256 if c >= 128 else c
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def fnv1(s):
    """AptResolvePathString_sub_826CD770 key: FNV-1 32 (h = h*16777619 ^ c)."""
    h = 2166136261
    for c in s.encode("latin1"):
        h = ((h * 16777619) & 0xFFFFFFFF) ^ c
    return h


def _crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


CRC_TABLE = _crc_table()


def mcm_crc(data):
    """sub_826424F8."""
    if len(data) < 4:
        return 0
    crc = ~struct.unpack_from(">I", data, 0)[0] & 0xFFFFFFFF
    for b in data[4:]:
        crc = (((crc << 8) & 0xFFFFFFFF) | b) ^ CRC_TABLE[crc >> 24]
    return ~crc & 0xFFFFFFFF


def slot_checksum(part2, count, slot):
    """AptDisplayListColumnChecksum_sub_821773E8 (checksum of save slot `slot`)."""
    acc = 0
    for a in range(count * slot + 1, count * (slot + 1)):
        live = struct.unpack_from(">I", part2, 16 * a + 8)[0]
        ckp = struct.unpack_from(">I", part2, 16 * (a + 3 * count) + 8)[0]
        s1 = 8 * ((a & 3) + 1)
        s2 = 8 * (((a + 3 * count) & 3) + 1)
        acc += (live << s1 if s1 < 32 else 0) & 0xFFFFFFFF
        acc += (ckp << s2 if s2 < 32 else 0) & 0xFFFFFFFF
    return acc & 0xFFFFFFFF


# ------------------------------------------------------------------ schema -----------
class Schema:
    """Record layout of one region. kinds[i] = (kind, name, type)."""

    def __init__(self, pads, stats, entities, das, source="?"):
        self.pads, self.stats, self.entities, self.das, self.source = pads, list(stats), list(entities), list(das), source
        k = [("header", None, None), ("music", None, "f32"), ("sfx", None, "f32")]
        for p in range(pads):
            k += [("scheme", p, "u32"), ("xinv", p, "u8"), ("yinv", p, "u8"), ("vibration", p, "u8")]
        k += [("stat", n, "f64") for n in self.stats]
        k += [("entity", n, "u8") for n in self.entities]
        k += [("metro", None, "u32")]
        k += [("das", n, {0: "i32", 1: "f32", 5: "u32"}.get(t, "u32")) for n, t in self.das]
        self.kinds = k

    @property
    def count(self):
        return len(self.kinds)

    def expected_key(self, kind, name):
        if kind in ("stat", "entity"):
            return java_hash(name)
        if kind == "das":
            return fnv1(name)
        return 0

    @classmethod
    def load(cls, path=SCHEMA_DEFAULT):
        with open(path) as f:
            j = json.load(f)
        return cls(j["pads"], j["stats"], j["entities"], [tuple(x) for x in j["das"]], source=path)

    def dump(self, path, provenance):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", newline="\n") as f:
            json.dump({"schema": "save_schema/1", "provenance": provenance, "pads": self.pads, "count": self.count,
                       "stats": self.stats, "entities": self.entities, "das": [list(x) for x in self.das]},
                      f, indent=1)
            f.write("\n")


# value bytes semantically written per kind: (offset, length) within the 16-byte record
WRITTEN = {"header": [(0, 4), (8, 4)], "music": [(0, 4), (8, 4)], "sfx": [(0, 4), (8, 4)],
           "scheme": [(0, 4), (8, 4)], "xinv": [(0, 4), (8, 1)], "yinv": [(0, 4), (8, 1)],
           "vibration": [(0, 4), (8, 1)], "stat": [(0, 4), (8, 8)], "entity": [(0, 4), (8, 1)],
           "metro": [(0, 4), (8, 4)], "das": [(0, 4), (8, 4)]}


def _val(rec, typ):
    if typ == "f32":
        return struct.unpack_from(">f", rec, 8)[0]
    if typ == "f64":
        return struct.unpack_from(">d", rec, 8)[0]
    if typ == "u8":
        return rec[8]
    if typ == "i32":
        return struct.unpack_from(">i", rec, 8)[0]
    return struct.unpack_from(">I", rec, 8)[0]


def _put(rec, typ, v):
    if typ == "f32":
        struct.pack_into(">f", rec, 8, v)
    elif typ == "f64":
        struct.pack_into(">d", rec, 8, v)
    elif typ == "u8":
        rec[8] = v & 0xFF
    elif typ == "i32":
        struct.pack_into(">i", rec, 8, v)
    else:
        struct.pack_into(">I", rec, 8, v & 0xFFFFFFFF)


# ------------------------------------------------------------------ container --------
def split(data):
    if len(data) < HDR:
        raise ValueError("file shorter than the 28-byte MCM2 header")
    magic, total, l1, l2, c1, c2, ch = struct.unpack_from(">7I", data, 0)
    p1 = data[HDR:HDR + l1]
    p2 = data[HDR + l1:HDR + l1 + l2]
    chk = {"magic_ok": magic == MCM2, "size_ok": total == len(data) == HDR + l1 + l2,
           "len1_ok": l1 == PART1, "len2_ok": l2 % (REGIONS * REC) == 0,
           "crc1_ok": mcm_crc(p1) == c1, "crc2_ok": mcm_crc(p2) == c2,
           "crc_hdr_ok": mcm_crc(data[:24]) == ch}
    return {"len1": l1, "len2": l2, "crc1": c1, "crc2": c2, "crc_hdr": ch}, p1, p2, chk


def build(part1, part2):
    """MCM2 container around the two parts, CRCs recomputed (the writer's own procedure)."""
    hdr = bytearray(struct.pack(">6I", MCM2, HDR + len(part1) + len(part2), len(part1), len(part2),
                                mcm_crc(part1), mcm_crc(part2)))
    hdr += struct.pack(">I", mcm_crc(bytes(hdr)))
    return bytes(hdr) + bytes(part1) + bytes(part2)


def infer_schema(part2, count, names=None):
    """Layout inference when no schema matches: options/metro/header keys are 0, stats/entities
    use Java-hash keys, DAS uses FNV-1 keys. Needs a non-empty region."""
    names = names or {}
    for region in range(REGIONS):
        keys = [struct.unpack_from(">I", part2, 16 * (region * count + i))[0] for i in range(count)]
        nz = [i for i, k in enumerate(keys) if k]
        if not nz:
            continue
        first = nz[0]
        if (first - 3) % 4:
            continue
        pads = (first - 3) // 4
        zeros_after = [i for i in range(first, count) if keys[i] == 0]
        if len(zeros_after) != 1:
            continue
        metro = zeros_after[0]
        stats, ents = [], []
        for i in range(first, metro):
            n = names.get(("java", keys[i]), "key:%08x" % keys[i])
            (ents if names.get(("kind", keys[i])) == "entity" else stats).append(n)
        das = [(names.get(("fnv", keys[i]), "key:%08x" % keys[i]), 5) for i in range(metro + 1, count)]
        return Schema(pads, stats, ents, das, source="inferred from region %d" % region)
    return None


def decode(data, schema=None):
    hdr, p1, p2, chk = split(data)
    out = {"container": dict(hdr, **chk)}
    if not chk["len2_ok"] or not chk["len1_ok"]:
        out["error"] = "unexpected part sizes"
        return out
    count = len(p2) // (REGIONS * REC)
    if schema is None or schema.count != count:
        inferred = infer_schema(p2, count)
        out["schema_note"] = ("schema count %d != file count %d; " % (schema.count, count) if schema else "") + \
            ("inferred layout" if inferred else "no layout: every region is empty")
        schema = inferred
    out["count"] = count
    out["schema"] = schema.source if schema else None
    out["reserved16"] = p1[:16].hex()
    slots = []
    for s in range(SLOTS):
        d = p1[16 + 24 * s: 40 + 24 * s]
        prog, _pad, playtime, chapter, _pad2 = struct.unpack(">fIdII", d)
        stored = struct.unpack_from(">I", p2, 16 * (s * count) + 8)[0]
        calc = slot_checksum(p2, count, s)
        slot = {"slot": s, "empty": prog <= -1.0, "progress": prog, "TotalPlayTime": playtime,
                "currentHeroPointChapter": chapter, "checksum_stored": stored, "checksum_calc": calc,
                "checksum_ok": stored == calc}
        if schema and not slot["empty"]:
            slot["live"] = decode_region(p2, count, s, schema)
            slot["checkpoint"] = decode_region(p2, count, s + 3, schema)
        slots.append(slot)
    out["slots"] = slots
    out["xuid_bound_fields"] = []
    return out


def decode_region(p2, count, region, schema):
    base = region * count
    fields, bad_keys = {}, []
    pads = {}
    for i, (kind, name, typ) in enumerate(schema.kinds):
        rec = p2[16 * (base + i): 16 * (base + i) + 16]
        key = struct.unpack_from(">I", rec, 0)[0]
        if key != schema.expected_key(kind, name):
            bad_keys.append((i, kind, name, "%08x" % key))
        if kind == "header":
            continue
        v = _val(rec, typ)
        if kind in ("scheme", "xinv", "yinv", "vibration"):
            pads.setdefault(name, {})[kind] = v
        elif kind in ("music", "sfx", "metro"):
            fields[{"metro": "MetropolisHealth"}.get(kind, kind)] = v
        else:
            fields.setdefault(kind, {})[name] = v
    fields["pads"] = pads
    das = fields.get("das", {})
    fields["objectivesWon"] = _bits(das, "objectiveStatus")
    fields["objectivesFound"] = _bits(das, "objectiveFound")
    fields["key_mismatches"] = bad_keys
    return fields


def _bits(das, prefix):
    out = []
    for w in range(9):
        v = das.get("%s%d" % (prefix, w))
        if v is None:
            continue
        for b in range(32):
            if v & (1 << b):
                out.append(32 * w + b)
    return out


def encode_into(template, decoded, schema):
    """Write every decoded semantic field back into a copy of `template` (stale bytes kept),
    recompute slot checksums (non-empty slots) and the container CRCs."""
    _hdr, p1, p2, _chk = split(template)
    p1, p2 = bytearray(p1), bytearray(p2)
    count = len(p2) // (REGIONS * REC)
    for sl in decoded["slots"]:
        s = sl["slot"]
        struct.pack_into(">f", p1, 16 + 24 * s, sl["progress"])
        struct.pack_into(">d", p1, 16 + 24 * s + 8, sl["TotalPlayTime"])
        struct.pack_into(">I", p1, 16 + 24 * s + 16, sl["currentHeroPointChapter"])
        if sl["empty"]:
            continue
        for region, key in ((s, "live"), (s + 3, "checkpoint")):
            f = sl[key]
            base = region * count
            for i, (kind, name, typ) in enumerate(schema.kinds):
                rec = memoryview(p2)[16 * (base + i): 16 * (base + i) + 16]
                if kind == "header":
                    continue
                if kind in ("scheme", "xinv", "yinv", "vibration"):
                    v = f["pads"][name][kind]
                elif kind in ("music", "sfx"):
                    v = f[kind]
                elif kind == "metro":
                    v = f["MetropolisHealth"]
                else:
                    v = f[kind][name]
                buf = bytearray(rec)
                _put(buf, typ, v)
                rec[:] = buf
        struct.pack_into(">I", p2, 16 * (s * count) + 8, slot_checksum(p2, count, s))
    return build(p1, p2)


def semantic_bytes(data, schema):
    """Only the bytes the game writes as a function of the saved state: container magic and
    sizes, slot descriptors, written record fields. The three CRC words (+0x10..+0x1B) are
    excluded: they cover the stale bytes, so two saves of the same state differ there
    (independent verifier, iter-463: the earlier version kept +0x10..+0x17 and reported a
    difference for every stale-byte change once the container was rebuilt as the game does)."""
    _hdr, p1, p2, _chk = split(data)
    count = len(p2) // (REGIONS * REC)
    out = bytearray(data[:16])
    for s in range(SLOTS):
        d = p1[16 + 24 * s: 40 + 24 * s]
        out += d[0:4] + d[8:20]
        if struct.unpack(">f", d[0:4])[0] <= -1.0:
            continue
        for region in (s, s + 3):
            for i, (kind, _n, _t) in enumerate(schema.kinds):
                rec = p2[16 * (region * count + i): 16 * (region * count + i) + 16]
                if region >= 3 and i == 0:
                    continue                    # checkpoint header record is not written
                for o, ln in WRITTEN[kind]:
                    out += rec[o:o + ln]
    return bytes(out)


# ------------------------------------------------------------------ memory images ----
def open_image(spec):
    # Image layer: tools/havok_snapshot.py (xenia:/ours: minidumps; ours: detects the guest base).
    # ADAPTER POINT: without it, xenia: falls back to tools/golden.py and ours: is unavailable.
    sys.path.insert(0, HERE)
    try:
        import havok_snapshot as hs
        return hs.open_image(spec)
    except ImportError:
        if not spec.startswith("xenia:"):
            raise SystemExit("ours: images need tools/havok_snapshot.py (image layer not present)")
        import golden

        class _G:
            def __init__(self, name):
                self.g = golden.GoldenDump(name)

            def read(self, va, n):
                return self.g.read(va, n)
        return _G(spec[6:])


def _u32(img, va):
    b = img.read(va, 4)
    return struct.unpack(">I", b)[0] if len(b) == 4 else 0


def _cstr(img, va, n=96):
    return img.read(va, n).split(b"\0")[0].decode("latin1") if va else ""


def schema_from_image(img):
    """Same walk as the writers: pads *(*0x8296D884+28); stats *0x8296DF70 {+0 array, +4 count,
    +16 flag&4 count; element +28 name, +34 flags}; entities *0x8296DF6C {+4 array, +8 count;
    element +8 name}; BaseWorldFlow DAS doc: flow list *0x8296DF74 (+8 names, +32 8 slots,
    +44 current index; slot +12 name, +104 doc), doc +88/+92 = type-bit-16 node vector;
    node {key, value, flags; type = flags>>17 & 0x7F}; names from the atom map *0x829A23B8."""
    pads = _u32(img, _u32(img, 0x8296D884) + 28)
    st = _u32(img, 0x8296DF70)
    stats = []
    for i in range(_u32(img, st + 4)):
        e = _u32(img, _u32(img, st) + 4 * i)
        if img.read(e + 34, 1)[0] & 4:
            stats.append(_cstr(img, _u32(img, e + 28)))
    en = _u32(img, 0x8296DF6C)
    ents = [_cstr(img, _u32(img, _u32(img, _u32(img, en + 4) + 4 * i) + 8)) for i in range(_u32(img, en + 8))]
    fl = _u32(img, 0x8296DF74)
    want = _cstr(img, _u32(img, _u32(img, fl + 8) + 8 * _u32(img, fl + 44) + 4))
    doc = 0
    for j in range(8):
        slot = _u32(img, _u32(img, fl + 32) + 4 * j)
        if slot and _cstr(img, slot + 12) == want:
            doc = _u32(img, slot + 104)
    atoms = {}
    m = _u32(img, _u32(img, 0x829A23B8))
    stack, seen = [_u32(img, m + 12)], set()
    while stack:
        n = stack.pop()
        if not n or n == m + 4 or n in seen:
            continue
        seen.add(n)
        atoms[_u32(img, n + 16)] = _cstr(img, _u32(img, n + 28), 128)
        stack += [_u32(img, n), _u32(img, n + 4)]
    das = []
    b, e = _u32(img, doc + 88), _u32(img, doc + 92)
    for i in range((e - b) // 4):
        nd = _u32(img, b + 4 * i)
        k, f = _u32(img, nd), _u32(img, nd + 8)
        das.append((atoms.get(k, "key:%08x" % k), (f >> 17) & 0x7F))
    return Schema(pads, stats, ents, das, source="image"), {"flow": want, "stat_flag4_count_field": _u32(img, st + 16)}


def file_from_image(img, schema):
    """The bytes the game would write now: 28-byte header + mgr+8[88] + *(mgr+96)[96*count];
    also the AutoSaveGame capture buffer *mgr (count records)."""
    mgr = _u32(img, 0x8296D8D4)
    p1 = img.read(mgr + 8, PART1)
    buf = _u32(img, mgr + 96)
    cap = _u32(img, mgr)
    info = {"mgr": hex(mgr), "slot_buffer": hex(buf), "capture_buffer": hex(cap),
            "cursor": _u32(img, mgr + 104), "loadgame": img.read(mgr + 108, 1)[0]}
    p2 = img.read(buf, REGIONS * REC * schema.count) if buf else b""
    capture = img.read(cap, REC * schema.count) if cap else b""
    return (build(p1, p2) if p2 else None), capture, info


# ------------------------------------------------------------------ synthetic --------
def synth(schema, seed=0, slots=(0,), stale=True):
    """A synthetic save built the way the writers build it (independent of decode())."""
    rnd = random.Random(seed)
    count = schema.count
    p1 = bytearray(rnd.randbytes(PART1) if stale else bytes(PART1))
    p1[:16] = bytes(16)
    p2 = bytearray(rnd.randbytes(REGIONS * REC * count) if stale else bytes(REGIONS * REC * count))
    for r in range(REGIONS * count):
        p2[16 * r: 16 * r + 4] = bytes(4)           # dword 0 zeroed at allocation
    for s in range(SLOTS):
        struct.pack_into(">f", p1, 16 + 24 * s, -1.0)
    for s in slots:
        struct.pack_into(">f", p1, 16 + 24 * s, rnd.choice([0.25, 0.5, 1.0]))
        struct.pack_into(">d", p1, 16 + 24 * s + 8, rnd.uniform(0, 1e5))
        struct.pack_into(">I", p1, 16 + 24 * s + 16, rnd.randint(1, 19))
        for region in (s, s + 3):
            for i, (kind, name, typ) in enumerate(schema.kinds):
                if kind == "header":
                    continue
                rec = bytearray(p2[16 * (region * count + i): 16 * (region * count + i) + 16])
                struct.pack_into(">I", rec, 0, schema.expected_key(kind, name))
                v = {"f32": rnd.uniform(0, 1), "f64": float(rnd.randint(0, 10 ** 6)), "u8": rnd.randint(0, 1),
                     "i32": rnd.randint(-5, 100), "u32": rnd.getrandbits(32)}[typ]
                _put(rec, typ, v)
                p2[16 * (region * count + i): 16 * (region * count + i) + 16] = rec
        struct.pack_into(">I", p2, 16 * (s * count) + 8, slot_checksum(p2, count, s))
    return build(p1, p2)


# ------------------------------------------------------------------ CLI --------------
def _load(path):
    if os.path.isdir(path):
        cands = [os.path.join(dp, f) for dp, _dn, fs in os.walk(path) for f in fs if f == "Superman Returns"
                 and os.path.basename(dp) == "Superman Returns"]
        if not cands:
            raise SystemExit("no '%s' under %s" % (SAVE_REL, path))
        path = cands[0]
    with open(path, "rb") as f:
        return f.read(), path


def _schema(args):
    p = getattr(args, "schema", None) or SCHEMA_DEFAULT
    return Schema.load(p) if os.path.exists(p) else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for nm in ("decode", "verify", "roundtrip"):
        p = sub.add_parser(nm)
        p.add_argument("path")
        p.add_argument("--schema")
        p.add_argument("--json")
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--schema")
    p = sub.add_parser("from-image")
    p.add_argument("image")
    p.add_argument("--schema")
    p.add_argument("--out")
    p = sub.add_parser("schema")
    p.add_argument("--image", required=True)
    p.add_argument("--out", default=SCHEMA_DEFAULT)
    p = sub.add_parser("synth")
    p.add_argument("--out", required=True)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--schema")
    args = ap.parse_args(argv)

    if args.cmd == "schema":
        img = open_image(args.image)
        sch, info = schema_from_image(img)
        if info["stat_flag4_count_field"] != len(sch.stats):
            print("WARNING: stats +16 says %d flag&4 stats, walk found %d" % (info["stat_flag4_count_field"], len(sch.stats)))
        sch.dump(args.out, {"image": args.image, "flow": info["flow"],
                            "note": "names from live memory; derived table (retail names only)"})
        print("schema: pads %d, stats %d, entities %d, DAS %d -> count %d; wrote %s"
              % (sch.pads, len(sch.stats), len(sch.entities), len(sch.das), sch.count, args.out))
        return 0
    if args.cmd == "synth":
        sch = _schema(args)
        open(args.out, "wb").write(synth(sch, args.seed))
        print("wrote", args.out)
        return 0
    if args.cmd == "from-image":
        sch = _schema(args)
        img = open_image(args.image)
        data, capture, info = file_from_image(img, sch)
        print("save manager:", info)
        if data is None:
            print("slot buffer not allocated: the game has not opened the save system's slot buffer "
                  "(no save/load yet) - nothing to write")
        else:
            if args.out:
                open(args.out, "wb").write(data)
            print(json.dumps(decode(data, sch), indent=1, default=str)[:4000])
        keys = [struct.unpack_from(">I", capture, 16 * i)[0] for i in range(len(capture) // 16)]
        print("capture buffer: %d records, %d with a nonzero key (0 = AutoSaveGame never ran)"
              % (len(keys), sum(1 for k in keys if k)))
        return 0
    if args.cmd == "diff":
        sch = _schema(args)
        a, _ = _load(args.a)
        b, _ = _load(args.b)
        da, db = decode(a, sch), decode(b, sch)
        diffs = []

        def walk(x, y, path):
            if isinstance(x, dict) and isinstance(y, dict):
                for k in sorted(set(x) | set(y), key=str):
                    walk(x.get(k), y.get(k), path + [str(k)])
            elif isinstance(x, list) and isinstance(y, list) and len(x) == len(y):
                for i, (u, v) in enumerate(zip(x, y)):
                    walk(u, v, path + [str(i)])
            elif x != y:
                diffs.append((".".join(path), x, y))
        walk(da["slots"], db["slots"], ["slots"])
        for p_, x, y in diffs:
            if "checksum" in p_:
                continue
            print("%s: %r -> %r" % (p_, x, y))
        print("%d semantic differences (stale bytes ignored)" % len([d for d in diffs if "checksum" not in d[0]]))
        return 0
    sch = _schema(args)
    data, path = _load(args.path)
    dec = decode(data, sch)
    if args.cmd == "decode":
        s = json.dumps(dec, indent=1, default=str)
        if args.json:
            open(args.json, "w").write(s)
        print(s if not args.json else "wrote " + args.json)
        return 0
    if args.cmd == "verify":
        c = dec["container"]
        ok = all(v for k, v in c.items() if k.endswith("_ok"))
        for sl in dec.get("slots", []):
            if not sl["empty"]:
                ok &= sl["checksum_ok"]
                # A slot saved before the first chapter advance has no AutoSaveGame capture: its
                # checkpoint region is all zero keys (iter-465, the first real save, roll
                # archive/v1_save/rolls/n1). That is the game's own state, not a defect.
                cp = sl["checkpoint"]["key_mismatches"]
                cp_none = bool(cp) and all(m[3] == "00000000" for m in cp)
                ok &= not sl["live"]["key_mismatches"] and (not cp or cp_none)
        print(path, "OK" if ok else "FAILED", {k: v for k, v in c.items() if k.endswith("_ok")},
              [(sl["slot"], "empty" if sl["empty"] else sl["checksum_ok"]) for sl in dec.get("slots", [])])
        return 0 if ok else 1
    if args.cmd == "roundtrip":
        if sch is None or dec.get("count") != sch.count:
            sch = infer_schema(split(data)[2], dec["count"])
        again = encode_into(data, dec, sch)
        same = again == data
        print(path, "round trip byte-identical" if same else "round trip DIFFERS")
        return 0 if same else 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
