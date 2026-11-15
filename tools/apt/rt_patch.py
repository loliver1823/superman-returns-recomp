#!/usr/bin/env python3
"""rt_patch.py - regression test for patch_movie.py (every op, on real movies) and
build_custom_frontend.py (form A), against the user's own read-only ui.AST.

Expectations are computed from the INDEPENDENT reader (apt_model walker + apt_asm.disasm,
branch targets and function ends turned into instruction indices), never from
aptasm/apt_graph, the code under test. The splice spec the test encodes:
  prepend: the snippet sits at the body start; every reference to an index >= that
           point moves by the snippet length (so branches keep their targets);
  append : the snippet sits at the body end; references to that point from INSIDE the
           body (branches, nested function ends) now land on the snippet, the target
           function's own end moves past it, everything else moves.
Checks: no-op patches byte-identical; replace-const; prepend/append to a function in
ScrPressStart and in ScrGameplayOpt; prepend/append to an action (incl. a block that
gets its first ConstantPool); add-initaction (auto + explicit sprite, first/last);
add-doaction (existing frame, and a frame with no items); every edited movie also
relocates (+0x40 shifted re-parse, as rt_graph); malformed patches rejected;
determinism; build_custom_frontend empty set byte-identical and a real set verified.
Unload contract (section Q, tools/apt/apt_unload.py, the iter-463 QUIT GAME soft-lock):
retail passes 820/820; the real custom_frontend build passes 820/820 with every patched
movie actually renumbered; MUTATIONS each fail the check: the same build without
renumber_walk_order (all 5 movies; SCRPRESSSTART's registerClass pool reloads as
[reg2, reg2, reg2], the soft-lock mechanism), two .const items swapped, two index slots
swapped, one .const item dropped; the duplicate path (an index array shared by two Push
instructions) is copied and passes.
Exit status 0 only when every check passes.

usage (repo root):  python tools/apt/rt_patch.py [ui.AST]
"""
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bgfa import Bgfa, DEFAULT_UI, TAG_APT, TAG_CONST  # noqa: E402
from apt_model import Apt, coverage  # noqa: E402
from apt_asm import disasm, NAMES, logical_view  # noqa: E402
from apt_graph import from_apt, Block  # noqa: E402
import apt_unload  # noqa: E402
import struct  # noqa: E402
import patch_movie as pm  # noqa: E402
import build_custom_frontend as bcf  # noqa: E402
import aptasm  # noqa: E402

POOLOPS = {0xA2, 0xA3, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3}

# ---------------------------------------------------------------- independent view
def view(a, start):
    """instruction list of the block at `start`, targets as absolute instruction indices."""
    ins = disasm(a, start)
    nxt = [ins[i + 1]["at"] for i in range(len(ins) - 1)] + [ins[-1]["at"] + 1]
    at_idx = {x["at"]: i for i, x in enumerate(ins)}
    at_idx[nxt[-1]] = len(ins)
    pool, out = None, []
    for i, x in enumerate(ins):
        op = x["op"]
        if op == 0x88:
            pool = x["idx"]
        t = NAMES.get(op, f"op_{op:02X}")
        if op in (0x99, 0x9D, 0xB8, 0x94):
            t += f" ->{at_idx[nxt[i] + x['i32']]}"
        elif op == 0x8E:
            t += f" {x['name']!r} {x['params']} flags={x['w'][0]:#x} end->{at_idx[nxt[i] + x['w'][1]]}"
        elif op == 0x9B:
            t += f" {x['name']!r} {x['params']} end->{at_idx[nxt[i] + x['w'][0]]}"
        elif op in (0x88, 0x96):
            t += " [" + ", ".join(a.c.item_text(j) for j in x["idx"]) + "]"
        elif op in POOLOPS:
            v = int.from_bytes(x["imm"], "big")
            t += " " + (a.c.item_text(pool[v]) if pool is not None and v < len(pool) else f"pool[{v}]")
        elif "i32" in x:
            t += f" {x['i32']}"
        elif "s" in x:
            t += f" {x['s']!r}"
        elif "imm" in x:
            t += f" {int.from_bytes(x['imm'], 'big')}"
        out.append(t)
    return out


POOL_NAME = re.compile(r"(?:EA_\w+\??) '([^']*)'$")


def expect(v0, at, snippet, keep=lambda i, kind: False):
    """spec: v0 with `snippet` (targets '->@k' relative to its start) inserted at `at`;
    the host ConstantPool (line 0) gains, in first-use order, every name the snippet's
    pool-index ops use that it does not already hold."""
    n = len(snippet)
    if v0 and v0[0].startswith("ConstantPool ["):
        have = re.findall(r"'([^']*)'", v0[0])
        add = []
        for ln in snippet:
            m = POOL_NAME.match(ln)
            if m and not ln.startswith("EA_PushString") and m.group(1) not in have and m.group(1) not in add:
                add.append(m.group(1))
        if add:
            v0 = [v0[0][:-1] + "".join(f", '{x}'" for x in add) + "]"] + v0[1:]

    def remap(i, line):
        def rep(m):
            kind = "end" if m.group(1) else "tgt"
            t = int(m.group(2))
            if t > at or (t == at and not keep(i, kind)):
                t += n
            return f"{m.group(1) or ''}->{t}"
        return re.sub(r"(end)?->(\d+)", rep, line)
    out = [remap(i, ln) for i, ln in enumerate(v0)]
    snip = [re.sub(r"->@(\d+)", lambda m: f"->{int(m.group(1)) + at}", s) for s in snippet]
    return out[:at] + snip + out[at:]


def frame_items(a, char_off, frame):
    """[(type, itemOff)] of one frame (walker-level reads)."""
    fr = a.u(char_off + 0x14)
    n, arr = a.u(fr + 8 * frame), a.u(fr + 8 * frame + 4)
    return [(a.u(a.u(arr + 4 * q)), a.u(arr + 4 * q)) for q in range(n)] if n else []


def relocates(apt2, cst2):
    """rt_graph's shifted check on an edited movie: +0x40 filler, re-parse, same logical movie."""
    a = Apt(apt2, cst2)
    tail = len(apt2) - max(o + s for o, s, k in a.blocks)
    g = from_apt(a)
    g.add_block(Block("filler", bytes(0x40)), after=g.order[0])
    b, c, _ = g.emit(orig_tail=tail)
    a2 = Apt(b, c)
    return logical_view(a2) == logical_view(a) and not coverage(a2)[3] and len(b) == len(apt2) + 0x40


def fn_index(v, block_view_member):
    """index of the DefineFunction(2) right after the line `block_view_member`."""
    hits = [i + 1 for i, ln in enumerate(v[:-1]) if ln == block_view_member and v[i + 1].startswith("DefineFunction")]
    assert len(hits) == 1, (block_view_member, hits)
    return hits[0]


