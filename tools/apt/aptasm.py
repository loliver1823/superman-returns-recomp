#!/usr/bin/env python3
"""aptasm.py - text assembler + canonical disassembler for Apt 1:7:4 ActionScript
bytecode (CUSTOM_FRONTEND.md section 7.3, step 3).

The syntax IS the disassembler's (apt_asm.fmt / apt2txt.py instruction lines), plus:
  L1:                          a label (an identifier followed by ':'); branch operands
                               (If / Jump / EA_BranchIfFalse / With) name labels
  .func2 'name'(r3 'a', r5 'b') flags=0x60129     DefineFunction2 ... body ... .end
  .func  'name'('a', 'b')                           DefineFunction  ... body ... .end
  .end                         closes the innermost .func/.func2 body
  .pool 'a', 'b', reg1         = ConstantPool ['a', 'b', reg1]
  .const NAME = <value>        assembler-time alias, used as $NAME wherever a value,
                               string, integer or pool name is expected
  .block <text>                header line of `aptasm.py dis` output (ignored by the
                               single-block assembler; splits multi-block files)
  ; comment                    to end of line (outside quotes)
apt2txt's raw forms are accepted too, so a pasted apt2txt listing assembles: an
optional 6-hex-digit address column, numeric branch offsets (`If 293`, relative to the
next instruction) and `DefineFunction2 name=b'' params=[(3, b'x')] w=[flags, bodySize,
m1, m2]` / `DefineFunction name=b'' params=[b'x'] w=[bodySize, m1, m2]`. Raw sizes and
offsets are converted to instruction targets against the text's own layout (a block
always starts 4-aligned), so an instruction boundary is required.

Operands (all as apt2txt prints them):
  Push / ConstantPool   [v, v, ...]   v = 'string' | b'string' | regN | undefined | true |
                        false | lookupN | int | float | typeT:0xV (raw item) | $ALIAS
  pool-index ops        EA_PushConstByte? / EA_PushConstWord / EA_PushValueOfVar /
  (A2 A3 AE AF B0-B3)   EA_GetNamedMember / EA_CallNamed*: `pool[N]` (optionally
                        `pool[N]='name'`, checked against the pool in effect) or just
                        'name' (symbolic: resolved in the pool in effect, appended to it
                        when missing). The pool in effect is the most recent ConstantPool
                        in linear order, as in apt_asm.fmt; a snippet spliced into an
                        existing block uses that block's pool (retail: <= 1 pool per
                        block, always its first instruction, 598/598 blocks).
  strings               'text' | b'text' | "text" | null
  GetURL                'url', 'target'   (or apt2txt's [b'url', b'target'])
  EA_PushByte/Short/Long  integer (negative = two's complement); EA_PushFloat 0.5 | bits=0xHEX
  Try                   raw='<40 hex digits>' catch='name'|null   (no retail use; sizes raw)

Two ways to turn parsed text into graph instructions (apt_graph.Ins):
  materialize_new()  fresh blocks + fresh .const items for every operand. Retail rule
                     (census 2026-09-30): every .const item is referenced by exactly one
                     Push/ConstantPool array element (19,812/19,812) and every index array
                     is private, so nothing is ever shared. An empty DefineFunction(2)
                     params array keeps a NON-null pointer, as in retail (926/926), aimed
                     at the function's own name string.
  bind()             reuses the operand storage (strings, index arrays, .const items,
                     params arrays) of an existing block and overwrites it with the
                     assembled values; tools/apt/rt_asm.py uses it to prove that
                     disassemble -> assemble is byte-exact for every block.

usage (repo root):
  python tools/apt/aptasm.py dis SCRPRESSSTART              # every bytecode block
  python tools/apt/aptasm.py dis SCRPRESSSTART --block 0x41e4
  python tools/apt/aptasm.py check my.aptasm [--movie SCRPRESSSTART]   # assemble into a
        scratch copy of the movie (new block), re-parse, print the re-disassembly
"""
import ast
import os
import re
import struct
import sys

try:
    from .apt_model import Apt, coverage
    from .apt_asm import NAMES
    from .apt_graph import Block, Ins, from_apt, positions, BRANCH, PLAIN_I32, STRPTR1
    from .bgfa import movie as bgfa_movie, DEFAULT_UI
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from apt_model import Apt, coverage
    from apt_asm import NAMES
    from apt_graph import Block, Ins, from_apt, positions, BRANCH, PLAIN_I32, STRPTR1
    from bgfa import movie as bgfa_movie, DEFAULT_UI

M1, M2 = 0x98765432, 0x12345678            # DefineFunction(2) trailer words: 1551/1551 retail
POOL_OPS = {0xA2: 1, 0xA3: 2, 0xAE: 1, 0xAF: 1, 0xB0: 1, 0xB1: 1, 0xB2: 1, 0xB3: 1}  # -> index width
LIT_OPS = {0xB4, 0xB5, 0xB6, 0xB7}
WITH = 0x94
TERMINATORS = {0x3E, 0x99, 0x2A}           # Return, Jump, Throw: nothing falls through them

