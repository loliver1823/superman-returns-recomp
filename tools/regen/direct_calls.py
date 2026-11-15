#!/usr/bin/env python3
"""direct_calls.py - iter-467 guestcode EXPERIMENT: hot guest call edges call the callee's body
(__imp__sub_X) directly instead of its weak noinline alias (sub_X), so the compiler can inline it.

Every generated guest function is `__attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
PPC_FUNC_IMPL(__imp__sub_X) {...}`: callers call sub_X, the weak noinline alias a kernel strong
definition replaces (the override mechanism). No call can be inlined. For an edge whose callee has
NO override, calling __imp__sub_X is the same function - but a future override of sub_X would be
silently bypassed on that edge, hence the guard: the list of converted callees is written next to
the code (guest/ppc/direct_calls.list) and tools/regen/direct_calls_guard.py, run by every build of
the guest lib, fails the build when any of them is in docs/OVERRIDES.json or has a strong
definition in kernel/ or guest/ (the trees gen_override_manifest.py scans).

  python tools/regen/direct_calls.py select --edges archive/hot_edges_cm8.json [--min-self 0.02]
         [--max-bytes 2048] [--out tools/regen/direct_calls_edges.json]
  python tools/regen/direct_calls.py apply  guest/ppc --edges tools/regen/direct_calls_edges.json [--force-inline]
  python tools/regen/direct_calls.py revert guest/ppc

`apply` rewrites, inside the caller's body only, each statement `sub_Y(ctx, base);` of a selected
edge to `__imp__sub_Y(ctx, base); /*DC*/` (with --force-inline: `[[clang::always_inline]]` in
front), adds `extern "C"` declarations of the callees at the top of the file, and writes the list.
`revert` undoes exactly those marks. The edge list comes from tools/perf/hot_edges.py (host
profile of the render thread).
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MARK = "/*DC*/"
DECL_BEGIN = "// [DIRECT_CALLS] begin (tools/regen/direct_calls.py)"
DECL_END = "// [DIRECT_CALLS] end"
LIST = "direct_calls.list"
STRONG = re.compile(r"^\s*(?:extern\s+\"C\"\s+)?void\s+(sub_[0-9A-F]{8})\s*\(\s*PPCContext", re.M)
STRONG_MACRO = re.compile(r"(?<!WEAK_)PPC_FUNC\((sub_[0-9A-F]{8})\)")


def overridden():
    """Every guest function with a host strong definition: OVERRIDES.json keys + a scan of the trees."""
    names = set(json.load(open(os.path.join(ROOT, "docs", "OVERRIDES.json"), encoding="utf-8")).keys())
    gen = os.path.normcase(os.path.join(ROOT, "guest", "ppc"))
    for tree in ("kernel", "guest", "gfx", "app"):
        for dp, dn, fn in os.walk(os.path.join(ROOT, tree)):
            if os.path.normcase(dp).startswith(gen):
                continue  # the generated tree: its weak aliases are what an override replaces
            for f in fn:
                if f.endswith((".cpp", ".hpp", ".h", ".inl")):
                    try:
                        t = open(os.path.join(dp, f), encoding="utf-8", errors="replace").read()
                    except OSError:
                        continue
                    names.update(STRONG.findall(t))
                    names.update(STRONG_MACRO.findall(t))
    return names


def cmd_select(a):
    d = json.load(open(a.edges, encoding="utf-8"))
    ov = overridden()
    rows = [r for r in d["edges"] if r["callee_bytes"] <= a.max_bytes and r["edge_self_pct"] >= a.min_self
            and r["callee"] not in ov and r["caller"] not in ov and r["callee"] != r["caller"]]
    out = {"source": d.get("source"), "thread": d.get("thread"), "files": d.get("files"),
           "rule": "callee_bytes <= %d, edge_self_pct >= %g, neither end overridden" % (a.max_bytes, a.min_self),
           "edges": [{k: r[k] for k in ("caller", "callee", "edge_self_pct", "callee_bytes")} for r in rows]}
    json.dump(out, open(a.out, "w", encoding="utf-8"), indent=1)
    print("selected %d edges (edge self sum %.2f%%) -> %s" % (len(rows), sum(r["edge_self_pct"] for r in rows), a.out))


def bodies(text):
    """[(name, start, end)] of each PPC_FUNC_IMPL(__imp__sub_X) body in a generated file."""
    starts = [(m.group(1), m.start()) for m in re.finditer(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]{8})\)", text)]
    out = []
    for i, (n, s) in enumerate(starts):
        e = starts[i + 1][1] if i + 1 < len(starts) else len(text)
        out.append((n, s, e))
    return out


def cmd_apply(a):
    sel = json.load(open(a.edges, encoding="utf-8"))["edges"]
    ov = overridden()
    bad = sorted({r["callee"] for r in sel if r["callee"] in ov})
    if bad:
        sys.exit("direct_calls: refusing - overridden callees in the edge list: " + ", ".join(bad))
    by_caller = {}
    for r in sel:
        by_caller.setdefault(r["caller"], set()).add(r["callee"])
    names = sorted(n for n in os.listdir(a.dir) if n.startswith("ppc_recomp.") and n.endswith(".cpp"))
    done, callees, files = 0, set(), 0
    for n in names:
        p = os.path.join(a.dir, n)
        text = open(p, encoding="utf-8", errors="surrogateescape").read()
        new_parts, last, used = [], 0, set()
        for fn, s, e in bodies(text):
            if fn not in by_caller:
                continue
            body = text[s:e]
            for cal in by_caller[fn]:
                pre = "[[clang::always_inline]] " if a.force_inline else ""
                rx = re.compile(r"^(\s*)" + cal + r"\(ctx, base\);[ \t]*$", re.M)
                body, k = rx.subn(lambda m: "%s%s__imp__%s(ctx, base); %s" % (m.group(1), pre, cal, MARK), body)
                if k:
                    done += k
                    used.add(cal)
            new_parts.append(text[last:s])
            new_parts.append(body)
            last = e
        if not used:
            continue
        new_parts.append(text[last:])
        text = "".join(new_parts)
        decl = DECL_BEGIN + "\n" + "".join('extern "C" PPC_FUNC(__imp__%s);\n' % c for c in sorted(used)) + DECL_END + "\n"
        i = text.find("\n", text.rfind("#include")) + 1
        text = text[:i] + decl + text[i:]
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write(text)
        callees |= used
        files += 1
    # The list = every marked direct call in the tree (also those of an earlier, interrupted apply).
    allc = set()
    mx = re.compile(r"__imp__(sub_[0-9A-F]{8})\(ctx, base\); " + re.escape(MARK))
    for n in names:
        allc.update(mx.findall(open(os.path.join(a.dir, n), encoding="utf-8", errors="surrogateescape").read()))
    with open(os.path.join(a.dir, LIST), "w", encoding="ascii", newline="\n") as f:
        f.write("".join(c + "\n" for c in sorted(allc)))
    print("direct calls: %d call statements in %d files, %d callees (%s)" % (done, files, len(callees),
                                                                           "forced inline" if a.force_inline else "compiler's choice"))


def cmd_revert(a):
    names = sorted(n for n in os.listdir(a.dir) if n.startswith("ppc_recomp.") and n.endswith(".cpp"))
    n_calls = 0
    rx = re.compile(r"(?:\[\[clang::always_inline\]\] )?__imp__(sub_[0-9A-F]{8})\(ctx, base\); " + re.escape(MARK))
    for n in names:
        p = os.path.join(a.dir, n)
        text = open(p, encoding="utf-8", errors="surrogateescape").read()
        if MARK not in text and DECL_BEGIN not in text:
            continue
        text, k = rx.subn(lambda m: "%s(ctx, base);" % m.group(1), text)
        n_calls += k
        i = text.find(DECL_BEGIN)
        if i >= 0:
            j = text.find(DECL_END, i) + len(DECL_END) + 1
            text = text[:i] + text[j:]
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write(text)
    lp = os.path.join(a.dir, LIST)
    if os.path.exists(lp):
        os.remove(lp)
    print("reverted %d direct calls" % n_calls)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("select")
    s.add_argument("--edges", required=True)
    s.add_argument("--min-self", type=float, default=0.02)
    s.add_argument("--max-bytes", type=int, default=2048)
    s.add_argument("--out", default=os.path.join(HERE, "direct_calls_edges.json"))
    p = sub.add_parser("apply")
    p.add_argument("dir")
    p.add_argument("--edges", default=os.path.join(HERE, "direct_calls_edges.json"))
    p.add_argument("--force-inline", action="store_true")
    r = sub.add_parser("revert")
    r.add_argument("dir")
    a = ap.parse_args()
    {"select": cmd_select, "apply": cmd_apply, "revert": cmd_revert}[a.cmd](a)


if __name__ == "__main__":
    main()
