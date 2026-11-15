#!/usr/bin/env python3
"""worldflow_fsm.py - decode the game's FSM graphs and their Lua from a BGFA .AST.

Static, offline, read-only. Built for the level-skip design (iter-463 dev tools):
it shows the game's OWN chapter/objective flow (BaseWorldFlow) so a skip can be
built on the natural path instead of hand-written state pokes.

Formats decoded here (all verified by the self-checks below, not assumed):

* Binary XML document (the engine's parsed-XML form; FSM graphs, DAS docs).
  Header H (big-endian, 0x2C bytes):
    +0x00 u32 magic 0x05F5E0FF      +0x04 u32 0x14
    +0x08 u32 node table offset      +0x0C u32 0
    +0x10 u16 node count, u16 0x0100 +0x14 u32 string-entry offset (0x2C)
    +0x18 u16 string count (x2)      +0x1C u32 string data offset
    +0x20 u32 string data size (x2)  +0x28 u32 1
  String entry: {u32 len, u32 off (from string data), u32 hash}.
  Node (12 bytes): {u32 attr offset (from H), u16 attr count, u16 tag string,
  u16 index of the node's LAST descendant, u16 0xFFFF}. Document order = preorder.
  Attribute: {u16 name string, u16 value string}.
* Script TOC: 54-byte records {u16 0x1001, 8-byte hash, u16le ?, u16le length,
  40-byte upper-case NUL-padded name}; 0x1000 records = named binary-XML docs.
  Body offset = seg_base + u16*16 with seg_base solved (unique, 4 KiB aligned) - bodies
  are NOT always NUL-separated. Every script pairing is checked by exact length, Lua
  start token, S/T kind (state `function main(mode)`, transition `function main()`) and
  a chunk-closing `end`. Scope: single-segment containers (baseworl.AST); objtiv.AST
  keeps a TOC per package and is not handled.

FSM semantics (docs/ENGINE_MAP.md "FE flow FSM"): a state script's name is
`<Graph>FSM_s_<hex(state id)>`; a transition's `dest` is a state id; a transition
with no script is named `TransitionTrue`/`IsCurStateDone` (engine-evaluated).

Usage:
  python tools/worldflow_fsm.py graphs                     # list FSM docs
  python tools/worldflow_fsm.py dump BaseWorldFlow [--lua] # states + transitions
  python tools/worldflow_fsm.py grep GoToNextChapter       # which scripts/states use a token
  python tools/worldflow_fsm.py json OUT.json              # everything, machine-readable
  python tools/worldflow_fsm.py docs [NAME]                # named attribute docs (+ categories)
  python tools/worldflow_fsm.py triggers                   # trigger DAS docs, doc-level view
    (no entity-group placement overrides; tools/objective_table.py is the authoritative table)
  Findings built on it: docs/LEVEL_SKIP_NATURAL_PATH.md. Tests: tools/test_worldflow_fsm.py.
  (--ast PATH selects the container; default: retail DATA/baseworl.AST)
"""
import argparse
import json
import os
import re
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_AST = os.path.join(REPO, "Superman Returns (USA)", "DATA", "baseworl.AST")
BXML_MAGIC = 0x05F5E0FF
TOC_REC = 54


class FormatError(Exception):
    pass


# ---------------------------------------------------------------- binary XML
def parse_bxml(d, H):
    """Parse one binary-XML document whose header starts at H. Returns node list."""
    magic, x14, node_off, zero, ncount, _one, str_ent_off, nstr, nstr2, sdat_off, ssz, ssz2, one = \
        struct.unpack_from(">IIIIHHIHHIIII", d, H)
    if magic != BXML_MAGIC or x14 != 0x14 or str_ent_off != 0x2C or nstr != nstr2 or ssz != ssz2:
        raise FormatError(f"bad bxml header at {H:#x}")
    if sdat_off != 0x2C + 12 * nstr or node_off != sdat_off + ssz:
        raise FormatError(f"bxml section layout mismatch at {H:#x}")
    strs = []
    for k in range(nstr):
        ln, off, _h = struct.unpack_from(">III", d, H + 0x2C + 12 * k)
        a = H + sdat_off + off
        if off + ln >= ssz or d[a + ln] != 0:
            raise FormatError(f"string {k} not NUL-terminated at {H:#x}")
        strs.append(d[a:a + ln].decode("latin1"))
    nodes = []
    for k in range(ncount):
        aoff, acnt, tag, last, pad = struct.unpack_from(">IHHHH", d, H + node_off + 12 * k)
        if pad != 0xFFFF or tag >= nstr or last < k or last >= ncount:
            raise FormatError(f"node {k} malformed at {H:#x}")
        attrs = []
        for j in range(acnt):
            kx, vx = struct.unpack_from(">HH", d, H + aoff + 4 * j)
            if kx >= nstr or vx >= nstr:
                raise FormatError(f"attr {j} of node {k} out of range at {H:#x}")
            attrs.append((strs[kx], strs[vx]))
        nodes.append({"i": k, "tag": strs[tag], "attrs": attrs, "last": last})
    # tree: children = nodes whose parent span is the nearest enclosing
    for n in nodes:
        n["children"] = []
    stack = []
    for n in nodes:
        while stack and stack[-1]["last"] < n["i"]:
            stack.pop()
        if stack:
            if n["last"] > stack[-1]["last"]:
                raise FormatError(f"node {n['i']} span escapes its parent at {H:#x}")
            stack[-1]["children"].append(n)
        stack.append(n)
    return nodes


