#!/usr/bin/env python3
"""apt_asm.py - Apt 1:7:4 ActionScript bytecode disassembler (disasm / fmt / listing).
Re-assembly lives in apt_graph.Graph.assemble (and apt_write.assemble for the
relayout harness); the text assembler for this syntax is aptasm.py.

Opcode operand layout = AptActionBytecodeRelocate_sub_8269F3A0 (IDA).
Opcode handler table = 0x82931420, 185 slots (0x00..0xB8), names from data/ida_names.tsv.
Promoted from archive/v1_frontend/apt_format/apt_asm.py (2026-09-30).
"""
import struct

try:
    from .apt_model import Apt, align4, OP_BYTE, OP_WORD, OP_DWORD
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    from apt_model import Apt, align4, OP_BYTE, OP_WORD, OP_DWORD

NAMES = {
    0x00: "End", 0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop",
    0x0A: "Add", 0x0B: "Subtract", 0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals",
    0x0F: "Less", 0x10: "And", 0x11: "Or", 0x12: "Not", 0x13: "StringEquals",
    0x14: "StringLength", 0x15: "StringExtract", 0x17: "Pop", 0x18: "ToInteger",
    0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2", 0x21: "StringAdd",
    0x22: "GetProperty", 0x23: "SetProperty", 0x24: "CloneSprite", 0x25: "RemoveSprite",
    0x26: "Trace", 0x27: "StartDrag", 0x28: "EndDrag", 0x2A: "Throw", 0x2B: "CastOp",
    0x2C: "ImplementsOp", 0x30: "Random", 0x33: "AsciiToChar", 0x34: "GetTime",
    0x3A: "Delete", 0x3B: "Delete2", 0x3C: "DefineLocal", 0x3D: "CallFunction",
    0x3E: "Return", 0x3F: "Modulo", 0x40: "NewObject", 0x41: "DefineLocal2",
    0x42: "InitArray", 0x43: "InitObject", 0x44: "TypeOf", 0x45: "TargetPath",
    0x46: "Enumerate", 0x47: "Add2", 0x48: "Less2", 0x49: "Equals2", 0x4A: "ToNumber",
    0x4B: "ToString", 0x4C: "PushDuplicate", 0x4D: "StackSwap", 0x4E: "GetMember",
    0x4F: "SetMember", 0x50: "Increment", 0x51: "Decrement", 0x52: "CallMethod",
    0x53: "NewMethod", 0x54: "InstanceOf", 0x55: "Enumerate2", 0x56: "EA_PushBuiltin",
    0x58: "EA_PushConstValue", 0x5B: "EA_CallMethodPop", 0x5C: "EA_CallMethod",
    0x5D: "EA_CallFunctionPop", 0x5E: "EA_CallFunction", 0x60: "BitAnd", 0x61: "BitOr",
    0x62: "BitXor", 0x63: "BitLShift", 0x64: "BitRShift", 0x65: "BitURShift",
    0x66: "StrictEquals", 0x67: "Greater", 0x69: "Extends", 0x70: "EA_PushNamedValue",
    0x81: "GotoFrame", 0x83: "GetURL", 0x87: "StoreRegister", 0x88: "ConstantPool",
    0x8B: "SetTarget", 0x8C: "GotoLabel", 0x8E: "DefineFunction2", 0x8F: "Try",
    0x94: "With", 0x96: "Push", 0x99: "Jump", 0x9A: "GetURL2", 0x9B: "DefineFunction",
    0x9D: "If", 0x9E: "CallFrame", 0x9F: "GotoFrame2", 0xA1: "EA_PushString",
    0xA2: "EA_PushConstByte?", 0xA3: "EA_PushConstWord", 0xA4: "EA_GetStringVar",
    0xA5: "EA_GetStringMember", 0xA6: "EA_SetStringVar", 0xA7: "EA_SetStringMember",
    0xAE: "EA_PushValueOfVar", 0xAF: "EA_GetNamedMember", 0xB0: "EA_CallNamedFuncPop",
    0xB1: "EA_CallNamedFunc", 0xB2: "EA_CallNamedMethodPop", 0xB3: "EA_CallNamedMethod",
    0xB4: "EA_PushFloat", 0xB5: "EA_PushByte", 0xB6: "EA_PushShort", 0xB7: "EA_PushLong",
    0xB8: "EA_BranchIfFalse",
}


