#!/usr/bin/env python3
"""abi_audit.py - whole-program register-convention audit for the XenonRecomp
register-as-local flags, and the ABI BRIDGE spec that makes them safe (iter-464 codegen lane).

WHY. The flags (cr/ctr/xer/reserved/non_argument/non_volatile _as_local) turn guest registers
into C++ locals of EACH recompiled function (zero at entry, invisible to callers/callees).
Under the PPC ABI that is exact: a function never reads a caller's register other than the
arguments, never leaves a result in a volatile non-argument register, and restores every
non-volatile register it writes. This binary has code that does not follow the ABI:
  - MSVC SEH __finally/__except funclets (CRT 0x8279F..0x827AF and game code): the parent
    does `addi r12,r31,176; bl funclet`, the funclet reads r12 and the PARENT's r30/r31, and
    some funclets hand a result back in the parent's r29/r11;
  - hand-written decoder asm (XMV/WMV/VC-1/WMA, 0x827C..0x8290): helpers that take inputs
    in r11/r15/r20.., keep VMX constants in v64+, nested two or three deep;
  - __chkstk-style helpers (r12 = frame size), D3D asm fragments (r11 inputs), the guest
    setjmp (reads every non-volatile register to save it), XenonAnalyse chunk duplicates
    and pointer-table prefixes lifted as functions (read r11/r14 at entry; never called).

WHAT. A forward must-define dataflow per function over the localized text (locals are bare
names r31/cr6/ctr/xer...), where a direct call kills the registers the callee leaves changed
(its own unsaved writes plus its callees', transitively):
  live_in(F)  = locals F reads on some path from entry before writing them. A call to a
                bridged G is a USE of IN(G) at the call site, so a pass-through register
                propagates to the callers that do not define it (the nested asm helpers);
                the closure stops at a caller that defines the register or that is
                ctx-style (see below).
  post-call   = a caller reads X after `call G` before redefining it, and G (transitively)
                changes X -> G returns a value in X: X in OUT(G). If G itself does not
                define X on a return path the value comes from the callee K that killed it
                (X in OUT(K), G reloads after K) or from G's entry (X in IN(G), pass-through).
Unsaved non-volatile writes by themselves are NOT bridged: with locals the clobber is
invisible to the caller, which is the ABI-correct behaviour; only an observed post-call
read makes it a convention. (The first version bridged every unsaved write and every
tail-call live-out and exploded to 18k functions through longjmp and the VMX leaves.)

CTX-STYLE FUNCTIONS. Hand bodies (splice_bodies.json), the __save*/__rest* helpers and the
synthetic carried functions are NOT localized: they keep the `ctx.X` form, so for them the
ctx fields ARE the registers. The audit parses them with every register defined at entry
(they never need a flush, reload or entry load - they read and write ctx natively) but with
their writes in the clobber summaries (a localized caller that reads X after calling a
ctx-style function that wrote ctx.X needs the reload). Their stack saves still count.

flag_transform.py then emits, at every direct call of a bridged G from a localized caller:
`ctx.X = X;` for X in IN(G) before the call and `X = ctx.X;` for X in OUT(G) after it, with
the non-volatile ctx fields saved and restored around the call (a ctx-style ancestor expects
them preserved, as the flags-off __restgprlr epilogues did); and in a localized G:
`X = ctx.X;` at entry for IN(G), `ctx.X = X;` before every return for OUT(G).

Reads that only SAVE a value to the stack frame (`std r31,-16(r1)`, stvx128 v127,r1,r12,
`stw r12,-8(r1)` after mflr) are BENIGN: the garbage is reloaded into the same local at the
epilogue and dropped at return. xer.so is never written by generated code (no mtxer/o-forms),
so compare()'s read of it is constant in both models.

    python tools/regen/abi_audit.py TREE [--base FLAGS_OFF_TREE] [--json out.json] [--bridge-out spec.json]
    (the deployed-tree run with hand bodies goes through flag_transform.py --solve)
"""
import argparse
import json
import os
import re
import sys

