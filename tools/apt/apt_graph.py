#!/usr/bin/env python3
"""apt_graph.py - editable block graph for an Apt 1:7:4 movie (.apt + .const).

from_apt(Apt) -> Graph; Graph.emit() -> (apt_bytes, const_bytes, {blockKey: newOffset}).
  * every block keeps its content + a map of pointer fields -> target block key
  * bytecode blocks are INSTRUCTION LISTS with symbolic branch targets and
    function-body ends, re-assembled at emit time (alignment pads recomputed,
    branch offsets / DefineFunction(2) body sizes recomputed)
  * the .const is a list of items [(type, value | ('str', bytes))], re-emitted
    header + items + 4-aligned strings (the original tool's layout)
  * blocks are emitted in self.order, each 4-aligned. Graph.gc() drops blocks that
    nothing reachable points to any more and .const items that no Push/ConstantPool
    array references (renumbering the arrays); on an unedited movie it is a no-op
    (tools/apt/rt_asm.py checks 820/820 byte-identical after gc()).
  * Graph.renumber_walk_order() (after gc(), before emit(); patch_movie.Patcher.finish
    runs it for every patched movie) stores the .const items in the runtime's UNLOAD
    walk order, one item per index slot: the runtime rewrites every slot as a running
    counter on unload and a reload reuses those bytes (tools/apt/apt_unload.py checks it
    on the output bytes). Edits append items at the end (add_const_string, aptasm,
    patch_movie._grow_pool) and gc() keeps that order; renumbering is what makes the
    output reloadable. A no-op on an unedited movie (rt_graph.py, 820/820).
  * There is deliberately NO string/item dedupe: in the retail corpus every .const
    item is referenced by exactly one Push/ConstantPool array element (19,812/19,812,
    0 shared, 0 unreferenced) and every index array is private, so new code gets a
    fresh item per element (tools/apt/aptasm.py) instead of sharing.
Identity round trip (no edits) is byte-exact on all 820 ui.AST movies, and a
shifted emit re-parses to the same logical movie (tools/apt/rt_graph.py).
Promoted from archive/v1_frontend/apt_format/apt_graph.py (2026-09-30).
"""
import struct

try:
    from .apt_model import Apt, align4, insn_end, OP_BYTE, OP_WORD, OP_DWORD, Const
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    from apt_model import Apt, align4, insn_end, OP_BYTE, OP_WORD, OP_DWORD, Const

BRANCH = {0x99, 0x9D, 0xB8}               # target = next_pc + i32
PLAIN_I32 = {0x81, 0x87, 0x9F}            # GotoFrame, StoreRegister, GotoFrame2
STRPTR1 = {0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7}
CODE_PTR_FIELDS = ("s", "arr", "name", "params", "catch")   # Ins fields holding block keys


# position after an instruction with opcode `op` starting at byte `p` (operand sizes
# depend only on the opcode and on 4-alignment): the runtime's own table, apt_model.insn_end
ins_size_after = insn_end


def positions(ops):
    """byte offset of every instruction (plus one past the last) for a block that
    starts 4-aligned; `ops` is a list of opcodes."""
    pos = [0]
    for op in ops:
        pos.append(ins_size_after(op, pos[-1]))
    return pos


def code_refs(code):
    """every block key an instruction list points at (strings, index arrays,
    function names / params, catch names)."""
    out = []
    for ins in code:
        for fld in CODE_PTR_FIELDS:
            v = ins.f.get(fld)
            if v is None:
                continue
            if isinstance(v, list):
                out.extend(x for x in v if x is not None)
            else:
                out.append(v)
    return out


class Ins:
    __slots__ = ("op", "f")

    def __init__(self, op, **f):
        self.op = op
        self.f = f

    def __repr__(self):
        return f"Ins({self.op:#04x}, {self.f})"


class Block:
    def __init__(self, kind, data=None, ptrs=None, code=None):
        self.kind = kind
        self.data = bytearray(data) if data is not None else None
        self.ptrs = dict(ptrs or {})        # relOff -> target key
        self.code = code                    # list[Ins] for bytecode blocks


