#!/usr/bin/env python3
"""as2asm.py - compile a small ActionScript-2 subset into aptasm (tools/apt/aptasm.py) text.

iter-465 viewer lane: the COSTUME VIEWER page is real UI code (layout, a scrolling list, an info
panel); hand-writing it as stack assembly is error-prone, so its source is AS-like text and this
tool emits the aptasm the patch files splice in. Generated .aptasm files say so in their header and
are rebuilt from their .as source (python tools/apt/as2asm.py SRC.as OUT.aptasm).

Subset (what the page needs, nothing more):
  statements  var x [= e];  x = e;  a.b = e;  a[i] = e;  e;  if (c) S [else S]  while (c) S
              return [e];  delete a.b;  { ... }
  expressions numbers, 'strings' / "strings", true false undefined null this _global,
              locals, other names (a variable lookup), a.b, a[e], calls f(...) / a.b(...),
              new Name(...), [a, b], function (p, q) { ... }, unary ! -, binary * / % + -
              < > <= >= == != && || (short-circuit), ( e ), fscommand('CMD', e)
Code shape: every name and string is a Push of a fresh .const item (no ConstantPool use, so a
snippet never grows the host block's pool past its byte index); DefineFunction2 with r1 = this,
r2 = _global, parameters from r3 and locals after them (retail flags 0x129 | nRegs << 16).
Top-level code (an action block) may use `_global`, names and locals (registers 1..3).
"""
import re
import sys

TOKEN = re.compile(r"""\s*(?:(//[^\n]*)|(/\*.*?\*/)|(\d+\.\d*|\.\d+|\d+)|('(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")|
                     ([A-Za-z_$][A-Za-z0-9_$]*)|(==|!=|<=|>=|&&|\|\||[-+*/%<>!=(){}\[\];,.]))""", re.S | re.X)


class CompileError(Exception):
    pass


def tokenize(src):
    out, pos = [], 0
    while pos < len(src):
        m = TOKEN.match(src, pos)
        if not m or m.end() == pos:
            if src[pos:].strip() == "":
                break
            raise CompileError("bad character at %r" % src[pos:pos + 20])
        pos = m.end()
        if m.group(1) or m.group(2):
            continue
        if m.group(3):
            out.append(("num", m.group(3)))
        elif m.group(4):
            s = m.group(4)[1:-1]
            s = re.sub(r"\\(.)", lambda mm: {"n": "\n", "t": "\t"}.get(mm.group(1), mm.group(1)), s)
            out.append(("str", s))
        elif m.group(5):
            out.append(("id", m.group(5)))
        elif m.group(6):
            out.append(("op", m.group(6)))
    out.append(("eof", ""))
    return out