LOC = r"(?:ctr|xer|reserved|cr[0-7]|[rfv]\d{1,3})"
TOK = re.compile(r"(?<![\w.])(" + LOC + r")\b(?:\.(\w+))?")
FUNC_RE = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\) \{")
DECL = re.compile(r"^\t(?:PPCRegister|PPCXERRegister|PPCCRRegister|PPCVRegister) (" + LOC + r")\{\};$")
LABEL = re.compile(r"^(loc_[0-9A-F]+):$")
GOTO = re.compile(r"^\s*goto (loc_[0-9A-F]+);\s*$")
CGOTO = re.compile(r"^\s*if \((.*)\) goto (loc_[0-9A-F]+);\s*$")
CRET = re.compile(r"^\s*if \((.*)\) return;\s*$")
IFBLOCK = re.compile(r"^\s*if \((.*)\) \{\s*$")
RET = re.compile(r"^\s*return;\s*$")
SWITCH = re.compile(r"^\s*switch \((.*)\) \{\s*$")
ASSIGN = re.compile(r"^\s*(" + LOC + r")((?:\.\w+)(?:\[\d+\])?)?\s*(=|\|=|&=|\^=|\+=|-=)(?!=)")
INCDEC = re.compile(r"^\s*(?:--|\+\+)(" + LOC + r")\.")
METHOD_DEF = re.compile(r"^\s*(" + LOC + r")\.(compare|setFromMask)\b")
VSTORE_REG = re.compile(r"^\s*simde_mm_store_\w+\(\s*(?:\(simde__m128i\*\)\s*)?(" + LOC + r")\.")
STORE_MEM = re.compile(r"^\s*PPC_STORE_U(?:8|16|32|64)\((.*)\);\s*$")
VSTORE_MEM = re.compile(r"^\s*simde_mm_store_si128\(\(simde__m128i\*\)\(base \+ (?:PPC_GUEST_OFFSET)?\(\((.*?)\) & ~0xF\)\)")
CALL_RE = re.compile(r"^\s*((?:sub_[0-9A-F]+)|(?:__imp__\w+)|(?:__\w+))\(ctx, base\);\s*$")
HOOK_RE = re.compile(r"\b\w+MidAsmHook\w*\(")
VOLATILE = re.compile(r"^(?:r(?:0|2|11|12)|f0|v(?:3[2-9]|[45]\d|6[0-3])|cr[01567]|ctr|xer\.ca|reserved)$")
NONVOL = re.compile(r"^(?:r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|v(?:1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7])|cr[234])$")
HELPER = re.compile(r"^__(save|rest)(gprlr|fpr|vmx)_\d+$")
# every register the eight flags can localize, as the `ctx.X` token of a ctx-style body
CTX_LOCALIZABLE = re.compile(r"\bctx\.(r(?:0|2|11|12|1[4-9]|2\d|3[01])|f(?:0|1[4-9]|2\d|3[01])|"
                             r"v(?:1[4-9]|2\d|3[01]|3[2-9]|[4-9]\d|1[01]\d|12[0-7])|cr[0-7]|ctr|xer|reserved)\b")
ALL_KEYS = (["r%d" % i for i in (0, 2, 11, 12)] + ["r%d" % i for i in range(14, 32)]
            + ["f0"] + ["f%d" % i for i in range(14, 32)] + ["v%d" % i for i in range(14, 32)]
            + ["v%d" % i for i in range(32, 128)] + ["cr%d" % i for i in range(8)]
            + ["ctr", "xer.ca", "xer.so", "reserved"])
# Functions whose entry reads are SAVES into a guest buffer, not inputs: the guest setjmp
# (stores r1, r13-r31, f14-f31, LR, CR into the jmp_buf). Its only working counterpart is the
# registered C++-throw longjmp (splice sub_82762400 / srGuestLongjmpTryThrow), which restores
# the catching frame's locals by host unwinding, so the saved register values are never used.
SAVE_ONLY = {"sub_827A45E0"}
# Functions that never return to their caller in a working run: the guest longjmp (restores
# every non-volatile register from the jmp_buf, then "returns" to the setjmp site - which a
# recompiled call cannot do; the registered path throws SrGuestLongjmp instead). They clobber
# nothing a caller could observe, so they get an empty clobber summary.
NO_RETURN = {"sub_827A4270"}


def code_of(line):
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    return re.sub(r"//.*$", "", line)


def key_of(nm, fld):
    if nm == "xer":
        return "xer.ca" if fld == "ca" else "xer.so"   # compare() reads .so only
    return nm


def def_keys(nm, fld):
    if nm == "xer":
        if fld == "ca":
            return ["xer.ca"]
        if fld == "so":
            return ["xer.so"]
        return ["xer.ca", "xer.so"]
    return [nm]


def parse(lines, ctx_style=False):
    """Function body -> flat instruction list:
       ("label", name, ln) ("use", [(nm,fld,ln,pos)], ln) ("def", [(nm,fld)], ln)
       ("stmt", uses, defs, ln, save, mfcr) ("call", callee, ln, tail) ("jump"/"cjump", lbl, ln)
       ("multi", [lbls], rets, ln) ("ret", ln)
    ctx_style: the body keeps the `ctx.X` spelling (hand body / helper); its localizable
    tokens are read as the bare register names so uses, defs and saves parse the same way.
    The caller (solve) prepends the every-register-defined entry for such a function."""
    if ctx_style:
        lines = [CTX_LOCALIZABLE.sub(r"\1", ln) for ln in lines]
    ins, stack, synth = [], [], [0]

    def new_label():
        synth[0] += 1
        return "syn_%d" % synth[0]

    def toks(text, base=0):
        return [(t.group(1), t.group(2), base + t.start()) for t in TOK.finditer(text)]

    i, n = 0, len(lines)
    while i < n:
        code = code_of(lines[i]).rstrip()
        s = code.strip()
        i += 1
        if not s:
            continue
        m = LABEL.match(s)
        if m:
            ins.append(("label", m.group(1), i))
            continue
        if DECL.match(code) or s == "PPC_FUNC_PROLOGUE();":
            continue
        m = SWITCH.match(code)
        if m:
            # XenonRecomp jump-table switch: every case is `goto loc_X;` or `return;`.
            j, depth, targets, rets, pure = i, 1, [], False, True
            while j < n and depth:
                c = code_of(lines[j]).strip()
                j += 1
                if c.endswith("{"):
                    depth += 1
                if c.startswith("}"):
                    depth -= 1
                    continue
                g = GOTO.match(c)
                if g:
                    targets.append(g.group(1))
                elif RET.match(c):
                    rets = True
                elif c and not re.match(r"^(case \w+:|default:|__builtin_unreachable\(\);)$", c):
                    pure = False
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(m.group(1))], i))
            if pure:
                ins.append(("multi", targets, rets, j))
                i = j
                continue
            # hand-written switch with statements: treat the body as a conditional block
            skip = new_label()
            ins.append(("cjump", skip, i))
            stack.append(["if", skip])
            continue
        # ---- generic block structure (XenonRecomp if-blocks + hand code in splice bodies) ----
        if s.startswith("}"):
            if not stack:
                # falling off the end right after a call: the compiler emitted no blr because
                # it knew the callee never returns (assert/error helpers ending in longjmp)
                if ins and ins[-1][0] == "call" and not ins[-1][3]:
                    ins.append(("falloff", ins[-1][1], i))
                else:
                    ins.append(("ret", i))
                break
            rest = s[1:].strip()
            closes = 1
            while rest.startswith("}"):
                closes += 1
                rest = rest[1:].strip()
            for _ in range(closes - 1):
                if stack:
                    fr = stack.pop()
                    ins.append(("label", fr[1], i))
            fr = stack.pop() if stack else ["block", new_label()]
            nxt = code_of(lines[i]).strip() if i < n else ""
            cont = rest if rest.startswith(("else", "catch")) else (
                nxt if fr[0] == "if" and nxt.startswith("else") else "")
            if cont and fr[0] == "if":
                if cont is nxt:
                    i += 1
                end = new_label()
                ins.append(("jump", end, i))
                ins.append(("label", fr[1], i))
                ei = IFBLOCK.match(re.sub(r"^(else|catch)\s*", "", cont))
                if cont.startswith("else if") and ei:
                    ins.append(("use", [(a, b, i, p) for a, b, p in toks(ei.group(1))], i))
                    skip2 = new_label()
                    ins.append(("cjump", skip2, i))
                    stack.append(["elif", end, skip2])
                else:
                    stack.append(["else", end])
            elif fr[0] == "elif":
                # close of an `else if` body: its own skip label, then the shared end
                if cont:
                    if cont is nxt:
                        i += 1
                    ins.append(("jump", fr[1], i))
                    ins.append(("label", fr[2], i))
                    ei = IFBLOCK.match(re.sub(r"^else\s*", "", cont))
                    if cont.startswith("else if") and ei:
                        ins.append(("use", [(a, b, i, p) for a, b, p in toks(ei.group(1))], i))
                        skip3 = new_label()
                        ins.append(("cjump", skip3, i))
                        stack.append(["elif", fr[1], skip3])
                    else:
                        stack.append(["else", fr[1]])
                else:
                    ins.append(("label", fr[2], i))
                    ins.append(("label", fr[1], i))
            else:
                ins.append(("label", fr[1], i))
            continue
        m = IFBLOCK.match(code)
        if m:
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(m.group(1))], i))
            if HOOK_RE.search(m.group(1)):
                ins.append(("def", [(a, b) for a, b, _ in toks(m.group(1))], i))
            skip = new_label()
            ins.append(("cjump", skip, i))
            stack.append(["if", skip])
            continue
        if s.endswith("{"):
            if s in ("{", "do {", "try {"):
                stack.append(["block", new_label()])
                continue
            # for/while bodies, lambdas, multi-line conditions: may run zero times here
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(code)], i))
            skip = new_label()
            ins.append(("cjump", skip, i))
            stack.append(["if", skip])
            continue
        m = GOTO.match(code)
        if m:
            ins.append(("jump", m.group(1), i))
            continue
        m = CGOTO.match(code)
        if m:
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(m.group(1))], i))
            ins.append(("cjump", m.group(2), i))
            continue
        m = CRET.match(code)
        if m:
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(m.group(1))], i))
            skip = new_label()
            ins.append(("cjump", skip, i))
            ins.append(("ret", i))
            ins.append(("label", skip, i))
            continue
        if RET.match(code):
            ins.append(("ret", i))
            continue
        cm = CALL_RE.match(code)
        if cm:
            nxt = code_of(lines[i]).strip() if i < n else ""
            callee = cm.group(1)
            if callee.startswith("__imp__sub_"):
                callee = callee[len("__imp__"):]   # hand bodies call the impl directly
            ins.append(("call", callee, i, nxt == "return;"))
            continue
        if "PPC_CALL_INDIRECT_FUNC(" in code:
            ins.append(("use", [(a, b, i, p) for a, b, p in toks(code)], i))
            nxt = code_of(lines[i]).strip() if i < n else ""
            ins.append(("call", "<indirect>", i, nxt == "return;"))
            continue
        tk = toks(code)
        if not tk:
            continue
        defpos, compound = None, False
        a = ASSIGN.match(code)
        md = METHOD_DEF.match(code)
        vs = VSTORE_REG.match(code)
        idm = INCDEC.match(code)
        if a:
            defpos, compound = a.start(1), a.group(3) != "="
        elif md:
            defpos = md.start(1)
        elif vs:
            defpos = vs.start(1)
        elif idm:
            defpos, compound = idm.start(1), True
        uses, defs = [], []
        for nm, fld, pos in tk:
            if pos == defpos:
                defs.append((nm, fld))
                if compound:
                    uses.append((nm, fld, i, pos))
            else:
                uses.append((nm, fld, i, pos))
        if defpos is None and HOOK_RE.search(code):
            defs = [(nm, fld) for nm, fld, _ in tk]
        save = None
        sm = STORE_MEM.match(code)
        if sm:
            args, depth, cut = sm.group(1), 0, None
            for ci, ch in enumerate(args):
                depth += (ch == "(") - (ch == ")")
                if ch == "," and depth == 0:
                    cut = ci
                    break
            if cut is not None:
                addr = args[:cut]
                save = {"addr_regs": [t.group(1) for t in TOK.finditer(addr)],
                        "r1": "ctx.r1" in addr, "value_from": code.index(args) + cut}
        vm = VSTORE_MEM.match(code)
        if vm:
            save = {"addr_regs": [t.group(1) for t in TOK.finditer(vm.group(1))],
                    "r1": "ctx.r1" in vm.group(1), "value_from": vm.end(1)}
        mfcr = bool(re.match(r"^\s*(" + LOC + r")\.u64 \|?= cr[0-7]\.(lt|gt|eq|so) \? ", code))
        ins.append(("stmt", uses, defs, i, save, mfcr))
    return ins