def find_fsm_docs(d):
    """Every binary-XML doc whose root tag is `fsm`. Returns [(H, nodes)]."""
    out = []
    pos = 0
    while True:
        i = d.find(b"fsm\x00", pos)
        if i < 0:
            break
        pos = i + 1
        # root tag 'fsm' is string 0 -> its entry has off 0 and is the first entry
        for nstr in range(1, 4096):
            tab = i - 0 - 12 * nstr  # candidate: data starts at i, entries end right before i
            H = tab - 0x2C
            if H < 0:
                break
            if struct.unpack_from(">I", d, H)[0] == BXML_MAGIC and \
                    struct.unpack_from(">H", d, H + 0x18)[0] == nstr:
                try:
                    nodes = parse_bxml(d, H)
                except FormatError:
                    break
                if nodes and nodes[0]["tag"] == "fsm":
                    out.append((H, nodes))
                break
    return out


# ---------------------------------------------------------------- script TOC
def find_script_toc(d):
    """All script TOC records (name, length) in file order, across every run."""
    recs = []
    for m in re.finditer(rb"\x10\x01.{12}([A-Z0-9_]+_[ST]_[0-9A-F]+)\x00", d, re.S):
        p = m.start()
        name = m.group(1).decode()
        ln = struct.unpack_from("<H", d, p + 12)[0]
        recs.append((p, name, ln))
    return recs


def _slice_ok(d, off, ln):
    # The byte after a chunk is a NUL, the next chunk (chunks can abut), or, for the
    # last chunk, a section tag - so only the start token and exact length are checked
    # here; the S/T kind check in pair_scripts is the second, independent witness.
    body = d[off:off + ln]
    return len(body) == ln and (body.startswith(b"function") or body.startswith(b"--"))


def pair_scripts(d, recs):
    """Map each TOC record to its Lua body: body = seg_base + u16le(+10)*16, len = u16le(+12).

    seg_base is solved, not assumed: it must be 4 KiB aligned and make EVERY record's
    slice start with a Lua token. Bodies are not always
    NUL-separated (some chunks abut), so the TOC offset, not a NUL scan, is the truth.
    """
    if not recs:
        raise FormatError("no script TOC")
    units = [(r, struct.unpack_from("<H", d, r[0] + 10)[0]) for r in recs]
    (r0, u0) = units[0]
    bases = []
    for m in re.finditer(rb"function main\(", d):
        base = m.start() - u0 * 16
        if base >= 0 and base % 0x1000 == 0 and _slice_ok(d, m.start(), r0[2]):
            if all(_slice_ok(d, base + u * 16, r[2]) for r, u in units):
                bases.append(base)
    if len(bases) != 1:
        raise FormatError(f"script segment base not unique: {[hex(b) for b in bases]}")
    base = bases[0]
    scripts = {}
    for (p, name, ln), u in units:
        bp = base + u * 16
        body = d[bp:bp + ln].decode("latin1")
        kind = name.rsplit("_", 2)[-2]
        is_state = re.search(r"function main\(\s*mode\s*\)", body) is not None
        if (kind == "S") != is_state:
            raise FormatError(f"kind mismatch {name} at {bp:#x}")
        if not body.rstrip().endswith("end"):  # every chunk closes its last function
            raise FormatError(f"length/offset mismatch {name}: body does not end a chunk at {bp:#x}")
        scripts[name] = {"offset": bp, "len": ln, "lua": body}
    return scripts


