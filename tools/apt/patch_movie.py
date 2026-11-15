#!/usr/bin/env python3
"""patch_movie.py - apply a patch spec to one Apt movie, re-serialise, verify by
re-parsing (CUSTOM_FRONTEND.md section 7.3, step 3).

PATCH FILE (line based; '#' or ';' at the start of a line = comment):

  movie SCRPRESSSTART                     select the movie by its ui.AST TOC name;
                                          every following op applies to it (a file may
                                          hold several movie sections)
  replace-const 'OLD' -> 'NEW' [count=N]  change every .const string item equal to OLD
                                          (count= asserts how many there must be)
  prepend-to function NAME [in CTX] [nth=K]   code runs first when the function runs
  append-to  function NAME [in CTX] [nth=K]   code runs where the function falls off
                                          its end (branches to the body end inside the
                                          body reach it too; an early Return skips it)
  append-to function NAME [in CTX] after-call=METHOD
                                          insert immediately after the unique own-level
                                          PushConst METHOD / CallMethod pair. Existing
                                          branches keep their original targets. This
                                          can extend a returned array without wrapping
                                          a global native method or replacing its body.
  prepend-to action SEL                   code runs at the start of an action block
                                          (after its ConstantPool)
  append-to  action SEL                   code runs before the block's End
  add-initaction [sprite=auto|N] [char=C] [frame=F] [position=last|first]
                                          a new InitAction frame item {8, spriteId,
                                          bytecode*} with its own code block
  add-doaction   [char=C] [frame=F] [position=last|first]
                                          a new DoAction frame item {1, bytecode*}
  clone-place NAME as NEW char=C [frame=F] depth=D [x=X] [y=Y] [tabindex=N] [with-char=K]
                                          (iter-464) a copy of the PlaceObject named NAME
                                          (a component instance such as a BasicButton, with
                                          its clip-action parameters), placed right after it
                                          in the frame, at a new depth and position; the copy
                                          owns copies of its name, clip-action records, their
                                          bytecode and every string they point at;
                                          with-char=K (iter-466) shows the movie's character
                                          K instead (a named template of an unnamed sprite)
  edit-place NAME char=C [frame=F] [x=X] [y=Y] [tabindex=N] [cxmul=AARRGGBB] [cxadd=AARRGGBB]
                                          (iter-466: NAME '@D' = the placement at depth D, named or
                                          not; cxmul / cxadd set its colour transform)
                                          (iter-464) move a PlaceObject (matrix tx / ty) and/or
                                          set the m_tabIndex its clip action assigns
  Every code op takes its body inline (the lines up to a line `end`) or from
  `file=NAME.aptasm` (relative to the patch file). A body line `.include 'NAME.aptasm'`
  is replaced by that file's text (relative to the patch file; nests, no cycles), so one
  expression or function can be spliced into several movies. Bodies are aptasm.py source: prepend
  and append bodies are snippets (no End, no ConstantPool; pool names resolve in the
  host block's ConstantPool and missing names are appended to it); add-* bodies are
  whole blocks (End appended when missing; a ConstantPool is created from the names
  used when the body has none).

  NAME  a function's member name - the name pushed right before DefineFunction(2) and
        stored by the SetMember after it (e.g. InitGui) - or its own name.
  CTX / SEL  initaction:61 | initaction:ScrPressStart | doaction:CHAR/FRAME |
        bytecode@0x41e4 (original offset). `in` also takes a bare name or sprite id. A
        name first matches AS2 class exports ('__Packages.X', with or without the
        prefix); only when none matches does it try plain linkage exports (SCRPRESSSTART
        exports 'ScrPressStart' for sprite 59 AND '__Packages.ScrPressStart' for 61:
        initaction:ScrPressStart = 61, initaction:59 = the other). An ambiguous selector
        is an error that lists the candidates; nth=K (1-based) picks one.

InitAction facts this relies on (IDA, 2026-09-30): AptExecuteFrameTags_sub_826BE100
runs every type-8 item of a frame first, in item order, while +4 (spriteId) >= 0, then
negates +4; AptSpriteFramesRelocate_sub_826A39A0 relocates only +8; the type-1 enqueue
(sub_8268C198) and PlaceObject walker (sub_8269B7B0) ignore type 8. So +4 is only a
run-once flag there: it must be > 0 (0 negates to 0 and would re-run). auto = the
movie's char count (the first unused char id). Not proven exhaustive: no whole-binary
search for other readers of +4.

VERIFICATION (every apply, before anything is returned):
  1. the emitted .apt/.const re-parse (apt_model) with 0 uncovered non-zero bytes;
  2. the re-parsed movie is isomorphic to the edited graph: every block, pointer,
     instruction and .const item came back (so the walker reaches all new code);
  3. the edited movie round-trips byte-exact through apt_graph;
  4. every original block the patch did not touch is unchanged, and every original
     .const item except the replaced ones (checked on the edit, before step 5's renumbering);
  5. the runtime's UNLOAD contract (apt_graph.Graph.renumber_walk_order runs first; the
     checker is the independent byte-level tools/apt/apt_unload.py): every Push /
     ConstantPool index slot holds its position in the unload walk, #slots == #items,
     every slot resolves to the constant it named before the renumbering, and a reload
     after a simulated unload resolves the same constants. Without it a movie loads once
     and then reloads with wrong constants (iter-463 QUIT GAME soft-lock).

THE FIRST-REGISTRAR RULE (lane E, step 5 E3-E6 r3; the Ally roll of 2026-10-23): every AS2
class's InitAction is guarded by `if (!_global.X)`, so the FIRST movie that registers X wins
and every later movie's copy of X never runs. MAIN (_level0, loaded first on every boot)
exports 40 classes as `__Packages.*` (docs/ENGINE_MAP.md), among them ScrGenPop, ButtonIcon,
ButtonHints, DisplayWell and PartText. A function op (prepend-to / append-to function) that
lands in a method of such a class inside ANY movie other than MAIN is therefore dead code, and
patch_container / _main refuse it by name: patch the class in movie MAIN instead. A method is a
function DEFINED INSIDE the guarded body (class_guard_body: op_71, EA_GetNamedMember..., Not, Not,
If -> the body's end; E3-E6 r4), found on the pristine block and tracked through every op (code
prepended to the InitAction moves the guard, not the rule; E3-E6 r5). An action op on the InitAction block itself is live (it runs
whether or not the guard skips the body), and so is a function defined outside the guarded body. (Found the hard way: the E4 BACK
hint and the E5 hint icon ids were patched into SCRGENPOP / BUTTONICON and never ran.)

usage (repo root):
  python tools/apt/patch_movie.py PATCH [--ui ui.AST] [--out DIR]
     applies each movie section, prints the report; --out writes <MOVIE>.apt/.const and
     the canonical disassembly of every changed block there (never the game folder).
"""
import os
import re
import struct
import sys
import ast

try:
    from .bgfa import Bgfa, DEFAULT_UI, DATA_DIR, TAG_APT, TAG_CONST
    from .apt_model import Apt, coverage
    from .apt_graph import Block, Ins, from_apt
    from . import aptasm, apt_unload
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bgfa import Bgfa, DEFAULT_UI, DATA_DIR, TAG_APT, TAG_CONST
    from apt_model import Apt, coverage
    from apt_graph import Block, Ins, from_apt
    import aptasm
    import apt_unload


