#!/usr/bin/env python3
"""account_diffs.py - account for EVERY function that differs between two deployed guest/ppc trees.

Release lane R1 (docs/SPLICE_CONVERSION.md): the guest code is the pristine XenonRecomp output plus
host hooks. This tool proves that nothing that needs a hook was missed. It compares two deployed
trees function by function (all ~34,500), e.g. the pre-conversion tree (pristine + the 310 splices +
transforms + any drift) against the tree the converted build generates, and every function that
differs must be ACCOUNTED FOR by a machine-checkable reference:

  (a) hook       the new body calls a [[midasm_hook]] of tools/regen/superman_regen.toml, or the
                 function has a kernel override registered in docs/OVERRIDES.json with `default_exe`
                 (an R1 port); every such hook / override must be registered. A hook deleted
                 from the toml is accounted when the old body minus that hook's call (a call
                 statement, or the whole `if (hook(...)) {...}` block) equals the new body
  (b) patch      every differing line lies in the code of an instruction a recompiler patch in
                 tools/regen/patches/ implements (PPC_INST_<MNEMONIC> in the patch), or the
                 function / its new call is a toml `functions` entry
  (c) transform  the bodies are equal once comments, labels, whitespace and the post-regen
                 division guard is normalised (the form patch 0006 emits, patch_div_guards.py
                 wrote before iter-467; hand guards in old bodies count as the same division)
  (d) census     the function is a DELETE in the census tables of docs/SPLICE_CONVERSION.md, with
                 its class and reason; for a "logging" class the removed lines are checked
                 mechanically (no register / guest-memory write, call, return, goto or sleep)

Anything else is UNEXPLAINED. Exit 1 on any UNEXPLAINED function, on an unregistered hook, and
(with --strict) on a census claim the mechanical check contradicts (CHECK).

    python tools/regen/account_diffs.py --old <old guest/ppc> --new <new guest/ppc>
                                        [--out table.tsv] [--json summary.json] [--strict]

Typical: --old = a checkout of dev before R1 (its guest/ppc), --new = this checkout's guest/ppc.
ASCII output. ~5-10 min on two 140 MB trees (HDD).
"""
import argparse
import difflib
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
HEAD = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\)")
LABEL = re.compile(r"^loc_[0-9A-Fa-f]+:$")
INSN_COMMENT = re.compile(r"^\s*//\s*([a-z][a-z0-9.]*)\b")
DIV = re.compile(r"^(?P<lhs>ctx\.\w+\.\w+|\w+\.\w+)\s*=\s*.*?\?.*?(?P<a>ctx\.\w+\.[us](?:32|64)|\w+\.[us](?:32|64))\s*/\s*"
                 r"(?P<b>ctx\.\w+\.[us](?:32|64)|\w+\.[us](?:32|64)).*;")
def bodies(d):
    out, files = {}, sorted(n for n in os.listdir(d) if re.match(r"ppc_recomp\.\d+\.cpp$", n))
    for name in files:
        cur, buf = None, []
        with open(os.path.join(d, name), encoding="utf-8", errors="surrogateescape") as f:
            for line in f:
                m = HEAD.match(line)
                if m:
                    cur, buf = m.group(1), [line]
                    continue
                if cur is not None:
                    buf.append(line)
                    if line.startswith("}"):
                        out[cur] = "".join(buf)
                        cur = None
    return out


MNEMONICS = set()  # filled from the new tree's instruction comments (it holds no hand code)


def norm(body):
    """[(normalised line, mnemonic of the instruction block it belongs to)], div-canonical flag."""
    out, mn, div = [], "", False
    for raw in body.replace("\r\n", "\n").split("\n"):
        s = raw.strip()
        m = INSN_COMMENT.match(s)
        if m and (not MNEMONICS or m.group(1) in MNEMONICS):
            mn = m.group(1)
        if not s or s.startswith("//") or LABEL.match(s):
            continue
        if '"' not in s and "//" in s:
            s = s[:s.index("//")].rstrip()
            if not s:
                continue
        s = re.sub(r"\s+", " ", s)
        d = DIV.match(s)
        if d:
            s = "%s = %s / %s;" % (d.group("lhs"), d.group("a"), d.group("b"))
            div = True
        out.append((s, mn))
    return out, div