# ---------------------------------------------------------------- FSM model
def fsm_model(nodes, scripts):
    root = nodes[0]
    ra = dict(root["attrs"])
    g = {"name": ra.get("name"), "start": ra.get("start_state"), "file": ra.get("filename"),
         "states": []}
    for st in (n for n in nodes if n["tag"] == "state"):
        a = dict(st["attrs"])
        s = {"id": a.get("id"), "script": a.get("script"), "kind": a.get("name"),
             "attrs": {}, "transitions": [], "extra": [(k, v) for k, v in st["attrs"]
                                                       if k not in ("id", "script", "name")]}
        for c in st["children"]:
            if c["tag"] == "attributes":
                for at in c["children"]:
                    aa = dict(at["attrs"])
                    s["attrs"][aa.get("key", "?")] = aa.get("value")
            if c["tag"] == "connectivity":
                for t in c["children"]:
                    if t["tag"] != "transition":
                        continue
                    ta = dict(t["attrs"])
                    s["transitions"].append({"dest": ta.get("dest"), "script": ta.get("script"),
                                             "name": ta.get("name")})
        g["states"].append(s)
    ids = {s["id"] for s in g["states"]}
    for s in g["states"]:
        sc = s["script"]
        s["lua_ok"] = sc is None or sc.upper() in scripts
        if sc and s["id"] and not sc.upper().endswith("_S_%X" % int(s["id"])):
            s["id_script_mismatch"] = True
        for t in s["transitions"]:
            t["dest_known"] = t["dest"] in ids
            t["lua_ok"] = t["script"] is None or t["script"].upper() in scripts
    return g


def summarize_lua(body, width=110):
    prints = re.findall(r'LuaDebugPrint\("([^"]{0,70})', body)
    calls = []
    for c in re.findall(r"\b([A-Z][A-Za-z]+)\(", body):
        if c not in calls and c not in ("LuaDebugPrint", "GetDASValue", "GetTickCount"):
            calls.append(c)
    s = " / ".join(prints[:2])
    return (s + (" | " if s else "") + ",".join(calls[:9]))[:width]


# ---------------------------------------------------------------- named attribute docs
def find_doc_toc(d):
    """TOC records {u16 0x1000, ...} = named binary-XML docs (attribute sets)."""
    recs = []
    for m in re.finditer(rb"\x10\x00.{12}([A-Z0-9][A-Z0-9_]{2,39})\x00", d, re.S):
        p = m.start()
        recs.append((p, m.group(1).decode(), struct.unpack_from("<H", d, p + 10)[0]))
    return recs


def named_docs(d, recs=None):
    """{NAME: [(key, value, category)]} for every named attribute doc.

    Same offset rule as scripts (seg_base + u16*16); seg_base is solved as the unique
    4 KiB-aligned base that puts the bxml magic under EVERY *ATTRIBUTES record.
    """
    recs = [r for r in (recs if recs is not None else find_doc_toc(d)) if r[1].endswith("ATTRIBUTES")]
    if not recs:
        return {}
    mag = struct.pack(">I", BXML_MAGIC)
    bases = set()
    for m in re.finditer(re.escape(mag), d):
        b = m.start() - recs[0][2] * 16
        if b >= 0 and b % 0x1000 == 0 and all(d[b + u * 16:b + u * 16 + 4] == mag for _p, _n, u in recs):
            bases.add(b)
    if len(bases) != 1:
        raise FormatError(f"attribute-doc base not unique: {[hex(b) for b in sorted(bases)]}")
    base = bases.pop()
    out = {}
    for _p, name, u in recs:
        nodes = parse_bxml(d, base + u * 16)
        rows = []
        for n in nodes:
            if n["tag"] == "attribute":
                a = dict(n["attrs"])
                rows.append((a.get("key"), a.get("value"), a.get("category")))
        out[name] = {"H": base + u * 16, "attrs": rows}
    return out


def trigger_table(docs):
    """Objective triggers: one row per *TRIGGERATTRIBUTES doc (retail data)."""
    rows = []
    for name, doc in sorted(docs.items()):
        if not name.endswith("TRIGGERATTRIBUTES"):
            continue
        a = {k: v for k, v, _c in doc["attrs"]}
        rows.append({"doc": name,
                     "objectiveIndex": a.get("objectiveIndex"),
                     "worldflow": a.get("objectiveWorldFlowName"),
                     "type": a.get("triggerType"),
                     "deps": a.get("triggerDependencies", ""),
                     "op": a.get("triggerLogicalOperator"),
                     "hpChapterRequired": a.get("heroPointChapterRequired"),
                     "available": a.get("objectiveAvailable"),
                     "repeatable": a.get("triggerIsRepeatable"),
                     "arenaChange": a.get("triggerArenaChange"),
                     "power": a.get("triggerPower")})
    return rows