class PatchError(Exception):
    pass


# The movie that registers its __Packages classes first on every boot (_level0; see THE
# FIRST-REGISTRAR RULE above).
FIRST_REGISTRAR = "MAIN"


CODE_OPS = {"prepend-to", "append-to", "add-initaction", "add-doaction"}
PLACE_OPS = {"clone-place", "edit-place"}


class Op:
    def __init__(self, kind, words, opts, body, line, src, file=None):
        self.kind, self.words, self.opts, self.body, self.line, self.src = kind, words, opts, body, line, src
        self.file = file                        # the .aptasm the body came from, if any

    def where(self):
        return f"{self.src}:{self.line}"

    def __repr__(self):
        return f"{self.kind} {' '.join(map(str, self.words))} {self.opts}"


_WTOK = re.compile(r"""\s*(?:(?P<str>[bB]?(?:'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*"))|(?P<word>\S+))""")


def _words(s, where):
    out = []
    pos = 0
    s = s.rstrip()
    while pos < len(s):
        m = _WTOK.match(s, pos)
        if not m or m.end() == pos:
            raise PatchError(f"{where}: cannot read {s[pos:]!r}")
        pos = m.end()
        if m.group("str"):
            v = ast.literal_eval(m.group("str"))
            out.append(v.encode("latin1") if isinstance(v, str) else v)
        else:
            out.append(m.group("word"))
    return out


_INCLUDE = re.compile(r"""^\s*\.include\s+(?:'([^']+)'|"([^"]+)"|(\S+))\s*(?:;.*)?$""")


def expand_includes(body, base_dir, where, depth=0, seen=None):
    """Textual `.include 'NAME.aptasm'` lines (relative to the patch directory) ->
    (expanded body, [absolute paths of every included file])."""
    if depth > 8:
        raise PatchError(f"{where}: .include nested deeper than 8")
    out, files = [], []
    for ln in body.splitlines():
        m = _INCLUDE.match(ln)
        if not m:
            out.append(ln)
            continue
        name = m.group(1) or m.group(2) or m.group(3)
        path = os.path.abspath(os.path.join(base_dir, name))
        if seen and path in seen:
            raise PatchError(f"{where}: .include cycle through {name}")
        if not os.path.isfile(path):
            raise PatchError(f"{where}: .include {name}: no such file")
        sub, subfiles = expand_includes(open(path, encoding="utf-8").read(), base_dir, f"{where} > {name}",
                                        depth + 1, (seen or set()) | {path})
        out.append(sub.rstrip("\n"))
        files += [path] + subfiles
    return "\n".join(out) + "\n", files


def own_ins(ins):
    """The snippet's own instructions: a nested DefineFunction(2) body has its own
    registers, so the host function's register checks skip it (the DefineFunction
    instruction itself is kept)."""
    i = 0
    while i < len(ins):
        a = ins[i]
        yield a
        if a.op in (0x8E, 0x9B) and a.f.get("end") is not None and a.f["end"] > i:
            i = a.f["end"]
        else:
            i += 1


def parse_patch(text, src="<patch>", base_dir="."):
    """-> list of (movieName, [Op])."""
    sections = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i]
        i += 1
        s = raw.strip()
        if not s or s[0] in "#;":
            continue
        where = f"{src}:{i}"
        kind, _, rest = s.partition(" ")
        if kind == "movie":
            w = _words(rest, where)
            if len(w) != 1 or not isinstance(w[0], str):
                raise PatchError(f"{where}: movie NAME")
            sections.append((w[0].upper(), []))
            continue
        if not sections:
            raise PatchError(f"{where}: an op before any 'movie' line")
        words, opts = [], {}
        for w in _words(rest, where):
            if isinstance(w, str) and "=" in w and not w.startswith("="):
                k, _, v = w.partition("=")
                opts[k] = v
            else:
                words.append(w)
        body = None
        path = None
        if kind in CODE_OPS:
            if "file" in opts:
                path = os.path.join(base_dir, opts.pop("file"))
                with open(path, encoding="utf-8") as body_file:
                    body = body_file.read()
            else:
                buf = []
                while True:
                    if i >= len(lines):
                        raise PatchError(f"{where}: body not closed by a line 'end'")
                    ln = lines[i]
                    i += 1
                    if ln.strip() == "end":
                        break
                    buf.append(ln)
                body = "\n".join(buf) + "\n"
        elif kind != "replace-const" and kind not in PLACE_OPS:
            raise PatchError(f"{where}: unknown op {kind!r}")
        includes = []
        if body is not None:
            body, includes = expand_includes(body, base_dir, where)
        op = Op(kind, words, opts, body, i, src, path)
        op.includes = includes
        sections[-1][1].append(op)
    return sections


def load_patch(path):
    with open(path, encoding="utf-8") as patch_file:
        return parse_patch(patch_file.read(), src=os.path.basename(path),
                           base_dir=os.path.dirname(os.path.abspath(path)))


# ============================================================ movie structure (graph)
def u32(b, o):
    return struct.unpack_from(">I", b.data, o)[0]


