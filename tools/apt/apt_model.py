#!/usr/bin/env python3
"""apt_model.py - 'Apt Data:1:7:4' (.apt) + 'Apt constant file' (.const) walker.

The pointer map is taken from the GAME'S OWN load-time relocation code (IDA,
read-only worker 13341), so the set of fields this walker treats as pointers is
exactly the set the runtime rebases:
  AptParseReloc_826AE6D0          const+0x14 (movie offset, +aptBase), const+0x1C (items, +constBase)
  AptRegistrar_826ADD40           movie/char table, per-type char fields, exports, imports
  AptSpriteFramesRelocate_826A39A0 frames -> item arrays -> items (types 1/2/3/8)
  AptActionBytecodeRelocate_8269F3A0 bytecode operand pointers (0x83,0x88,0x8B,0x8C,0x8E,0x8F,0x94,0x96,0x9B,0xA1,0xA4-0xA7)
Every pointer is a file-relative offset (0 = null) except const string values
(relative to the .const) and With (0x94, relative to the next instruction).

It records BLOCKS (start, size, kind) to measure coverage, and a RELOC list
(fieldOffset, targetOffset) - the complete set of words that must be rewritten
if blocks move.

Promoted from archive/v1_frontend/apt_format/apt_model.py (2026-09-30). Regression:
tools/apt/rt_test.py, tools/apt/rt_graph.py.

Character types (char table at movie+0x20). A NULL char-table slot is an IMPORTED
character: every 16-byte import record's local char id names a NULL slot (112/112).
  1 Shape  - geometry is a separate asset in the movie's nested BGFA (tag 0x264923D2,
             id low dword = char index, 925/925); the registrar's case 1 fetches it via
             cb 0x829A1AC0 = GfxRendererCreateBoundResource_sub_826D1590 into char+0x20.
             It is NOT an import.
  7 Image  - texture asset tag 0x264923D3, id = char index (905/905).
Frame-item types (IDA AptExecuteFrameTags_sub_826BE100 + AptSpriteFramesRelocate_826A39A0;
census over all 820 movies: 3615 x3, 820 x5, 204 x8, 159 x1, 60 x2, 56 x4):
  1 Action       {+4 bytecode}  DoAction; NOT run by sub_826BE100 (queued per frame)
  2 FrameLabel   {+4 name, +0xC frame}  folded into the sprite's label map at load
  3 PlaceObject  {0x40 bytes, +0x34 name, +0x3C clip actions}
  4 RemoveObject {+4 depth}     display-list lookup by depth, then remove
  5 BackgroundColor {+4 0xAARRGGBB}  exactly one per movie; applied once per process
                 (byte_829A0AA7) via cb 0x829A1A70 -> executor vt+0x34 = store at exec+0x18
  8 InitAction   {+4 sprite id, +8 bytecode}  run once: sub_826BE100 negates +4 after running
"""
import struct

CHAR_NAMES = {1: "Shape", 2: "Text", 3: "Font", 5: "Sprite", 7: "Image", 9: "Movie"}
ITEM_NAMES = {1: "Action", 2: "FrameLabel", 3: "PlaceObject", 4: "RemoveObject", 5: "BackgroundColor", 8: "InitAction"}

# operand layout per opcode, from AptActionBytecodeRelocate_8269F3A0 (default = no operand)
OP_ALIGN_U32 = {0x81, 0x87, 0x99, 0x9D, 0x9F, 0xB8}           # align4, 4 bytes (int)
OP_ALIGN_STRPTR = {0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7}  # align4, 1 string ptr
OP_BYTE = {0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB5}
OP_WORD = {0xA3, 0xB6}
OP_DWORD = {0xB4, 0xB7}                                          # unaligned 4 bytes


def align4(x):
    return (x + 3) & ~3