def disasm(a: Apt, start):
    """-> list of instructions: dict(op, at, pad(bytes), fields)."""
    b = a.b
    pc = start
    out = []
    while True:
        at = pc
        op = b[pc]; pc += 1
        ins = {"op": op, "at": at}
        if op == 0:
            out.append(ins); break
        def aligned(n):
            nonlocal pc
            al = align4(pc)
            ins["pad"] = b[pc:al]
            pc = al + n
            return al
        if op in (0x81, 0x87, 0x99, 0x9D, 0x9F, 0xB8):
            p = aligned(4); ins["i32"] = struct.unpack_from(">i", b, p)[0]
        elif op == 0x83:
            p = aligned(8); ins["s"] = [a.b[a.u(p):a.b.index(b"\0", a.u(p))] if a.u(p) else None,
                                         a.b[a.u(p + 4):a.b.index(b"\0", a.u(p + 4))] if a.u(p + 4) else None]
        elif op in (0x88, 0x96):
            p = aligned(8); n = a.u(p); arr = a.u(p + 4)
            ins["idx"] = [a.u(arr + 4 * k) for k in range(n)]
        elif op in (0x8B, 0x8C, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7):
            p = aligned(4); v = a.u(p)
            ins["s"] = b[v:b.index(b"\0", v)] if v else None
        elif op == 0x8E:
            p = aligned(28)
            nm = a.u(p); ins["name"] = b[nm:b.index(b"\0", nm)] if nm else None
            n = a.u(p + 4); arr = a.u(p + 12)
            ins["w"] = [a.u(p + 8), a.u(p + 16), a.u(p + 20), a.u(p + 24)]
            ins["params"] = [(a.u(arr + 8 * k), (lambda v: b[v:b.index(b"\0", v)] if v else None)(a.u(arr + 8 * k + 4))) for k in range(n)]
        elif op == 0x8F:
            p = aligned(20)
            ins["raw"] = b[p:p + 20]
            ins["catch"] = None
            if not (b[p + 12] & 4) and a.u(p + 16):
                v = a.u(p + 16); ins["catch"] = b[v:b.index(b"\0", v)]
        elif op == 0x94:
            p = aligned(4); ins["i32"] = struct.unpack_from(">i", b, p)[0]
        elif op == 0x9B:
            p = aligned(24)
            nm = a.u(p); ins["name"] = b[nm:b.index(b"\0", nm)] if nm else None
            n = a.u(p + 4); arr = a.u(p + 8)
            ins["w"] = [a.u(p + 12), a.u(p + 16), a.u(p + 20)]
            ins["params"] = [(lambda v: b[v:b.index(b"\0", v)] if v else None)(a.u(arr + 4 * k)) for k in range(n)]
        elif op in OP_BYTE:
            ins["imm"] = b[pc:pc + 1]; pc += 1
        elif op in OP_WORD:
            ins["imm"] = b[pc:pc + 2]; pc += 2
        elif op in OP_DWORD:
            ins["imm"] = b[pc:pc + 4]; pc += 4
        out.append(ins)
    return out


def fmt(ins, const=None, pool=None):
    """pool = index list of the most recent ConstantPool (0x88) in this block:
    byte/word operands of 0xA2/A3/AE/AF/B0-B3 index THAT pool (verified on
    MAIN: pools starting at const item 599/808 only make sense this way)."""
    op = ins["op"]
    s = f"{ins['at']:06x}  {NAMES.get(op, f'op_{op:02X}')}"
    if "i32" in ins: s += f" {ins['i32']}"
    if "s" in ins: s += " " + repr(ins["s"])
    if "idx" in ins:
        s += " [" + ", ".join(const.item_text(i) if const else str(i) for i in ins["idx"]) + "]"
    if "name" in ins: s += f" name={ins['name']!r} params={ins['params']} w={ins['w']}"
    if "imm" in ins:
        v = int.from_bytes(ins["imm"], "big")
        if op in (0xA2, 0xA3, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3) and const is not None and pool is not None and v < len(pool):
            s += f" pool[{v}]={const.item_text(pool[v])}"
        elif op in (0xA2, 0xA3, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3):
            s += f" pool[{v}]"
        elif op == 0xB4:
            s += f" {struct.unpack('>f', ins['imm'])[0]}"
        else:
            s += f" {v}"
    return s


def listing(a, start):
    pool = None
    lines = []
    for ins in disasm(a, start):
        if ins["op"] == 0x88:
            pool = ins["idx"]
        lines.append(fmt(ins, a.c, pool))
    return lines


def logical_view(a):
    """Offset-independent structural view of a parsed movie, for round-trip tests.

    Two parses are logically equal when their views are equal: the same movie header,
    the same blocks in the same order with the same kinds and sizes, every non-pointer
    byte equal, every pointer field naming the same (block index, delta) target, every
    bytecode block disassembling to the same instructions (strings and const-pool
    operands resolved by content; branch/function sizes are relative), and the same
    .const item list. Absolute offsets never appear, so a shifted layout compares
    equal to the original only if every relocation was rewritten correctly."""
    import bisect
    uniq = {}
    for off, size, kind in a.blocks:
        if off not in uniq or size > uniq[off][0]:
            uniq[off] = (size, kind)
    starts = sorted(uniq)

    def target(t):
        if t == 0:
            return None
        j = bisect.bisect_right(starts, t) - 1
        return (j, t - starts[j]) if j >= 0 else ("outside", t)

    reloc = {f: t for f, t, k in a.relocs}
    fields = sorted(reloc)
    view = [("movie", a.width, a.height, a.ms_per_frame, len(a.chars), a.u(a.movie + 0x10)),
            ("exports", a.exports), ("imports", [(e[0], e[1], e[2]) for e in a.imports])]
    for off in starts:
        size, kind = uniq[off]
        if kind == "bytecode":
            view.append((kind, [fmt(ins, a.c).split("  ", 1)[1] for ins in disasm(a, off)]))
            continue
        parts = []
        f = off
        k = bisect.bisect_left(fields, off)
        while k < len(fields) and fields[k] < off + size:
            p = fields[k]
            parts.append(bytes(a.b[f:p]))
            parts.append(("ptr", target(reloc[p])))
            f = p + 4
            k += 1
        parts.append(bytes(a.b[f:off + size]))
        view.append((kind, size, tuple(parts)))
    view.append(("const", [(t, a.c.strings.get(v) if t == 1 and v else v) for t, v in a.c.items]))
    return view