class Ctx:
    """chars, exports, frames and code owners of a graph, recomputed per op."""

    def __init__(self, g):
        self.g = g
        mv = g.blocks[g.movie]
        self.nchars = u32(mv, 0x1C)
        tab = g.blocks[mv.ptrs[0x20]]
        self.chars = {0: g.movie}
        for i in range(1, self.nchars):
            k = tab.ptrs.get(4 * i)
            if k is not None:
                self.chars[i] = k
        self.exports = {}                       # char id -> [names]
        nexp = u32(mv, 0x38)
        if nexp:
            eb = g.blocks[mv.ptrs[0x3C]]
            for q in range(nexp):
                nm = aptasm.gstr(g, eb.ptrs.get(8 * q))
                self.exports.setdefault(u32(eb, 8 * q + 4), []).append(nm.decode("latin1") if nm else None)
        self.owner = {}                         # bytecode key -> set of context names
        self.fallback = {}                      # bytecode key -> plain export names (2nd tier)
        self.initactions = []                   # (charId, frame, itemKey, spriteId, bytecodeKey)
        self.doactions = []                     # (charId, frame, nth, itemKey, bytecodeKey)
        for cid, ck in self.chars.items():
            cb = g.blocks[ck]
            t = 9 if ck == g.movie else u32(cb, 0)
            if t not in (5, 9):
                continue
            nfr = u32(cb, 0x10)
            fk = cb.ptrs.get(0x14)
            if not nfr or fk is None:
                continue
            fb = g.blocks[fk]
            for fr in range(nfr):
                n = u32(fb, 8 * fr)
                ak = fb.ptrs.get(8 * fr + 4)
                if not n or ak is None:
                    continue
                ab = g.blocks[ak]
                nth = 0
                for q in range(n):
                    ik = ab.ptrs[4 * q]
                    ib = g.blocks[ik]
                    it = u32(ib, 0)
                    if it == 8:
                        sid = struct.unpack_from(">i", ib.data, 4)[0]
                        bk = ib.ptrs[8]
                        self.initactions.append((cid, fr, ik, sid, bk))
                        names = {f"initaction:{sid}", str(sid), f"bytecode@{_k(bk)}"}
                        plain = set()
                        for e in self.exports.get(sid, []):
                            if not e:
                                continue
                            if e.startswith("__Packages."):   # an AS2 class: the primary name
                                names |= {e, f"initaction:{e}", e[11:], f"initaction:{e[11:]}"}
                            else:                             # a plain linkage name: fallback only
                                plain |= {e, f"initaction:{e}"}
                        self.owner.setdefault(bk, set()).update(names)
                        self.fallback.setdefault(bk, set()).update(plain)
                    elif it == 1:
                        nth += 1
                        bk = ib.ptrs[4]
                        self.doactions.append((cid, fr, nth, ik, bk))
                        self.owner.setdefault(bk, set()).update(
                            {f"doaction:{cid}/{fr}", f"doaction:{cid}/{fr}#{nth}", f"bytecode@{_k(bk)}"})
                    elif it == 3 and 0x3C in ib.ptrs:
                        ca = g.blocks[ib.ptrs[0x3C]]
                        rk = ca.ptrs.get(4)
                        if rk is None:
                            continue
                        rb = g.blocks[rk]
                        for r in range(u32(ca, 0)):
                            bk = rb.ptrs.get(12 * r + 8)
                            if bk is not None:
                                self.owner.setdefault(bk, set()).add(f"bytecode@{_k(bk)}")
        for k in g.order:
            if g.blocks[k].code is not None:
                self.owner.setdefault(k, set()).add(f"bytecode@{_k(k)}")

    def pool_of(self, key):
        code = self.g.blocks[key].code
        return aptasm.pool_values(self.g, code[0]) if code and code[0].op == 0x88 else None

    def functions(self):
        """-> [(bytecodeKey, index, memberName, ownName)]"""
        out = []
        g = self.g
        for k in g.order:
            code = g.blocks[k].code
            if code is None:
                continue
            pool = self.pool_of(k)
            for i, ins in enumerate(code):
                if ins.op not in (0x8E, 0x9B):
                    continue
                member = None
                if i:
                    p = code[i - 1]
                    if p.op in (0xA2, 0xA3) and pool is not None:
                        v = int.from_bytes(p.f["imm"], "big")
                        if v < len(pool) and pool[v][0] == 1 and pool[v][1] is not None:
                            member = pool[v][1].decode("latin1")
                    elif p.op == 0xA1:
                        s = aptasm.gstr(g, p.f["s"])
                        member = s.decode("latin1") if s is not None else None
                own = aptasm.gstr(g, ins.f["name"])
                out.append((k, i, member, own.decode("latin1") if own else None))
        return out


def _k(k):
    return f"{k:#x}" if isinstance(k, int) else f"new{k[1]}"


def _pick(cands, nth, what, where, describe):
    if not cands:
        raise PatchError(f"{where}: no {what}")
    if nth is not None:
        if not 1 <= nth <= len(cands):
            raise PatchError(f"{where}: nth={nth} but {len(cands)} {what} candidates")
        return cands[nth - 1]
    if len(cands) > 1:
        raise PatchError(f"{where}: {len(cands)} candidates for {what}; add 'in ...' or nth=:\n  "
                         + "\n  ".join(f"nth={i + 1}: {describe(c)}" for i, c in enumerate(cands)))
    return cands[0]


def _in(ctx, k, sel, tier):
    return sel in (ctx.owner if tier == 0 else ctx.fallback).get(k, ())


def find_function(ctx, name, in_ctx, nth, where):
    fns = [(k, i) for k, i, member, own in ctx.functions() if name in (member, own)]
    cands = fns
    if in_ctx is not None:
        cands = [c for c in fns if _in(ctx, c[0], in_ctx, 0)] or [c for c in fns if _in(ctx, c[0], in_ctx, 1)]
    return _pick(cands, nth, f"function {name!r}" + (f" in {in_ctx}" if in_ctx else ""), where,
                 lambda c: f"bytecode@{_k(c[0])} ins {c[1]} ({', '.join(sorted(ctx.owner.get(c[0], ())))})")


def find_action(ctx, sel, nth, where):
    code = [k for k in ctx.g.order if ctx.g.blocks[k].code is not None]
    cands = [k for k in code if _in(ctx, k, sel, 0)] or [k for k in code if _in(ctx, k, sel, 1)]
    return _pick(cands, nth, f"action {sel}", where,
                 lambda k: f"bytecode@{_k(k)} ({', '.join(sorted(ctx.owner.get(k, ())))})")


# ============================================================ helpers on code
def refcount(g, key):
    """real users of a block: an empty DefineFunction(2) params pointer that happens to
    aim at it is not one (apt_graph.Graph.refs strong=True)."""
    return sum(1 for k in g.order for t in g.refs(k, strong=True) if t == key)


def splice(code, at, new, keep_at):
    """insert `new` (whose own tgt/end are relative to its start) before index `at`.
    References to indices > at shift by len(new); a reference == at shifts unless
    keep_at(instructionIndex, field) says it should land on the inserted code."""
    n = len(new)
    for j, ins in enumerate(code):
        for fld in ("tgt", "end"):
            if fld in ins.f:
                t = ins.f[fld]
                if t > at or (t == at and not keep_at(j, fld)):
                    ins.f[fld] = t + n
    for ins in new:
        for fld in ("tgt", "end"):
            if fld in ins.f:
                ins.f[fld] += at
    code[at:at] = new


def nested_ranges(code, lo, hi):
    """index ranges of function bodies nested inside [lo, hi)."""
    out = []
    for j in range(lo, hi):
        if code[j].op in (0x8E, 0x9B):
            out.append((j + 1, code[j].f["end"]))
    return out


def own_level(code, lo, hi):
    """indices in [lo, hi) that are not inside a nested function body."""
    inner = nested_ranges(code, lo, hi)
    return [j for j in range(lo, hi) if not any(s <= j < e for s, e in inner)]


