#!/usr/bin/env python3
"""apt_write.py - re-serialise an .apt/.const pair from the apt_model block graph.

relayout(a, insert_after=None, filler=b"") moves every block to a new offset
(original order, 4-byte aligned, optional filler inserted after one block) and
rewrites EVERY pointer field in the reloc set, plus const+0x14 (movie offset).
Bytecode blocks are RE-ASSEMBLED from the disassembly (not copied), strings are
re-emitted from their decoded text, so an identical result proves the
disassembler/assembler and the pointer map, not just a memcpy.

This is the independent second writer used by tools/apt/rt_test.py (identity and
shifted+reparsed); edits go through apt_graph.py. Promoted from
archive/v1_frontend/apt_format/apt_write.py (2026-09-30).
"""
import struct

try:
    from .apt_model import Apt, align4, OP_BYTE, OP_WORD, OP_DWORD
    from .apt_asm import disasm
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    from apt_model import Apt, align4, OP_BYTE, OP_WORD, OP_DWORD
    from apt_asm import disasm


def assemble(ins_list, base, addr_of_str, addr_of_block, a):
    """Re-assemble bytecode at new address `base`. Pointer operands are looked
    up through addr_of_str(bytes-or-None, oldFieldTarget) / addr_of_block(oldOff).
    Returns bytes."""
    out = bytearray()
    for ins in ins_list:
        op = ins["op"]
        out.append(op)
        if op == 0:
            break
        def pad():
            p = ins.get("pad", b"")
            out.extend(p)
            assert (base + len(out)) % 4 == 0, "alignment model broken"
        old = ins["at"]
        # recover the ORIGINAL operand words so pointer operands can be mapped
        opnd = align4(old + 1)
        if op in (0x81, 0x87, 0x99, 0x9D, 0x9F, 0xB8, 0x94):
            pad(); out += struct.pack(">i", ins["i32"])
        elif op == 0x83:
            pad()
            for k in range(2):
                out += struct.pack(">I", addr_of_block(a.u(opnd + 4 * k)))
        elif op in (0x88, 0x96):
            pad()
            out += struct.pack(">II", a.u(opnd), addr_of_block(a.u(opnd + 4)))
        elif op in (0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7):
            pad(); out += struct.pack(">I", addr_of_block(a.u(opnd)))
        elif op == 0x8E:
            pad()
            out += struct.pack(">IIII", addr_of_block(a.u(opnd)), a.u(opnd + 4), a.u(opnd + 8), addr_of_block(a.u(opnd + 12)))
            out += struct.pack(">III", a.u(opnd + 16), a.u(opnd + 20), a.u(opnd + 24))
        elif op == 0x8F:
            pad()
            raw = bytearray(ins["raw"])
            if not (raw[12] & 4):
                struct.pack_into(">I", raw, 16, addr_of_block(a.u(opnd + 16)))
            out += raw
        elif op == 0x9B:
            pad()
            out += struct.pack(">III", addr_of_block(a.u(opnd)), a.u(opnd + 4), addr_of_block(a.u(opnd + 8)))
            out += struct.pack(">III", a.u(opnd + 12), a.u(opnd + 16), a.u(opnd + 20))
        elif op in OP_BYTE or op in OP_WORD or op in OP_DWORD:
            out += ins["imm"]
    return bytes(out)


def relayout(a: Apt, insert_after=None, filler=b""):
    """insert_after: original block offset after which `filler` bytes are
    inserted (forces every later block to move)."""
    blocks = sorted(set(a.blocks))
    # merge duplicates (same start) keeping the largest size
    uniq = {}
    for off, size, kind in blocks:
        if off not in uniq or size > uniq[off][0]:
            uniq[off] = (size, kind)
    order = sorted(uniq)
    new = {}
    cur = 0
    layout = []
    for off in order:
        size, kind = uniq[off]
        cur = align4(cur) if off != 0 else 0
        new[off] = cur
        layout.append((off, cur, size, kind))
        cur += size
        if insert_after is not None and off == insert_after:
            cur = align4(cur); cur += len(filler)
    total = align4(cur) if cur % 4 else cur
    # match original tail padding
    tail = len(a.b) - (order[-1] + uniq[order[-1]][0])
    total = cur + tail

    def addr(old):
        if old == 0:
            return 0
        if old in new:
            return new[old]
        # pointer into the middle of a block
        for o in reversed(order):
            if o <= old < o + uniq[o][0]:
                return new[o] + (old - o)
        raise KeyError(hex(old))

    out = bytearray(total)
    reloc_fields = {f: t for f, t, k in a.relocs}
    code_starts = {s for s, e, own in a.bytecode}
    for off, noff, size, kind in layout:
        if kind == "bytecode":
            ins = disasm(a, off)
            blob = assemble(ins, noff, None, addr, a)
            assert len(blob) == size, (hex(off), len(blob), size)
            out[noff:noff + size] = blob
            continue
        if kind in ("str",) or kind.endswith(".str") or kind.endswith("name") or kind in (
                "label", "code.str", "code.fname", "code.pname", "code.catchname", "po.name",
                "text.str3C", "text.str40", "font.name", "import.url", "import.symbol", "export.name"):
            s = a.b[off:a.b.index(b"\0", off)]
            out[noff:noff + len(s) + 1] = s + b"\0"
            continue
        out[noff:noff + size] = a.b[off:off + size]
        for f in range(off, off + size, 4):
            if f in reloc_fields:
                struct.pack_into(">I", out, noff + (f - off), addr(reloc_fields[f]))
    if insert_after is not None:
        # filler lands right after the chosen block
        pos = align4(new[insert_after] + uniq[insert_after][0])
        out[pos:pos + len(filler)] = filler
    c = bytearray(a.c.b)
    struct.pack_into(">I", c, 0x14, new[a.c.movie_off])
    return bytes(out), bytes(c), new