MNEMONIC = {}
for _op, _n in NAMES.items():
    MNEMONIC[_n.lower()] = _op
for _op in range(256):
    MNEMONIC[f"op_{_op:02x}"] = _op
# input-only aliases (IDA-verified handler semantics; output keeps apt_asm.NAMES)
MNEMONIC["ea_pushconstbyte"] = 0xA2          # NAMES spells it 'EA_PushConstByte?'
MNEMONIC["ea_pushzero"] = 0x59               # sub_826965A8: AptAS2AllocIntObj(0)
MNEMONIC["ea_pushone"] = 0x5A                # sub_82696608: AptAS2AllocIntObj(1)


class AsmError(Exception):
    pass


class A:
    """value-level instruction: operands are values (bytes, ints, item tuples,
    instruction indices), not block keys."""
    __slots__ = ("op", "f", "line")

    def __init__(self, op, line=0, **f):
        self.op = op
        self.f = f
        self.line = line

    def key(self):
        return (self.op, tuple(sorted((k, _freeze(v)) for k, v in self.f.items() if not k.startswith("_"))))

    def __repr__(self):
        return f"A({self.op:#04x}, {self.f})"


def _freeze(v):
    if isinstance(v, list):
        return tuple(_freeze(x) for x in v)
    if isinstance(v, tuple):
        return tuple(_freeze(x) for x in v)
    if isinstance(v, bytearray):
        return bytes(v)
    return v


# ======================================================================== graph -> values
def gstr(g, k):
    if k is None:
        return None
    d = bytes(g.blocks[k].data)
    return d[:d.index(b"\0")] if b"\0" in d else d


def item_value(it):
    """graph .const item -> value item (t, v); strings as bytes, a null string as (1, None)."""
    t, v = it
    if t == 1:
        return (1, v[1]) if isinstance(v, tuple) else (1, None)
    return (t, v)


def graph_item(val):
    t, v = val
    if t == 1:
        return (1, ("str", v)) if v is not None else (1, 0)
    return (t, v)


def pool_values(g, ins):
    """a Push / ConstantPool Ins of graph g -> its value items."""
    if ins.f["arr"] is None:
        return []
    data = g.blocks[ins.f["arr"]].data
    return [item_value(g.items[struct.unpack_from(">I", data, 4 * q)[0]]) for q in range(ins.f["n"])]


def lower(g, key):
    """the bytecode block `key` of graph g -> list[A]."""
    out = []
    for ins in g.blocks[key].code:
        op, f = ins.op, ins.f
        a = A(op)
        if op in BRANCH or op == WITH:
            a.f["tgt"] = f["tgt"]
        elif op in PLAIN_I32:
            a.f["i32"] = f["i32"]
        elif op in STRPTR1:
            a.f["s"] = gstr(g, f["s"])
        elif op == 0x83:
            a.f["s"] = [gstr(g, x) for x in f["s"]]
        elif op in (0x88, 0x96):
            a.f["vals"], a.f["arrnull"] = pool_values(g, ins), f["arr"] is None
        elif op == 0x8E:
            a.f.update(name=gstr(g, f["name"]), flags=f["flags"], end=f["end"], m1=f["m1"], m2=f["m2"])
            if f["params"] is None:
                a.f["params"] = None
            else:
                pb = g.blocks[f["params"]]
                a.f["params"] = [(struct.unpack_from(">I", pb.data, 8 * q)[0], gstr(g, pb.ptrs.get(8 * q + 4)))
                                 for q in range(f["np"])]
        elif op == 0x9B:
            a.f.update(name=gstr(g, f["name"]), end=f["end"], m1=f["m1"], m2=f["m2"])
            if f["params"] is None:
                a.f["params"] = None
            else:
                pb = g.blocks[f["params"]]
                a.f["params"] = [gstr(g, pb.ptrs.get(4 * q)) for q in range(f["np"])]
        elif op == 0x8F:
            a.f["raw"] = bytes(f["raw"])
            a.f["catch"] = gstr(g, f.get("catch"))
        elif op in POOL_OPS:
            a.f["pidx"] = int.from_bytes(f["imm"], "big")
        elif op in LIT_OPS:
            a.f["imm"] = bytes(f["imm"])
        out.append(a)
    return out


# ======================================================================== values -> text
def qs(b):
    return "null" if b is None else repr(b.decode("latin1"))


def fmt_item(val):
    t, v = val
    if t == 1:
        return qs(v) if v is not None else "type1:0x0"
    if t == 4:
        return f"reg{v}"
    if t == 3 and v == 0:
        return "undefined"
    if t == 5 and v in (0, 1):
        return "true" if v else "false"
    if t == 6:
        fv = struct.unpack(">f", struct.pack(">I", v))[0]
        s = repr(fv)
        if ("." in s or "e" in s or "inf" in s) and struct.pack(">f", float(s)) == struct.pack(">I", v):
            return s
    if t == 7:
        return str(v - (1 << 32) if v & 0x80000000 else v)
    if t == 8:
        return f"lookup{v}"
    return f"type{t}:{v:#x}"


def mnemonic(op):
    return NAMES.get(op, f"op_{op:02X}")