class Graph:
    def __init__(self):
        self.blocks = {}
        self.order = []
        self.movie = None
        self.const_hdr = None
        self.items = []
        self._n = 0

    def new_key(self):
        self._n += 1
        return ("new", self._n)

    def add_block(self, blk, after=None):
        k = self.new_key()
        self.blocks[k] = blk
        if after is None:
            self.order.append(k)
        else:
            self.order.insert(self.order.index(after) + 1, k)
        return k

    def add_string(self, s: bytes, after=None):
        return self.add_block(Block("str", s + b"\0"), after)

    def add_const_string(self, s: bytes):
        """append a string item to the .const; returns its item index."""
        self.items.append((1, ("str", s)))
        return len(self.items) - 1

    # ------------------------------------------------------------ assembly
    def assemble(self, code, labels_out=None):
        """code -> (bytes, {relOff: targetKey}). Branch targets are instruction
        indices ('tgt'), function bodies end at instruction index ('end')."""
        # operand sizes depend only on opcode + alignment, so one pass fixes every position
        pos = positions([ins.op for ins in code])
        out = bytearray()
        ptrs = {}
        for idx, ins in enumerate(code):
            op = ins.op
            out.append(op)
            if op == 0:
                continue
            f = ins.f

            def pad():
                while len(out) % 4:
                    out.append(0)
            nxt = pos[idx + 1]
            if op in BRANCH or op == 0x94:
                pad(); out += struct.pack(">i", pos[f["tgt"]] - nxt)
            elif op in PLAIN_I32:
                pad(); out += struct.pack(">i", f["i32"])
            elif op in STRPTR1:
                pad(); ptrs[len(out)] = f["s"]; out += b"\0\0\0\0"
            elif op == 0x83:
                pad()
                for k in f["s"]:
                    if k is not None:
                        ptrs[len(out)] = k
                    out += b"\0\0\0\0"
            elif op in (0x88, 0x96):
                pad(); out += struct.pack(">I", f["n"])
                if f["arr"] is not None:
                    ptrs[len(out)] = f["arr"]
                out += b"\0\0\0\0"
            elif op == 0x8E:
                pad()
                if f["name"] is not None:
                    ptrs[len(out)] = f["name"]
                out += b"\0\0\0\0" + struct.pack(">II", f["np"], f["flags"])
                if f["params"] is not None:
                    ptrs[len(out)] = f["params"]
                out += b"\0\0\0\0"
                body = pos[f["end"]] - nxt
                out += struct.pack(">III", body, f["m1"], f["m2"])
            elif op == 0x9B:
                pad()
                if f["name"] is not None:
                    ptrs[len(out)] = f["name"]
                out += b"\0\0\0\0" + struct.pack(">I", f["np"])
                if f["params"] is not None:
                    ptrs[len(out)] = f["params"]
                out += b"\0\0\0\0"
                body = pos[f["end"]] - nxt
                out += struct.pack(">III", body, f["m1"], f["m2"])
            elif op == 0x8F:
                pad()
                raw = bytearray(f["raw"])
                if f.get("catch") is not None:
                    ptrs[len(out) + 16] = f["catch"]
                    raw[16:20] = b"\0\0\0\0"
                out += raw
            elif op in OP_BYTE or op in OP_WORD or op in OP_DWORD:
                out += f["imm"]
            assert len(out) == pos[idx + 1], (idx, hex(op), len(out), pos[idx + 1])
        return bytes(out), ptrs

    # ------------------------------------------------------------ gc
    def refs(self, k, strong=False):
        """block keys that block k points at. strong=True leaves out the params pointer
        of a DefineFunction(2) with no params: retail keeps it non-null (926/926) but
        aims it at whatever block follows, and nothing reads it."""
        b = self.blocks[k]
        if b.code is not None:
            if not strong:
                return code_refs(b.code)
            return code_refs([i if not (i.op in (0x8E, 0x9B) and not i.f["np"]) else
                              Ins(i.op, **{f: v for f, v in i.f.items() if f != "params"}) for i in b.code])
        return [t for t in b.ptrs.values() if t is not None]

    def gc(self):
        """Drop orphans: blocks not reachable from the roots (the block at offset 0 =
        the magic, and the movie char that const+0x14 names) and .const items that no
        reachable Push/ConstantPool array references. Surviving items are renumbered
        in order and every array rewritten. Returns (removed_block_keys,
        removed_item_count). A no-op on an unedited movie."""
        roots = [self.order[0], self.movie]
        seen = set()
        stack = list(roots)
        while stack:
            k = stack.pop()
            if k in seen:
                continue
            seen.add(k)
            stack.extend(t for t in self.refs(k, strong=True) if t not in seen)
        removed = [k for k in self.order if k not in seen]
        for k in removed:
            del self.blocks[k]
        self.order = [k for k in self.order if k in seen]
        # an empty params pointer whose target went away: aim it at the function's name
        # (or its own bytecode block), keeping it non-null like retail
        for k in self.order:
            b = self.blocks[k]
            if b.code is None:
                continue
            for ins in b.code:
                if ins.op in (0x8E, 0x9B) and not ins.f["np"] and ins.f["params"] is not None \
                        and ins.f["params"] not in self.blocks:
                    ins.f["params"] = ins.f["name"] if ins.f["name"] is not None else k
        # .const items: referenced only through Push (0x96) / ConstantPool (0x88) arrays
        arrays = []
        for k in self.order:
            b = self.blocks[k]
            if b.code is None:
                continue
            for ins in b.code:
                if ins.op in (0x88, 0x96) and ins.f["arr"] is not None and ins.f["n"]:
                    arrays.append((ins.f["arr"], ins.f["n"]))
        used = set()
        for ak, n in arrays:
            data = self.blocks[ak].data
            used.update(struct.unpack_from(">I", data, 4 * q)[0] for q in range(n))
        assert all(i < len(self.items) for i in used), "an index array names a missing .const item"
        if len(used) == len(self.items):
            return removed, 0
        keep = sorted(used)
        remap = {old: new for new, old in enumerate(keep)}
        dropped = len(self.items) - len(keep)
        self.items = [self.items[i] for i in keep]
        done = set()
        for ak, n in arrays:
            if ak in done:
                continue
            done.add(ak)
            data = self.blocks[ak].data
            for q in range(n):
                struct.pack_into(">I", data, 4 * q, remap[struct.unpack_from(">I", data, 4 * q)[0]])
        return removed, dropped

    # ------------------------------------------------------------ unload walk order
    def _u32(self, k, off):
        return struct.unpack_from(">I", self.blocks[k].data, off)[0]

    def unload_code_blocks(self):
        """bytecode block keys in the order the runtime's UNLOAD walks them
        (AptMovieUnrelocate_sub_826A3180 -> AptSpriteFramesUnrelocate_sub_826A0758; the
        byte-level twin and the IDA addresses are in tools/apt/apt_unload.py): chars in
        index order, types 5/9 only, NULL slots and import-table indices skipped (by
        membership: imported slots are live, not NULL, during the runtime's walk) -> frames -> items
        (type 1 Action +4, type 3 PlaceObject clip-action records +8, type 8 InitAction +8)."""
        mv = self.movie
        nchars = self._u32(mv, 0x1C)
        tab = self.blocks[self.blocks[mv].ptrs[0x20]]
        imported = set()
        nimp = self._u32(mv, 0x30)
        ik = self.blocks[mv].ptrs.get(0x34)
        if nimp and ik is not None:
            imported = {self._u32(ik, 16 * q + 8) for q in range(nimp)}
        out = []
        for idx in range(nchars):
            ck = tab.ptrs.get(4 * idx)
            if ck is None or idx in imported:
                continue
            t = 9 if ck == mv else self._u32(ck, 0)
            if t not in (5, 9):
                continue
            fk = self.blocks[ck].ptrs.get(0x14)
            nfr = self._u32(ck, 0x10)
            if not nfr or fk is None:
                continue
            for f in range(nfr):
                n = self._u32(fk, 8 * f)
                ak = self.blocks[fk].ptrs.get(8 * f + 4)
                if not n or ak is None:
                    continue
                for q in range(n):
                    it = self.blocks[ak].ptrs[4 * q]
                    ty = self._u32(it, 0)
                    ib = self.blocks[it]
                    if ty == 1 and ib.ptrs.get(4) is not None:
                        out.append(ib.ptrs[4])
                    elif ty == 3 and ib.ptrs.get(0x3C) is not None:
                        ca = ib.ptrs[0x3C]
                        rk = self.blocks[ca].ptrs.get(4)
                        for r in range(self._u32(ca, 0) if rk is not None else 0):
                            bk = self.blocks[rk].ptrs.get(12 * r + 8)
                            if bk is not None:
                                out.append(bk)
                    elif ty == 8 and ib.ptrs.get(8) is not None:
                        out.append(ib.ptrs[8])
        return out

    def renumber_walk_order(self):
        """Store the .const items in the runtime's UNLOAD order: slot k (the k-th
        Push/ConstantPool index slot of the unload walk) gets index k, and item k is the
        constant that slot named, so #items == #slots. On unload the runtime rewrites every
        slot as a running counter in exactly this walk (it never restores the original
        index), and a reload reuses those bytes; any other order makes the reload resolve
        wrong constants (the iter-463 QUIT GAME soft-lock: ScrPressStart's registerClass pool
        read [reg2, reg2, reg2]). Retail satisfies this in 820/820 movies, so on an unedited
        movie this is a no-op. Run it after gc() and before emit() (Patcher.finish does).
        An index array used by two instructions is copied (each slot is renumbered once per
        visit), and a constant named by two slots becomes two items. Raises ValueError when
        a bytecode block is reached twice (the runtime would relocate it twice) or when a
        bytecode block with index arrays is not reached (its slots would never be renumbered).
        Returns {slots, moved, duplicated, dropped, arrays_copied}."""
        seen_code, seen_arr = set(), set()
        slots = []
        copied = 0
        for bk in self.unload_code_blocks():
            if bk in seen_code:
                raise ValueError(f"bytecode block {bk} is reached twice by the unload walk")
            seen_code.add(bk)
            for ins in self.blocks[bk].code:
                if ins.op not in (0x88, 0x96) or not ins.f["n"] or ins.f["arr"] is None:
                    continue
                ak = ins.f["arr"]
                if ak in seen_arr:
                    ak = self.add_block(Block("code.constidx", bytes(self.blocks[ak].data)), after=ak)
                    ins.f["arr"] = ak
                    copied += 1
                seen_arr.add(ak)
                slots.extend((ak, q) for q in range(ins.f["n"]))
        missed = [k for k in self.order if self.blocks[k].code is not None and k not in seen_code
                  and any(i.op in (0x88, 0x96) and i.f["n"] for i in self.blocks[k].code)]
        if missed:
            raise ValueError(f"{len(missed)} bytecode block(s) with index arrays are not on the unload walk: {missed[:3]}")
        old = [struct.unpack_from(">I", self.blocks[ak].data, 4 * q)[0] for ak, q in slots]
        if any(i >= len(self.items) for i in old):
            raise ValueError("an index array names a missing .const item")
        for pos, (ak, q) in enumerate(slots):
            struct.pack_into(">I", self.blocks[ak].data, 4 * q, pos)
        n_items = len(self.items)
        self.items = [self.items[i] for i in old]
        return {"slots": len(slots), "moved": sum(1 for pos, i in enumerate(old) if i != pos),
                "duplicated": len(old) - len(set(old)), "dropped": n_items - len(set(old)),
                "arrays_copied": copied}

    # ------------------------------------------------------------ emit
    def emit(self, orig_tail=0):
        # 1) materialise every block's bytes (bytecode sizes are position-independent
        #    because every block starts 4-aligned)
        mat = {}
        for k in self.order:
            b = self.blocks[k]
            if b.code is not None:
                data, ptrs = self.assemble(b.code)
                mat[k] = (bytearray(data), ptrs)
            else:
                mat[k] = (bytearray(b.data), dict(b.ptrs))
        # 2) layout
        where = {}
        cur = 0
        for k in self.order:
            cur = align4(cur)
            where[k] = cur
            cur += len(mat[k][0])
        out = bytearray(cur + orig_tail)
        for k in self.order:
            data, ptrs = mat[k]
            for rel, tgt in ptrs.items():
                struct.pack_into(">I", data, rel, where[tgt] if tgt is not None else 0)
            out[where[k]:where[k] + len(data)] = data
        # 3) const
        c = bytearray(self.const_hdr) + struct.pack(">III", where[self.movie], len(self.items), 0x20)
        c += bytes(8 * len(self.items))
        soff = {}
        for i, (t, v) in enumerate(self.items):
            if t == 1 and isinstance(v, tuple):
                c += bytes(align4(len(c)) - len(c))
                soff[i] = len(c)
                c += v[1] + b"\0"
        for i, (t, v) in enumerate(self.items):
            struct.pack_into(">II", c, 0x20 + 8 * i, t, soff[i] if i in soff else v)
        return bytes(out), bytes(c), where