def insn_end(op, p):
    """Byte position after the instruction whose opcode byte `op` sits at `p`. Operand
    sizes depend only on the opcode and on 4-alignment; this is the operand-size switch of
    AptActionBytecodeRelocate_sub_8269F3A0, which the runtime also uses for its linear
    UNLOAD scan (tools/apt/apt_unload.py). The one table: apt_graph and apt_unload use it."""
    p += 1
    if op == 0:
        return p
    if op in OP_ALIGN_U32 or op in OP_ALIGN_STRPTR or op == 0x94:
        return align4(p) + 4
    if op in (0x83, 0x88, 0x96):
        return align4(p) + 8
    if op == 0x8E:
        return align4(p) + 28
    if op == 0x8F:
        return align4(p) + 20
    if op == 0x9B:
        return align4(p) + 24
    if op in OP_BYTE:
        return p + 1
    if op in OP_WORD:
        return p + 2
    if op in OP_DWORD:
        return p + 4
    return p


class Const:
    def __init__(self, b: bytes):
        self.b = b
        assert b[:17] == b"Apt constant file", b[:20]
        self.movie_off, self.count, self.items_off = struct.unpack_from(">III", b, 0x14)
        self.items = []
        self.blocks = [(0, 0x20, "const.header")]
        self.blocks.append((self.items_off, 8 * self.count, "const.items"))
        self.strings = {}
        for i in range(self.count):
            t, v = struct.unpack_from(">II", b, self.items_off + 8 * i)
            self.items.append((t, v))
            if t == 1 and v:
                e = b.index(b"\0", v)
                self.strings[v] = b[v:e]
        for off, s in self.strings.items():
            self.blocks.append((off, len(s) + 1, "const.str"))

    def item_text(self, i):
        t, v = self.items[i]
        if t == 1:
            return repr(self.strings.get(v, b"").decode("latin1"))
        if t == 6:
            return str(struct.unpack(">f", struct.pack(">I", v))[0])
        if t == 7:
            return str(v - (1 << 32) if v & 0x80000000 else v)
        if t == 5:
            return "true" if v else "false"
        if t == 4:
            return f"reg{v}"
        if t == 8:
            return f"lookup{v}"
        if t == 3:
            return "undefined"
        return f"type{t}:{v:#x}"