# ---------------------------------------------------------------- fixtures
SNIP_OPTIONS = """
    ; if (_global.SRFrontEnd.inSettings) { if (keyCode == TRIANGLE || keyCode == START) {
    ;   _global.playSound(_global.SOUND.CLOSE); _global.SRFrontEnd.BackFromSettings(); return false; } }
    Push [reg2]
    EA_GetNamedMember 'SRFrontEnd'
    EA_GetNamedMember 'inSettings'
    Not
    If skip
    Push [reg3]
    Push [reg2]
    EA_GetNamedMember 'INPUTCODE'
    EA_GetNamedMember 'TRIANGLE'
    Equals2
    If back
    Push [reg3]
    Push [reg2]
    EA_GetNamedMember 'INPUTCODE'
    EA_GetNamedMember 'START'
    Equals2
    Not
    If skip
back:
    Push [reg2]
    EA_GetNamedMember 'SOUND'
    EA_GetNamedMember 'CLOSE'
    op_5A
    Push [reg2]
    EA_CallNamedMethodPop 'playSound'
    op_59
    Push [reg2]
    EA_GetNamedMember 'SRFrontEnd'
    EA_CallNamedMethodPop 'BackFromSettings'
    op_74
    Return
skip:
"""
VIEW_OPTIONS = [
    "Push [reg2]", "EA_GetNamedMember 'SRFrontEnd'", "EA_GetNamedMember 'inSettings'", "Not", "If ->@30",
    "Push [reg3]", "Push [reg2]", "EA_GetNamedMember 'INPUTCODE'", "EA_GetNamedMember 'TRIANGLE'", "Equals2",
    "If ->@18",
    "Push [reg3]", "Push [reg2]", "EA_GetNamedMember 'INPUTCODE'", "EA_GetNamedMember 'START'", "Equals2",
    "Not", "If ->@30",
    "Push [reg2]", "EA_GetNamedMember 'SOUND'", "EA_GetNamedMember 'CLOSE'", "op_5A", "Push [reg2]",
    "EA_CallNamedMethodPop 'playSound'", "op_59", "Push [reg2]", "EA_GetNamedMember 'SRFrontEnd'",
    "EA_CallNamedMethodPop 'BackFromSettings'", "op_74", "Return"]
SNIP_INITGUI = """
    ; _global.SRFrontEnd.Install();   (the design's call appended to ScrPressStart.InitGui)
    op_59
    Push [reg2]
    EA_GetNamedMember 'SRFrontEnd'
    EA_CallNamedMethodPop 'Install'
"""
VIEW_INITGUI = ["op_59", "Push [reg2]", "EA_GetNamedMember 'SRFrontEnd'", "EA_CallNamedMethodPop 'Install'"]
BLOCK_INIT = """
    ; if (!!_global.SRFrontEnd) skip; _global.SRFrontEnd = new Object(); _global.SRFrontEnd.Install = function () {...}
    op_71
    EA_GetNamedMember 'SRFrontEnd'
    Not
    Not
    If done
    op_71
    EA_PushConstByte? 'SRFrontEnd'
    op_59
    EA_PushConstByte? 'Object'
    NewObject
    SetMember
    op_71
    EA_GetNamedMember 'SRFrontEnd'
    EA_PushConstByte? 'Install'
    .func2 ''() flags=0x30129
        EA_PushConstByte? 'SRFrontEnd.Install()'
        Trace
    .end
    SetMember
done:
"""
VIEW_INIT = ["ConstantPool ['SRFrontEnd', 'Object', 'Install', 'SRFrontEnd.Install()']",
             "op_71", "EA_GetNamedMember 'SRFrontEnd'", "Not", "Not", "If ->19", "op_71",
             "EA_PushConstByte? 'SRFrontEnd'", "op_59", "EA_PushConstByte? 'Object'", "NewObject", "SetMember",
             "op_71", "EA_GetNamedMember 'SRFrontEnd'", "EA_PushConstByte? 'Install'",
             "DefineFunction2 b'' [] flags=0x30129 end->18", "EA_PushConstByte? 'SRFrontEnd.Install()'", "Trace",
             "SetMember", "End"]
BLOCK_DO = """
    EA_PushString 'SR doaction'
    Trace
"""
VIEW_DO = ["EA_PushString b'SR doaction'", "Trace", "End"]
BLOCK_FEATURES = """
.const KEY = reg3                  ; assembler-time aliases
.const FE = 'SRFrontEnd'
.pool 'SRFrontEnd', 'Install'      ; explicit pool; 'Extra' below gets appended to it
    op_71
    EA_GetNamedMember $FE
    EA_GetNamedMember 'Extra'
    Pop
    Push [$KEY, reg1]
    Pop
    Pop
    EA_PushByte -1
    EA_PushShort 1000
    EA_PushLong 70000
    EA_PushFloat 0.5
    EA_PushFloat bits=0x3f800000
    Pop
    Pop
    Pop
    Pop
    Pop
    .func 'named'('a', 'b')        ; DefineFunction (0x9B)
        EA_PushString 'in func'
        Trace
    .end
    Pop
    GetURL 'FSCommand:SR_Quit', '' ; = fscommand("SR_Quit", "") (CUSTOM_FRONTEND.md 7.3)
    Jump out
    Pop
out:
"""
VIEW_FEATURES = [
    "ConstantPool ['SRFrontEnd', 'Install', 'Extra']", "op_71", "EA_GetNamedMember 'SRFrontEnd'",
    "EA_GetNamedMember 'Extra'", "Pop", "Push [reg3, reg1]", "Pop", "Pop", "EA_PushByte 255",
    "EA_PushShort 1000", "EA_PushLong 70000", "EA_PushFloat 1056964608", "EA_PushFloat 1065353216",
    "Pop", "Pop", "Pop", "Pop", "Pop", "DefineFunction b'named' [b'a', b'b'] end->21",
    "EA_PushString b'in func'", "Trace", "Pop", "GetURL [b'FSCommand:SR_Quit', b'']", "Jump ->25", "Pop", "End"]