def render(alist, pool=None, labels_prefix="L", indent="    "):
    """list[A] -> canonical .aptasm text (labels, .func/.end, pool names resolved).
    `pool` = the pool in effect before the first instruction (snippets)."""
    targets = sorted({a.f["tgt"] for a in alist if "tgt" in a.f})
    lab = {t: f"{labels_prefix}{i + 1}" for i, t in enumerate(targets)}
    ends = []                       # stack of open function end indices
    lines = []
    cur_pool = pool

    def flush(idx):
        while ends and ends[-1] == idx:
            ends.pop()
            lines.append(indent * (len(ends) + 1) + ".end")

    for idx, a in enumerate(alist):
        flush(idx)
        if idx in lab:
            lines.append(f"{lab[idx]}:")
        ind = indent * (len(ends) + 1)
        op, f = a.op, a.f
        if op == 0x88:
            cur_pool = f["vals"]
        if op in (0x8E, 0x9B):
            if op == 0x8E:
                ps = "null" if f["params"] is None else "(" + ", ".join(f"r{r} {qs(n)}" for r, n in f["params"]) + ")"
                s = f".func2 {qs(f['name'])}{ps} flags={f['flags']:#x}"
            else:
                ps = "null" if f["params"] is None else "(" + ", ".join(qs(n) for n in f["params"]) + ")"
                s = f".func {qs(f['name'])}{ps}"
            if (f["m1"], f["m2"]) != (M1, M2):
                s += f" m={f['m1']:#x},{f['m2']:#x}"
            lines.append(ind + s)
            if f["end"] <= idx:
                raise AsmError(f"function at {idx} ends at {f['end']} (not after itself)")
            if ends and f["end"] > ends[-1]:
                raise AsmError(f"function at {idx} is not nested in its parent")
            ends.append(f["end"])
            continue
        s = mnemonic(op)
        if op in BRANCH or op == WITH:
            s += " " + lab[f["tgt"]]
        elif op in PLAIN_I32:
            s += f" {f['i32']}"
        elif op in STRPTR1:
            s += " " + qs(f["s"])
        elif op == 0x83:
            s += " " + ", ".join(qs(x) for x in f["s"])
        elif op in (0x88, 0x96):
            s += " null" if f["arrnull"] else " [" + ", ".join(fmt_item(v) for v in f["vals"]) + "]"
        elif op == 0x8F:
            s += f" raw='{f['raw'].hex()}' catch={qs(f['catch'])}"
        elif op in POOL_OPS:
            v = f["pidx"]
            s += f" pool[{v}]"
            if cur_pool is not None and v < len(cur_pool):
                s += "=" + fmt_item(cur_pool[v])
        elif op == 0xB4:
            fv = struct.unpack(">f", f["imm"])[0]
            if struct.pack(">f", float(repr(fv))) == f["imm"]:
                s += f" {fv!r}"
            else:
                s += f" bits={int.from_bytes(f['imm'], 'big'):#010x}"
        elif op in LIT_OPS:
            s += f" {int.from_bytes(f['imm'], 'big')}"
        lines.append(ind + s)
    flush(len(alist))
    if len(alist) in lab:
        lines.append(f"{lab[len(alist)]}:")
    if ends:
        raise AsmError("unclosed function at end of block")
    return "\n".join(lines) + "\n"


def disassemble(g, key, header=None):
    txt = render(lower(g, key))
    return (f".block {header}\n" if header else "") + txt


def block_owners(g):
    """bytecode key -> owner description, from the item / clip-event blocks."""
    own = {}
    for k in g.order:
        b = g.blocks[k]
        if b.code is not None or b.data is None:
            continue
        if b.kind == "item.Action" and 4 in b.ptrs:
            own.setdefault(b.ptrs[4], f"action item={_kdesc(k)}")
        elif b.kind == "item.InitAction" and 8 in b.ptrs:
            sid = struct.unpack_from(">i", b.data, 4)[0]
            own.setdefault(b.ptrs[8], f"initaction item={_kdesc(k)} sprite={sid}")
        elif b.kind == "po.clipaction.recs":
            for rel, t in b.ptrs.items():
                if rel % 12 == 8:
                    flags, key = struct.unpack_from(">II", b.data, rel - 8)
                    own.setdefault(t, f"clipevent rec={_kdesc(k)}+{rel - 8:#x} flags={flags:#010x} key={key:#x}")
    return own


def _kdesc(k):
    return f"{k:#x}" if isinstance(k, int) else f"new{k[1]}"


# ======================================================================== text -> values
_TOK = re.compile(r"""
    (?P<ws>\s+)
  | (?P<str>[bB]?(?:'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*"))
  | (?P<num>-?(?:0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+|\d+|inf\b))
  | (?P<id>[A-Za-z_$][A-Za-z0-9_$?]*)
  | (?P<p>[\[\](),=:])
""", re.X)