def dataflow(ins, bridge, clob, noret=frozenset()):
    """Must-define dataflow. bridge: {callee: (IN, OUT)} - a direct call to a bridged callee
    USES IN at the call site and DEFINES OUT after it. clob: {callee: set} - the locals a
    direct call kills (registers the callee, transitively, leaves changed); an indirect call
    kills the ABI-volatile set. Returns dict with:
      entry {key: (ln, cls)}, postcall {callee: {key}}, writes {key}, saved {key},
      tails {callee}, calls {callee}, mustret (keys defined at every return) or None,
      rets [(defined keys, {key: killers}) at each return]"""
    labels = {x[1] for x in ins if x[0] == "label"}
    state_at = {lbl: None for lbl in labels}    # None = TOP (not reached yet)
    res = None
    for _round in range(80):
        changed = False
        cur, killed, reach = set(), {}, True
        entry, postcall, writes, saved, tails, calls = {}, {}, set(), set(), set(), set()
        mustret = None
        returns = False
        falloff_callees = set()
        rets = []

        def note(kk, ln, cls):
            if kk in killed:
                for c in killed[kk]:
                    postcall.setdefault(c, set()).add(kk)
                return
            prev = entry.get(kk)
            if prev is None or (prev[1] != "READ" and cls == "READ"):
                entry[kk] = (ln, cls)

        for x in ins:
            kind = x[0]
            if kind == "label":
                st = state_at[x[1]]
                if reach:
                    if st is not None:
                        cur = cur & st[0]
                        nk = {k: set(v) for k, v in st[1].items()}
                        for k, v in killed.items():
                            nk.setdefault(k, set()).update(v)
                        killed = {k: v for k, v in nk.items() if k not in cur}
                else:
                    if st is None:
                        reach = False
                        continue
                    cur, killed = set(st[0]), {k: set(v) for k, v in st[1].items()}
                reach = True
                continue
            if not reach:
                continue
            if kind == "use":
                for nm, fld, ln, pos in x[1]:
                    kk = key_of(nm, fld)
                    if kk not in cur:
                        note(kk, ln, "READ")
                continue
            if kind == "def":
                for nm, fld in x[1]:
                    for kk in def_keys(nm, fld):
                        cur.add(kk)
                        killed.pop(kk, None)
                        writes.add(kk)
                continue
            if kind == "entrydef":
                # ctx-style function: every register is defined at entry (the ctx field IS
                # the register); not a write of the function itself
                cur.update(x[1])
                continue
            if kind == "call":
                callee, ln, tail = x[1], x[2], x[3]
                calls.add(callee)
                if tail:
                    tails.add(callee)
                if callee in bridge:
                    # the flush of IN(callee) at this call site USES the register: a value
                    # this function never defined is a pass-through from its own entry
                    # (nested asm helpers). One a previous callee clobbered is left as the
                    # caller's local: the flags-off value would have been that callee's
                    # leftover, which only a garbage read could have wanted.
                    B, OUT = bridge[callee]
                    for kk in B:
                        if kk not in cur and kk not in killed:
                            note(kk, ln, "READ")
                # the callee kills every register it (transitively) changes, defined here or
                # not: a later read of it is the callee's value in the flags-off model
                if callee == "<indirect>":
                    kills = [kk for kk in ALL_KEYS if VOLATILE.match(kk)]
                else:
                    kills = clob.get(callee, ())
                for kk in kills:
                    cur.discard(kk)
                    killed.setdefault(kk, set()).add(callee)
                if callee in bridge:
                    for kk in bridge[callee][1]:
                        cur.add(kk)
                        killed.pop(kk, None)
                if callee in noret:
                    reach = False          # the call never returns: the path ends here
                continue
            if kind == "falloff":
                falloff_callees.add(x[1])
                reach = False
                continue
            if kind == "stmt":
                save, mfcr = x[4], x[5]
                for nm, fld, ln, pos in x[1]:
                    kk = key_of(nm, fld)
                    if kk not in cur:
                        cls = "READ"
                        if mfcr:
                            cls = "mfcr"
                            saved.add(kk)      # CR save word (mfcr) covers cr2-cr4
                        elif save and pos >= save["value_from"]:
                            if save["r1"] or (save["addr_regs"] and all(
                                    key_of(r, None) in cur for r in save["addr_regs"])):
                                cls = "benign-save"
                                saved.add(kk)
                        note(kk, ln, cls)
                for nm, fld in x[2]:
                    for kk in def_keys(nm, fld):
                        cur.add(kk)
                        killed.pop(kk, None)
                        writes.add(kk)
                continue
            targets = []
            if kind in ("jump", "cjump"):
                targets = [x[1]]
            elif kind == "multi":
                targets = x[1]
            for t in targets:
                if t not in state_at:
                    continue
                old = state_at[t]
                if old is None:
                    state_at[t] = (set(cur), {k: set(v) for k, v in killed.items()})
                    changed = True
                else:
                    nd = old[0] & cur
                    nk = {k: set(v) for k, v in old[1].items()}
                    for k, v in killed.items():
                        nk.setdefault(k, set()).update(v)
                    nk = {k: v for k, v in nk.items() if k not in nd}
                    if nd != old[0] or nk != old[1]:
                        state_at[t] = (nd, nk)
                        changed = True
            if kind == "ret" or (kind == "multi" and x[2]):
                mustret = set(cur) if mustret is None else (mustret & cur)
                returns = True
                rets.append((set(cur), {k: set(v) for k, v in killed.items()}))
            if kind in ("jump", "ret", "multi"):
                reach = False
        res = {"entry": entry, "postcall": postcall, "writes": writes, "saved": saved,
               "tails": tails, "calls": calls, "mustret": mustret or set(),
               "returns": returns, "falloff_callees": falloff_callees, "rets": rets}
        if not changed:
            break
    return res