# ── AST ───────────────────────────────────────────────────────────────────────────────────
class P:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def take(self, kind=None, val=None):
        tk = self.t[self.i]
        if (kind and tk[0] != kind) or (val is not None and tk[1] != val):
            raise CompileError("expected %s %r, got %r (token %d)" % (kind, val, tk, self.i))
        self.i += 1
        return tk

    def at(self, val):
        return self.peek()[0] in ("op", "id") and self.peek()[1] == val

    def program(self):
        out = []
        while self.peek()[0] != "eof":
            out.append(self.stmt())
        return out

    def block(self):
        if self.at("{"):
            self.take("op", "{")
            out = []
            while not self.at("}"):
                out.append(self.stmt())
            self.take("op", "}")
            return ("block", out)
        return self.stmt()

    def stmt(self):
        if self.at("var"):
            self.take()
            name = self.take("id")[1]
            init = None
            if self.at("="):
                self.take()
                init = self.expr()
            self.take("op", ";")
            return ("var", name, init)
        if self.at("if"):
            self.take()
            self.take("op", "(")
            c = self.expr()
            self.take("op", ")")
            a = self.block()
            b = None
            if self.at("else"):
                self.take()
                b = self.block()
            return ("if", c, a, b)
        if self.at("while"):
            self.take()
            self.take("op", "(")
            c = self.expr()
            self.take("op", ")")
            return ("while", c, self.block())
        if self.at("delete"):
            self.take()
            t = self.expr()
            self.take("op", ";")
            if t[0] != "member":
                raise CompileError("delete needs a.b")
            return ("delete", t)
        if self.at("return"):
            self.take()
            e = None if self.at(";") else self.expr()
            self.take("op", ";")
            return ("return", e)
        if self.at("{"):
            return self.block()
        e = self.expr()
        if self.at("="):
            self.take()
            v = self.expr()
            self.take("op", ";")
            return ("assign", e, v)
        self.take("op", ";")
        return ("expr", e)

    def expr(self):
        return self.binary(0)

    LEVELS = [["||"], ["&&"], ["==", "!="], ["<", ">", "<=", ">="], ["+", "-"], ["*", "/", "%"]]

    def binary(self, lvl):
        if lvl == len(self.LEVELS):
            return self.unary()
        left = self.binary(lvl + 1)
        while self.peek()[0] == "op" and self.peek()[1] in self.LEVELS[lvl]:
            op = self.take()[1]
            left = ("bin", op, left, self.binary(lvl + 1))
        return left

    def unary(self):
        if self.at("!"):
            self.take()
            return ("not", self.unary())
        if self.at("-"):
            self.take()
            return ("neg", self.unary())
        return self.postfix()

    def args(self, close):
        out = []
        while not self.at(close):
            out.append(self.expr())
            if not self.at(close):
                self.take("op", ",")
        self.take("op", close)
        return out

    def postfix(self):
        e = self.primary()
        while True:
            if self.at("."):
                self.take()
                e = ("member", e, ("str", self.take("id")[1]))
            elif self.at("["):
                self.take()
                k = self.expr()
                self.take("op", "]")
                e = ("member", e, k)
            elif self.at("("):
                self.take()
                e = ("call", e, self.args(")"))
            else:
                return e

    def primary(self):
        kind, v = self.peek()
        if kind == "num":
            self.take()
            return ("num", float(v) if "." in v else int(v))
        if kind == "str":
            self.take()
            return ("str", v)
        if self.at("("):
            self.take()
            e = self.expr()
            self.take("op", ")")
            return e
        if self.at("["):
            self.take()
            return ("array", self.args("]"))
        if kind == "id":
            self.take()
            if v in ("true", "false", "undefined", "null", "this", "_global"):
                return ("lit", v)
            if v == "new":
                name = self.take("id")[1]
                self.take("op", "(")
                return ("new", name, self.args(")"))
            if v == "function":
                self.take("op", "(")
                params = []
                while not self.at(")"):
                    params.append(self.take("id")[1])
                    if not self.at(")"):
                        self.take("op", ",")
                self.take("op", ")")
                return ("func", params, self.block())
            return ("name", v)
        raise CompileError("unexpected %r" % (self.peek(),))


# ── code generation ───────────────────────────────────────────────────────────────────────
class Scope:
    def __init__(self, params, top=False):
        self.top = top
        self.regs = {}
        nxt = 1 if top else 3
        for p in params:
            self.regs[p] = nxt
            nxt += 1
        self.next = nxt

    def local(self, name):
        if name not in self.regs:
            self.regs[name] = self.next
            self.next += 1
        return self.regs[name]