def _strip_comment(line):
    out, q = [], None
    i = 0
    while i < len(line):
        ch = line[i]
        if q:
            out.append(ch)
            if ch == "\\" and i + 1 < len(line):
                out.append(line[i + 1]); i += 2; continue
            if ch == q:
                q = None
        elif ch in "'\"":
            q = ch; out.append(ch)
        elif ch == ";":
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def lex(s, lineno):
    toks = []
    pos = 0
    while pos < len(s):
        m = _TOK.match(s, pos)
        if not m:
            raise AsmError(f"line {lineno}: cannot read {s[pos:pos + 20]!r}")
        pos = m.end()
        kind = m.lastgroup
        if kind == "ws":
            continue
        text = m.group(kind)
        if kind == "str":
            v = ast.literal_eval(text)
            if isinstance(v, str):
                v = v.encode("latin1")
            toks.append(("str", v))
        elif kind == "num":
            if re.fullmatch(r"-?0[xX][0-9a-fA-F]+|-?\d+", text):
                toks.append(("int", int(text, 0)))
            else:
                toks.append(("float", float(text)))
        elif kind == "id":
            toks.append(("id", text))
        else:
            toks.append(("p", text))
    return toks


class Cur:
    def __init__(self, toks, lineno, aliases):
        self.t, self.i, self.ln, self.al = toks, 0, lineno, aliases

    def err(self, msg):
        return AsmError(f"line {self.ln}: {msg}")

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def take(self):
        tok = self.peek()
        if tok[0] is None:
            raise self.err("unexpected end of line")
        self.i += 1
        return tok

    def accept(self, kind, val=None):
        tk = self.peek()
        if tk[0] == kind and (val is None or tk[1] == val):
            self.i += 1
            return True
        return False

    def expect(self, kind, val=None):
        tk = self.take()
        if tk[0] != kind or (val is not None and tk[1] != val):
            raise self.err(f"expected {val or kind}, got {tk[1]!r}")
        return tk[1]

    def done(self):
        if self.i != len(self.t):
            raise self.err(f"unexpected {self.t[self.i][1]!r}")

    def alias(self, name):
        if name not in self.al:
            raise self.err(f"unknown alias {name}")
        return self.al[name]

    # ---- operand forms
    def value(self):
        """-> value item (t, v)."""
        k, v = self.take()
        if k == "str":
            return (1, v)
        if k == "int":
            return (7, v & 0xFFFFFFFF)
        if k == "float":
            return (6, struct.unpack(">I", struct.pack(">f", v))[0])
        if k == "id":
            if v.startswith("$"):
                a = self.alias(v)
                if isinstance(a, tuple):
                    return a
                if isinstance(a, bytes):
                    return (1, a)
                if isinstance(a, int):
                    return (7, a & 0xFFFFFFFF)
                raise self.err(f"alias {v} is not a value")
            m = re.fullmatch(r"reg(\d+)", v)
            if m:
                return (4, int(m.group(1)))
            m = re.fullmatch(r"lookup(\d+)", v)
            if m:
                return (8, int(m.group(1)))
            if v == "undefined":
                return (3, 0)
            if v in ("true", "false"):
                return (5, 1 if v == "true" else 0)
            if v in ("inf",):
                return (6, struct.unpack(">I", struct.pack(">f", float("inf")))[0])
            m = re.fullmatch(r"type(\d+)", v)
            if m and self.accept("p", ":"):
                raw = self.expect("int")
                t = int(m.group(1))
                return (1, None) if (t == 1 and raw == 0) else (t, raw)
        raise self.err(f"bad value {v!r}")

    def value_list(self):
        """[v, ...] | v, v | null -> (vals, arrnull)."""
        if self.peek() == ("id", "null") or self.peek() == ("id", "None"):
            self.take()
            return [], True
        vals = []
        br = self.accept("p", "[")
        if br and self.accept("p", "]"):
            return [], False
        while True:
            vals.append(self.value())
            if not self.accept("p", ","):
                break
        if br:
            self.expect("p", "]")
        return vals, False

    def string(self):
        k, v = self.take()
        if k == "str":
            return v
        if k == "id" and v in ("null", "None"):
            return None
        if k == "id" and v.startswith("$"):
            a = self.alias(v)
            if isinstance(a, tuple) and a[0] == 1:
                return a[1]
            if isinstance(a, bytes):
                return a
        raise self.err(f"expected a string, got {v!r}")

    def integer(self):
        k, v = self.take()
        if k == "int":
            return v
        if k == "id" and v.startswith("$"):
            a = self.alias(v)
            if isinstance(a, int):
                return a
            if isinstance(a, tuple) and a[0] in (4, 7):
                return a[1]
        raise self.err(f"expected an integer, got {v!r}")


def _params2(c):
    """(r3 'a', r5 null) | null  -> list[(reg, bytes|None)] | None."""
    if c.accept("id", "null") or c.accept("id", "None"):
        return None
    c.expect("p", "(")
    out = []
    if c.accept("p", ")"):
        return out
    while True:
        k, v = c.take()
        m = re.fullmatch(r"r(?:eg)?(\d+)", v) if k == "id" else None
        if not m:
            raise c.err(f"expected rN, got {v!r}")
        out.append((int(m.group(1)), c.string()))
        if c.accept("p", ")"):
            return out
        c.expect("p", ",")