class Apt:
    def __init__(self, apt: bytes, const: bytes):
        self.b = apt
        self.c = Const(const)
        assert apt[:14] == b"Apt Data:1:7:4", apt[:16]
        self.blocks = [(0, 0x10, "apt.magic")]
        self.relocs = []          # (fieldOff, targetOff, kindOfTarget)
        self.const_refs = []      # (fieldOff, constIndex) push/constpool indices
        self.chars = {}
        self.bytecode = []        # (start, end, owner)
        self._seen = set()
        self.walk()

    # ---- helpers
    def u(self, o):
        return struct.unpack_from(">I", self.b, o)[0]

    def i(self, o):
        return struct.unpack_from(">i", self.b, o)[0]

    def blk(self, off, size, kind):
        key = (off, kind)
        if key in self._seen:
            return False
        self._seen.add(key)
        self.blocks.append((off, size, kind))
        return True

    def ptr(self, field, kind):
        v = self.u(field)
        if v:
            self.relocs.append((field, v, kind))
        return v

    def cstr(self, field, kind="str"):
        v = self.ptr(field, kind)
        if v:
            e = self.b.index(b"\0", v)
            self.blk(v, e - v + 1, kind)
            return self.b[v:e].decode("latin1")
        return None

    # ---- walk
    def walk(self):
        mv = self.c.movie_off
        self.movie = mv
        # Movie char (type 9): 0x10 char header + P[0..12]  (P = movie+0x10)
        self.blk(mv, 0x44, "char.Movie")
        self.sprite(mv)                                   # P[0]/P[1] frames
        nchars = self.u(mv + 0x1C)
        tab = self.ptr(mv + 0x20, "chartab")
        self.blk(tab, 4 * nchars, "chartab")
        self.width, self.height, self.ms_per_frame = self.u(mv + 0x24), self.u(mv + 0x28), self.u(mv + 0x2C)
        # movie+0x30/+0x34: 16-byte IMPORT records {movieUrl*, symbol*, localCharId, rtSlot}
        #   (e.g. SCRMAINMENU: 'components/IconMenu','IconMenu',char 1 - char 1 is NULL in
        #    the file; the registrar keys the url into ctx+0x1C and fills +12 at runtime)
        # movie+0x38/+0x3C: 8-byte EXPORT/linkage records {name*, charId} (e.g. '__Packages.X')
        # (registrar sub_826ADD40: P=movie+0x10, P+0x20/+0x24 = imports, P+0x28/+0x2C = exports;
        #  ENGINE_MAP called the 16-byte table "exports" until 2026-09-30)
        nimp = self.u(mv + 0x30)
        imp = self.ptr(mv + 0x34, "imports") if nimp else self.u(mv + 0x34)
        nexp = self.u(mv + 0x38)
        exp = self.ptr(mv + 0x3C, "exports") if nexp else self.u(mv + 0x3C)
        self.exports, self.imports = [], []
        if nimp:
            self.blk(imp, 16 * nimp, "imports")
            for k in range(nimp):
                e = imp + 16 * k
                n = self.cstr(e, "import.url")
                n2 = self.cstr(e + 4, "import.symbol")
                self.imports.append((n, n2, self.u(e + 8), self.u(e + 12)))
        if nexp:
            self.blk(exp, 8 * nexp, "exports")
            for k in range(nexp):
                e = exp + 8 * k
                self.exports.append((self.cstr(e, "export.name"), self.u(e + 4)))
        for idx in range(nchars):
            c = self.ptr(tab + 4 * idx, "char") if self.u(tab + 4 * idx) else 0
            if not c or c == mv:
                self.chars[idx] = (c, 9 if c == mv else None)
                continue
            t = self.u(c)
            self.chars[idx] = (c, t)
            if t == 2:        # Text: +0x3C, +0x40 strings
                self.blk(c, 0x44, "char.Text")
                self.cstr(c + 0x3C, "text.str3C")
                self.cstr(c + 0x40, "text.str40")
            elif t == 3:      # Font: +0x10 name, +0x14 count, +0x18 array
                self.blk(c, 0x1C, "char.Font")
                self.cstr(c + 0x10, "font.name")
                n = self.u(c + 0x14)
                a = self.ptr(c + 0x18, "font.glyphs")
                if a:
                    self.blk(a, 4 * n, "font.glyphs")   # element size NOT verified
            elif t in (5, 9):
                self.blk(c, 0x1C, "char.Sprite")
                self.sprite(c)
            elif t == 1:
                self.blk(c, 0x24, "char.Shape")          # size inferred, see coverage
            elif t == 7:
                self.blk(c, 0x14, "char.Image")          # size inferred, see coverage
            else:
                self.blk(c, 0x10, f"char.type{t}")

    def sprite(self, c):
        nfr = self.u(c + 0x10)
        fr = self.ptr(c + 0x14, "frames")
        if not nfr or not fr:
            return
        self.blk(fr, 8 * nfr, "frames")
        for f in range(nfr):
            n = self.u(fr + 8 * f)
            arr = self.ptr(fr + 8 * f + 4, "itemptrs") if n else self.u(fr + 8 * f + 4)
            if not n:
                continue
            self.blk(arr, 4 * n, "itemptrs")
            for k in range(n):
                it = self.ptr(arr + 4 * k, "item")
                self.item(it, c)

    def item(self, it, owner):
        t = self.u(it)
        if t == 1:          # Action: +4 bytecode
            self.blk(it, 8, "item.Action")
            bc = self.ptr(it + 4, "bytecode")
            self.code(bc, f"action@{it:#x}")
        elif t == 2:        # FrameLabel: +4 string (+8.. flags?)
            self.blk(it, 16, "item.FrameLabel")        # +0xC = frame index (coverage-verified)
            self.cstr(it + 4, "label")
        elif t == 3:        # PlaceObject: +0x34 name, +0x3C clip actions
            self.blk(it, 0x40, "item.PlaceObject")
            self.cstr(it + 0x34, "po.name")
            ca = self.ptr(it + 0x3C, "po.clipactions")
            if ca:
                self.blk(ca, 8, "po.clipactions")
                n = self.u(ca)
                recs = self.ptr(ca + 4, "po.clipaction.recs")
                self.blk(recs, 12 * n, "po.clipaction.recs")
                for k in range(n):
                    bc = self.ptr(recs + 12 * k + 8, "bytecode")
                    self.code(bc, f"clipevent@{recs + 12 * k:#x}")
        elif t == 8:        # InitAction: +4 sprite id, +8 bytecode
            self.blk(it, 12, "item.InitAction")
            bc = self.ptr(it + 8, "bytecode")
            self.code(bc, f"initaction@{it:#x}")
        elif t == 4:        # RemoveObject: +4 depth (sub_826BE100 -> lookup by depth)
            self.blk(it, 8, "item.RemoveObject")       # 8 B leaves 0 uncovered non-zero bytes
        elif t == 5:        # BackgroundColor: +4 0xAARRGGBB (one per movie)
            self.blk(it, 8, "item.BackgroundColor")    # 8 B leaves 0 uncovered non-zero bytes
        else:
            self.blk(it, 4, f"item.type{t}")

    def code(self, pc, owner):
        if not pc or (pc, "bytecode") in self._seen:
            return
        start = pc
        b = self.b
        while True:
            op = b[pc]
            pc += 1
            if op == 0:
                break
            if op in OP_ALIGN_U32:
                pc = align4(pc) + 4
            elif op == 0x83:
                a = align4(pc); pc = a + 8
                self.cstr(a, "code.str"); self.cstr(a + 4, "code.str")
            elif op in (0x88, 0x96):
                a = align4(pc); pc = a + 8
                n = self.u(a)
                arr = self.ptr(a + 4, "code.constidx")
                if n:
                    self.blk(arr, 4 * n, "code.constidx")
                    for k in range(n):
                        self.const_refs.append((arr + 4 * k, self.u(arr + 4 * k)))
            elif op in (0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7):
                a = align4(pc); pc = a + 4
                self.cstr(a, "code.str")
            elif op == 0x8E:   # DefineFunction2: 7 dwords
                a = align4(pc); pc = a + 28
                self.cstr(a, "code.fname")
                n = self.u(a + 4)
                arr = self.ptr(a + 12, "code.f2params")
                if n and arr:
                    self.blk(arr, 8 * n, "code.f2params")
                    for k in range(n):
                        self.cstr(arr + 8 * k + 4, "code.pname")
            elif op == 0x8F:   # Try: 5 dwords
                a = align4(pc); pc = a + 20
                if not (b[a + 12] & 4):
                    self.cstr(a + 16, "code.catchname")
            elif op == 0x94:   # With: relative
                a = align4(pc); pc = a + 4
            elif op == 0x9B:   # DefineFunction: 6 dwords
                a = align4(pc); pc = a + 24
                self.cstr(a, "code.fname")
                n = self.u(a + 4)
                arr = self.ptr(a + 8, "code.fparams")
                if n and arr:
                    self.blk(arr, 4 * n, "code.fparams")
                    for k in range(n):
                        self.cstr(arr + 4 * k, "code.pname")
            elif op in OP_BYTE:
                pc += 1
            elif op in OP_WORD:
                pc += 2
            elif op in OP_DWORD:
                pc += 4
        self.blk(start, pc - start, "bytecode")
        self._seen.add((start, "bytecode"))
        self.bytecode.append((start, pc, owner))


def coverage(a: Apt, data=None, blocks=None):
    data = a.b if data is None else data
    blocks = a.blocks if blocks is None else blocks
    cov = bytearray(len(data))
    for off, size, kind in blocks:
        for k in range(off, min(off + size, len(data))):
            cov[k] = 1
    gaps = []
    k = 0
    while k < len(data):
        if not cov[k]:
            s = k
            while k < len(data) and not cov[k]:
                k += 1
            gaps.append((s, k))
        else:
            k += 1
    covered = sum(cov)
    nonzero_gaps = [(s, e) for s, e in gaps if any(data[s:e])]
    return covered, len(data), gaps, nonzero_gaps