def from_apt(a: Apt) -> Graph:
    g = Graph()
    uniq = {}
    for off, size, kind in a.blocks:
        if off not in uniq or size > uniq[off][0]:
            uniq[off] = (size, kind)
    reloc = {f: t for f, t, k in a.relocs}
    code_starts = {s: (s, e) for s, e, own in a.bytecode}
    for off in sorted(uniq):
        size, kind = uniq[off]
        if kind == "bytecode":
            g.blocks[off] = Block("bytecode", code=_lift_code(a, off))
        else:
            data = a.b[off:off + size]
            ptrs = {f - off: reloc[f] for f in range(off, off + size) if f in reloc}
            # empty export/import arrays of the movie: keep the raw value
            g.blocks[off] = Block(kind, data, ptrs)
        g.order.append(off)
    g.movie = a.c.movie_off
    g.const_hdr = a.c.b[:0x14]
    g.items = [(t, ("str", a.c.strings[v]) if (t == 1 and v) else v) for t, v in a.c.items]
    return g


def _lift_code(a: Apt, start):
    """disassemble into Ins list with symbolic targets."""
    b = a.b
    pc = start
    raw = []
    addr_to_idx = {}
    while True:
        at = pc
        addr_to_idx[at] = len(raw)
        op = b[pc]; pc += 1
        f = {}
        if op == 0:
            raw.append((at, Ins(0), None)); break
        if op in BRANCH or op == 0x94:
            p = align4(pc); pc = p + 4
            f["_abs"] = pc + struct.unpack_from(">i", b, p)[0]
        elif op in PLAIN_I32:
            p = align4(pc); pc = p + 4; f["i32"] = struct.unpack_from(">i", b, p)[0]
        elif op in STRPTR1:
            p = align4(pc); pc = p + 4; f["s"] = a.u(p) or None
        elif op == 0x83:
            p = align4(pc); pc = p + 8; f["s"] = [a.u(p) or None, a.u(p + 4) or None]
        elif op in (0x88, 0x96):
            p = align4(pc); pc = p + 8; f["n"] = a.u(p); f["arr"] = a.u(p + 4) or None
        elif op == 0x8E:
            p = align4(pc); pc = p + 28
            f.update(name=a.u(p) or None, np=a.u(p + 4), flags=a.u(p + 8), params=a.u(p + 12) or None,
                     m1=a.u(p + 20), m2=a.u(p + 24))
            f["_abs"] = pc + a.u(p + 16)
        elif op == 0x9B:
            p = align4(pc); pc = p + 24
            f.update(name=a.u(p) or None, np=a.u(p + 4), params=a.u(p + 8) or None,
                     m1=a.u(p + 16), m2=a.u(p + 20))
            f["_abs"] = pc + a.u(p + 12)
        elif op == 0x8F:
            p = align4(pc); pc = p + 20
            f["raw"] = b[p:p + 20]
            f["catch"] = (a.u(p + 16) or None) if not (b[p + 12] & 4) else None
        elif op in OP_BYTE:
            f["imm"] = b[pc:pc + 1]; pc += 1
        elif op in OP_WORD:
            f["imm"] = b[pc:pc + 2]; pc += 2
        elif op in OP_DWORD:
            f["imm"] = b[pc:pc + 4]; pc += 4
        raw.append((at, Ins(op, **f), pc))
    addr_to_idx[pc] = len(raw)   # one past End
    code = []
    for at, ins, nxt in raw:
        if "_abs" in ins.f:
            tgt = addr_to_idx[ins.f.pop("_abs")]
            ins.f["tgt" if ins.op in BRANCH or ins.op == 0x94 else "end"] = tgt
        code.append(ins)
    return code