def split_functions(text):
    """{fn: body lines} of one generated .cpp text (body = lines after the header line,
    through the closing brace)."""
    lines = text.split("\n")
    out = {}
    k = 0
    while k < len(lines):
        m = FUNC_RE.match(lines[k])
        if not m:
            k += 1
            continue
        body, depth = [], 1
        k += 1
        while k < len(lines) and depth:
            c = code_of(lines[k])
            depth += c.count("{") - c.count("}")
            body.append(lines[k])
            k += 1
        out[m.group(1)] = body
    return out


def iter_functions(d):
    for fname in sorted(os.listdir(d)):
        if not (fname.startswith("ppc_recomp.") and fname.endswith(".cpp")):
            continue
        lines = open(os.path.join(d, fname), encoding="utf-8", errors="replace").read().split("\n")
        k = 0
        while k < len(lines):
            m = FUNC_RE.match(lines[k])
            if not m:
                k += 1
                continue
            body, depth = [], 1
            k += 1
            while k < len(lines) and depth:
                c = code_of(lines[k])
                depth += c.count("{") - c.count("}")
                body.append(lines[k])
                k += 1
            yield fname, m.group(1), body


SAVE_CALL = re.compile(r"^\s*__save(gprlr|fpr|vmx)_(\d+)\(ctx, base\);")