def load(ast):
    with open(ast, "rb") as f:
        d = f.read()
    scripts = pair_scripts(d, find_script_toc(d))
    graphs = [fsm_model(nodes, scripts) | {"H": H} for H, nodes in find_fsm_docs(d)]
    return d, scripts, graphs


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ast", default=DEFAULT_AST)
    ap.add_argument("cmd", choices=["graphs", "dump", "grep", "json", "docs", "triggers"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--lua", action="store_true", help="dump: print full Lua bodies")
    a = ap.parse_args(argv)
    d, scripts, graphs = load(a.ast)
    if a.cmd == "docs":
        docs = named_docs(d)
        for name, doc in sorted(docs.items()):
            if a.arg and a.arg.upper() not in name:
                continue
            print(f"== {name} @{doc['H']:#x} ({len(doc['attrs'])} attrs)")
            if a.arg:
                for k, v, c in doc["attrs"]:
                    print(f"    {k:<40} = {v!s:<30} cat={c}")
        return 0
    if a.cmd == "triggers":
        rows = trigger_table(named_docs(d))
        rows.sort(key=lambda r: int(r["objectiveIndex"]) if (r["objectiveIndex"] or "").lstrip("-").isdigit() else 9999)
        print(f"{'idx':>4} {'worldflow':<24} {'type':<22} {'deps':<14} {'op':<4} {'hpCh':>4} av rp ar  doc")
        for r in rows:
            print(f"{r['objectiveIndex'] or '-':>4} {r['worldflow'] or '-':<24} {r['type'] or '-':<22} "
                  f"{r['deps'] or '-':<14} {r['op'] or '-':<4} {r['hpChapterRequired'] or '-':>4} "
                  f"{r['available'] or '-':>2} {r['repeatable'] or '-':>2} {r['arenaChange'] or '-':>2}  {r['doc']}")
        return 0
    if a.cmd == "graphs":
        print(f"{len(scripts)} scripts paired (length+kind checked)")
        for g in graphs:
            print(f"{g['H']:#010x}  {g['name']:<24} start={g['start']:<11} states={len(g['states'])}")
        return 0
    if a.cmd == "json":
        json.dump({"scripts": scripts, "graphs": graphs}, open(a.arg, "w", encoding="utf-8"), indent=1)
        print("wrote", a.arg)
        return 0
    if a.cmd == "grep":
        for g in graphs:
            for s in g["states"]:
                for role, sc in [("state", s["script"])] + [("->" + str(t["dest"]), t["script"])
                                                           for t in s["transitions"]]:
                    if sc and sc.upper() in scripts and a.arg in scripts[sc.upper()]["lua"]:
                        print(f"{g['name']}: state {s['id']} {role} {sc} "
                              f"@{scripts[sc.upper()]['offset']:#x}")
        return 0
    want = [g for g in graphs if g["name"] == a.arg] or graphs
    for g in want:
        print(f"== graph {g['name']} (start {g['start']}, {len(g['states'])} states, {g['file']})")
        for s in g["states"]:
            sc = (s["script"] or "").upper()
            body = scripts.get(sc, {}).get("lua", "")
            tag = " START" if s["id"] == g["start"] else ""
            print(f"[{s['id']}]{tag} {s['kind']} {s['script'] or ''} "
                  f"{('@%#x' % scripts[sc]['offset']) if sc in scripts else ''}"
                  f"{' extra=' + str(s['extra']) if s['extra'] else ''}"
                  f"{' attrs=' + str(s['attrs']) if s['attrs'] else ''}")
            if body:
                print("    " + summarize_lua(body))
                if a.lua:
                    print("\n".join("      | " + ln for ln in body.replace("\r", "").split("\n")))
            for t in s["transitions"]:
                ts = (t["script"] or "").upper()
                cond = summarize_lua(scripts[ts]["lua"], 90) if ts in scripts else (t["name"] or "")
                print(f"    -> [{t['dest']}]{'' if t['dest_known'] else ' (other graph)'} "
                      f"{t['script'] or t['name'] or ''}: {cond}")
                if a.lua and ts in scripts:
                    print("\n".join("        | " + ln for ln in scripts[ts]["lua"].replace("\r", "").split("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