# ============================================================ the patcher
class Patcher:
    def __init__(self, apt, cst, name="?"):
        self.name = name
        self.apt, self.cst = apt, cst
        self.a = Apt(apt, cst)
        self.g0 = from_apt(self.a)             # pristine copy for the untouched check
        self.g = from_apt(self.a)
        self.tail = len(apt) - max(o + s for o, s, k in self.a.blocks)
        self.touched = set()
        self.replaced_items = set()
        self.report = []
        self.warnings = []
        self.results = []                      # per op: dict
        # THE FIRST-REGISTRAR RULE's class guards, found once on the pristine code (class_guard_body) and then
        # tracked by the guard's opening instruction (its op_71): splice keeps every instruction object and moves
        # its targets, so code an action op prepends to an InitAction (which moves the guard away from the block's
        # opening) does not hide the guarded body from a later function op (E3-E6 r5, Astra E3-E6 r4 P2).
        # None = the block has no guard.
        self.guards = {}
        for key, blk in self.g.blocks.items():
            if blk.code:
                self.guards[key] = blk.code[guard_open(blk.code)] if class_guard_body(blk.code) else None

    def guarded_body(self, key):
        """-> (lo, hi): the class guard's body in block `key`'s CURRENT code, or None. A block of the pristine
        movie keeps the guard it had: re-read (class_guard_body) where its opening now is; a block an op added is
        read at its own opening."""
        code = self.g.blocks[key].code
        if key not in self.guards:
            return class_guard_body(code) if code else None
        op71 = self.guards[key]
        if op71 is None:
            return None
        at = next((j for j, ins in enumerate(code) if ins is op71), None)
        gb = class_guard_body(code, at) if at is not None else None
        if gb is None:
            raise PatchError(f"{self.name}: bytecode@{_k(key)} lost its class guard (an op replaced or split it)")
        return gb

    # ---- host pool
    def _grow_pool(self, key, appended):
        """append value items to the host block's ConstantPool (fresh .const items, the
        array grown in place). A block without a pool gets one at index 0, which shifts
        every reference by one; returns that shift (0 or 1)."""
        if not appended:
            return 0
        g = self.g
        code = g.blocks[key].code
        shift = 0
        if not code or code[0].op != 0x88:
            splice(code, 0, [Ins(0x88, n=0, arr=None)], lambda j, f: False)
            shift = 1
            self.report.append(f"  created a ConstantPool at the start of bytecode@{_k(key)}")
        pins = code[0]
        idx = []
        for v in appended:
            g.items.append(aptasm.graph_item(v))
            idx.append(len(g.items) - 1)
        if pins.f["arr"] is None:
            pins.f["arr"] = g.add_block(Block("code.constidx", b""))
        elif refcount(g, pins.f["arr"]) > 1:
            old = g.blocks[pins.f["arr"]]
            pins.f["arr"] = g.add_block(Block("code.constidx", bytes(old.data)))
        ab = g.blocks[pins.f["arr"]]
        ab.data += b"".join(struct.pack(">I", i) for i in idx)
        pins.f["n"] += len(idx)
        self.touched.add(pins.f["arr"])
        self.report.append(f"  pool of bytecode@{_k(key)} grew by {len(idx)}: "
                           + ", ".join(aptasm.fmt_item(v) for v in appended))
        return shift

    # ---- ops
    def apply(self, op):
        where = op.where()
        if op.kind == "replace-const":
            return self._replace_const(op, where)
        if op.kind in ("add-initaction", "add-doaction"):
            return self._add_action(op, where)
        if op.kind in PLACE_OPS:
            return self._place_op(op, where)
        return self._splice_op(op, where)

    # ---- PlaceObject items (iter-464: a 10th pause-menu row)
    def _find_place(self, cid, fr, name, where):
        """-> (frames block key, item array key, index, item key) of the PlaceObject named
        `name` in char `cid` frame `fr`."""
        g = self.g
        ctx = Ctx(g)
        if cid not in ctx.chars:
            raise PatchError(f"{where}: no char {cid}")
        cb = g.blocks[ctx.chars[cid]]
        ctype = 9 if ctx.chars[cid] == g.movie else u32(cb, 0)
        if ctype not in (5, 9) or 0x14 not in cb.ptrs or fr >= u32(cb, 0x10):
            raise PatchError(f"{where}: char {cid} has no frame {fr}")
        fk = cb.ptrs[0x14]
        fb = g.blocks[fk]
        n = u32(fb, 8 * fr)
        ak = fb.ptrs.get(8 * fr + 4)
        hits = []
        for q in range(n if ak is not None else 0):
            ik = g.blocks[ak].ptrs[4 * q]
            ib = g.blocks[ik]
            if name.startswith("@"):   # iter-466: '@D' = the placement at depth D (unnamed ones too)
                if u32(ib, 0) == 3 and u32(ib, 8) == int(name[1:], 0):
                    hits.append((q, ik))
            elif u32(ib, 0) == 3 and 0x34 in ib.ptrs and aptasm.gstr(g, ib.ptrs[0x34]) == name.encode("latin1"):
                hits.append((q, ik))
        if len(hits) != 1:
            raise PatchError(f"{where}: {len(hits)} PlaceObject items named {name!r} in char {cid} frame {fr}")
        return fk, ak, hits[0][0], hits[0][1]

    def _clip_code_blocks(self, ik):
        g = self.g
        ib = g.blocks[ik]
        out = []
        ca = ib.ptrs.get(0x3C)
        if ca is None:
            return out
        rk = g.blocks[ca].ptrs.get(4)
        for r in range(u32(g.blocks[ca], 0) if rk is not None else 0):
            bk = g.blocks[rk].ptrs.get(12 * r + 8)
            if bk is not None:
                out.append(bk)
        return out

    def _set_tabindex(self, bk, value, where):
        """the clip action's `EA_PushString 'm_tabIndex'; <push value>; SetVariable`."""
        g = self.g
        code = g.blocks[bk].code
        for i, ins in enumerate(code[:-2]):
            if ins.op == 0xA1 and aptasm.gstr(g, ins.f.get("s")) == b"m_tabIndex" and code[i + 2].op == 0x1D:
                if not 0 <= value <= 255:
                    raise PatchError(f"{where}: tabindex {value} out of 0..255")
                code[i + 1] = Ins(0xB5, imm=bytes([value]))
                self.touched.add(bk)
                return
        raise PatchError(f"{where}: no `m_tabIndex` assignment in the clip action")

    def _place_op(self, op, where):
        g = self.g
        w = op.words
        if op.kind == "clone-place":
            if len(w) != 3 or w[1] != "as" or not isinstance(w[0], str) or not isinstance(w[2], str):
                raise PatchError(f"{where}: clone-place NAME as NEW char=C [frame=F] depth=D [x=X] [y=Y] [tabindex=N]")
            if "depth" not in op.opts:
                raise PatchError(f"{where}: clone-place needs depth=")
        elif len(w) != 1 or not isinstance(w[0], str):
            raise PatchError(f"{where}: edit-place NAME char=C [frame=F] [x=X] [y=Y] [tabindex=N]")
        cid = int(op.opts.get("char", "0"), 0)
        fr = int(op.opts.get("frame", "0"), 0)
        fk, ak, q, ik = self._find_place(cid, fr, w[0], where)
        if op.kind == "clone-place":
            depth = int(op.opts["depth"], 0)
            ab = g.blocks[ak]
            for qq in range(len(ab.ptrs)):
                ob = g.blocks[ab.ptrs[4 * qq]]
                if u32(ob, 0) == 3 and u32(ob, 8) == depth:
                    raise PatchError(f"{where}: depth {depth} is already used in char {cid} frame {fr}")
            src = g.blocks[ik]
            new_name = g.add_string(w[2].encode("latin1"), after=src.ptrs[0x34])
            g.blocks[new_name].kind = g.blocks[src.ptrs[0x34]].kind
            ptrs = dict(src.ptrs)
            ptrs[0x34] = new_name
            if 0x3C in src.ptrs:
                ca0 = g.blocks[src.ptrs[0x3C]]
                rk0 = ca0.ptrs.get(4)
                rb0 = g.blocks[rk0]
                new_recs_ptrs = {}
                for r in range(u32(ca0, 0)):
                    bk0 = rb0.ptrs.get(12 * r + 8)
                    if bk0 is None:
                        continue
                    code = []
                    for ins in g.blocks[bk0].code:
                        f = dict(ins.f)
                        if ins.op in (0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0x8B, 0x8C) and f.get("s") is not None:
                            sb = g.blocks[f["s"]]
                            f["s"] = g.add_block(type(sb)(sb.kind, bytes(sb.data), dict(sb.ptrs)), after=f["s"])
                        elif ins.op == 0x83 and f.get("s"):
                            f["s"] = [None if x is None else g.add_block(
                                type(g.blocks[x])(g.blocks[x].kind, bytes(g.blocks[x].data), dict(g.blocks[x].ptrs)),
                                after=x) for x in f["s"]]
                        elif ins.op in (0x88, 0x96) and f.get("n"):
                            raise PatchError(f"{where}: clip action with an index array (not supported)")
                        code.append(Ins(ins.op, **f))
                    nb = g.add_block(Block("bytecode", code=code), after=bk0)
                    new_recs_ptrs[12 * r + 8] = nb
                new_recs = g.add_block(Block(rb0.kind, bytes(rb0.data), new_recs_ptrs), after=rk0)
                new_ca = g.add_block(Block(ca0.kind, bytes(ca0.data), {**ca0.ptrs, 4: new_recs}), after=src.ptrs[0x3C])
                ptrs[0x3C] = new_ca
            data = bytearray(src.data)
            struct.pack_into(">I", data, 8, depth)
            if "with-char" in op.opts:   # iter-466 map: the copy shows another character of the movie
                struct.pack_into(">I", data, 12, int(op.opts["with-char"], 0))
            nik = g.add_block(Block(src.kind, bytes(data), ptrs), after=ik)
            items = [ab.ptrs[4 * qq] for qq in range(len(ab.ptrs))]
            if refcount(g, ak) > 1:
                ak2 = g.add_block(Block("itemptrs", bytes(ab.data), dict(ab.ptrs)), after=ak)
                g.blocks[fk].ptrs[8 * fr + 4] = ak2
                ak = ak2
                ab = g.blocks[ak]
            items.insert(q + 1, nik)
            ab.data = bytearray(4 * len(items))
            ab.ptrs = {4 * qq: k for qq, k in enumerate(items)}
            struct.pack_into(">I", g.blocks[fk].data, 8 * fr, len(items))
            self.touched |= {fk, ak}
            target = nik
            what = f"clone-place {w[0]!r} as {w[2]!r} char {cid} frame {fr} depth {depth} item {q + 1}/{len(items)}"
            if "with-char" in op.opts:
                what += f" with-char {int(op.opts['with-char'], 0)}"
        else:
            target = ik
            self.touched.add(ik)
            what = f"edit-place {w[0]!r} char {cid} frame {fr}"
        tb = g.blocks[target]
        for key, off in (("x", 0x20), ("y", 0x24)):
            if key in op.opts:
                struct.pack_into(">f", tb.data, off, float(op.opts[key]))
                what += f" {key}={float(op.opts[key])}"
        for key, off in (("cxmul", 0x28), ("cxadd", 0x2C)):   # iter-466: a colour transform, AARRGGBB
            if key in op.opts:
                struct.pack_into(">I", tb.data, off, int(op.opts[key], 16))
                struct.pack_into(">I", tb.data, 4, u32(tb, 4) | 0x08)   # the cxform flag
                what += f" {key}={op.opts[key]}"
        if "tabindex" in op.opts:
            codes = self._clip_code_blocks(target)
            if len(codes) != 1:
                raise PatchError(f"{where}: tabindex= needs exactly one clip-action block ({len(codes)})")
            self._set_tabindex(codes[0], int(op.opts["tabindex"], 0), where)
            what += f" tabindex={op.opts['tabindex']}"
        self.report.append(what)
        res = {"op": op.kind, "item": target}
        self.results.append(res)
        return res

    def _replace_const(self, op, where):
        w = op.words
        if len(w) != 3 or w[1] != "->" or not isinstance(w[0], bytes) or not isinstance(w[2], bytes):
            raise PatchError(f"{where}: replace-const 'OLD' -> 'NEW' [count=N]")
        old, new = w[0], w[2]
        if b"\0" in new:
            raise PatchError(f"{where}: NUL in the new string")
        hits = [i for i, (t, v) in enumerate(self.g.items) if t == 1 and isinstance(v, tuple) and v[1] == old]
        if not hits:
            raise PatchError(f"{where}: no .const string {old!r}")
        if "count" in op.opts and len(hits) != int(op.opts["count"]):
            raise PatchError(f"{where}: {len(hits)} .const items equal {old!r}, count={op.opts['count']}")
        for i in hits:
            self.g.items[i] = (1, ("str", new))
            self.replaced_items.add(i)
        self.report.append(f"replace-const {aptasm.qs(old)} -> {aptasm.qs(new)}: {len(hits)} item(s) {hits}")
        res = {"op": op.kind, "items": hits}
        self.results.append(res)
        return res

    def _add_action(self, op, where):
        g = self.g
        ctx = Ctx(g)
        init = op.kind == "add-initaction"
        cid = int(op.opts.get("char", "0"), 0)
        fr = int(op.opts.get("frame", "0"), 0)
        position = op.opts.get("position", "last")
        if position not in ("last", "first"):
            raise PatchError(f"{where}: position=last|first")
        if cid not in ctx.chars:
            raise PatchError(f"{where}: no char {cid}")
        cb = g.blocks[ctx.chars[cid]]
        ctype = 9 if ctx.chars[cid] == g.movie else u32(cb, 0)
        if ctype not in (5, 9):
            raise PatchError(f"{where}: char {cid} is not a sprite/movie (type {ctype})")
        nfr = u32(cb, 0x10)
        if fr >= nfr or 0x14 not in cb.ptrs:
            raise PatchError(f"{where}: char {cid} has {nfr} frames")
        if init:
            sp = op.opts.get("sprite", "auto")
            sid = ctx.nchars if sp == "auto" else int(sp, 0)
            if sid <= 0:
                raise PatchError(f"{where}: sprite id must be > 0 (the run-once flag is its sign)")
            if any(s == sid for _, _, _, s, _ in ctx.initactions):
                self.warnings.append(f"{where}: sprite id {sid} is already used by another InitAction")
        # code
        P = aptasm.parse(op.body, mode="block", auto_end=True)
        bk = g.add_block(Block("bytecode", code=[]))
        g.blocks[bk].code = aptasm.materialize_new(P.ins, g, bk)
        if init:
            item = g.add_block(Block("item.InitAction", struct.pack(">IiI", 8, sid, 0), {8: bk}))
        else:
            item = g.add_block(Block("item.Action", struct.pack(">II", 1, 0), {4: bk}))
        # frame item-pointer array
        fk = cb.ptrs[0x14]
        fb = g.blocks[fk]
        n = u32(fb, 8 * fr)
        ak = fb.ptrs.get(8 * fr + 4) if n else None
        if ak is None:
            ak = g.add_block(Block("itemptrs", b""))
            fb.ptrs[8 * fr + 4] = ak
            items = []
        else:
            if refcount(g, ak) > 1:
                old = g.blocks[ak]
                ak = g.add_block(Block("itemptrs", bytes(old.data), dict(old.ptrs)))
                fb.ptrs[8 * fr + 4] = ak
            ab = g.blocks[ak]
            items = [ab.ptrs[4 * q] for q in range(n)]
        want = 8 if init else 1
        same = [q for q, ik in enumerate(items) if u32(g.blocks[ik], 0) == want]
        if position == "last":
            at = same[-1] + 1 if same else len(items)
        else:
            at = same[0] if same else len(items)
        items.insert(at, item)
        ab = g.blocks[ak]
        ab.data = bytearray(4 * len(items))
        ab.ptrs = {4 * q: ik for q, ik in enumerate(items)}
        struct.pack_into(">I", fb.data, 8 * fr, len(items))
        self.touched |= {fk, ak}
        what = f"InitAction sprite={sid}" if init else "DoAction"
        self.report.append(f"{op.kind}: {what} at char {cid} frame {fr} item {at}/{len(items)}; "
                           f"{len(P.ins)} instructions" + (" (ConstantPool created)" if P.auto_pool else ""))
        res = {"op": op.kind, "code": bk, "item": item, "char": cid, "frame": fr, "index": at,
               "sprite": sid if init else None, "ins": P.ins}
        self.results.append(res)
        return res

    def _splice_op(self, op, where):
        g = self.g
        ctx = Ctx(g)
        prepend = op.kind == "prepend-to"
        if not op.words or op.words[0] not in ("function", "action"):
            raise PatchError(f"{where}: {op.kind} function NAME [in CTX] | {op.kind} action SEL")
        nth = int(op.opts["nth"]) if "nth" in op.opts else None
        if op.words[0] == "function":
            w = op.words[1:]
            if not w:
                raise PatchError(f"{where}: function name missing")
            name = w[0]
            in_ctx = None
            if len(w) >= 3 and w[1] == "in":
                in_ctx = w[2]
            elif len(w) != 1:
                raise PatchError(f"{where}: function NAME [in CTX]")
            key, fi = find_function(ctx, name, in_ctx, nth, where)
            target = f"function {name}" + (f" in {in_ctx}" if in_ctx else "")
        else:
            if len(op.words) != 2:
                raise PatchError(f"{where}: action SEL")
            key = find_action(ctx, op.words[1], nth, where)
            fi = None
            target = f"action {op.words[1]}"
        # pool context: a copy of the host block's pool (an empty one when it has none;
        # a ConstantPool is then created only if the snippet names a pool entry)
        host_pool = ctx.pool_of(key)
        P = aptasm.parse(op.body, mode="snippet", pool=list(host_pool) if host_pool is not None else [])
        shift = self._grow_pool(key, P.pool_appended)
        code = g.blocks[key].code
        if fi is not None:
            fi += shift
        if fi is not None:
            s, e = fi + 1, code[fi].f["end"]
            fn = code[fi]
            if fn.op == 0x8E:
                nreg = fn.f["flags"] >> 16
                for a in own_ins(P.ins):
                    r = None
                    if a.op == 0x96:
                        for t, v in a.f["vals"]:
                            if t == 4 and v >= nreg:
                                r = v
                    elif a.op == 0x87 and a.f["i32"] >= nreg:
                        r = a.f["i32"]
                    if r is not None:
                        raise PatchError(f"{where}: register {r} but the function has {nreg} registers "
                                         f"(flags {fn.f['flags']:#x})")
        else:
            s = 1 if code and code[0].op == 0x88 else 0
            e = len(code) - 1                       # the End
        for a in own_ins(P.ins):
            if a.op == 0x87:
                self.warnings.append(f"{where}: snippet writes register {a.f['i32']}; the original "
                                     f"code of {target} must not rely on it")
        new = aptasm.materialize_new(P.ins, g, key)
        if "after-call" in op.opts:
            if prepend or fi is None:
                raise PatchError(f"{where}: after-call= requires append-to function")
            pool = ctx.pool_of(key)
            method = op.opts["after-call"].encode("latin1")
            hits = []
            for j in own_level(code, s, e):
                if code[j].op != 0x52 or j <= s or code[j - 1].op not in (0xA2, 0xA3):
                    continue
                index = int.from_bytes(code[j - 1].f["imm"], "big")
                if pool is not None and index < len(pool) and pool[index] == (1, method):
                    hits.append(j + 1)
            if len(hits) != 1:
                raise PatchError(f"{where}: after-call={op.opts['after-call']} needs exactly one named "
                                 f"CallMethod in {target}; found {len(hits)}")
            at = hits[0]
            # Only fall-through from this call enters the snippet. A branch that
            # skipped the call must continue skipping the extension as well.
            splice(code, at, new, lambda j, f: False)
        elif prepend:
            at = s
            splice(code, at, new, lambda j, f: False)
        else:
            at = e
            lvl = own_level(code, s, e)
            if lvl and code[lvl[-1]].op in aptasm.TERMINATORS and not any(
                    code[j].f.get("tgt") == e for j in range(s, e)):
                raise PatchError(f"{where}: {target} ends with {aptasm.mnemonic(code[lvl[-1]].op)} and no "
                                 f"branch reaches its end, so appended code would never run")
            rets = [j for j in lvl if code[j].op == 0x3E]
            if rets:
                self.warnings.append(f"{where}: {target} has {len(rets)} Return(s) that skip the appended code")

            def keep_at(j, fld, s=s, e=e, fi=fi):
                if fld == "end" and j == fi:
                    return False                   # the target function grows
                return s <= j < e                  # branches / nested ends inside the body stay on the new code

            splice(code, at, new, keep_at)
        self.touched.add(key)
        self.report.append(f"{op.kind} {target}: {len(P.ins)} instructions at bytecode@{_k(key)} index {at}"
                           + (f" (function at {fi}, body {s}..{code[fi].f['end']})" if fi is not None else ""))
        # the first-registrar rule: a FUNCTION op lands in a method the class's InitAction defines behind its
        # `if (!_global.X)` guard - dead when another movie registered X first. An ACTION op is not: code
        # prepended / appended to the InitAction block itself runs whether or not the guard skips the body; nor is
        # a function defined OUTSIDE the guarded body (E3-E6 r4, Astra r3 P3: e.g. a helper an action op added).
        # The body is the guard's as tracked through every earlier op (guarded_body; E3-E6 r5).
        guarded = self.guarded_body(key) if fi is not None else None
        classes = sorted({n[len("__Packages."):] for n in ctx.owner.get(key, ()) if n.startswith("__Packages.")}) \
            if guarded and guarded[0] <= fi < guarded[1] else []
        res = {"op": op.kind, "code": key, "fn": fi, "at": at, "n": len(P.ins), "ins": P.ins,
               "pool_appended": list(P.pool_appended), "classes": classes, "target": target}
        self.results.append(res)
        return res

    # ---- finish
    def finish(self, renumber=True):
        """gc -> untouched check -> renumber_walk_order -> emit -> verify. `renumber=False`
        exists only for the mutation test (rt_patch.py): the output must then fail check 5."""
        g = self.g
        removed, dropped = g.gc()
        untouched = check_untouched(self, removed, dropped)       # check 4, on the edit itself
        pre_apt, pre_cst, _ = g.emit(orig_tail=self.tail)         # the reference for check 5
        self.renumbered = g.renumber_walk_order() if renumber else None
        apt2, cst2, where = g.emit(orig_tail=self.tail)
        self.where = where
        checks = verify(self, apt2, cst2, where, pre=(pre_apt, pre_cst))
        checks.insert(3, untouched)
        return apt2, cst2, checks


