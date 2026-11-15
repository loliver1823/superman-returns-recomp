#!/usr/bin/env python3
"""bgfa.py - read AND write EA Tiburon BGFA1.05 containers (ui.AST, text.AST and the
nested per-movie containers inside ui.AST), byte-exact.

Promoted from archive/v1_frontend/apt_format/bgfa.py (2026-09-30, CUSTOM_FRONTEND.md
section 7.3). Regression: tools/apt/rt_test.py (outer + nested + text.AST rebuilds).

Layout (decoded 2026-09-30 against ui.AST; round-trip verified by rebuild()):
  0x00  8s   'BGFA1.05'
  0x08  u32  hash/ident (opaque, copied; shared by containers with the same TOC shape)
  0x0C  u32  entry count (LE)
  0x10  u32  TOC offset (LE)          0x14 u32 0
  0x18  u32  TOC length (LE)          0x1C u32 0
  0x20  u8   mode (0 = outer/named, 1 = nested/typed)
  0x21  u8   width of field A (1)     - outer: type byte; nested: tag index
  0x22  u8   width of id  (8)
  0x23  u8   width of offset (2 or 3)
  0x24  u8   width of size   (2 or 3)
  0x25  u8   width of uncompressed size (0 = stored)
  0x26  u8   offset shift (outer 4 -> 16-byte aligned, nested 3 -> 8-byte)
  0x27  u8   0
  0x28  u32  number of 4-byte TOC-header words (outer 4, nested = #type tags)
  0x2C  u32  name length (outer 25, nested 0)
  0x30..0x3F zero
  TOC:  tagWords[n] (u32 each), then count records:
        u8 lead (0x10 outer / 0x00 nested) | A | id | offset>>shift | size | name
  outer A (ui.AST): 0 = PNG, 1 = nested BGFA movie, 2 = LOCH, 3 = LOCA
  outer id = FNV-1a-64(UPPERCASE name), little-endian (828/828 on ui.AST)
  nested type tags seen: 0x264923D5 = .apt, 0x264923D6 = .const,
        0x264923D2 shape geometry / 0x264923D3 image texture (id low dword = char index),
        0x26AA71A7 = TTF (FONTS)
The reader honours the header's field widths and shift (as the game's reader
sub_82706170 does); nested TOC ids must stay sorted ascending (bsearch, sub_827066D8).
"""
import os
import struct
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DATA_DIR = os.path.join(REPO, "Superman Returns (USA)", "DATA")
DEFAULT_UI = os.path.join(DATA_DIR, "ui.AST")
DEFAULT_TEXT = os.path.join(DATA_DIR, "text.AST")

TAG_APT = 0x264923D5
TAG_CONST = 0x264923D6