def parse_census(path):
    """{function: (section, class, conversion, reason)} from the census tables."""
    out, section = {}, ""
    if not os.path.exists(path):
        return out
    ident_section = False
    for line in open(path, encoding="utf-8"):
        if line.startswith("### "):
            section = line[4:].strip()
            ident_section = "identical" in section.lower()
            continue
        if line.startswith("## "):
            section, ident_section = "", False
            continue
        if not section:
            continue
        if line.startswith("| `"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            name = cells[0].strip("`")
            if len(cells) < 5:
                continue
            out[fn_of(name)] = (section, cells[1], cells[4], (cells[6] if len(cells) > 6 else "") or cells[3],
                                cells[5] if len(cells) > 5 else "")
        elif ident_section and "`" in line and not line.startswith("|"):
            for name in re.findall(r"`(\w+)`", line):
                out.setdefault(fn_of(name), (section, "IDENTICAL", "DELETE", "identical to the generated code", ""))
    return out


def fn_of(name):
    if re.fullmatch(r"sub_[0-9A-Fa-f]{8}", name):
        return name
    m = re.search(r"([0-9A-F]{8})$", name)
    return "sub_" + m.group(1) if m else name


def patched_mnemonics(pdir):
    """Mnemonics a recompiler patch implements: PPC_INST_<X> on its added lines, plus the
    mnemonics its Subject names (a token with a digit or a trailing '.', or one the patch
    names as PPC_INST_<X> anywhere, e.g. in the context of the case it edits)."""
    out = set()
    for p in sorted(glob.glob(os.path.join(pdir, "*.patch"))):
        text = open(p, encoding="utf-8", errors="replace").read()
        anywhere = {m.lower() for m in re.findall(r"PPC_INST_([A-Z0-9_]+)", text)}
        out |= {m.lower() for l in text.splitlines() if l.startswith("+")
                for m in re.findall(r"PPC_INST_([A-Z0-9_]+)", l)}
        subj = next((l for l in text.splitlines() if l.startswith("Subject:")), "")
        for tok in re.findall(r"[a-z][a-z0-9]*\.?", subj.lower()):
            if re.search(r"\d", tok) or tok.endswith(".") or tok.rstrip(".") in anywhere:
                out.add(tok.rstrip("."))
    return out


LOGCALL = re.compile(r"(?:sr::kernel::|sr::|::)?(?:log\w*|fprintf|printf|fflush|puts|OutputDebugStringA?|SR_LOG\w*)\s*\(")
GENTOKEN = re.compile(r"ctx\.|\bPPC_|\bsub_[0-9A-Fa-f]{8}|\bgoto\b|\breturn\b|\bloc_|__imp__|\bbase\b|\b[rfv]\d+\.|\bcr\d\.|"
                      r"\bctr\.|\bxer\.|\btemp\.|\bea\b|simde_")


def strip_logs(body):
    """The body with every log call replaced by an empty statement `;` (so an `if (n < 5) log();`
    keeps its shape as `if (n < 5) ;`). Calls may span lines; comments are dropped first."""
    lines = []
    for raw in body.replace("\r\n", "\n").split("\n"):
        s = raw.strip()
        if s.startswith("//"):
            continue
        if '"' not in s and "//" in s:
            s = s[:s.index("//")]
        lines.append(s)
    s = "\n".join(lines)
    out, pos = [], 0
    while True:
        m = LOGCALL.search(s, pos)
        if not m:
            out.append(s[pos:])
            break
        out.append(s[pos:m.start()])
        i, depth, quote = m.end(), 1, None
        while i < len(s) and depth:
            c = s[i]
            if quote:
                if c == "\\":
                    i += 1
                elif c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
            else:
                depth += (c == "(") - (c == ")")
            i += 1
        while i < len(s) and s[i] in " \t":
            i += 1
        if i < len(s) and s[i] == ";":
            i += 1
        out.append(LOGMARK + ";")
        pos = i
    return "".join(out)


DECL = re.compile(r"^(?:static\s+|const\s+|constexpr\s+|thread_local\s+)*(?:u?int\d*_t|int|unsigned(?:\s+\w+)?|long|"
                  r"bool|auto|float|double|char|size_t|PPCRegister|PPCVRegister|std::\w+(?:<[^>]*>)?)\b[^;]*$")
WRITES = re.compile(r"\bsub_[0-9A-Fa-f]{8}\s*\(|__imp__\w+\s*\(|PPC_STORE_|writeGuest|PPC_MM_STORE|PPC_CALL_INDIRECT_FUNC|"
                    r"ctx\.\w+(?:\.\w+|\[\w+\])*\s*(?:=|\+=|-=|\|=|&=|\^=)(?!=)")


LOGMARK = "__srlog__"


def _tokens(text):
    """Split at ';' '{' '}' outside parens and strings. Yields statement strings and the
    markers '{' / '}'. Labels (`loc_X:`, `case N:`) are dropped."""
    toks, cur, depth, quote, i = [], [], 0, None, 0
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "([":
            depth += 1
            cur.append(c)
        elif c in ")]":
            depth -= 1
            cur.append(c)
        elif depth == 0 and c in ";{}":
            s = re.sub(r"\s+", " ", "".join(cur)).strip()
            if s or c == ";":
                toks.append(s or ";")
            if c in "{}":
                toks.append(c)
            cur = []
        elif depth == 0 and c == ":" and re.fullmatch(r"\s*(?:loc_[0-9A-Fa-f]+|case [^:]+|default)\s*", "".join(cur)):
            cur = []
        else:
            cur.append(c)
        i += 1
    s = re.sub(r"\s+", " ", "".join(cur)).strip()
    if s:
        toks.append(s)
    return toks


HEADER = re.compile(r"^(?:(?:else )?if ?\(.*\)|while ?\(.*\)|for ?\(.*\)|else|do)$")


def _parse(toks, k=0):
    """[(kind, text, children)]: kind 's' statement, 'b' block {children}, 'h' header + one item."""
    items = []
    while k < len(toks):
        t = toks[k]
        if t == "}":
            return items, k + 1
        if t == "{":
            sub, k = _parse(toks, k + 1)
            items.append(("b", "", sub))
            continue
        k += 1
        items.append(("s", t, None))
    return items, k


def _attach(items):
    """Attach each header (`if (C)` ...) to the item that follows it."""
    out, k = [], 0
    while k < len(items):
        kind, text, ch = items[k]
        if kind == "b":
            out.append(("b", "", _attach(ch)))
            k += 1
            continue
        if HEADER.match(text) and k + 1 < len(items):
            nk, nt, nch = items[k + 1]
            body = ("b", "", _attach(nch)) if nk == "b" else ("s", nt, None)
            out.append(("h", text, [body]))
            k += 2
            continue
        out.append(("s", text, None))
        k += 1
    return out


# an assignment to a HOST variable (a log's counter, backtrace array or last-value cache), not to
# a register, the recompiler's temporaries or guest memory
HOSTSET = re.compile(r"^(?!ctx\.|temp\.|vTemp\.|ea\b|[rfv]\d+\.|cr\d\.|ctr\.|xer\.|reserved\.|env\b|fpscr\.)"
                     r"(?:\+\+|--)?[A-Za-z_]\w*(?:\[[^\]]*\])?\s*(?:=|\+=|-=|\+\+|--)(?!=)")


def _keep_stmt(s):
    if s in (";", "") or LOGMARK in s and re.fullmatch(r"(?:.*\) )?" + LOGMARK + r"(?:\(\))?", s):
        return False
    if not GENTOKEN.search(s):
        return False   # host-only: a log counter
    if DECL.match(s) and not WRITES.search(s):
        return False   # a local read for a log line, or the recompiler's `PPCRegister temp{}`
    if HOSTSET.match(s) and not WRITES.search(s):
        return False   # host bookkeeping of a log (bt[d] = PPC_LOAD_U32(...), t_last = ctx.r31.u32)
    return True


def _flatten(items):
    out = []
    for kind, text, ch in items:
        if kind == "s":
            if _keep_stmt(text):
                out.append(DIVSTMT(text))
        elif kind == "b":
            out += _flatten(ch)
        else:  # header + one item
            inner = _flatten(ch)
            if not inner:
                continue   # a guard of nothing left
            if len(inner) == 1:
                out.append(text + " " + inner[0])
            else:
                out += [text + " {"] + inner + ["}"]
    return out


def statements(text):
    """The C statements of a body that do something besides logging: split at ';' '{' '}',
    headers attached to their statement or block, a block with one statement written without
    braces. Dropped: empty statements, log markers (strip_logs), host-only statements (a log
    counter), local declarations that call or write nothing, and headers left guarding nothing."""
    return _flatten(_attach(_parse(_tokens(text))[0]))


def DIVSTMT(s):
    d = DIV.match(s + ";")
    return "%s = %s / %s" % (d.group("lhs"), d.group("a"), d.group("b")) if d else s


CALLED = re.compile(r"^\s*(?:if \()?([A-Za-z_]\w*)\(")
JUMP = re.compile(r"^\s*(?:goto loc_[0-9A-Fa-f]+|return);\s*$")


def removed_hooks(o, n, hooks):
    """Hook calls in the OLD body whose hook is no longer in the toml (a deleted hook): the names
    called as a statement or an `if (` condition in old, called nowhere in new."""
    names = set()
    for line in o.split("\n"):
        m = CALLED.match(line)
        if m and m.group(1) not in hooks and not m.group(1).startswith(("sub_", "__", "PPC_")) \
                and not re.search(r"\b" + re.escape(m.group(1)) + r"\s*\(", n):
            names.add(m.group(1))
    return sorted(names)


def strip_hook_calls(body, names, eat_jump):
    """The body without the calls of the given hooks: a void call statement (with the
    `goto` / `return` of its jump_address when eat_jump) or a whole `if (hook(...)) {...}
    [else {...}]` block."""
    lines, out, i = body.split("\n"), [], 0
    pat = re.compile(r"^\s*(?:if \()?(?:" + "|".join(map(re.escape, names)) + r")\(")
    while i < len(lines):
        line = lines[i]
        if not pat.match(line):
            out.append(line)
            i += 1
            continue
        i += 1
        if line.lstrip().startswith("if ("):
            depth = line.count("{") - line.count("}")
            while i < len(lines) and depth > 0:
                depth += lines[i].count("{") - lines[i].count("}")
                i += 1
            if i < len(lines) and lines[i].strip().startswith("else"):
                depth = lines[i].count("{") - lines[i].count("}")
                i += 1
                while i < len(lines) and depth > 0:
                    depth += lines[i].count("{") - lines[i].count("}")
                    i += 1
        elif eat_jump and i < len(lines) and JUMP.match(lines[i]):
            i += 1
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--toml", default=os.path.join(HERE, "superman_regen.toml"))
    ap.add_argument("--overrides", default=os.path.join(ROOT, "docs", "OVERRIDES.json"))
    ap.add_argument("--census", default=os.path.join(ROOT, "docs", "SPLICE_CONVERSION.md"))
    ap.add_argument("--patches", default=os.path.join(HERE, "patches"))
    ap.add_argument("--out", default=None, help="TSV table (function, class, accounted_by, verified, detail)")
    ap.add_argument("--json", default=None)
    ap.add_argument("--strict", action="store_true", help="a CHECK (census claim contradicted) fails too")
    a = ap.parse_args()

    import tomllib
    cfg = tomllib.load(open(a.toml, "rb"))
    hooks = {h["name"]: "midasm:0x%08X" % int(h["address"]) for h in cfg.get("midasm_hook", [])}
    toml_entries = cfg.get("main", {}).get("functions", []) + cfg.get("functions", [])
    toml_fns = {"sub_%08X" % int(f["address"]) for f in toml_entries}
    toml_ranges = [(int(f["address"]), int(f["address"]) + int(f["size"])) for f in toml_entries]

    def in_toml_range(fn):
        m = re.fullmatch(r"sub_([0-9A-Fa-f]{8})", fn)
        va = int(m.group(1), 16) if m else -1
        return any(lo < va < hi for lo, hi in toml_ranges)
    reg = json.load(open(a.overrides, encoding="utf-8"))
    ports = {k for k, e in reg.items() if k.startswith("sub_") and e.get("default_exe")}
    census = parse_census(a.census)
    pmn = patched_mnemonics(a.patches)
    hook_re = re.compile(r"\b(" + "|".join(sorted(map(re.escape, hooks), key=len, reverse=True)) + r")\s*\(") if hooks else None

    print(f"account_diffs: reading {a.old} ...", flush=True)
    old = bodies(a.old)
    print(f"account_diffs: reading {a.new} ...", flush=True)
    new = bodies(a.new)
    for b in new.values():
        MNEMONICS.update(re.findall(r"(?m)^	// ([a-z][a-z0-9.]*)", b))
    rows = []
    for fn in sorted(set(old) | set(new)):
        o, n = old.get(fn), new.get(fn)
        if o is not None and n is not None and o == n:
            continue
        cen = census.get(fn)
        if n is None:
            if in_toml_range(fn):
                rows.append((fn, "patch", "absorbed by a toml functions entry (its bounds cover it)", "only in old", ""))
            elif cen and cen[2].upper().startswith("DELETE"):
                rows.append((fn, "census", f"DELETE ({cen[1]}): {cen[3][:120]}", "only in old", ""))
            else:
                rows.append((fn, "UNEXPLAINED", "", "only in old", ""))
            continue
        if o is None:
            if fn in toml_fns:
                rows.append((fn, "patch", "toml functions entry", "only in new", ""))
            else:
                rows.append((fn, "UNEXPLAINED", "", "only in new", ""))
            continue
        on, odiv = norm(o)
        nn, ndiv = norm(n)
        if [x[0] for x in on] == [x[0] for x in nn]:
            how = "division guard (hand guard = generated guard)" if (odiv or ndiv) else "comments / labels / format"
            rows.append((fn, "transform", how, "identical after normalisation", ""))
            continue
        sm = difflib.SequenceMatcher(None, [x[0] for x in on], [x[0] for x in nn], autojunk=False)
        removed, added = [], []
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op in ("replace", "delete"):
                removed += on[i1:i2]
            if op in ("replace", "insert"):
                added += nn[j1:j2]
        if fn in toml_fns:
            rows.append((fn, "patch", "toml functions entry (the function bounds)", "body re-emitted with its toml size",
                         f"-{len(removed)} +{len(added)} lines"))
            continue
        gone = removed_hooks(o, n, hooks)
        if gone and any([x[0] for x in norm(strip_hook_calls(o, gone, eat))[0]] == [x[0] for x in nn]
                        for eat in (False, True)):
            rows.append((fn, "hook", "hook removed: " + ", ".join(gone) + " (not in the toml)",
                         "old body minus the removed hook call(s) == new body", f"-{len(removed)} +{len(added)} lines"))
            continue
        fn_hooks =sorted({hooks[m.group(1)] for m in hook_re.finditer(n)}) if hook_re else []
        unreg = [h for h in fn_hooks if h not in reg]
        added_rest = [x for x in added if not (hook_re and hook_re.search(x[0]))]
        verified, detail = "", ""
        if fn_hooks or fn in ports:
            acct = ", ".join(fn_hooks + ([f"override {fn}"] if fn in ports else []))
            if unreg:
                rows.append((fn, "UNEXPLAINED", acct, "UNREGISTERED hook(s): " + " ".join(unreg), ""))
                continue
            conv = cen[2] if cen else "(not in census)"
            rows.append((fn, "hook", acct, f"registered; census {conv}", f"-{len(removed)} +{len(added)} lines"))
            continue
        if added_rest or removed:
            lines = [x for x in removed + added_rest if not (DECL.match(x[0].rstrip(";")) and not WRITES.search(x[0]))]
            pl = [x for x in lines if x[1] and x[1].rstrip(".") in pmn]
            if pl and len(pl) == len(lines):
                mns = sorted({x[1] for x in pl})
                rows.append((fn, "patch", "recompiler patch: " + ", ".join(mns), "every differing line in a patched instruction", ""))
                continue
            calls_toml = [x for x in added_rest if any(t + "(" in x[0] for t in toml_fns)]
            if cen and cen[2].upper().startswith("TOML") and calls_toml:
                rows.append((fn, "patch", "toml functions entry (" + ", ".join(sorted({t for t in toml_fns if any(t + "(" in x[0] for x in added_rest)})) + ")",
                             "new body tail-calls it", ""))
                continue
        if cen and cen[2].upper().startswith("DELETE"):
            # the mechanical check of a logging-only deletion: the old body minus its log
            # statements must equal the new body
            so = statements(strip_logs(o))
            sn = statements(strip_logs(n))
            logcls = cen[1].upper() == "DIAG" or "logging only" in cen[0].lower()
            rv = re.search(r"REVIEWED[^:]*:\s*(.*)", cen[4])
            if so == sn:
                verified = "verified: old body minus its log statements == new body"
            elif rv:
                verified = "reviewed (census): " + rv.group(1)[:100]
            elif logcls:
                d = [l for l in difflib.unified_diff(so, sn, lineterm="", n=0)
                     if l[:1] in "+-" and l[:3] not in ("+++", "---")]
                verified = "CHECK logging-only claim, residual: " + (d[0][:100] if d else "?")
            elif cen[1].upper() == "IDENTICAL":
                verified = "CHECK census says identical; differs"
            else:
                verified = "census reason (not mechanically checkable)"
            rows.append((fn, "census", f"DELETE ({cen[1]}): {cen[3][:120]}", verified, f"-{len(removed)} +{len(added)} lines"))
            continue
        rows.append((fn, "UNEXPLAINED", "", (cen[2] if cen else "not in census"), f"-{len(removed)} +{len(added)} lines"))

    counts = {}
    for r in rows:
        counts[r[1]] = counts.get(r[1], 0) + 1
    checks = [r for r in rows if r[3].startswith("CHECK")]
    unexplained = [r for r in rows if r[1] == "UNEXPLAINED"]
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write("function\tclass\taccounted_by\tverified\tdetail\n")
            for r in rows:
                f.write("\t".join(x.replace("\t", " ") for x in r) + "\n")
    summ = {"old_functions": len(old), "new_functions": len(new), "differing": len(rows), "by_class": counts,
            "check": len(checks), "unexplained": [r[0] for r in unexplained]}
    if a.json:
        json.dump(summ, open(a.json, "w", encoding="utf-8"), indent=1)
    for r in unexplained + checks:
        print("  %-12s %-14s %s | %s" % (r[1], r[0], r[3], r[2][:100]))
    print(f"account_diffs: {len(old)} old / {len(new)} new functions; {len(rows)} differ: {counts}; "
          f"{len(checks)} CHECK; {len(unexplained)} UNEXPLAINED")
    fail = bool(unexplained) or (a.strict and bool(checks))
    print("ACCOUNT_DIFFS " + ("FAIL" if fail else "PASS"))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