def _block_eq(b1, b2, keymap=lambda k: k):
    """equal content; b2's pointer targets mapped through keymap."""
    if (b1.code is None) != (b2.code is None):
        return False
    if b1.code is not None:
        if len(b1.code) != len(b2.code):
            return False
        for x, y in zip(b1.code, b2.code):
            if x.op != y.op or set(x.f) != set(y.f):
                return False
            for fld, v in x.f.items():
                w = y.f[fld]
                if fld in ("s", "arr", "name", "params", "catch"):
                    if isinstance(v, list):
                        if [None if q is None else q for q in v] != [None if q is None else keymap(q) for q in w]:
                            return False
                    elif v != (None if w is None else keymap(w)):
                        return False
                elif bytes(v) != bytes(w) if isinstance(v, (bytes, bytearray)) else v != w:
                    return False
        return True
    if len(b1.data) != len(b2.data) or set(b1.ptrs) != set(b2.ptrs):
        return False
    d1, d2 = bytearray(b1.data), bytearray(b2.data)
    for rel in b1.ptrs:
        d1[rel:rel + 4] = d2[rel:rel + 4] = b"\0\0\0\0"
        if b1.ptrs[rel] != keymap(b2.ptrs[rel]):
            return False
    return d1 == d2


def check_untouched(p, removed, dropped):
    """check 4 (on the edited graph after gc, before renumber_walk_order): every original
    block the patch did not touch is unchanged, and every original .const item except the
    replaced ones. Raises PatchError; returns the check line."""
    g, g0 = p.g, p.g0
    missing = [k for k in g0.order if k not in g.blocks]
    changed = [k for k in g0.order if k in g.blocks and k not in p.touched and not _block_eq(g0.blocks[k], g.blocks[k])]
    items_changed = [i for i in range(len(g0.items)) if i not in p.replaced_items and g0.items[i] != g.items[i]]
    if missing or changed or items_changed or removed or dropped:
        raise PatchError(f"{p.name}: untouched content changed: removed {[_k(k) for k in missing[:5]]}, "
                         f"changed {[_k(k) for k in changed[:5]]}, items {items_changed[:5]}, gc {len(removed)}/{dropped}")
    return (f"untouched original blocks unchanged: {len(g0.order) - len(p.touched & set(g0.order))}"
            f"/{len(g0.order)} (touched {len(p.touched & set(g0.order))}), original .const items unchanged "
            f"{len(g0.items) - len(p.replaced_items)}/{len(g0.items)} (before the walk-order renumbering)")