def fnv1a64(name: str) -> int:
    """Outer TOC id: FNV-1a-64 of the UPPERCASE name (stored little-endian)."""
    h = 0xCBF29CE484222325
    for c in name.upper().encode("latin1"):
        h ^= c
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Bgfa:
    def __init__(self, data: bytes):
        self.raw = data
        assert data[:8] == b"BGFA1.05", data[:8]
        self.hdr = bytearray(data[:0x40])
        self.count = struct.unpack_from("<I", data, 0x0C)[0]
        self.toc_off = struct.unpack_from("<I", data, 0x10)[0]
        self.toc_len = struct.unpack_from("<I", data, 0x18)[0]
        h = data
        self.mode = h[0x20]
        self.wA, self.wId, self.wOff, self.wSize, self.wUnc, self.shift = h[0x21], h[0x22], h[0x23], h[0x24], h[0x25], h[0x26]
        self.ntags = struct.unpack_from("<I", data, 0x28)[0]
        self.namelen = struct.unpack_from("<I", data, 0x2C)[0]
        t = self.toc_off
        self.tags = [struct.unpack_from("<I", data, t + 4 * i)[0] for i in range(self.ntags)]
        self.recsz = 1 + self.wA + self.wId + self.wOff + self.wSize + self.wUnc + self.namelen
        p = t + 4 * self.ntags
        self.entries = []
        for i in range(self.count):
            r = data[p:p + self.recsz]
            p += self.recsz
            q = 0
            lead = r[q]; q += 1
            a = int.from_bytes(r[q:q + self.wA], "little"); q += self.wA
            ident = r[q:q + self.wId]; q += self.wId
            off = int.from_bytes(r[q:q + self.wOff], "little") << self.shift; q += self.wOff
            size = int.from_bytes(r[q:q + self.wSize], "little"); q += self.wSize
            unc = int.from_bytes(r[q:q + self.wUnc], "little") if self.wUnc else None; q += self.wUnc
            name = r[q:q + self.namelen]
            self.entries.append({
                "lead": lead, "a": a, "id": ident, "off": off, "size": size, "unc": unc,
                "name_raw": name, "name": name.split(b"\0")[0].decode("latin1"),
                "tag": (self.tags[a] if self.mode == 1 and a < len(self.tags) else None),
                "data": data[off:off + size],
            })
        self.toc_end = p
        self.trailer = data[self.toc_end:]

    # ---------------------------------------------------------------- write
    def rebuild(self, pad_byte=None):
        """Re-emit the container from self.entries (data may have been replaced).
        Entries are laid out in their ORIGINAL file order (by original offset),
        each aligned to 1<<shift; alignment padding is zero-filled (or pad_byte),
        which is what the retail files contain (the identity rebuild is byte-exact).
        A TOC-first container (nested) keeps its original TOC region, so the entry
        count cannot grow there; a TOC-last container (ui.AST, text.AST) gets its
        TOC appended 4-aligned after the last entry."""
        align = 1 << self.shift
        order = sorted(range(len(self.entries)), key=lambda i: self.entries[i]["off"])
        # an entry that outgrew the size field widens it (header byte 0x24), as 308 retail nested
        # containers already use 3 bytes (MAIN's .apt is 105,010) and the game's reader takes the
        # widths from the header (iter-466 map: SCRMAP's .apt passed 65,535)
        while any(len(e["data"]) >= (1 << (8 * self.wSize)) for e in self.entries) and self.wSize < 4:
            self.wSize += 1
            self.recsz += 1
        out = bytearray(self.raw[:0x40])
        out[0x24] = self.wSize
        # the first entry starts where it did originally (the gap after the
        # header is part of the format: nested containers put the TOC first)
        first = self.entries[order[0]]["off"] if order else 0x40
        toc_first = self.toc_off < first
        new_offs = {}
        if toc_first:
            out += self.raw[0x40:first]  # TOC + padding placeholder (rewritten below)
            need = self.toc_off + 4 * len(self.tags) + len(self.entries) * self.recsz
            if need > len(out):          # a widened field grew the TOC: move the entries down
                out += bytes(need - len(out))
        for i in order:
            e = self.entries[i]
            cur = len(out)
            if cur % align:
                padn = align - cur % align
                out += bytes(padn) if pad_byte is None else bytes([pad_byte]) * padn
            new_offs[i] = len(out)
            out += e["data"]
        if not toc_first:
            cur = len(out)
            if cur % 4:                      # TOC-at-end is 4-aligned (text.AST: 0x38FBC)
                out += bytes(4 - cur % 4)
            toc_off = len(out)
        else:
            toc_off = self.toc_off
        # TOC
        toc = bytearray()
        for tg in self.tags:
            toc += struct.pack("<I", tg)
        for i, e in enumerate(self.entries):
            rec = bytearray([e["lead"]])
            rec += e["a"].to_bytes(self.wA, "little")
            rec += e["id"]
            off = new_offs[i]
            assert off % align == 0
            v = off >> self.shift
            assert v < (1 << (8 * self.wOff)), f"offset {off:#x} overflows {self.wOff}-byte field"
            assert len(e["data"]) < (1 << (8 * self.wSize)), f"size {len(e['data'])} overflows {self.wSize}-byte field"
            rec += v.to_bytes(self.wOff, "little")
            rec += len(e["data"]).to_bytes(self.wSize, "little")
            if self.wUnc:   # compressed containers (lunits/preload): keep the stored value
                rec += (e["unc"] if e["unc"] is not None else len(e["data"])).to_bytes(self.wUnc, "little")
            rec += e["name_raw"]
            toc += rec
        if toc_first:
            assert toc_off + len(toc) <= (min(new_offs.values()) if new_offs else first), "TOC-first container: TOC outgrew its region"
            out[toc_off:toc_off + len(toc)] = toc
        else:
            out += toc
        struct.pack_into("<I", out, 0x10, toc_off)
        struct.pack_into("<I", out, 0x18, len(toc))
        struct.pack_into("<I", out, 0x0C, len(self.entries))
        return bytes(out)


def movies(ui_path=DEFAULT_UI):
    """Yield (tocName, nestedBgfa, aptBytes, constBytes) for every nested movie
    container in ui.AST. Select movies by this TOC name, NEVER by the carve
    number of data/ui_ast_extracted/ (carve n holds TOC record n+2)."""
    outer = Bgfa(open(ui_path, "rb").read())
    for e in outer.entries:
        if e["data"][:8] != b"BGFA1.05":
            continue
        inner = Bgfa(e["data"])
        apt = [x["data"] for x in inner.entries if x["tag"] == TAG_APT]
        cst = [x["data"] for x in inner.entries if x["tag"] == TAG_CONST]
        yield e["name"], inner, (apt[0] if apt else None), (cst[0] if cst else None)


def movie(name, ui_path=DEFAULT_UI):
    """-> (nestedBgfa, aptBytes, constBytes) for the movie with TOC name `name`."""
    want = name.upper()
    for n, inner, apt, cst in movies(ui_path):
        if n.upper() == want:
            return inner, apt, cst
    raise KeyError(f"no movie {name!r} in {ui_path}")


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="List a BGFA1.05 container's TOC.")
    ap.add_argument("container", nargs="?", default=DEFAULT_UI)
    args = ap.parse_args(argv)
    b = Bgfa(open(args.container, "rb").read())
    print(f"# {args.container}: {b.count} entries, mode {b.mode}, TOC @{b.toc_off:#x}, "
          f"widths A{b.wA} id{b.wId} off{b.wOff} size{b.wSize} unc{b.wUnc} shift{b.shift}")
    for i, e in enumerate(b.entries):
        tag = f" tag={e['tag']:#010x}" if e["tag"] is not None else ""
        print(f"{i:4d}  A={e['a']}{tag}  off={e['off']:#010x}  size={e['size']:8d}  "
              f"id={e['id'].hex()}  {e['name']}")


if __name__ == "__main__":
    _main(sys.argv[1:])