def _params1(c):
    if c.accept("id", "null") or c.accept("id", "None"):
        return None
    c.expect("p", "(")
    out = []
    if c.accept("p", ")"):
        return out
    while True:
        out.append(c.string())
        if c.accept("p", ")"):
            return out
        c.expect("p", ",")


def _kv_opts(c):
    """flags=N m=A,B  -> dict."""
    out = {}
    while c.peek()[0] is not None:
        key = c.expect("id")
        c.expect("p", "=")
        if key == "flags":
            out["flags"] = c.integer()
        elif key == "m":
            out["m1"] = c.integer(); c.expect("p", ","); out["m2"] = c.integer()
        else:
            raise c.err(f"unknown option {key}")
    return out


def _raw_list(c):
    """apt2txt python-literal lists: [(3, b'x'), ...] / [b'x', None] / [1, 2]."""
    c.expect("p", "[")
    out = []
    if c.accept("p", "]"):
        return out
    while True:
        if c.accept("p", "("):
            reg = c.integer(); c.expect("p", ","); nm = c.string(); c.expect("p", ")")
            out.append((reg, nm))
        elif c.peek()[0] == "int":
            out.append(c.integer())
        else:
            out.append(c.string())
        if c.accept("p", "]"):
            return out
        c.expect("p", ",")


_ADDR = re.compile(r"^\s*[0-9a-fA-F]{6}\s{2,}(?=\S)")
_LABEL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:(?!:)\s*(.*)$")


class Parsed:
    def __init__(self):
        self.ins = []            # list[A]
        self.labels = {}         # name -> index
        self.pool_appended = []  # value items appended to an EXTERNAL (context) pool
        self.auto_pool = False   # a ConstantPool was created at index 0