def helper_saves(base):
    """flags-off tree (a directory, or {fn: body lines}): {fn: set(saved nonvol keys)}
    from __save* helper calls."""
    out = {}
    items = base.items() if isinstance(base, dict) else ((fn, b) for _, fn, b in iter_functions(base))
    for fn, body in items:
        s = set()
        for ln in body:
            m = SAVE_CALL.match(ln)
            if not m:
                continue
            kind, k = m.group(1), int(m.group(2))
            if kind == "gprlr":
                s.update("r%d" % i for i in range(k, 32))
            elif kind == "fpr":
                s.update("f%d" % i for i in range(k, 32))
            elif k < 64:
                s.update("v%d" % i for i in range(k, 32))
            else:
                s.update("v%d" % i for i in range(k, 128))
        if s:
            out[fn] = s
    return out


def classify_reg(kk):
    if kk.startswith("cr"):
        return "cr"
    if kk in ("ctr", "reserved") or kk.startswith("xer"):
        return kk
    m = re.match(r"([rfv])(\d+)", kk)
    return ("nonvol-" if NONVOL.match(kk) else "nonarg-") + m.group(1)


def solve(funcs, hsaves=None, ctx_fns=frozenset(), max_iter=60, log=print, saved_override=None):
    """Whole-program fixed point.
    funcs:   {fn: parse(body)} - localized bodies parsed plainly, ctx-style bodies parsed with
             ctx_style=True (solve prepends their every-register-defined entry)
    hsaves:  {fn: set(nonvol keys)} saved by __save* helper calls (helper_saves of the
             ctx-style tree)
    ctx_fns: the ctx-style functions (hand bodies, __save*/__rest* helpers, synthetics)
    saved_override: {fn: set} - inline stack saves of ctx-style functions, measured on the
             plain parse (with every register defined at entry the save classification
             cannot fire)
    Returns (results, bridge) with bridge = {fn: (IN, OUT)}; solve.noret / solve.clob /
    solve.unbridged / solve.callers record the pre-passes and the diagnostics."""
    hsaves = hsaves or {}
    # a ctx-style function's inline stack saves, measured on the plain parse (with every
    # register defined at entry the save classification cannot fire)
    saved_override = dict(saved_override or {})
    for fn in ctx_fns:
        if fn in funcs and fn not in saved_override:
            saved_override[fn] = dataflow(funcs[fn], {}, {}, set(NO_RETURN))["saved"]
    ALL = [("entrydef", list(ALL_KEYS), 0)]
    funcs = {fn: (ALL + ins if fn in ctx_fns else ins) for fn, ins in funcs.items()}
    # (A) no-return: no reachable explicit return, or the compiler fell off the end after it
    noret = set(NO_RETURN)
    for it in range(50):
        res = {fn: dataflow(ins, {}, {}, noret) for fn, ins in funcs.items()}
        nnr = set(noret)
        for fn, r in res.items():
            if not r["returns"]:
                nnr.add(fn)
            nnr |= {c for c in r["falloff_callees"] if c in funcs}
        log("[ABI_BRIDGE] no-return pass %d: %d functions" % (it, len(nnr)))
        if nnr == noret:
            break
        noret = nnr
    # (B) clobbers: own writes + callees' clobbers, minus the non-volatiles F saves itself.
    # The __save*/__rest* helpers are the prologue/epilogue protocol, not clobbers.
    VOL_ALL = {"r0", "r2", "r11", "r12", "f0", "cr0", "cr1", "cr5", "cr6", "cr7", "ctr",
               "xer.ca", "reserved"} | {"v%d" % i for i in range(32, 64)}
    own, sv = {}, {}
    for fn, r in res.items():
        sv[fn] = {k for k in (r["saved"] | hsaves.get(fn, set()) | saved_override.get(fn, set()))
                  if NONVOL.match(k)}
        o = set(r["writes"])
        if "<indirect>" in r["calls"]:
            o |= VOL_ALL
        own[fn] = set() if (fn in NO_RETURN or HELPER.match(fn)) else o
    clob = {fn: own[fn] - sv[fn] for fn in funcs}
    for _ in range(200):
        grew = False
        for fn, r in res.items():
            if fn in NO_RETURN or HELPER.match(fn):
                continue
            acc = set(clob[fn])
            for c in r["calls"]:
                if c in clob and c not in noret:
                    acc |= clob[c] - sv[fn]
            if acc != clob[fn]:
                clob[fn] = acc
                grew = True
        if not grew:
            break
    # (C) IN closure + (D) observed OUT, to a joint fixed point
    callers = {}
    for fn, r in res.items():
        for c in r["calls"]:
            callers.setdefault(c, set()).add(fn)

    def bridgeable(fn):
        return fn in funcs and not HELPER.match(fn) and fn not in SAVE_ONLY

    # Monotone: IN / OUT / pass-through sets only grow across passes (a bridge added in one
    # pass changes which reads the next pass sees; recomputing from scratch oscillated).
    live_in, new_out, new_pass = {}, {}, {}
    bridge = {}
    results = dict(res)
    dirty = set(funcs)
    for it in range(max_iter):
        for fn in dirty:
            results[fn] = dataflow(funcs[fn], bridge, clob, noret)
        for fn, r in results.items():
            if not bridgeable(fn) or fn in ctx_fns:
                continue
            li = {k for k, (ln, cls) in r["entry"].items() if cls == "READ" and k != "xer.so"}
            if li:
                live_in.setdefault(fn, set()).update(li)
        # OUT seeds: a direct post-call read of X after `call G` where G changes X
        for fn, r in results.items():
            for callee, keys in r["postcall"].items():
                if callee != "<indirect>" and bridgeable(callee):
                    new_out.setdefault(callee, set()).update(k for k in keys if k != "xer.so")
        # OUT propagation: where does G's X at return come from?
        for _ in range(50):
            grew = False
            for g in list(new_out):
                for k in list(new_out[g]):
                    for cur, killed in results[g]["rets"]:
                        if k in cur:
                            continue
                        if k in killed:
                            for c in killed[k]:
                                if c != "<indirect>" and bridgeable(c) and k not in new_out.get(c, ()):
                                    new_out.setdefault(c, set()).add(k)
                                    grew = True
                        elif g not in ctx_fns:
                            new_pass.setdefault(g, set()).add(k)
            if not grew:
                break
        nb = {}
        for fn in set(live_in) | set(new_out) | set(new_pass):
            nb[fn] = (live_in.get(fn, set()) | new_pass.get(fn, set()), new_out.get(fn, set()))
        changed_fns = {fn for fn in set(nb) | set(bridge) if nb.get(fn) != bridge.get(fn)}
        log("[ABI_BRIDGE] bridge pass %d: %d bridged functions (%d with IN, %d with OUT), %d changed"
            % (it, len(nb), sum(1 for b in nb.values() if b[0]), sum(1 for b in nb.values() if b[1]),
               len(changed_fns)))
        bridge, OUT, IN_pass = nb, new_out, new_pass
        if not changed_fns:
            break
        dirty = set()
        for fn in changed_fns:
            dirty |= callers.get(fn, set())
            dirty.add(fn)
        dirty &= set(funcs)
    # diagnostic: IN roots nobody calls directly (chunk duplicates, SEH filters, vtable-only
    # targets). They get the entry loads from ctx and a one-shot log if they ever run. (A
    # flush of a register the caller has not defined cannot happen elsewhere: that caller
    # would itself have the register as live-in and load it at entry.)
    unbridged = {fn for fn, (IN, O) in bridge.items()
                 if IN and not any(c != fn for c in callers.get(fn, ()))}
    solve.noret = noret
    solve.clob = clob
    solve.unbridged = unbridged
    solve.callers = callers
    return results, bridge