class Gen:
    def __init__(self, prefix):
        self.prefix = prefix
        self.n = 0

    def label(self):
        self.n += 1
        return "%s_%d" % (self.prefix, self.n)

    @staticmethod
    def q(s):
        if "'" not in s:
            return "'%s'" % s
        if '"' not in s:
            return '"%s"' % s
        raise CompileError("string holds both quote kinds: %r" % s)

    def push_str(self, out, s):
        out.append("Push [%s]" % self.q(s))

    def num(self, out, v):
        if isinstance(v, float) and not v.is_integer():
            out.append("EA_PushFloat %r" % v)
            return
        v = int(v)
        if v == 0:
            out.append("EA_PushZero")
        elif v == 1:
            out.append("EA_PushOne")
        elif -128 <= v <= 127:
            out.append("EA_PushByte %d" % v)
        elif -32768 <= v <= 32767:
            out.append("EA_PushShort %d" % v)
        else:
            out.append("EA_PushLong %d" % v)

    def expr(self, e, sc, out):
        k = e[0]
        if k == "num":
            self.num(out, e[1])
        elif k == "str":
            self.push_str(out, e[1])
        elif k == "lit":
            v = e[1]
            if v == "this":
                if sc.top:
                    raise CompileError("'this' at top level")
                out.append("Push [reg1]")
            elif v == "_global":
                out.append("op_71" if sc.top else "Push [reg2]")
            else:
                out.append({"true": "op_73", "false": "op_74", "null": "op_75", "undefined": "op_76"}[v])
        elif k == "name":
            if e[1] in sc.regs:
                out.append("Push [reg%d]" % sc.regs[e[1]])
            else:
                self.push_str(out, e[1])
                out.append("GetVariable")
        elif k == "member":
            self.expr(e[1], sc, out)
            self.expr(e[2], sc, out)
            out.append("GetMember")
        elif k == "call":
            self.call(e, sc, out)
        elif k == "new":
            for a in reversed(e[2]):
                self.expr(a, sc, out)
            self.num(out, len(e[2]))
            self.push_str(out, e[1])
            out.append("NewObject")
        elif k == "array":
            for a in reversed(e[1]):
                self.expr(a, sc, out)
            self.num(out, len(e[1]))
            out.append("InitArray")
        elif k == "func":
            self.func(e, out)
        elif k == "not":
            self.expr(e[1], sc, out)
            out.append("Not")
        elif k == "neg":
            self.num(out, 0)
            self.expr(e[1], sc, out)
            out.append("Subtract")
        elif k == "bin":
            op = e[1]
            if op in ("&&", "||"):
                end = self.label()
                self.expr(e[2], sc, out)
                out.append("PushDuplicate")
                if op == "&&":
                    out.append("Not")
                out.append("If %s" % end)
                out.append("Pop")
                self.expr(e[3], sc, out)
                out.append("%s:" % end)
                return
            self.expr(e[2], sc, out)
            self.expr(e[3], sc, out)
            out.extend({"+": ["Add2"], "-": ["Subtract"], "*": ["Multiply"], "/": ["Divide"], "%": ["Modulo"],
                        "<": ["Less2"], ">": ["Greater"], "<=": ["Greater", "Not"], ">=": ["Less2", "Not"],
                        "==": ["Equals2"], "!=": ["Equals2", "Not"]}[op])
        else:
            raise CompileError("cannot evaluate %r" % (k,))

    def call(self, e, sc, out):
        callee, args = e[1], e[2]
        if callee == ("name", "fscommand"):
            if len(args) != 2 or args[0][0] != "str":
                raise CompileError("fscommand('NAME', arg)")
            self.push_str(out, "FSCommand:" + args[0][1])
            self.expr(args[1], sc, out)
            out.append("GetURL2")
            out.append("op_76")   # fscommand has no value; keep the expression-statement Pop balanced
            return
        for a in reversed(args):
            self.expr(a, sc, out)
        self.num(out, len(args))
        if callee[0] == "member":
            self.expr(callee[1], sc, out)
            self.expr(callee[2], sc, out)
            out.append("CallMethod")
        elif callee[0] == "name" and callee[1] not in sc.regs:
            self.push_str(out, callee[1])
            out.append("CallFunction")
        else:   # a function value: CallMethod with an undefined name calls the object itself
            self.expr(callee, sc, out)
            out.append("op_76")
            out.append("CallMethod")

    def func(self, e, out):
        params, body = e[1], e[2]
        sc = Scope(params)
        inner = []
        self.stmt(body, sc, inner)
        nregs = max(sc.next, 3)
        plist = ", ".join("r%d %s" % (sc.regs[p], self.q(p)) for p in params)
        out.append(".func2 ''(%s) flags=0x%X" % (plist, (nregs << 16) | 0x129))
        out.extend("    " + x for x in inner)
        out.append(".end")

    def stmt(self, s, sc, out):
        k = s[0]
        if k == "block":
            for x in s[1]:
                self.stmt(x, sc, out)
        elif k == "var":
            r = sc.local(s[1])
            if sc.top and r > 3:
                raise CompileError("top level: at most 3 locals")
            if s[2] is not None:
                self.expr(s[2], sc, out)
                out.append("StoreRegister %d" % r)
                out.append("Pop")
        elif k == "assign":
            t, v = s[1], s[2]
            if t[0] == "name":
                if t[1] in sc.regs:
                    self.expr(v, sc, out)
                    out.append("StoreRegister %d" % sc.regs[t[1]])
                    out.append("Pop")
                else:
                    self.push_str(out, t[1])
                    self.expr(v, sc, out)
                    out.append("SetVariable")
            elif t[0] == "member":
                self.expr(t[1], sc, out)
                self.expr(t[2], sc, out)
                self.expr(v, sc, out)
                out.append("SetMember")
            else:
                raise CompileError("cannot assign to %r" % (t[0],))
        elif k == "expr":
            self.expr(s[1], sc, out)
            out.append("Pop")
        elif k == "if":
            els, end = self.label(), self.label()
            self.expr(s[1], sc, out)
            out.append("Not")
            out.append("If %s" % els)
            self.stmt(s[2], sc, out)
            if s[3] is not None:
                out.append("Jump %s" % end)
            out.append("%s:" % els)
            if s[3] is not None:
                self.stmt(s[3], sc, out)
                out.append("%s:" % end)
        elif k == "while":
            top, end = self.label(), self.label()
            out.append("%s:" % top)
            self.expr(s[1], sc, out)
            out.append("Not")
            out.append("If %s" % end)
            self.stmt(s[2], sc, out)
            out.append("Jump %s" % top)
            out.append("%s:" % end)
        elif k == "delete":
            self.expr(s[1][1], sc, out)
            self.expr(s[1][2], sc, out)
            out.append("Delete")
            out.append("Pop")
        elif k == "return":
            if s[1] is None:
                out.append("op_76")
            else:
                self.expr(s[1], sc, out)
            out.append("Return")
        else:
            raise CompileError("unknown statement %r" % (k,))