def parse(text, mode="block", pool=None, auto_end=False, aliases=None):
    """text -> Parsed (value level, every operand resolved).
    mode 'block'  : a complete action; must end with End (auto_end appends one).
    mode 'snippet': code to splice into an existing block: no End, no ConstantPool;
                    pool-index operands resolve against `pool` (a list of value items,
                    the host block's ConstantPool) and missing names are appended to it
                    (reported in Parsed.pool_appended)."""
    P = Parsed()
    al = dict(aliases or {})
    fstack = []                  # indices of open .func/.func2
    pending_label_refs = []      # (insIndex, labelName)
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw)
        line = _ADDR.sub("", line, count=1)
        if not line.strip():
            continue
        m = _LABEL.match(line)
        if m and not m.group(1).lower() in MNEMONIC:
            name = m.group(1)
            if name in P.labels:
                raise AsmError(f"line {lineno}: label {name} defined twice")
            P.labels[name] = len(P.ins)
            line = m.group(2)
            if not line.strip():
                continue
        s = line.strip()
        if s.startswith("."):
            dname, _, rest = s.partition(" ")
            c = Cur(lex(rest, lineno), lineno, al)
            d = dname.lower()
            if d == ".block":
                continue
            if d == ".const":
                nm = c.expect("id")
                if not nm.startswith("$"):
                    nm = "$" + nm
                c.expect("p", "=")
                k, v = c.peek()
                if k == "id" and v.startswith("$"):
                    c.take(); val = c.alias(v)
                elif k == "int" and c.i + 1 == len(c.t):
                    c.take(); val = v
                else:
                    val = c.value()
                c.done()
                al[nm] = val
                continue
            if d == ".pool":
                vals, _ = c.value_list() if c.peek()[0] is not None else ([], False)
                c.done()
                P.ins.append(A(0x88, lineno, vals=vals, arrnull=False))
                continue
            if d in (".func2", ".func"):
                name = c.string()
                if d == ".func2":
                    params = _params2(c)
                    opts = _kv_opts(c)
                    if "flags" not in opts:
                        raise AsmError(f"line {lineno}: .func2 needs flags= (register count << 16 | preload flags)")
                    a = A(0x8E, lineno, name=name, flags=opts["flags"], params=params,
                          m1=opts.get("m1", M1), m2=opts.get("m2", M2), end=None)
                else:
                    params = _params1(c)
                    opts = _kv_opts(c)
                    if "flags" in opts:
                        raise AsmError(f"line {lineno}: .func (DefineFunction) has no flags")
                    a = A(0x9B, lineno, name=name, params=params,
                          m1=opts.get("m1", M1), m2=opts.get("m2", M2), end=None)
                fstack.append(len(P.ins))
                P.ins.append(a)
                continue
            if d == ".end":
                c.done()
                if not fstack:
                    raise AsmError(f"line {lineno}: .end without .func")
                P.ins[fstack.pop()].f["end"] = len(P.ins)
                continue
            raise AsmError(f"line {lineno}: unknown directive {dname}")
        mn, _, rest = s.partition(" ")
        op = MNEMONIC.get(mn.lower())
        if op is None:
            raise AsmError(f"line {lineno}: unknown mnemonic {mn!r}")
        c = Cur(lex(rest, lineno), lineno, al)
        a = A(op, lineno)
        if op in BRANCH or op == WITH:
            k, v = c.take()
            if k == "id":
                pending_label_refs.append((len(P.ins), v))
            elif k == "int":
                a.f["_rel"] = v
            else:
                raise c.err("branch needs a label or a byte offset")
        elif op in PLAIN_I32:
            a.f["i32"] = c.integer()
        elif op in STRPTR1:
            a.f["s"] = c.string()
        elif op == 0x83:
            if c.accept("p", "["):
                x = c.string(); c.expect("p", ","); y = c.string(); c.expect("p", "]")
            else:
                x = c.string(); c.expect("p", ","); y = c.string()
            a.f["s"] = [x, y]
        elif op in (0x88, 0x96):
            a.f["vals"], a.f["arrnull"] = c.value_list()
        elif op in (0x8E, 0x9B):
            kv = {}
            while c.peek()[0] is not None:
                key = c.expect("id"); c.expect("p", "=")
                kv[key] = c.string() if key == "name" else _raw_list(c)
            if "w" not in kv:
                raise c.err("DefineFunction needs w=[...] (or use .func/.func2 ... .end)")
            w = kv["w"]
            params = kv.get("params", [])
            if op == 0x8E:
                if len(w) != 4:
                    raise c.err("DefineFunction2 w=[flags, bodySize, m1, m2]")
                a.f.update(name=kv.get("name"), flags=w[0], params=[tuple(p) for p in params],
                           m1=w[2], m2=w[3], end=None, _body=w[1])
            else:
                if len(w) != 3:
                    raise c.err("DefineFunction w=[bodySize, m1, m2]")
                a.f.update(name=kv.get("name"), params=list(params), m1=w[1], m2=w[2], end=None, _body=w[0])
        elif op == 0x8F:
            kv = {}
            while c.peek()[0] is not None:
                key = c.expect("id"); c.expect("p", "=")
                kv[key] = c.string()
            if "raw" not in kv or len(kv["raw"]) != 40:
                raise c.err("Try needs raw='<40 hex digits>'")
            a.f["raw"] = bytes.fromhex(kv["raw"].decode())
            a.f["catch"] = kv.get("catch")
        elif op in POOL_OPS:
            if c.accept("id", "pool"):
                c.expect("p", "[")
                a.f["pidx"] = c.integer()
                c.expect("p", "]")
                if c.accept("p", "="):
                    a.f["_check"] = c.value()
            else:
                k, v = c.peek()
                if k == "int":
                    a.f["pidx"] = c.integer()
                else:
                    a.f["_name"] = c.string()
                    if a.f["_name"] is None:
                        raise c.err("pool name cannot be null")
        elif op == 0xB4:
            if c.accept("id", "bits"):
                c.expect("p", "=")
                a.f["imm"] = (c.integer() & 0xFFFFFFFF).to_bytes(4, "big")
            else:
                k, v = c.take()
                if k not in ("int", "float") and not (k == "id" and v == "inf"):
                    raise c.err("EA_PushFloat needs a number")
                a.f["imm"] = struct.pack(">f", float(v))
        elif op in LIT_OPS:
            v = c.integer()
            width = {0xB5: 1, 0xB6: 2, 0xB7: 4}[op]
            if not -(1 << (8 * width - 1)) <= v < (1 << (8 * width)):
                raise c.err(f"{mn} operand {v} does not fit {width} byte(s)")
            a.f["imm"] = (v & ((1 << (8 * width)) - 1)).to_bytes(width, "big")
        c.done()
        P.ins.append(a)
    if fstack:
        raise AsmError(f"line {P.ins[fstack[-1]].line}: .func without .end")
    ins = P.ins
    # ---- structure checks
    if mode == "block":
        if auto_end and (not ins or ins[-1].op != 0):
            ins.append(A(0, 0))
        if not ins or ins[-1].op != 0:
            raise AsmError("a block must end with End")
        if any(a.op == 0 for a in ins[:-1]):
            raise AsmError("End before the last instruction (the walker stops at the first End)")
    elif mode == "snippet":
        for a in ins:
            if a.op == 0:
                raise AsmError(f"line {a.line}: End in a snippet")
            if a.op == 0x88:
                raise AsmError(f"line {a.line}: ConstantPool in a snippet would replace the host block's pool")
    else:
        raise ValueError(mode)
    # ---- auto ConstantPool for a new block that names pool entries but has none
    if mode == "block" and not any(a.op == 0x88 for a in ins) and any("_name" in a.f for a in ins):
        ins.insert(0, A(0x88, 0, vals=[], arrnull=False))
        P.auto_pool = True
        for n in P.labels:
            P.labels[n] += 1
        pending_label_refs = [(i + 1, n) for i, n in pending_label_refs]
        for a in ins:
            if a.op in (0x8E, 0x9B) and a.f.get("end") is not None:
                a.f["end"] += 1
    # ---- labels
    for i, name in pending_label_refs:
        if name not in P.labels:
            raise AsmError(f"line {ins[i].line}: undefined label {name}")
        ins[i].f["tgt"] = P.labels[name]
    # ---- raw (apt2txt) byte offsets -> instruction indices
    if any("_rel" in a.f or "_body" in a.f for a in ins):
        pos = positions([a.op for a in ins])
        at = {p: i for i, p in enumerate(pos)}
        for i, a in enumerate(ins):
            for fld, dst in (("_rel", "tgt"), ("_body", "end")):
                if fld in a.f:
                    t = pos[i + 1] + a.f.pop(fld)
                    if t not in at:
                        raise AsmError(f"line {a.line}: offset lands inside an instruction")
                    a.f[dst] = at[t]
    # ---- pool-index operands
    cur = pool
    ext = pool
    for a in ins:
        if a.op == 0x88:
            cur = a.f["vals"]
            continue
        if a.op not in POOL_OPS:
            continue
        width = POOL_OPS[a.op]
        if "_name" in a.f:
            if cur is None:
                raise AsmError(f"line {a.line}: pool name {a.f['_name']!r} used before any ConstantPool")
            want = (1, a.f.pop("_name"))
            if want in cur:
                a.f["pidx"] = cur.index(want)
            else:
                cur.append(want)
                a.f["pidx"] = len(cur) - 1
                if cur is ext:
                    P.pool_appended.append(want)
        elif "_check" in a.f:
            chk = a.f.pop("_check")
            if cur is not None:
                if a.f["pidx"] >= len(cur) or cur[a.f["pidx"]] != chk:
                    raise AsmError(f"line {a.line}: pool[{a.f['pidx']}] is not {fmt_item(chk)}")
        if a.f["pidx"] >= (1 << (8 * width)):
            raise AsmError(f"line {a.line}: pool index {a.f['pidx']} does not fit {mnemonic(a.op)} "
                           f"({width} byte); use EA_PushConstWord (0xA3) or a smaller pool")
    for i, a in enumerate(ins):
        if a.op in (0x8E, 0x9B) and (a.f["end"] is None or a.f["end"] <= i):
            raise AsmError(f"line {a.line}: bad function body end")
    return P