def verify(p, apt2, cst2, where, pre=None):
    """checks 1-3 and 5 of the module docstring on the emitted bytes; raises PatchError on
    the first failure. `pre` = (apt, const) emitted before renumber_walk_order: every slot
    must resolve to the same constant as there (check 4 runs earlier, check_untouched)."""
    checks = []
    a2 = Apt(apt2, cst2)
    nz = coverage(a2)[3]
    if nz:
        raise PatchError(f"{p.name}: re-parse leaves {len(nz)} uncovered non-zero ranges {nz[:3]}")
    checks.append("re-parsed: 0 uncovered non-zero bytes")
    g2 = from_apt(a2)
    inv = {off: k for k, off in where.items()}
    g = p.g
    bad = [k for k in g.order if where[k] not in g2.blocks
           or not _block_eq(g.blocks[k], g2.blocks[where[k]], lambda t: inv.get(t, ("?", t)))]
    if bad or len(g2.order) != len(g.order) or g2.items != g.items:
        raise PatchError(f"{p.name}: re-parsed movie differs from the edited graph: {len(bad)} blocks "
                         f"{[_k(k) for k in bad[:5]]}, blocks {len(g2.order)} vs {len(g.order)}, "
                         f"items equal {g2.items == g.items}")
    checks.append(f"re-parsed movie == edited graph ({len(g.order)} blocks, {len(g.items)} .const items)")
    b3, c3, _ = g2.emit(orig_tail=len(apt2) - max(o + s for o, s, k in a2.blocks))
    if (b3, c3) != (apt2, cst2):
        raise PatchError(f"{p.name}: the edited movie does not round-trip through apt_graph")
    checks.append("edited movie round-trips byte-exact")
    before = apt_unload.resolved(*pre) if pre is not None else None
    ok, problems, st = apt_unload.check(apt2, cst2, before=before)
    if not ok:
        raise PatchError(f"{p.name}: the movie would not survive the runtime's unload + reload "
                         f"(apt_unload): " + " | ".join(problems))
    rn = getattr(p, "renumbered", None) or {}
    checks.append(f"unload walk: {st['slots']} slots == {st['items']} .const items, index == counter for every "
                  f"slot, every slot resolves the constant it named before the renumbering, a reload after a "
                  f"simulated unload resolves the same constants"
                  + (f" (renumbered: {rn['moved']} moved, {rn['duplicated']} duplicated, "
                     f"{rn['arrays_copied']} arrays copied)" if rn else ""))
    return checks