def compile_text(src, prefix="sv"):
    prog = P(tokenize(src)).program()
    g = Gen(prefix)
    out = []
    sc = Scope([], top=True)
    for s in prog:
        g.stmt(s, sc, out)
    return out


def case_collisions(src):
    """Member names (after `this.` / `m.`) that differ only by case. Property lookup in this Apt is
    case-insensitive (iter-466 map: `this.endS = clip` replaced the method `Ends`), so two such names
    are one property at run time."""
    seen = {}
    for name in re.findall(r"\b(?:this|m)\.([A-Za-z_]\w*)", src):
        seen.setdefault(name.lower(), set()).add(name)
    return sorted(sorted(v) for v in seen.values() if len(v) > 1)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src = open(sys.argv[1], encoding="utf-8").read()
    clash = case_collisions(src)
    if clash:
        print("as2asm: member names that differ only by case are ONE property in Apt: %s" % clash)
        return 1
    prefix = sys.argv[3] if len(sys.argv) > 3 else re.sub(r"\W", "_", sys.argv[1].replace("\\", "/").split("/")[-1].split(".")[0])
    lines = compile_text(src, prefix)
    head = ["; GENERATED by tools/apt/as2asm.py from %s - edit the .as source, then rebuild:" % sys.argv[1].replace("\\", "/").split("/")[-1],
            ";   python tools/apt/as2asm.py %s %s" % (sys.argv[1].replace("\\", "/"), sys.argv[2].replace("\\", "/"))]
    open(sys.argv[2], "w", encoding="utf-8", newline="\n").write("\n".join(head + lines) + "\n")
    print("%s: %d lines" % (sys.argv[2], len(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