# ======================================================================== values -> graph
def _new_str(g, s, kind):
    return None if s is None else g.add_block(Block(kind, s + b"\0"))


def _new_arr(g, vals):
    idx = []
    for v in vals:
        g.items.append(graph_item(v))
        idx.append(len(g.items) - 1)
    return g.add_block(Block("code.constidx", b"".join(struct.pack(">I", i) for i in idx)))


def materialize_new(alist, g, host_key):
    """list[A] -> list[Ins] with FRESH blocks / .const items appended to g.
    host_key = the bytecode block the code will live in (an empty params array points
    at the function name, or at the host block when the name is null)."""
    out = []
    for a in alist:
        op, f = a.op, a.f
        n = {}
        if op in BRANCH or op == WITH:
            n["tgt"] = f["tgt"]
        elif op in PLAIN_I32:
            n["i32"] = f["i32"]
        elif op in STRPTR1:
            n["s"] = _new_str(g, f["s"], "code.str")
        elif op == 0x83:
            n["s"] = [_new_str(g, x, "code.str") for x in f["s"]]
        elif op in (0x88, 0x96):
            if f["arrnull"] or not f["vals"]:
                n.update(n=0, arr=None)
            else:
                n.update(n=len(f["vals"]), arr=_new_arr(g, f["vals"]))
        elif op == 0x8E:
            nk = _new_str(g, f["name"], "code.fname")
            if f["params"] is None:
                pk = None
            elif not f["params"]:
                pk = nk if nk is not None else host_key
            else:
                blk = Block("code.f2params", bytes(8 * len(f["params"])))
                for q, (reg, pname) in enumerate(f["params"]):
                    struct.pack_into(">I", blk.data, 8 * q, reg)
                    if pname is not None:
                        blk.ptrs[8 * q + 4] = _new_str(g, pname, "code.pname")
                pk = g.add_block(blk)
            n.update(name=nk, np=len(f["params"] or []), flags=f["flags"], params=pk,
                     end=f["end"], m1=f["m1"], m2=f["m2"])
        elif op == 0x9B:
            nk = _new_str(g, f["name"], "code.fname")
            if f["params"] is None:
                pk = None
            elif not f["params"]:
                pk = nk if nk is not None else host_key
            else:
                blk = Block("code.fparams", bytes(4 * len(f["params"])))
                for q, pname in enumerate(f["params"]):
                    if pname is not None:
                        blk.ptrs[4 * q] = _new_str(g, pname, "code.pname")
                pk = g.add_block(blk)
            n.update(name=nk, np=len(f["params"] or []), params=pk, end=f["end"], m1=f["m1"], m2=f["m2"])
        elif op == 0x8F:
            n["raw"] = f["raw"]
            n["catch"] = _new_str(g, f["catch"], "code.catchname") if not (f["raw"][12] & 4) else None
        elif op in POOL_OPS:
            n["imm"] = f["pidx"].to_bytes(POOL_OPS[op], "big")
        elif op in LIT_OPS:
            n["imm"] = f["imm"]
        out.append(Ins(op, **n))
    return out


