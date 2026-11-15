#!/usr/bin/env python3
"""flag_transform.py - register-as-local localization of the generated guest code, with ABI
bridges (iter-464 codegen lane).

WHY. XenonRecomp's optimizations (cr_as_local, ctr_as_local, xer_as_local, reserved_as_local,
non_argument_as_local, non_volatile_as_local, skip_lr, skip_msr) turn guest registers into C++
locals of each function: they stay in host registers across calls, dead CR/XER/ctx stores
vanish and the __save*/__rest* helper calls disappear. Two things stop us from just setting
the flags in [main] of superman_regen.toml:
  1. the HAND bodies (splice_bodies.json, the synthetic carried functions, the lines the
     post-regen patch scripts insert) are written in the flags-off `ctx.X` style and carry
     host C++ that reads ctx; rewriting them is risk for no gain, and check_splice_sync.py
     (plus every other lane's hand-edit workflow) expects them verbatim;
  2. this binary breaks the ABI in places (SEH funclets reading the parent's r12/r30 and
     writing its r29, hand-written decoder asm with register inputs, chunk duplicates); a
     per-function exception is something the recompiler cannot express.

WHAT. XenonRecomp runs with every [main] flag OFF (the ctx-style output the patch scripts were
written for). This script then applies EXACTLY the textual rules XenonRecomp's emitter applies
under each flag (recompiler.cpp: the r()/f()/v()/cr()/ctr()/xer()/reserved() lambdas,
printFunctionCall's __save*/__rest* suppression, the MFLR/MTLR/BL/BCTRL lr lines, MFMSR/MTMSRD;
locals declared after PPC_FUNC_PROLOGUE in Recompile(fn)'s order) to every GENERATED function
of the deployed tree per the [localize] table of superman_regen.toml. Hand bodies
(splice_bodies.json), the __save*/__rest* helpers and the synthetic carried functions stay
ctx-style: for them the ctx fields are the registers, and the two styles interoperate through
the ABI BRIDGES computed by abi_audit.py:
  - a localized function that reads a register at entry (SEH funclet, asm helper, chunk)
    loads it from ctx; its localized direct callers flush the register to ctx before the call
    (ctx-style callers already have it there);
  - a function that hands a result back in a register other than r3/r4/f1 (post-call read in
    a caller) writes it to ctx before returning when localized; a localized caller reloads
    it after the call;
  - around such a call the non-volatile ctx fields involved are saved and restored, so a
    ctx-style ancestor still sees them preserved (as the flags-off __restgprlr epilogues did).
Functions with live-in registers but no direct caller (chunks the analyser split off, SEH
filters, vtable-only targets) load from ctx at entry and log once ([ABI_UNBRIDGED]) if they
ever run.

PROOF. `--equivalence BASE_DIR FLAGS_DIR` applies the transform (no bridges) to every function
of a flags-off regen and compares it with the flags-on regen of the same XEX: 0 differences
over all 34,511 functions = the transform IS the recompiler's flag semantics.

    python tools/regen/flag_transform.py --equivalence tools/regen/ppc_out_base tools/regen/ppc_out_all --toml tools/regen/variants/all.toml
    python tools/regen/flag_transform.py --deployed guest/ppc --solve   # fresh ctx-style tree: audit + bridge spec + localize
    python tools/regen/flag_transform.py --deployed guest/ppc           # re-apply with the committed spec (idempotent)
    python tools/regen/flag_transform.py --collisions                   # generated-body locals that shadow register names
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_TOML = os.path.join(HERE, "superman_regen.toml")
DEFAULT_SPEC = os.path.join(HERE, "abi_bridge.json")
SPLICE_BODIES = os.path.join(HERE, "splice_bodies.json")
FLAG_NAMES = ("skip_lr", "skip_msr", "ctr_as_local", "xer_as_local", "reserved_as_local",
              "cr_as_local", "non_argument_as_local", "non_volatile_as_local")
FUNC_RE = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\) \{", re.M)
LOCALIZED_MARK = "#define SR_PPC_LOCALIZED 1"
HELPER = re.compile(r"^__(save|rest)(gprlr|fpr|vmx)_\d+$")


def load_flags(toml_path=DEFAULT_TOML, table=None):
    """The flag values of one toml table. table=None: [localize] when the file has one (the
    policy this script applies to the deployed tree), else [main] (XenonRecomp's own)."""
    text = open(toml_path, encoding="utf-8").read()
    if table is None:
        table = "localize" if re.search(r"^\[localize\]", text, re.M) else "main"
    flags = {k: False for k in FLAG_NAMES}
    cur = None
    for line in text.split("\n"):
        h = re.match(r"^\s*\[+([\w.]+)\]+", line)
        if h:
            cur = h.group(1)
            continue
        m = re.match(r"^\s*(\w+)\s*=\s*(true|false)\b", line)
        if m and m.group(1) in flags and cur == table:
            flags[m.group(1)] = m.group(2) == "true"
    return flags


def is_local(kind, idx, flags):
    """recompiler.cpp lambdas r()/f()/v()/cr()/ctr()/xer()/reserved()."""
    na, nv = flags["non_argument_as_local"], flags["non_volatile_as_local"]
    if kind == "r":
        return (na and idx in (0, 2, 11, 12)) or (nv and idx >= 14)
    if kind == "f":
        return (na and idx == 0) or (nv and idx >= 14)
    if kind == "v":
        return (na and 32 <= idx <= 63) or (nv and (14 <= idx <= 31 or 64 <= idx <= 127))
    if kind == "cr":
        return flags["cr_as_local"]
    if kind == "ctr":
        return flags["ctr_as_local"]
    if kind == "xer":
        return flags["xer_as_local"]
    if kind == "reserved":
        return flags["reserved_as_local"]
    return False


def is_local_key(key, flags):
    """abi_audit key (r30, cr6, xer.ca, v100 ...) -> localized under these flags?"""
    m = re.match(r"^([rfv])(\d+)$", key)
    if m:
        return is_local(m.group(1), int(m.group(2)), flags)
    if key.startswith("cr"):
        return is_local("cr", 0, flags)
    return is_local(key.split(".")[0], 0, flags)


CTX_TOKEN = re.compile(r"\bctx\.(?:(r|f|v)(\d{1,3})|(cr)([0-7])|(ctr|xer|reserved))\b")
SAVE_REST = re.compile(r"^\s*__(?:save|rest)(?:gprlr|fpr|vmx)_\d+\(ctx, base\);\s*$")
LR_LINE = re.compile(r"^\s*(?:ctx\.lr = [^;]*;|ctx\.\w+\.u64 = ctx\.lr;)\s*$")
MSR_LINE = re.compile(r"^\s*(?:ctx\.msr = [^;]*;|ctx\.\w+\.u64 = ctx\.msr;)\s*$")
DECL_TYPE = {"ctr": "PPCRegister", "xer": "PPCXERRegister", "reserved": "PPCRegister",
             "cr": "PPCCRRegister", "r": "PPCRegister", "f": "PPCRegister", "v": "PPCVRegister"}
LOCAL_DECL = re.compile(r"^\t(?:PPCRegister|PPCXERRegister|PPCCRRegister|PPCVRegister) "
                        r"(?:ctr|xer|reserved|cr[0-7]|[rfv]\d{1,3})\{\};$")
DECL_HAND = re.compile(r"\b(?:auto|bool|int|unsigned|long|uint\d+_t|int\d+_t|size_t|float|double|"
                       r"PPCRegister|PPCVRegister|PPCCRRegister|const)\s*[&*]?\s+(\w+)\s*[=;,)\[{]")
BRIDGE_TAG = "// abi-bridge"
CALL_LINE = re.compile(r"^(?:__imp__)?(sub_[0-9A-F]+)\(ctx, base\);$")
RET_LINE = re.compile(r"^(\s*)return;\s*$")
CRET_LINE = re.compile(r"^(\s*)if \((.*)\) return;\s*$")
NONVOL = re.compile(r"^(?:r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|v(?:1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7])|cr[234])$")
# iter-465: registers flushed at INDIRECT call sites when the function writes them (the VMX
# non-volatiles: table-dispatched codec helpers take their constants in v64+; see solve_spec)
INDIRECT_FLUSH = re.compile(r"^v(?:1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7])$")


def local_order(flags):
    """Every possible local name, in Recompile(fn)'s declaration order."""
    names = []
    for n in ("ctr", "xer", "reserved"):
        if is_local(n, 0, flags):
            names.append((n, n))
    for i in range(8):
        if flags["cr_as_local"]:
            names.append(("cr", "cr%d" % i))
    for kind, hi in (("r", 32), ("f", 32), ("v", 128)):
        for i in range(hi):
            if is_local(kind, i, flags):
                names.append((kind, "%s%d" % (kind, i)))
    return names


def strip_code(line):
    """Code part of a line: string literals and // comments removed."""
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    return re.sub(r"//.*$", "", line)


def rename_hand_shadows(lines, flags):
    """A generated body may carry C++ variables named like a register (a patch script's
    temporary, the old hand edits of a body that is no longer spliced). In a ctx-style body
    every guest register is spelled `ctx.X`, so a bare X can only be such a variable; once
    ctx.X becomes the local X they would collide. Rename the variable X -> X_h first.
    Idempotent: a flag-style body declares X as a register local and is left alone."""
    names = {n for _, n in local_order(flags)}
    reg_declared = {ln.split()[1].split("{")[0] for ln in lines if LOCAL_DECL.match(ln)}
    hand = set()
    for ln in lines:
        for m in DECL_HAND.finditer(strip_code(ln)):
            if m.group(1) in names and m.group(1) not in reg_declared:
                hand.add(m.group(1))
    if not hand:
        return lines, set()
    pat = re.compile(r"(?<![\w.])(" + "|".join(sorted(hand)) + r")\b")
    out = []
    for ln in lines:
        if "//" in ln and '"' not in ln:
            code, cmt = ln.split("//", 1)
            out.append(pat.sub(r"\1_h", code) + "//" + cmt)
        else:
            out.append(pat.sub(r"\1_h", ln))
    return out, hand


def reg_var(key):
    """bridge key (abi_audit: r30, cr6, xer.ca ...) -> local variable name."""
    return "xer" if key.startswith("xer") else key


def var_type(name):
    return DECL_TYPE[name if name in ("ctr", "xer", "reserved") else name[:1] if name[0] in "rfv" else "cr"]


def spec_regs(entry, which, flags):
    """The IN/OUT variable names of a spec entry that are locals under these flags."""
    return sorted({reg_var(k) for k in entry.get(which, []) if is_local_key(k, flags)})


def apply_bridges(lines, fn, spec, flags, localized):
    """Insert the ABI bridge lines. In a localized caller, at a direct call of a bridged G:
    save the non-volatile ctx fields involved, flush IN(G) to ctx, call, reload OUT(G),
    restore the saved fields. In a localized bridged F: write OUT(F) to ctx before every
    return. (The entry loads of IN(F) are added after the declarations by transform_body.)
    A ctx-style function needs none of it: it reads and writes ctx natively."""
    if not localized:
        return lines
    me = spec.get(fn)
    me_out = spec_regs(me, "out", flags) if me else []
    me_ind = spec_regs(me, "indirect", flags) if me else []
    out = []
    for ln in lines:
        code = strip_code(ln).strip()
        if me_ind and "PPC_CALL_INDIRECT_FUNC(" in code:
            # iter-465: indirect call site (bctrl/bctr) - the unknown callee reads its live-in
            # registers from ctx: flush the VMX non-volatiles this function writes, around a
            # keep/restore of the ctx fields (a ctx-style ancestor sees them preserved).
            ind = re.match(r"^(\s*)", ln).group(1)
            cmt = out.pop() if out and re.match(r"\s*// bctr", out[-1]) else None
            out.append("%s{ %s <indirect>" % (ind, BRIDGE_TAG))
            for x in me_ind:
                out.append("%s\t%s _sr_keep_%s = ctx.%s; %s keep" % (ind, var_type(x), x, x, BRIDGE_TAG))
            for x in me_ind:
                out.append("%s\tctx.%s = %s; %s flush" % (ind, x, x, BRIDGE_TAG))
            if cmt is not None:
                out.append("\t" + cmt)
            out.append("\t" + ln)
            for x in me_ind:
                out.append("%s\tctx.%s = _sr_keep_%s; %s keep" % (ind, x, x, BRIDGE_TAG))
            out.append("%s} %s" % (ind, BRIDGE_TAG))
            continue
        m = CALL_LINE.match(code)
        if m and m.group(1) in spec:
            g = spec[m.group(1)]
            g_in, g_out = spec_regs(g, "in", flags), spec_regs(g, "out", flags)
            if g_in or g_out:
                ind = re.match(r"^(\s*)", ln).group(1)
                keep = sorted({x for x in g_in + g_out if NONVOL.match(x)})
                # keep the recompiler's `// bl 0x...` comment directly above the call: it is
                # the discriminator scripts/audit_forcing.py uses for "generated call"
                cmt = out.pop() if out and re.match(r"\s*// b[l ]", out[-1]) else None
                out.append("%s{ %s %s" % (ind, BRIDGE_TAG, m.group(1)))
                for x in keep:
                    out.append("%s\t%s _sr_keep_%s = ctx.%s; %s keep" % (ind, var_type(x), x, x, BRIDGE_TAG))
                for x in g_in:
                    out.append("%s\tctx.%s = %s; %s flush" % (ind, x, x, BRIDGE_TAG))
                if cmt is not None:
                    out.append("\t" + cmt)
                out.append("\t" + ln)
                for x in g_out:
                    out.append("%s\t%s = ctx.%s; %s reload" % (ind, x, x, BRIDGE_TAG))
                for x in keep:
                    out.append("%s\tctx.%s = _sr_keep_%s; %s keep" % (ind, x, x, BRIDGE_TAG))
                out.append("%s} %s" % (ind, BRIDGE_TAG))
                continue
        if me_out:
            r = RET_LINE.match(ln)
            if r:
                for x in me_out:
                    out.append("%sctx.%s = %s; %s out" % (r.group(1), x, x, BRIDGE_TAG))
                out.append(ln)
                continue
            c = CRET_LINE.match(ln)
            if c:
                # `if (cr6.eq) return;` (a conditional blr: the condition is a CR-bit test,
                # no side effects) - flush under the same condition on a tagged line BEFORE
                # the untouched original, so a strip + re-apply is exact
                wb = " ".join("ctx.%s = %s;" % (x, x) for x in me_out)
                out.append("%sif (%s) { %s } %s out" % (c.group(1), c.group(2), wb, BRIDGE_TAG))
                out.append(ln)
                continue
        out.append(ln)
    return out


def strip_bridges(lines):
    """Drop prior bridge lines (idempotency). The call line inside a bridge block was
    indented one tab deeper; put it back."""
    out = []
    depth = 0
    for ln in lines:
        if BRIDGE_TAG in ln:
            s = ln.strip()
            if s.startswith("{"):
                depth += 1
            elif s.startswith("}"):
                depth -= 1
            continue
        if depth and ln.startswith("\t"):
            ln = ln[1:]
        out.append(ln)
    return out


def transform_body(body, flags, fn=None, spec=None, localize=True):
    """ctx-style PPC_FUNC_IMPL body -> flag style (+ ABI bridges from `spec`). Idempotent:
    a localized body is left as is; prior bridge lines are dropped and re-derived.
    localize=False (hand body, helper): the body is returned unchanged apart from the
    bridge-line strip (ctx-style functions never carry bridge lines)."""
    lines = strip_bridges(body.split("\n"))
    if not localize:
        return "\n".join(lines)
    lines, _ = rename_hand_shadows(lines, flags)
    out = []
    for ln in lines:
        code = strip_code(ln)
        if flags["non_volatile_as_local"] and SAVE_REST.match(code):
            continue
        if flags["skip_lr"] and LR_LINE.match(code):
            continue
        if flags["skip_msr"] and MSR_LINE.match(code):
            continue

        def sub(m):
            if m.group(1):
                kind, idx = m.group(1), int(m.group(2))
                return ("%s%d" % (kind, idx)) if is_local(kind, idx, flags) else m.group(0)
            if m.group(3):
                return ("cr" + m.group(4)) if flags["cr_as_local"] else m.group(0)
            name = m.group(5)
            return name if is_local(name, 0, flags) else m.group(0)
        out.append(CTX_TOKEN.sub(sub, ln))
    entry_in = []
    if spec:
        out = apply_bridges(out, fn, spec, flags, True)
        if fn in spec:
            entry_in = spec_regs(spec[fn], "in", flags)
    # which locals the code references (code text only; existing local decls excluded)
    used = set(entry_in)
    names = local_order(flags)
    namepat = re.compile(r"(?<![\w.])(" + "|".join(sorted({n for _, n in names}, key=len, reverse=True))
                         + r")(?=\.|\b)") if names else None
    declared = set()
    for ln in out:
        if LOCAL_DECL.match(ln):
            declared.add(ln.split()[1].split("{")[0])
            continue
        if namepat is None:
            continue
        for m in namepat.finditer(strip_code(ln)):
            used.add(m.group(1))
    need = [(k, n) for k, n in names if n in used and n not in declared]
    if need or entry_in:
        for i, ln in enumerate(out):
            if ln.strip() == "PPC_FUNC_PROLOGUE();":
                # keep Recompile(fn)'s order relative to any locals already declared
                j = i + 1
                existing = []
                while j < len(out) and LOCAL_DECL.match(out[j]):
                    existing.append(out[j].split()[1].split("{")[0])
                    j += 1
                allnames = [n for _, n in names if n in used or n in existing]
                block = ["\t%s %s{};" % (DECL_TYPE[k], n) for k, n in names if n in allnames]
                if entry_in:
                    if spec[fn].get("unbridged"):
                        block.append('\tSR_ABI_UNBRIDGED_ENTRY(0x%s, "%s"); %s entry'
                                     % (fn[4:], ",".join(entry_in), BRIDGE_TAG))
                    block += ["\t%s = ctx.%s; %s in" % (x, x, BRIDGE_TAG) for x in entry_in]
                out[i + 1:j] = block
                break
        else:
            raise ValueError("no PPC_FUNC_PROLOGUE() in body")
    return "\n".join(out)


def parse_bodies(text):
    res = {}
    for m in FUNC_RE.finditer(text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            c = text[i]
            depth += (c == "{") - (c == "}")
            i += 1
        res[m.group(1)] = (m.start(), i)
    return res


def read(path):
    return open(path, encoding="utf-8", errors="surrogateescape", newline="").read()


def write(path, text):
    open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)


def all_bodies(d):
    out = {}
    for name in sorted(os.listdir(d)):
        if name.startswith("ppc_recomp.") and name.endswith(".cpp"):
            t = read(os.path.join(d, name))
            for fn, (a, b) in parse_bodies(t).items():
                out[fn] = t[a:b]
    return out


ACTIVE_TOML = DEFAULT_TOML   # main() sets it from --toml


def keep_ctx_style(toml_path=None):
    """[localize] keep_ctx_style = ["sub_X", ...]: GENERATED functions kept ctx-style on purpose
    (they count as hand bodies for the ABI bridges). iter-465: sub_822C6E30 (cloth tangents) -
    tools/regen/cloth_capacity.py copies its ctx-style body text into the build at configure
    time (kernel/cloth_capacity.cpp; the body is hash-checked), so it must keep that spelling."""
    text = open(toml_path or ACTIVE_TOML, encoding="utf-8").read()
    m = re.search(r"^\s*keep_ctx_style\s*=\s*\[([^\]]*)\]", text, re.M)
    return set(re.findall(r'"(\w+)"', m.group(1))) if m else set()


def hand_functions():
    """The ctx-style set: splice bodies (incl. the synthetic carried functions, which are
    spliced too) + the [localize] keep_ctx_style list + the __save*/__rest* helpers (checked
    by name at the call sites). The register-based keep (keep_ctx_style_regs) needs the bodies:
    cmd_deployed adds it (keep_by_regs)."""
    # R1 (docs/SPLICE_CONVERSION.md): there are no spliced bodies any more; the functions that
    # were spliced stay ctx-style through [localize] keep_ctx_style. A legacy checkout that still
    # has splice_bodies.json gets the old union.
    bodies = json.load(open(SPLICE_BODIES, encoding="utf-8")) if os.path.exists(SPLICE_BODIES) else {}
    return {fn for fn, b in bodies.items() if isinstance(b, str)} | keep_ctx_style()


def keep_ctx_style_regs(toml_path=None):
    """[localize] keep_ctx_style_regs = ["v64-v127", ...]: register RANGES whose users stay
    ctx-style. Returns a compiled pattern over `ctx.<reg>` references, or None."""
    text = open(toml_path or ACTIVE_TOML, encoding="utf-8").read()
    m = re.search(r"^\s*keep_ctx_style_regs\s*=\s*\[([^\]]*)\]", text, re.M)
    if not m:
        return None
    alts = []
    for item in re.findall(r'"([^"]+)"', m.group(1)):
        r = re.match(r"^([rfv])(\d+)-[rfv](\d+)$", item)
        if r:
            lo, hi = int(r.group(2)), int(r.group(3))
            alts += ["%s%d" % (r.group(1), i) for i in range(lo, hi + 1)]
        else:
            alts.append(item)
    return re.compile(r"ctx\.(?:" + "|".join(sorted(alts, key=len, reverse=True)) + r")\b") if alts else None


def localize_despite_regs(toml_path=None):
    """iter-466 tick2: [localize] localize_despite_regs = ["sub_X", ...]: GENERATED functions the
    keep_ctx_style_regs rule would keep ctx-style but that only use those registers as their own
    callee-saved scratch (saved by __savevmx_* at entry, restored at exit, never an input or an
    output - checked by the abi_audit solve), so they localize like any other body. Not for the
    codec asm cluster the rule exists for."""
    text = open(toml_path or ACTIVE_TOML, encoding="utf-8").read()
    m = re.search(r"^\s*localize_despite_regs\s*=\s*\[([^\]]*)\]", text, re.M)
    return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()


def keep_by_regs(texts, hand):
    """iter-465: every GENERATED function whose body references a register of the
    keep_ctx_style_regs ranges stays ctx-style. WHY: the XDK codec asm (XMV = VC-1, WMA) uses
    v64-v127 as a register file shared across table-dispatched helpers and transparent
    orchestrators (constants written by one function, read at entry by another reached
    through bctrl, passed through functions that never touch them); dataflow over direct call
    edges cannot carry that, ctx can. Bridge lines are ignored (a localized body's bridges
    mention ctx.<reg>), so a re-apply on a localized tree finds the same set."""
    pat = keep_ctx_style_regs()
    if pat is None:
        return set()
    despite = localize_despite_regs()
    keep = set()
    for t in texts.values():
        for fn, (a, b) in parse_bodies(t).items():
            if fn in hand or HELPER.match(fn) or fn in despite:
                continue
            body = t[a:b]
            if pat.search(body) and any(pat.search(ln) for ln in body.split("\n") if BRIDGE_TAG not in ln):
                keep.add(fn)
    return keep


def is_ctx_style(fn, hand):
    return fn in hand or bool(HELPER.match(fn))


def cmd_equivalence(base_dir, flags_dir, flags):
    base = all_bodies(base_dir)
    new = all_bodies(flags_dir)
    common = sorted(set(base) & set(new))
    diffs = [fn for fn in common if transform_body(base[fn], flags) != new[fn]]
    print("[FLAG_EQUIV] flags=%s" % ",".join(k for k in FLAG_NAMES if flags[k]))
    print("[FLAG_EQUIV] base %d fns, flags %d fns, common %d, only-base %d, only-flags %d, DIFFER %d"
          % (len(base), len(new), len(common), len(set(base) - set(new)), len(set(new) - set(base)), len(diffs)))
    import difflib
    for fn in diffs[:20]:
        print("--- %s" % fn)
        for l in list(difflib.unified_diff(transform_body(base[fn], flags).split("\n"),
                                           new[fn].split("\n"), lineterm="", n=1))[:30]:
            print("   " + l)
    return 1 if diffs else 0


def solve_spec(base_bodies, loc_bodies, flags, hand, log=print):
    """abi_audit over the localized bodies (in memory) -> bridge spec dict."""
    sys.path.insert(0, HERE)
    import abi_audit as A
    ctx_fns = {fn for fn in loc_bodies if is_ctx_style(fn, hand)}
    funcs = {}
    for fn, b in loc_bodies.items():
        lines = A.split_functions(b)[fn]
        funcs[fn] = A.parse(lines, ctx_style=fn in ctx_fns)
    hsaves = A.helper_saves({fn: A.split_functions(b)[fn] for fn, b in base_bodies.items()})
    results, bridge = A.solve(funcs, hsaves, ctx_fns=ctx_fns, log=log)
    callers = A.solve.callers
    spec = {}
    for fn, (IN, OUT) in sorted(bridge.items()):
        direct = sorted(c for c in callers.get(fn, ()) if c != fn)
        spec[fn] = {"in": sorted(IN), "out": sorted(OUT), "callers": len(direct),
                    "style": "ctx" if fn in ctx_fns else "local"}
        if fn in A.solve.unbridged:
            spec[fn]["unbridged"] = True
    # iter-465: INDIRECT call sites. A callee reached through a function table (bctrl) that
    # reads a register at entry gets it from ctx (its entry load), so a localized caller must
    # have the CURRENT value there: at every indirect call site flush the VMX non-volatile
    # registers (v14-v31, v64-v127) this function writes, keeping/restoring the ctx fields
    # around the call like a direct bridge. Found live: the XMV (VC-1) decoder keeps its
    # constants in v64+ and dispatches the motion-compensation predictors through tables -
    # without this the intro movies decode to macroblock garbage (archive/v1_codegen/rolls/
    # sheet_local1_frames.png vs sheet_r5_frames.png). The data (abi_res0): v64+ is 2226 of
    # the entry-read (fn, reg) pairs, v14-31 171; 190 + 18 indirect-calling functions write
    # them; post-indirect-call reads (the other direction) occur in 1 function (r-vol).
    n_ind = 0
    for fn, r in results.items():
        if fn in ctx_fns or "<indirect>" not in r["calls"]:
            continue
        regs = sorted(k for k in r["writes"] if INDIRECT_FLUSH.match(k))
        if regs:
            spec.setdefault(fn, {"in": [], "out": [], "callers": 0, "style": "local"})["indirect"] = regs
            n_ind += 1
    log("[ABI_BRIDGE] indirect call sites: %d localized functions flush written VMX non-volatiles (v14-31, v64-127)" % n_ind)
    cls = {}
    for fn, e in spec.items():
        for k in e["in"]:
            cls["in:" + A.classify_reg(k)] = cls.get("in:" + A.classify_reg(k), 0) + 1
        for k in e["out"]:
            cls["out:" + A.classify_reg(k)] = cls.get("out:" + A.classify_reg(k), 0) + 1
    meta = {"flags": [k for k in FLAG_NAMES if flags[k]], "functions": len(funcs),
            "ctx_style": len(ctx_fns), "bridged": len(spec),
            "unbridged": sum(1 for v in spec.values() if v.get("unbridged")),
            "no_return": len(A.solve.noret), "save_only": sorted(A.SAVE_ONLY),
            "no_return_seed": sorted(A.NO_RETURN), "per_class": dict(sorted(cls.items()))}
    return spec, meta


# iter-466 tick2: the deployed-tree stamp. guest/ppc is untracked, so a commit that changes what
# this transform produces (the transform itself, the bridge spec, the [localize] policy) reaches
# a checkout only when it re-runs `flag_transform.py --deployed guest/ppc`.
# Every successful deploy writes <dir>/.transform_stamp; guest/CMakeLists.txt recomputes the same
# hash at configure time (re-run whenever an input or the stamp changes) and FAILS on a mismatch
# with the command to run. KEEP stamp_inputs / stamp_hash IN SYNC WITH guest/CMakeLists.txt.
STAMP_VERSION = "1"
STAMP_NAME = ".transform_stamp"


def stamp_inputs():
    return [os.path.join(HERE, n) for n in
            ("flag_transform.py", "abi_bridge.json", "superman_regen.toml")]


def stamp_hash():
    """sha256 over the concatenated lowercase hex sha256 of each input file's bytes (the same
    construction as CMake: file(SHA256) per input, then string(SHA256) of the concatenation)."""
    import hashlib
    parts = "".join(hashlib.sha256(open(f, "rb").read()).hexdigest() for f in stamp_inputs())
    return hashlib.sha256(parts.encode("ascii")).hexdigest()


def write_stamp(d):
    with open(os.path.join(d, STAMP_NAME), "w", encoding="ascii", newline="\n") as f:
        f.write("flag_transform v%s %s\n" % (STAMP_VERSION, stamp_hash()))


def cmd_deployed(d, flags, spec_path, solve):
    names = sorted(n for n in os.listdir(d) if n.startswith("ppc_recomp.") and n.endswith(".cpp"))
    texts = {n: read(os.path.join(d, n)) for n in names}
    cfg = os.path.join(d, "ppc_config.h")
    cfg_text = read(cfg)
    localized = LOCALIZED_MARK in cfg_text
    hand = hand_functions()
    by_regs = keep_by_regs(texts, hand)
    if by_regs:
        print("[FLAG_TRANSFORM] keep_ctx_style_regs: %d generated functions stay ctx-style (they touch the kept register ranges)" % len(by_regs))
        hand = hand | by_regs
    if solve:
        if localized:
            print("[FLAG_TRANSFORM] FATAL: %s is already localized - --solve needs the fresh "
                  "ctx-style tree (re-deploy tools/regen/assembled + the patch scripts first)" % d)
            return 1
        base, loc = {}, {}
        for n, t in texts.items():
            for fn, (a, b) in parse_bodies(t).items():
                body = t[a:b].replace("\r\n", "\n")
                base[fn] = body
                loc[fn] = transform_body(body, flags, localize=not is_ctx_style(fn, hand))
        spec, meta = solve_spec(base, loc, flags, hand)
        json.dump({"meta": meta, "functions": spec}, open(spec_path, "w", encoding="utf-8"), indent=1)
        print("[FLAG_TRANSFORM] bridge spec -> %s: %d bridged (%d ctx-style), %d unbridged (no direct caller); %s"
              % (os.path.relpath(spec_path, ROOT), meta["bridged"],
                 sum(1 for v in spec.values() if v["style"] == "ctx"), meta["unbridged"], meta["per_class"]))
    spec = json.load(open(spec_path, encoding="utf-8"))["functions"] if os.path.exists(spec_path) else {}
    if any(flags.values()) and not spec and (flags["non_volatile_as_local"] or flags["non_argument_as_local"]):
        print("[FLAG_TRANSFORM] FATAL: localizing argument/non-volatile registers without a bridge "
              "spec (%s) - run with --solve" % spec_path)
        return 1
    changed = 0
    for n, t in texts.items():
        spans = parse_bodies(t)
        nt = t
        for fn, (a, b) in sorted(spans.items(), key=lambda kv: -kv[1][0]):
            body = nt[a:b]
            crlf = "\r\n" in body
            nb = transform_body(body.replace("\r\n", "\n"), flags, fn, spec, localize=not is_ctx_style(fn, hand))
            if crlf:
                nb = nb.replace("\n", "\r\n")
            if nb != body:
                nt = nt[:a] + nb + nt[b:]
                changed += 1
        if nt != t:
            write(os.path.join(d, n), nt)
    if any(flags.values()) and not localized:
        cfg_text = cfg_text.rstrip("\n").rstrip("\r")
        idx = cfg_text.rfind("#endif")
        add = ("\n// flag_transform.py: guest registers per [localize] of superman_regen.toml are C++ locals\n"
               "// of every generated function (%s). The ctx.<those> fields are maintained only by the\n"
               "// ctx-style functions ([localize] keep_ctx_style) and the ABI bridges - host code must not read them.\n"
               "%s\n" % (", ".join(k for k in FLAG_NAMES if flags[k]), LOCALIZED_MARK))
        cfg_text = cfg_text[:idx] + add + cfg_text[idx:] + "\n"
        write(cfg, cfg_text)
    print("[FLAG_TRANSFORM] %d function bodies rewritten in %s (flags: %s; %d bridged functions; %d ctx-style hand bodies kept)"
          % (changed, d, ",".join(k for k in FLAG_NAMES if flags[k]) or "none", len(spec), len(hand)))
    write_stamp(d)
    print("[FLAG_TRANSFORM] stamp %s written (inputs: %s)" % (os.path.join(d, STAMP_NAME),
          ", ".join(os.path.basename(f) for f in stamp_inputs())))
    return 0


def cmd_collisions(flags, d):
    """Generated bodies whose code declares a C++ variable named like a register local."""
    names = {n for _, n in local_order(flags)}
    hand = hand_functions()
    hits = 0
    for fn, b in sorted(all_bodies(d).items()):
        if is_ctx_style(fn, hand):
            continue
        for ln in b.split("\n"):
            for m in DECL_HAND.finditer(strip_code(ln)):
                if m.group(1) in names:
                    print("  %s: local '%s' shadows a register local (renamed %s_h): %s"
                          % (fn, m.group(1), m.group(1), ln.strip()))
                    hits += 1
    print("[FLAG_COLLISIONS] %d (handled by rename_hand_shadows)" % hits)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toml", default=DEFAULT_TOML)
    ap.add_argument("--table", help="toml table with the flags (default [localize], else [main])")
    ap.add_argument("--spec", default=DEFAULT_SPEC)
    ap.add_argument("--equivalence", nargs=2, metavar=("BASE_DIR", "FLAGS_DIR"))
    ap.add_argument("--deployed", metavar="DIR")
    ap.add_argument("--solve", action="store_true")
    ap.add_argument("--collisions", metavar="DIR")
    a = ap.parse_args()
    flags = load_flags(a.toml, a.table)
    if a.equivalence:
        return cmd_equivalence(a.equivalence[0], a.equivalence[1], flags)
    if a.deployed:
        return cmd_deployed(a.deployed, flags, a.spec, a.solve)
    if a.collisions:
        return cmd_collisions(flags, a.collisions)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