def patch_movie(apt, cst, ops, name="?"):
    """apply ops to one movie -> (apt2, cst2, Patcher). Raises PatchError."""
    p = Patcher(apt, cst, name)
    for op in ops:
        p.apply(op)
    apt2, cst2, checks = p.finish()
    p.checks = checks
    return apt2, cst2, p


def guard_open(code):
    """-> the index a block's opening guard starts at: after its ConstantPool."""
    return 1 if code and code[0].op == 0x88 else 0


def class_guard_body(code, j=None):
    """-> (lo, hi): the instruction range an AS2 class InitAction defines its class in - the body its opening guard
    `if (!_global.a.b.X) { ... }` (op_71, EA_GetNamedMember x N, Not, Not, If -> hi) skips once X exists - or None
    when the block does not open with that guard (then nothing in it is a guarded class method). j: where the guard
    starts (default: the block's opening; Patcher.guarded_body passes the tracked position)."""
    if j is None:
        j = guard_open(code)
    if j >= len(code) or code[j].op != 0x71:              # op_71: _global
        return None
    j += 1
    k = j
    while k < len(code) and code[k].op == 0xAF:           # EA_GetNamedMember, one per name component
        k += 1
    if k == j or k + 2 >= len(code) or code[k].op != 0x12 or code[k + 1].op != 0x12 or code[k + 2].op != 0x9D:
        return None
    tgt = code[k + 2].f.get("tgt")
    return (k + 3, tgt) if isinstance(tgt, int) and tgt > k + 2 else None