def bind(alist, g, key):
    """Re-assemble block `key` IN PLACE: pair the parsed instructions 1:1 with the
    block's current ones, write every assembled operand value into the block's own
    operand storage (strings, index arrays + their .const items, params arrays) and
    return the new Ins list. A difference in count, opcode or array length raises; a
    wrong value is written through and shows up as a byte difference at emit."""
    orig = g.blocks[key].code
    if len(alist) != len(orig):
        raise AsmError(f"bind: {len(alist)} instructions vs {len(orig)} in the block")
    written = {}

    def put(k, s):
        if s is None:
            return None
        if k is None:
            raise AsmError("bind: a null string pointer got a value")
        data = s + b"\0"
        if written.get(k, data) != data:
            raise AsmError(f"bind: shared string block {_kdesc(k)} gets two values")
        written[k] = data
        g.blocks[k].data = bytearray(data)
        return k

    out = []
    for a, o in zip(alist, orig):
        if a.op != o.op:
            raise AsmError(f"bind: opcode {a.op:#04x} vs {o.op:#04x}")
        op, f, of = a.op, a.f, o.f
        n = {}
        if op in BRANCH or op == WITH:
            n["tgt"] = f["tgt"]
        elif op in PLAIN_I32:
            n["i32"] = f["i32"]
        elif op in STRPTR1:
            n["s"] = put(of["s"], f["s"])
        elif op == 0x83:
            n["s"] = [put(k, s) for k, s in zip(of["s"], f["s"])]
        elif op in (0x88, 0x96):
            if f["arrnull"]:
                n.update(n=0, arr=None)
            else:
                if of["arr"] is None or of["n"] != len(f["vals"]):
                    raise AsmError("bind: index array length differs")
                data = g.blocks[of["arr"]].data
                for q, v in enumerate(f["vals"]):
                    g.items[struct.unpack_from(">I", data, 4 * q)[0]] = graph_item(v)
                n.update(n=of["n"], arr=of["arr"])
        elif op in (0x8E, 0x9B):
            nk = put(of["name"], f["name"])
            stride, roff = (8, 4) if op == 0x8E else (4, 0)
            if f["params"] is None:
                pk = None
            elif not f["params"]:
                pk = of["params"] if of["params"] is not None else (nk if nk is not None else key)
            else:
                if of["params"] is None or of["np"] != len(f["params"]):
                    raise AsmError("bind: params array length differs")
                pk = of["params"]
                pb = g.blocks[pk]
                for q, p in enumerate(f["params"]):
                    reg, pname = p if op == 0x8E else (None, p)
                    if op == 0x8E:
                        struct.pack_into(">I", pb.data, 8 * q, reg)
                    if pname is None:
                        pb.ptrs.pop(stride * q + roff, None)
                    else:
                        put(pb.ptrs.get(stride * q + roff), pname)
            n.update(name=nk, np=len(f["params"] or []), params=pk, end=f["end"], m1=f["m1"], m2=f["m2"])
            if op == 0x8E:
                n["flags"] = f["flags"]
        elif op == 0x8F:
            n["raw"] = f["raw"]
            n["catch"] = put(of.get("catch"), f["catch"])
        elif op in POOL_OPS:
            n["imm"] = f["pidx"].to_bytes(POOL_OPS[op], "big")
        elif op in LIT_OPS:
            n["imm"] = f["imm"]
        out.append(Ins(op, **n))
    return out


def split_blocks(text):
    """multi-block .aptasm (dis output) -> [(headerText, blockText)]."""
    out, head, cur = [], None, []
    for line in text.splitlines():
        if line.strip().lower().startswith(".block"):
            if cur or head is not None:
                out.append((head, "\n".join(cur) + "\n"))
            head, cur = line.strip()[6:].strip(), []
        else:
            cur.append(line)
    if cur and any(x.strip() for x in cur):
        out.append((head, "\n".join(cur) + "\n"))
    return out


# ======================================================================== CLI
def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Apt bytecode assembler / canonical disassembler.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dis", help="disassemble a movie's bytecode blocks to .aptasm")
    d.add_argument("movie")
    d.add_argument("--block", help="original offset of one bytecode block (hex)")
    d.add_argument("--ui", default=DEFAULT_UI)
    k = sub.add_parser("check", help="parse a single-block .aptasm and print its canonical form")
    k.add_argument("file")
    args = ap.parse_args(argv)
    if args.cmd == "dis":
        _, apt, cst = bgfa_movie(args.movie, args.ui)
        g = from_apt(Apt(apt, cst))
        own = block_owners(g)
        keys = [kk for kk in g.order if g.blocks[kk].code is not None]
        if args.block:
            keys = [int(args.block, 16)]
        for kk in keys:
            sys.stdout.write(disassemble(g, kk, header=f"{own.get(kk, '?')} bytecode={_kdesc(kk)}"))
            sys.stdout.write("\n")
        return 0
    P = parse(open(args.file, encoding="utf-8").read(), mode="block", auto_end=True)
    sys.stdout.write(render(P.ins))
    print(f"; {len(P.ins)} instructions, auto pool: {P.auto_pool}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