def solve_tree(tree, base=None, max_iter=60, log=print):
    """CLI form: a flags-ON regen directory (no hand bodies) + the flags-OFF directory for
    the __save* coverage."""
    funcs = {fn: parse(body) for _, fn, body in iter_functions(tree)}
    hsaves = helper_saves(base) if base else {}
    results, bridge = solve(funcs, hsaves, max_iter=max_iter, log=log)
    return funcs, results, bridge


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--base", help="flags-OFF tree of the same XEX (for __save* helper coverage)")
    ap.add_argument("--json")
    ap.add_argument("--bridge-out")
    ap.add_argument("--show", type=int, default=60)
    a = ap.parse_args()
    funcs, results, bridge = solve_tree(a.tree, a.base)
    by_cls = {}
    for fn, r in results.items():
        for kk, (ln, cls) in r["entry"].items():
            c = "helper" if HELPER.match(fn) else ("benign-const" if kk == "xer.so" else cls)
            by_cls.setdefault((classify_reg(kk), c), set()).add(fn)
    print("[ABI_AUDIT] %d functions scanned in %s" % (len(funcs), a.tree))
    for (c, cls), fns in sorted(by_cls.items()):
        print("  %-12s %-13s %6d functions" % (c, cls, len(fns)))
    nin = sum(1 for b in bridge.values() if b[0])
    nout = sum(1 for b in bridge.values() if b[1])
    regs = {}
    for fn, (B, OUT) in bridge.items():
        for k in B:
            regs[classify_reg(k)] = regs.get(classify_reg(k), 0) + 1
    print("[ABI_BRIDGE] %d bridged functions (%d with IN, %d with OUT, %d unbridged roots); per class: %s"
          % (len(bridge), nin, nout, len(solve.unbridged), ", ".join("%s=%d" % kv for kv in sorted(regs.items()))))
    ind = {}
    for fn, r in results.items():
        if "<indirect>" in r["postcall"]:
            ind[fn] = sorted(r["postcall"]["<indirect>"])
    print("[ABI_BRIDGE] post-INDIRECT-call volatile reads (cannot be bridged by callee): %d functions" % len(ind))
    for fn in sorted(ind)[:a.show]:
        print("    %s reads %s after an indirect call" % (fn, ",".join(ind[fn])))
    for fn in sorted(bridge)[:a.show]:
        B, OUT = bridge[fn]
        print("  %s IN=%s OUT=%s" % (fn, ",".join(sorted(B)), ",".join(sorted(OUT))))
    if a.bridge_out:
        json.dump({fn: {"in": sorted(B), "out": sorted(OUT)} for fn, (B, OUT) in sorted(bridge.items())},
                  open(a.bridge_out, "w"), indent=1)
    if a.json:
        json.dump({fn: {"entry": {k: list(v) for k, v in r["entry"].items()},
                        "postcall": {k: sorted(v) for k, v in r["postcall"].items()}}
                   for fn, r in results.items() if r["entry"] or r["postcall"]},
                  open(a.json, "w"), indent=0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