def movie_parts(outer, name):
    """-> (.apt, .const) bytes of the movie `name` (TOC name, any case) in a Bgfa, or None."""
    ent = next((e for e in outer.entries if e["name"].upper() == name.upper()), None)
    if ent is None:
        return None
    inner = Bgfa(ent["data"])
    return (next(x for x in inner.entries if x["tag"] == TAG_APT)["data"],
            next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"])


def class_exports(apt, cst):
    """-> the AS2 classes ('X' of an export '__Packages.X') a movie exports."""
    ctx = Ctx(from_apt(Apt(apt, cst)))
    return {n[len("__Packages."):] for names in ctx.exports.values() for n in names
            if n and n.startswith("__Packages.")}


def first_registrar_classes(outer):
    """-> the classes FIRST_REGISTRAR (MAIN) exports in this container (empty when it has no MAIN:
    a fixture container)."""
    parts = movie_parts(outer, FIRST_REGISTRAR)
    return class_exports(*parts) if parts else set()


def check_first_registrar(name, patcher, main_classes):
    """THE FIRST-REGISTRAR RULE: refuse a code op of movie `name` (not MAIN) that lands in a class
    MAIN registers first - that copy of the class never runs. Raises PatchError."""
    if name.upper() == FIRST_REGISTRAR:
        return
    for res in patcher.results:
        dead = [c for c in res.get("classes", ()) if c in main_classes]
        if dead:
            raise PatchError(
                f"{name}: {res['op']} {res.get('target', '?')} lands in class {dead[0]}, which {FIRST_REGISTRAR} "
                f"also exports (__Packages.{dead[0]}). {FIRST_REGISTRAR} is _level0 and registers its copy first "
                f"(if (!_global.{dead[0]})), so this movie's copy never runs: patch it in movie {FIRST_REGISTRAR} "
                f"(tools/apt/patch_movie.py, THE FIRST-REGISTRAR RULE)")


def patch_container(ui_bytes, sections):
    """apply movie sections to a whole ui.AST (bytes) -> (newBytes, [Patcher])."""
    outer = Bgfa(ui_bytes)
    by_name = {}
    for name, ops in sections:
        by_name.setdefault(name, []).extend(ops)
    done = []
    main_classes = first_registrar_classes(outer) if any(n != FIRST_REGISTRAR for n in by_name) else set()
    for name, ops in by_name.items():
        ents = [e for e in outer.entries if e["name"].upper() == name]
        if not ents:
            raise PatchError(f"no movie {name} in the container")
        ent = ents[0]
        inner = Bgfa(ent["data"])
        ea = next(x for x in inner.entries if x["tag"] == TAG_APT)
        ec = next(x for x in inner.entries if x["tag"] == TAG_CONST)
        apt2, cst2, p = patch_movie(ea["data"], ec["data"], ops, name)
        check_first_registrar(name, p, main_classes)
        # the movie's own container stores each entry size in inner.wSize bytes (2 for every retail
        # movie: 65,535 bytes max). Fail here, by name, before the writer's assertion (iter-466 map:
        # SCRMAP's .apt reached 65,644 and was trimmed to 64,792 - docs/MAP.md 4.5)
        limit = (1 << 32) - 1   # bgfa widens the size field (up to 4 bytes) when an entry outgrows it
        for what, data in ((".apt", apt2), (".const", cst2)):
            if len(data) > limit:
                raise PatchError(f"{name}{what} is {len(data):,} bytes after the patch; the movie's container "
                                 f"holds at most {limit:,} per entry ({inner.wSize}-byte size field). Trim the "
                                 f"patch code or move the addition into its own movie (docs/MAP.md 4.5)")
        ea["data"], ec["data"] = apt2, cst2
        ent["data"] = inner.rebuild()
        done.append(p)
    return outer.rebuild(), done           # always through the writer, even for an empty set


def game_folders(ui):
    """The folders no tool may write into, symlinks and junctions resolved: the default
    game folder, and for the ui.AST being read its own folder and - when that folder is
    the disc's DATA folder - the game root above it (E:/SR/DATA/ui.AST protects E:/SR)."""
    def norm(p):
        return os.path.normcase(os.path.realpath(os.path.abspath(p)))
    out = {norm(os.path.dirname(DATA_DIR))}
    d = norm(os.path.dirname(os.path.abspath(ui)))
    out.add(d)
    if os.path.basename(d.rstrip(os.sep)).lower() == "data":
        out.add(norm(os.path.dirname(d.rstrip(os.sep))))
    return out


def refuse_game_folder(out, ui):
    """An output path must never point into the game folder (the default one, or the
    folder of the ui.AST being read and its game root, or any folder under them): it is
    the user's read-only disc copy. Symlinks and junctions are resolved first. Used by
    patch_movie --out and build_custom_frontend (every output). Raises PatchError."""
    target = os.path.normcase(os.path.realpath(os.path.abspath(out)))
    for g in sorted(game_folders(ui)):
        if target == g or target.startswith(g.rstrip(os.sep) + os.sep):
            raise PatchError(f"refusing to write inside the game folder: {out} (under {g})")


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Apply a patch spec to Apt movies and verify.")
    ap.add_argument("patch")
    ap.add_argument("--ui", default=DEFAULT_UI)
    ap.add_argument("--out", help="directory for <MOVIE>.apt/.const + changed-block disassembly")
    args = ap.parse_args(argv)
    if args.out:
        refuse_game_folder(args.out, args.ui)
    sections = load_patch(args.patch)
    outer = Bgfa(open(args.ui, "rb").read())
    main_classes = first_registrar_classes(outer)
    for name, ops in sections:
        ent = next((e for e in outer.entries if e["name"].upper() == name), None)
        if ent is None:
            raise PatchError(f"no movie {name}")
        inner = Bgfa(ent["data"])
        apt = next(x for x in inner.entries if x["tag"] == TAG_APT)["data"]
        cst = next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"]
        apt2, cst2, p = patch_movie(apt, cst, ops, name)
        check_first_registrar(name, p, main_classes)
        print(f"== {name}: .apt {len(apt)} -> {len(apt2)} B, .const {len(cst)} -> {len(cst2)} B")
        for line in p.report:
            print("  " + line)
        for wn in p.warnings:
            print("  warning: " + wn)
        for c in p.checks:
            print("  [ok] " + c)
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            open(os.path.join(args.out, f"{name}.apt"), "wb").write(apt2)
            open(os.path.join(args.out, f"{name}.const"), "wb").write(cst2)
            g2 = from_apt(Apt(apt2, cst2))
            with open(os.path.join(args.out, f"{name}.changed.aptasm"), "w", encoding="utf-8") as fh:
                where = p.where
                for k in sorted({r["code"] for r in p.results if "code" in r}, key=lambda k: where[k]):
                    fh.write(aptasm.disassemble(g2, where[k], header=f"bytecode={_k(k)} -> {where[k]:#x}"))
            print(f"  wrote {args.out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_main(sys.argv[1:]))
    except (PatchError, aptasm.AsmError) as ex:
        sys.exit(f"patch_movie: {ex}")