class T:
    def __init__(self, ui_path):
        self.ui_path = ui_path
        self.ui = open(ui_path, "rb").read()
        self.outer = Bgfa(self.ui)
        self.results = []

    def movie(self, name):
        e = next(x for x in self.outer.entries if x["name"] == name)
        inner = Bgfa(e["data"])
        return (next(x for x in inner.entries if x["tag"] == TAG_APT)["data"],
                next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"])

    def run(self, name, patch_text):
        (mv, ops), = pm.parse_patch(patch_text)
        apt, cst = self.movie(mv)
        apt2, cst2, p = pm.patch_movie(apt, cst, ops, mv)
        return apt, cst, apt2, cst2, p

    def check(self, label, fn):
        try:
            ok, detail = fn()
        except Exception as ex:  # a crash is a failure of that check
            ok, detail = False, f"exception {ex!r}"[:300]
        self.results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {label}" + (f" - {detail}" if detail else ""))


def main(argv):
    ui_path = argv[0] if argv else DEFAULT_UI
    t0 = time.time()
    t = T(ui_path)

    # ---- A. no-op patches
    NOOP = """
movie SCRPRESSSTART
replace-const 'SUP_PRESS_START' -> 'SUP_PRESS_START' count=1
prepend-to function InitGui in ScrPressStart
end
append-to function InitGui in __Packages.ScrPressStart
end
prepend-to function HandleInput in initaction:61
end
prepend-to action initaction:ScrPressStart
end
append-to action doaction:0/0
end
movie SCRGAMEPLAYOPT
prepend-to function HandleInput in ScrGameplayOpt
end
append-to action bytecode@0x428
end
movie SCRAUDIOOPT
"""

    def a_noop():
        secs = pm.parse_patch(NOOP)
        new_ui, ps = pm.patch_container(t.ui, secs)
        per = []
        for name, ops in secs:
            apt, cst = t.movie(name)
            apt2, cst2, _ = pm.patch_movie(apt, cst, ops, name)
            per.append(apt2 == apt and cst2 == cst)
        return all(per) and new_ui == t.ui, (f"{sum(per)}/{len(per)} movies byte-identical after 8 no-op ops "
                                              f"(+1 op-less section); ui.AST repack byte-identical: {new_ui == t.ui}")
    t.check("no-op patch is byte-identical (movie + whole ui.AST)", a_noop)

    # ---- B. replace-const (E1 step 2's edit)
    def b_const():
        apt, cst, apt2, cst2, p = t.run("SCRPRESSSTART",
                                         "movie SCRPRESSSTART\nreplace-const 'SUP_PRESS_START' -> 'Generic_QUITGAME' count=1\n")
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        s1, s2 = sorted(a.c.strings.values()), sorted(b.c.strings.values())
        want = sorted([x if x != b"SUP_PRESS_START" else b"Generic_QUITGAME" for x in s1])
        v0, v1 = view(a, 0x41E4), view(b, 0x41E4)
        want_v = [ln.replace("'SUP_PRESS_START'", "'Generic_QUITGAME'") for ln in v0]
        ndiff = sum(x != y for x, y in zip(v0, v1))
        ok = apt2 == apt and cst2 != cst and s2 == want and v1 == want_v and ndiff >= 2
        return ok, (f".apt byte-identical {apt2 == apt}; .const strings = original with 1 replaced {s2 == want}; "
                    f"class block == original with the name swapped {v1 == want_v} "
                    f"({ndiff} lines: the pool entry + the op that pushes it)")
    t.check("replace-const SUP_PRESS_START -> Generic_QUITGAME (SCRPRESSSTART)", b_const)

    # ---- C/D. functions
    def fn_case(movie, patch, block, member, snippet_view, append):
        apt, cst, apt2, cst2, p = t.run(movie, patch)
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        v0 = view(a, block)
        fi = fn_index(v0, member)
        s = fi + 1
        e = int(re.search(r"end->(\d+)", v0[fi]).group(1))
        new_block = p.where[block]
        v1 = view(b, new_block)
        if append:
            want = expect(v0, e, snippet_view,
                          keep=lambda i, kind: s <= i < e and not (kind == "end" and i == fi))
        else:
            want = expect(v0, s, snippet_view)
        # the new pool entries must be real (view resolves names through the pool)
        diff = [i for i, (x, y) in enumerate(zip(want, v1)) if x != y]
        ok = v1 == want and relocates(apt2, cst2)
        return ok, (f"block {len(v0)} -> {len(v1)} instructions, body {s}..{e}, re-parsed == spec "
                    f"{v1 == want}{'' if not diff else f' (first diff at {diff[0]}: {want[diff[0]]!r} vs {v1[diff[0]]!r})'}; "
                    f"relocates {relocates(apt2, cst2)}")

    t.check("prepend-to function HandleInput in ScrGameplayOpt (SCRGAMEPLAYOPT, design 5.2 snippet)",
            lambda: fn_case("SCRGAMEPLAYOPT", "movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n"
                            + SNIP_OPTIONS + "end\n", 0x428, "EA_PushConstByte? 'HandleInput'", VIEW_OPTIONS, False))
    t.check("prepend-to function HandleInput in ScrPressStart (SCRPRESSSTART)",
            lambda: fn_case("SCRPRESSSTART", "movie SCRPRESSSTART\nprepend-to function HandleInput in ScrPressStart\n"
                            + SNIP_OPTIONS + "end\n", 0x41E4, "EA_PushConstByte? 'HandleInput'", VIEW_OPTIONS, False))
    t.check("append-to function InitGui in ScrPressStart (SCRPRESSSTART, design 5.1 call)",
            lambda: fn_case("SCRPRESSSTART", "movie SCRPRESSSTART\nappend-to function InitGui in ScrPressStart\n"
                            + SNIP_INITGUI + "end\n", 0x41E4, "EA_PushConstByte? 'InitGui'", VIEW_INITGUI, True))
    t.check("append-to function ExitScreen in ScrGameplayOpt (SCRGAMEPLAYOPT)",
            lambda: fn_case("SCRGAMEPLAYOPT", "movie SCRGAMEPLAYOPT\nappend-to function ExitScreen in ScrGameplayOpt\n"
                            + SNIP_INITGUI + "end\n", 0x428, "EA_PushConstByte? 'ExitScreen'", VIEW_INITGUI, True))

    def branch_to_end(movie, cls, member, block):
        """append where a branch inside the body targets the body end: it must land on the snippet."""
        ok, detail = fn_case(movie, f"movie {movie}\nappend-to function {member} in {cls}\n" + SNIP_INITGUI + "end\n",
                             block, f"EA_PushConstByte? '{member}'", VIEW_INITGUI, True)
        a = Apt(*t.movie(movie))
        v0 = view(a, block)
        fi = fn_index(v0, f"EA_PushConstByte? '{member}'")
        e = int(re.search(r"end->(\d+)", v0[fi]).group(1))
        nbr = sum(1 for ln in v0[fi + 1:e] if re.search(r"(?<!end)->" + str(e) + r"$", ln))
        last = v0[max(j for j in range(fi + 1, e) if not v0[j].startswith(" "))].split()[0]
        return ok and nbr > 0, f"{nbr} branch(es) to the body end, last op {last}; " + detail
    t.check("append-to function LoadComplete in ScrPressStart (a branch to the body end lands on the snippet)",
            lambda: branch_to_end("SCRPRESSSTART", "ScrPressStart", "LoadComplete", 0x41E4))
    t.check("append-to function showConfigGraphic in ScrControllerOpt (body ends in Jump; reachable via its branch)",
            lambda: branch_to_end("SCRCONTROLLEROPT", "ScrControllerOpt", "showConfigGraphic", 0x880))

    # ---- G/H. actions
    def g_prepend_action():
        apt, cst, apt2, cst2, p = t.run("SCRPRESSSTART", "movie SCRPRESSSTART\nprepend-to action initaction:ScrPressStart\n"
                                         + SNIP_INITGUI + "end\n")
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        v0, v1 = view(a, 0x41E4), view(b, p.where[0x41E4])
        want = expect(v0, 1, VIEW_INITGUI)
        return v1 == want and relocates(apt2, cst2), f"inserted after the ConstantPool: {v1 == want}"
    t.check("prepend-to action initaction:ScrPressStart (SCRPRESSSTART)", g_prepend_action)

    def h_append_action_newpool():
        apt, cst, apt2, cst2, p = t.run("SCRPRESSSTART", "movie SCRPRESSSTART\nappend-to action doaction:0/0\n"
                                         + SNIP_INITGUI + "end\n")
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        k = next(s for s, e, own in a.bytecode if own.startswith("action@"))
        v0 = view(a, k)
        v1 = view(b, p.where[k])
        want = ["ConstantPool ['SRFrontEnd', 'Install']"] + expect(v0, len(v0) - 1, VIEW_INITGUI)
        return v0 == ["Stop", "End"] and v1 == want and relocates(apt2, cst2), \
            f"'Stop; End' -> {v1}"
    t.check("append-to action doaction:0/0 (SCRPRESSSTART; the block gets its first ConstantPool)", h_append_action_newpool)

    # ---- E/F. new frame items
    def e_initaction(movie, opts, want_sprite, position):
        apt, cst, apt2, cst2, p = t.run(movie, f"movie {movie}\nadd-initaction {opts}\n" + BLOCK_INIT + "end\n")
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        f0, f1 = frame_items(a, a.movie, 0), frame_items(b, b.movie, 0)
        t0_, t1_ = [x for x, _ in f0], [x for x, _ in f1]
        inits0 = [i for i, x in enumerate(t0_) if x == 8]
        at = (inits0[-1] + 1) if position == "last" else inits0[0]
        new_it = f1[at][1]
        sid, bc = b.i(new_it + 4), b.u(new_it + 8)
        ok_types = t1_ == t0_[:at] + [8] + t0_[at:]
        # every original InitAction keeps its sprite id and bytecode listing
        olds = [(a.i(o + 4), view(a, a.u(o + 8))) for x, o in f0 if x == 8]
        news = [(b.i(o + 4), view(b, b.u(o + 8))) for i, (x, o) in enumerate(f1) if x == 8 and i != at]
        v = view(b, bc)
        ok = ok_types and sid == want_sprite and v == VIEW_INIT and olds == news and relocates(apt2, cst2) \
            and b.u(b.u(b.movie + 0x14)) == len(f0) + 1
        return ok, (f"frame 0 items {t0_} -> {t1_}; new InitAction at {at}, sprite {sid} (want {want_sprite}); "
                    f"listing == spec {v == VIEW_INIT}; other InitActions unchanged {olds == news}; "
                    f"relocates {relocates(apt2, cst2)}")
    apt_s, cst_s = t.movie("SCRPRESSSTART")
    nchars_s = Apt(apt_s, cst_s).u(Apt(apt_s, cst_s).movie + 0x1C)
    t.check("add-initaction sprite=auto (SCRPRESSSTART, the design's _global.SRFrontEnd shape)",
            lambda: e_initaction("SCRPRESSSTART", "sprite=auto", nchars_s, "last"))
    t.check("add-initaction sprite=77 position=first (SCRGAMEPLAYOPT)",
            lambda: e_initaction("SCRGAMEPLAYOPT", "sprite=77 position=first", 77, "first"))

    def f_doaction(movie, char, frame_pick):
        a = Apt(*t.movie(movie))
        coff = a.movie if char == 0 else a.u(a.u(a.movie + 0x20) + 4 * char)
        frame = frame_pick(a, coff)
        apt, cst, apt2, cst2, p = t.run(movie, f"movie {movie}\nadd-doaction char={char} frame={frame}\n" + BLOCK_DO + "end\n")
        a, b = Apt(apt, cst), Apt(apt2, cst2)
        coff2 = b.movie if char == 0 else b.u(b.u(b.movie + 0x20) + 4 * char)
        f0, f1 = frame_items(a, coff, frame), frame_items(b, coff2, frame)
        t0_, t1_ = [x for x, _ in f0], [x for x, _ in f1]
        dos = [i for i, x in enumerate(t0_) if x == 1]
        at = dos[-1] + 1 if dos else len(t0_)
        v = view(b, b.u(f1[at][1] + 4))
        ok = t1_ == t0_[:at] + [1] + t0_[at:] and v == VIEW_DO and relocates(apt2, cst2)
        return ok, f"char {char} frame {frame}: items {t0_} -> {t1_}; listing == spec {v == VIEW_DO}"

    def features():
        apt, cst, apt2, cst2, p = t.run("SCRPRESSSTART", "movie SCRPRESSSTART\nadd-doaction\n" + BLOCK_FEATURES + "end\n")
        b = Apt(apt2, cst2)
        f1 = frame_items(b, b.movie, 0)
        bc = b.u(f1[2][1] + 4)
        v = view(b, bc)
        diff = [i for i, (x, y) in enumerate(zip(VIEW_FEATURES, v)) if x != y]
        return v == VIEW_FEATURES and relocates(apt2, cst2), \
            (f"{len(v)} instructions == spec {v == VIEW_FEATURES}"
             + (f" (first diff {diff[0]}: {VIEW_FEATURES[diff[0]]!r} vs {v[diff[0]]!r})" if diff else ""))
    t.check("add-doaction using .const aliases, .pool (+auto-append), .func, GetURL fscommand, immediates, end label",
            features)

    def empty_frame(a, coff):
        fr = a.u(coff + 0x14)
        return next(f for f in range(a.u(coff + 0x10)) if a.u(fr + 8 * f) == 0)
    t.check("add-doaction char=0 frame=0 (SCRPRESSSTART, after the existing DoAction)",
            lambda: f_doaction("SCRPRESSSTART", 0, lambda a, c: 0))
    t.check("add-doaction into a frame with no items (SCRPRESSSTART char 59: new item-pointer array)",
            lambda: f_doaction("SCRPRESSSTART", 59, empty_frame))

    # ---- I. rejected patches
    BAD = [
        ("ambiguous function", "movie SCRPRESSSTART\nprepend-to function InitGui\n    Pop\nend\n"),
        ("unknown function", "movie SCRPRESSSTART\nprepend-to function NoSuchFn\n    Pop\nend\n"),
        ("register beyond the function's count", "movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n    Push [reg9]\n    Pop\nend\n"),
        ("ConstantPool in a snippet", "movie SCRPRESSSTART\nprepend-to function InitGui in ScrPressStart\n    ConstantPool ['x']\nend\n"),
        ("End in a snippet", "movie SCRPRESSSTART\nappend-to function InitGui in ScrPressStart\n    End\nend\n"),
        ("append after a final Return", "movie SCRPRESSSTART\nappend-to function HandleInput in BistroBaseScreen\n    Pop\nend\n"),
        ("replace-const count mismatch", "movie SCRPRESSSTART\nreplace-const 'SUP_PRESS_START' -> 'X' count=2\n"),
        ("replace-const of a missing string", "movie SCRPRESSSTART\nreplace-const 'NOPE_NOT_HERE' -> 'X'\n"),
        ("add-initaction sprite=0", "movie SCRPRESSSTART\nadd-initaction sprite=0\n    Pop\nend\n"),
        ("op before any movie", "replace-const 'A' -> 'B'\n"),
        ("body without end", "movie SCRPRESSSTART\nprepend-to function InitGui in ScrPressStart\n    Pop\n"),
        ("unknown op", "movie SCRPRESSSTART\nfrobnicate x\n"),
        ("frame out of range", "movie SCRPRESSSTART\nadd-doaction char=0 frame=5\n    Pop\nend\n"),
        ("undefined label in a snippet", "movie SCRPRESSSTART\nprepend-to function InitGui in ScrPressStart\n    Jump nowhere\nend\n"),
    ]

    def i_bad():
        rej = []
        for label, text in BAD:
            try:
                secs = pm.parse_patch(text)
                for mv, ops in secs:
                    apt, cst = t.movie(mv)
                    pm.patch_movie(apt, cst, ops, mv)
                rej.append((label, False))
            except (pm.PatchError, aptasm.AsmError):
                rej.append((label, True))
        missed = [lb for lb, r in rej if not r]
        return not missed, f"{sum(r for _, r in rej)}/{len(rej)} rejected" + (f"; NOT rejected: {missed}" if missed else "")
    t.check("malformed / unsafe patches rejected", i_bad)

    # ---- Q. the runtime's unload contract (apt_unload; iter-463 QUIT GAME soft-lock)
    def q_retail():
        n, fails = apt_unload.check_container(t.ui)
        return n == 820 and not fails, f"retail: {n - len(fails)}/{n} movies pass" + (
            f"; FAIL {[f[0] for f in fails[:5]]}" if fails else "")
    t.check("unload contract: retail ui.AST passes 820/820 (index == counter, slots == items, reload == first load)",
            q_retail)

    real_secs = []
    for f in sorted(os.listdir(bcf.DEFAULT_PATCHES)):
        if f.endswith(".patch"):
            real_secs += pm.load_patch(os.path.join(bcf.DEFAULT_PATCHES, f))
    real_by = {}
    for mv, ops in real_secs:
        real_by.setdefault(mv, []).extend(ops)

    def q_real():
        new_ui, ps = pm.patch_container(t.ui, real_secs)
        n, fails = apt_unload.check_container(new_ui)
        moved = {p.name: p.renumbered["moved"] for p in ps}
        ok = n == 820 and not fails and len(ps) == len(real_by) and all(v > 0 for v in moved.values())
        return ok, f"custom_frontend build: {n - len(fails)}/{n} movies pass; renumbered (slots moved) {moved}"
    t.check("unload contract: the real custom_frontend build passes 820/820, every patched movie renumbered", q_real)

    def unrenumbered(name):
        apt, cst = t.movie(name)
        p = pm.Patcher(apt, cst, name)
        for op in real_by[name]:
            p.apply(op)
        p.g.gc()
        b, c, _ = p.g.emit(orig_tail=p.tail)
        return b, c

    def q_mut_norenumber():
        caught, notes = 0, []
        for name in sorted(real_by):
            apt, cst = t.movie(name)
            p = pm.Patcher(apt, cst, name)
            for op in real_by[name]:
                p.apply(op)
            try:
                p.finish(renumber=False)
                notes.append(f"{name} NOT caught")
            except pm.PatchError as ex:
                caught += "unload" in str(ex)
        # the mechanism, offline: the reload of the un-renumbered SCRPRESSSTART resolves
        # InitAction 59's registerClass ConstantPool to registers
        b, c = unrenumbered("SCRPRESSSTART")
        a = Apt(b, c)
        slots = apt_unload.unload_slots(None, a=a)
        ia59 = [k for k, sl in enumerate(slots) if sl[2] == 0x88 and "sprite 59" in sl[3]]
        first = [apt_unload.value(a.c, slots[k][1]) for k in ia59]
        a2 = Apt(apt_unload.simulate_unload(b, c), c)
        again = [apt_unload.value(a2.c, sl[1]) for sl in [apt_unload.unload_slots(None, a=a2)[k] for k in ia59]]
        # after the unload the same slots name REGISTERS (type 4); which register depends on how
        # many InitActions the build adds, so only the kind is pinned
        mech = (first == [(1, b"ScrPressStart"), (1, b"Object"), (1, b"registerClass")]
                and len(again) == 3 and all(v[0] == 4 for v in again))
        notes.append(f"IA59 pool first load {[v[1] for v in first]} -> after unload {again}")
        return caught == len(real_by) and mech, f"finish(renumber=False) rejected {caught}/{len(real_by)}; " + "; ".join(notes)
    t.check("MUTATION: the real build without renumber_walk_order is rejected (every patched movie), reload -> registers x3",
            q_mut_norenumber)

    def q_mut_container():
        # build_custom_frontend.verify_container (what --check runs on the final bytes) on its
        # own: the real build's container with the 5 movies swapped for un-renumbered ones
        outer = Bgfa(t.ui)
        for e in outer.entries:
            if e["name"].upper() in real_by:
                b, c = unrenumbered(e["name"].upper())
                inner = Bgfa(e["data"])
                next(x for x in inner.entries if x["tag"] == TAG_APT)["data"] = b
                next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"] = c
                e["data"] = inner.rebuild()
        bad_ui = outer.rebuild()
        try:
            bcf.verify_container(t.ui, bad_ui, set(real_by))
            return False, f"verify_container accepted a container with {len(real_by)} un-renumbered movies"
        except pm.PatchError as ex:
            return f"{len(real_by)}/820" in str(ex), f"rejected: {str(ex)[:150]}"
    t.check("MUTATION: build_custom_frontend.verify_container (--check) rejects the un-renumbered container",
            q_mut_container)

    def q_mut_bytes():
        new_ui, ps = pm.patch_container(t.ui, real_secs)
        e = next(x for x in Bgfa(new_ui).entries if x["name"] == "SCRPRESSSTART")
        inner = Bgfa(e["data"])
        apt = next(x for x in inner.entries if x["tag"] == TAG_APT)["data"]
        cst = next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"]
        before = apt_unload.resolved(apt, cst)
        good = apt_unload.check(apt, cst, before=before)[0]
        a = Apt(apt, cst)
        io = a.c.items_off
        i, j = next((i, j) for i in range(a.c.count) for j in range(i + 1, a.c.count)
                    if apt_unload.value(a.c, i) != apt_unload.value(a.c, j))
        c2 = bytearray(cst)
        c2[io + 8 * i:io + 8 * i + 8], c2[io + 8 * j:io + 8 * j + 8] = cst[io + 8 * j:io + 8 * j + 8], cst[io + 8 * i:io + 8 * i + 8]
        swap_items = apt_unload.check(apt, bytes(c2), before=before)
        sl = apt_unload.unload_slots(None, a=a)
        b2 = bytearray(apt)
        struct.pack_into(">I", b2, sl[0][0], sl[1][1])
        struct.pack_into(">I", b2, sl[1][0], sl[0][1])
        swap_slots = apt_unload.check(bytes(b2), cst)
        c3 = bytearray(cst)
        struct.pack_into(">I", c3, 0x18, a.c.count - 1)
        drop = apt_unload.check(apt, bytes(c3))
        ok = good and not swap_items[0] and not swap_slots[0] and not drop[0]
        return ok, (f"good passes {good}; items {i}<->{j} swapped caught {not swap_items[0]} "
                    f"({swap_items[1][0][:60] if swap_items[1] else ''}); slots 0<->1 swapped caught {not swap_slots[0]}; "
                    f"one item dropped caught {not drop[0]}")
    t.check("MUTATION: swapped .const items / swapped index slots / a dropped item each fail the check", q_mut_bytes)

    def q_dup():
        apt, cst = t.movie("SCRPRESSSTART")
        a = Apt(apt, cst)
        g = from_apt(a)
        tail = len(apt) - max(o + s for o, s, k in a.blocks)
        pushes = [ins for k in g.order if g.blocks[k].code for ins in g.blocks[k].code
                  if ins.op == 0x96 and ins.f["n"] == 1]
        x, y = pushes[0], pushes[1]
        y.f["arr"] = x.f["arr"]                 # two Push instructions share one array (and item)
        g.gc()
        pre = g.emit(orig_tail=tail)
        rn = g.renumber_walk_order()
        b, c, _ = g.emit(orig_tail=tail)
        ok_u, problems, st = apt_unload.check(b, c, before=apt_unload.resolved(pre[0], pre[1]))
        ok = ok_u and rn["arrays_copied"] == 1 and rn["duplicated"] == 1 and not coverage(Apt(b, c))[3]
        return ok, f"renumber {rn}; check {ok_u} {problems[:1]}; slots {st['slots']} == items {st['items']}"
    t.check("renumber_walk_order: a shared index array is copied and its item duplicated; the output passes", q_dup)

    # ---- R. THE FIRST-REGISTRAR RULE (lane E, step 5 E3-E6 r3; patch_movie.py): MAIN (_level0) registers its
    # __Packages classes first, so a function op on such a class in any other movie is dead code and is refused
    SNIP0 = "    EA_PushZero\n    Pop\nend\n"
    R_DEAD = {
        "SCRGENPOP PopupInit (the E4 r2 target)": "movie SCRGENPOP\nappend-to function PopupInit in ScrGenPop\n" + SNIP0,
        "SCRGENPOP HandleInput": "movie SCRGENPOP\nprepend-to function HandleInput in ScrGenPop\n" + SNIP0,
        "BUTTONICON SetIcon (the E5 r2 target)": "movie BUTTONICON\nprepend-to function SetIcon in ButtonIcon\n" + SNIP0,
        "PARTTEXT SetFontColor": "movie PARTTEXT\nprepend-to function SetFontColor in PartText\n" + SNIP0,
        # E3-E6 r5 (Astra r4 P2): code prepended to the InitAction moves the guard off the block's opening; the
        # method behind it is still dead (the guard is tracked through the splice)
        "SCRGENPOP an action prepend, then PopupInit":
            "movie SCRGENPOP\nprepend-to action initaction:ScrGenPop\n" + SNIP0
            + "append-to function PopupInit in ScrGenPop\n" + SNIP0,
    }
    R_LIVE = {
        "MAIN PopupInit in ScrGenPop": "movie MAIN\nappend-to function PopupInit in ScrGenPop\n" + SNIP0,
        "MAIN SetIcon in ButtonIcon": "movie MAIN\nprepend-to function SetIcon in ButtonIcon\n" + SNIP0,
        "SCRPRESSSTART action initaction:BistroBaseScreen (runs whatever the guard skips)":
            "movie SCRPRESSSTART\nappend-to action initaction:BistroBaseScreen\n" + SNIP0,
        "SCRGAMEPLAYOPT HandleInput in its own class":
            "movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n" + SNIP0,
        # E3-E6 r4 (Astra r3 P3): a helper an action op defines in a MAIN class's InitAction but OUTSIDE its
        # `if (!_global.X)` body runs on every load, so a function op on it is live (classified by position)
        "SCRGENPOP a helper defined outside the ScrGenPop guard (an action op), then a function op on it":
            "movie SCRGENPOP\nappend-to action initaction:ScrGenPop\n    op_71\n    EA_PushConstByte 'srProbeHelper'\n"
            "    .func2 ''() flags=0x30129\n        EA_PushZero\n        Pop\n    .end\n    SetMember\nend\n"
            "prepend-to function srProbeHelper in ScrGenPop\n" + SNIP0,
    }

    def r_build(text):
        try:
            pm.patch_container(t.ui, pm.parse_patch(text))
            return "built"
        except pm.PatchError as ex:
            return "refused" + (" (names MAIN)" if "registers its copy first" in str(ex) else " (other reason)")

    def r_rule():
        main_cls = pm.first_registrar_classes(t.outer)
        dead = {k: r_build(v) for k, v in R_DEAD.items()}
        live = {k: r_build(v) for k, v in R_LIVE.items()}
        ok = (len(main_cls) == 40 and {"ScrGenPop", "ButtonIcon", "ButtonHints", "PartText", "DisplayWell"} <= main_cls
              and all(v == "refused (names MAIN)" for v in dead.values()) and all(v == "built" for v in live.values()))
        return ok, (f"MAIN exports {len(main_cls)} classes; dead: " + ", ".join(f"{k} {v}" for k, v in dead.items())
                    + "; live: " + ", ".join(f"{k} {v}" for k, v in live.items()))
    t.check("first-registrar rule: a function op on a class MAIN exports is refused in any other movie; the same op "
            "in MAIN, an InitAction action op and a movie's own class build", r_rule)

    def r_mut_guard():
        saved = pm.check_first_registrar
        pm.check_first_registrar = lambda *a, **k: None
        try:
            got = {k: r_build(v) for k, v in R_DEAD.items()}
        finally:
            pm.check_first_registrar = saved
        return all(v == "built" for v in got.values()), "without the guard: " + ", ".join(f"{k} {v}" for k, v in got.items())
    t.check("MUTATION: without check_first_registrar the dead ops build (the refusal above is the guard's)", r_mut_guard)

    # ---- J/K. combined patch set: determinism + build_custom_frontend
    tmp = tempfile.mkdtemp(prefix="sr_rt_patch_")
    try:
        pdir = os.path.join(tmp, "patches")
        os.makedirs(pdir)
        open(os.path.join(pdir, "srfrontend.aptasm"), "w", encoding="utf-8").write(BLOCK_INIT)
        open(os.path.join(pdir, "10_scrpressstart.patch"), "w", encoding="utf-8").write(
            "movie SCRPRESSSTART\nadd-initaction sprite=auto file=srfrontend.aptasm\n"
            "append-to function InitGui in ScrPressStart\n" + SNIP_INITGUI + "end\n")
        open(os.path.join(pdir, "20_options.patch"), "w", encoding="utf-8").write(
            "".join(f"movie {m}\nprepend-to function HandleInput in {c}\n{SNIP_OPTIONS}end\n"
                    for m, c in (("SCRGAMEPLAYOPT", "ScrGameplayOpt"), ("SCRCONTROLLEROPT", "ScrControllerOpt"),
                                 ("SCRAUDIOOPT", "ScrAudioOpt"))))
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        quiet = lambda *a, **k: None  # noqa: E731

        def j_determinism():
            secs = []
            for f in sorted(os.listdir(pdir)):
                if f.endswith(".patch"):
                    secs += pm.load_patch(os.path.join(pdir, f))
            u1, _ = pm.patch_container(t.ui, secs)
            secs2 = []
            for f in sorted(os.listdir(pdir)):
                if f.endswith(".patch"):
                    secs2 += pm.load_patch(os.path.join(pdir, f))
            u2, _ = pm.patch_container(t.ui, secs2)
            return u1 == u2 and u1 != t.ui, f"two builds of the same set byte-identical: {u1 == u2}"
        t.check("determinism (the 4-movie set built twice)", j_determinism)

        def k_empty():
            root = os.path.join(tmp, "root_empty")
            bcf.build(ui_path, empty, None, root, write=True, log=quiet)
            out = bcf.form_a_path(root, bcf.DEFAULT_MOD)
            got = open(out, "rb").read()
            return got == t.ui, f"{out} {len(got)} B, byte-identical to the input: {got == t.ui}"
        t.check("build_custom_frontend: empty patch set -> byte-identical ui.AST (written + re-read)", k_empty)

        def k_real():
            root = os.path.join(tmp, "root_real")
            lines = []
            new_ui, ps = bcf.build(ui_path, pdir, None, root, write=True, log=lines.append)
            got = open(bcf.form_a_path(root, bcf.DEFAULT_MOD), "rb").read()
            o1, o2 = Bgfa(t.ui), Bgfa(got)
            names = {p.name for p in ps}
            same = sum(1 for x, y in zip(o1.entries, o2.entries) if x["data"] == y["data"])
            # the patched movies in the written file are exactly what patch_movie produced
            match = 0
            for p in ps:
                e = next(x for x in o2.entries if x["name"] == p.name)
                inner = Bgfa(e["data"])
                apt = next(x for x in inner.entries if x["tag"] == TAG_APT)["data"]
                apt_want, _, _ = p.g.emit(orig_tail=p.tail)
                match += apt == apt_want
            ok = got == new_ui and names == {"SCRPRESSSTART", "SCRGAMEPLAYOPT", "SCRCONTROLLEROPT", "SCRAUDIOOPT"} \
                and same == len(o1.entries) - 4 and match == 4
            return ok, (f"patched {sorted(names)}; untouched entries byte-identical {same}/{len(o1.entries) - 4}; "
                        f"written movies == patch_movie output {match}/4; {len(t.ui)} -> {len(got)} B")
        t.check("build_custom_frontend: 2 patch files + 1 .aptasm over 4 movies (written + re-read)", k_real)

        def l_form_b():
            # form b: one DATA/_workinprogress/<MOVIE>.ast per patched movie, equal to that
            # movie's nested container in the form-a build; a stale form-a ui.AST is removed.
            root = os.path.join(tmp, "root_real")  # holds k_real's form-a output
            stale_a = bcf.form_a_path(root, bcf.DEFAULT_MOD)
            had_a = os.path.isfile(stale_a)
            new_ui, ps = bcf.build(ui_path, pdir, None, root, write=True, log=quiet, form="b")
            bdir = bcf.form_b_dir(root, bcf.DEFAULT_MOD)
            files = sorted(os.listdir(bdir))
            want = {e["name"].upper(): e["data"] for e in Bgfa(new_ui).entries}
            same = sum(open(os.path.join(bdir, f), "rb").read() == want[os.path.splitext(f)[0]] for f in files)
            ok = had_a and not os.path.exists(stale_a) and len(files) == 4 and same == 4 \
                and all(f.endswith(".ast") and f[:-4].isupper() for f in files)
            return ok, f"stale form a removed: {had_a and not os.path.exists(stale_a)}; files {files}; equal {same}/4"
        t.check("build_custom_frontend --form b: per-movie _workinprogress/<MOVIE>.ast, stale form a removed", l_form_b)

        def m_enable():
            root = os.path.join(tmp, "root_enable")
            bcf.enable_mod(root, "custom_frontend", log=quiet)
            bcf.enable_mod(root, "Custom_Frontend", log=quiet)  # already listed (case-insensitive)
            bcf.enable_mod(root, "other", log=quiet)
            lines = [l for l in open(os.path.join(root, "mods", "enabled.txt"), encoding="utf-8").read().splitlines()
                     if l and not l.startswith("#")]
            return lines == ["custom_frontend", "other"], f"enabled.txt names {lines}"
        t.check("enable_mod: appends once (case-insensitive), keeps order", m_enable)

        def m_names():
            # the tool refuses exactly the names the exe would skip (validModName)
            good = ["custom_frontend", "hd-pack.v2", "A" * 64]
            bad = ["my frontend", ".hidden", "..", "a/b", "a\\b", "c:x", "", "A" * 65, "café"]
            root = os.path.join(tmp, "root_names")
            refused = 0
            for n in bad:
                try:
                    bcf.enable_mod(root, n, log=quiet)
                except pm.PatchError:
                    refused += 1
            ok = all(bcf.valid_mod_name(n) for n in good) and refused == len(bad) \
                and not os.path.exists(os.path.join(root, "mods", "enabled.txt"))
            return ok, f"good accepted {sum(map(bcf.valid_mod_name, good))}/{len(good)}; bad refused {refused}/{len(bad)}"
        t.check("--mod/--enable refuse names the exe would skip (same rule as validModName)", m_names)

        def n_out_guard():
            # patch_movie --out refuses the game folder (default and the --ui one, any depth)
            game = os.path.dirname(pm.DATA_DIR)
            bad = [game, pm.DATA_DIR, os.path.join(pm.DATA_DIR, "x"), os.path.join(game, "..",
                   os.path.basename(game), "DATA"), os.path.join(os.path.dirname(ui_path), "y")]
            refused = 0
            for o in bad:
                try:
                    pm.refuse_game_folder(o, ui_path)
                except pm.PatchError:
                    refused += 1
            try:
                pm.refuse_game_folder(os.path.join(tmp, "out_ok"), ui_path)
                allowed = True
            except pm.PatchError:
                allowed = False
            return refused == len(bad) and allowed, f"refused {refused}/{len(bad)}; a temp dir allowed: {allowed}"
        t.check("patch_movie --out refuses the game folder, allows elsewhere", n_out_guard)

        def n_custom_ui_guard():
            # review finding 5: a --ui outside the repo protects its game ROOT, not just
            # DATA/, and build_custom_frontend uses the same rule (before any build work),
            # through junctions too.
            g = os.path.join(tmp, "SR")
            os.makedirs(os.path.join(g, "DATA"), exist_ok=True)
            cui = os.path.join(g, "DATA", "ui.AST")
            open(cui, "wb").write(b"not a container")
            bad = [os.path.join(g, "x.AST"), os.path.join(g, "DATA", "ui.AST"), g]
            refused = 0
            for o in bad:
                try:
                    pm.refuse_game_folder(o, cui)
                except pm.PatchError:
                    refused += 1
            notes = [f"patch_movie refused {refused}/{len(bad)}"]
            ok = refused == len(bad)
            # build_custom_frontend --ui X/DATA/ui.AST --out X/DATA/ui.AST: refused before reading
            try:
                bcf.build(cui, pdir, cui, tmp, write=True, log=quiet)
                ok = False; notes.append("build overwrote the --ui file")
            except pm.PatchError:
                notes.append("build refused --out == --ui")
            ok = ok and open(cui, "rb").read() == b"not a container"
            # --out through a junction into the default game folder
            jn = os.path.join(tmp, "jn")
            try:
                import _winapi
                _winapi.CreateJunction(pm.DATA_DIR, jn)
                try:
                    bcf.build(ui_path, pdir, os.path.join(jn, "ui.AST"), tmp, write=False, log=quiet)
                    ok = False; notes.append("junction NOT refused")
                except pm.PatchError:
                    notes.append("junction refused")
                finally:
                    os.rmdir(jn)  # removes the junction only, never the target
            except (ImportError, OSError) as ex:
                notes.append(f"junction case skipped ({ex})")
            return ok, "; ".join(notes)
        t.check("custom --ui: game root protected; build_custom_frontend same rule, via junction too", n_custom_ui_guard)

        def o_include():
            # step 7: `.include 'x.aptasm'` splices a file (nested, relative to the patch dir);
            # a cycle or a missing file is an error; build_custom_frontend counts includes as used.
            d = os.path.join(tmp, "inc")
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, "a.aptasm"), "w").write("    EA_PushZero\n.include 'b.aptasm'\n")
            open(os.path.join(d, "b.aptasm"), "w").write("    Pop\n")
            open(os.path.join(d, "c.aptasm"), "w").write(".include 'c.aptasm'\n")
            (mv, ops), = pm.parse_patch("movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n"
                                        ".include 'a.aptasm'\nend\n", base_dir=d)
            body = [ln.strip() for ln in ops[0].body.splitlines() if ln.strip()]
            incs = sorted(os.path.basename(x) for x in ops[0].includes)
            notes = [f"body {body}", f"includes {incs}"]
            ok = body == ["EA_PushZero", "Pop"] and incs == ["a.aptasm", "b.aptasm"]
            for bad in ("c.aptasm", "missing.aptasm"):
                try:
                    pm.parse_patch(f"movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n"
                                   f".include '{bad}'\nend\n", base_dir=d)
                    ok = False; notes.append(f"{bad} NOT refused")
                except pm.PatchError:
                    notes.append(f"{bad} refused")
            return ok, "; ".join(notes)
        t.check(".include splices nested files; cycles and missing files refused", o_include)

        def p_nested_regs():
            # a nested DefineFunction2 has its own registers: the host function's register
            # check must skip its body (ScrGameplayOpt.HandleInput has 6 registers), but still
            # reject the same register used by the snippet's own code.
            nested = ("movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n"
                      "    Push [reg2]\n    EA_PushConstByte 'SR_T'\n"
                      "    .func2 ''(r3 'x') flags=0xA0129\n        Push [reg9]\n        StoreRegister 8\n"
                      "        Pop\n    .end\n    SetMember\nend\n")
            own = ("movie SCRGAMEPLAYOPT\nprepend-to function HandleInput in ScrGameplayOpt\n"
                   "    Push [reg9]\n    Pop\nend\n")
            try:
                _, _, _, _, p = t.run("SCRGAMEPLAYOPT", nested)
                ok1 = not any("register" in w for w in p.warnings)
            except pm.PatchError as ex:
                return False, f"nested body wrongly refused: {ex}"
            try:
                t.run("SCRGAMEPLAYOPT", own)
                return False, "reg9 in the snippet's own code NOT refused"
            except pm.PatchError:
                pass
            return ok1, f"nested reg9/StoreRegister 8 accepted without register warnings: {ok1}; own reg9 refused"
        t.check("register checks skip nested function bodies (own code still checked)", p_nested_regs)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    ok = all(t.results)
    print(f"{'ALL PASS' if ok else 'FAILURES'}: {sum(t.results)}/{len(t.results)} checks ({time.time() - t0:.1f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
