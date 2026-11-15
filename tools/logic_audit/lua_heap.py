#!/usr/bin/env python3
"""lua_heap.py - step-4 lane B (logic audit, Fable ruling 7): a read-only census of the game's Lua 5.0.2 script heap.

    python tools/logic_audit/lua_heap.py dump [--dump sound_gameplay|<path>] [--out census.json]   # a Xenia golden dump
    python tools/logic_audit/lua_heap.py diff <a.json> <b.json> [--top 40]                       # what b holds that a does not
    py -3.13 tools/logic_audit/lua_heap.py live --pid <pid> --side ours|xenia [--out census.json]  # one census, read only
    (live: tools/oracle/metro_watch.py samples nblocks every poll and writes a census every --lua-census-every s)

The game never collects its script heap (docs/ENGINE_MAP.md 6c: GCthreshold 432,537,600 = a KB/bytes slip, so Lua's
automatic GC never fires; there is no explicit collect either). So every object the scripts ever made is still linked:
  - strings in the string table G->strt (G+0 hash[], G+4 nuse, G+8 size; chains through TString+0);
  - every other collectable object on G->rootgc (G+0x0C), NEWEST FIRST (luaC_link sub_82764658 pushes at the head);
  - userdata on G->rootudata (G+0x10).
So the census of a running heap says what the scripts have been allocating, and the first objects of rootgc are the
most recent allocations.

Layouts (this title's Lua 5.0.2, verified in IDA 2026-10-20): TObject = 8 B (int tt + a 4-byte value: lua_Number is a
float; ltable_setnodevector 82767AC0 makes 20-byte Nodes), common header next +0, tt +4, marked +5.
  TString (lstring_newlstr 827606E0): hash +8, len +12, chars +16; allocation len + 17.
  Table   (luaH_new 82767B98): flags +6, lsizenode +7, metatable +8, array +12, node +16, firstfree +20, gclist +24,
          sizearray +28; allocation 32 + 8 * sizearray + 20 << lsizenode (none when node = G+64, the dummy node).
  Lua closure (luaF_newLclosure 82767280): isC +6 = 0, nupvalues +7, p +12 (Proto), g +16; allocation 24 + 4 * nup.
  C closure (sub_82767228): isC = 1, f +12, upvalues +16; allocation 16 + 8 * nup.
  Proto (luaF_newproto 82767410, 0x48 B): source +32 (TString), sizek +40, sizecode +44, sizep +52, lineDefined +60.
  tt: 4 string, 5 table, 6 function, 7 userdata, 8 thread, 9 proto, 10 upvalue.
mgr = *0x8296D8CC, L = *(mgr+0x0C), G = *(L+0x10); G+0x20 GCthreshold, G+0x24 nblocks (bytes, luaM_realloc 82760928).
Reads are not atomic on a live process (pages are fetched while the game runs). Without a GC nothing is freed, so the
lists only grow at their heads and a walk from a snapshot head stays valid; a string-table resize during a walk can
tear a chain (counted as `torn`).
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

VA_SCRIPT_MGR = 0x8296D8CC
VA_RUNSTRING_COUNT = 0x8296DBF0   # ++ in AptVmInvokeAction_sub_82176EB8 only (IDA xrefs), which first calls
                                  # lua_setgcthreshold(L, 0): every count is one FULL collection
TT = {4: "string", 5: "table", 6: "function", 7: "userdata", 8: "thread", 9: "proto", 10: "upvalue"}
MAX_OBJECTS = 2_000_000


class PageReader:
    """read(va, n) over a fetch(list of 4 KB page numbers) -> {page: bytes or None}; caches, prefetches a run."""

    def __init__(self, fetch, run=16):
        self.fetch, self.run, self.pages = fetch, run, {}

    def _page(self, pg):
        if pg not in self.pages:
            want = [p for p in range(pg, pg + self.run) if p not in self.pages]
            self.pages.update(self.fetch(want))
            self.pages.setdefault(pg, None)
        return self.pages[pg]

    def read(self, va, n):
        out = b""
        while n > 0:
            pg, off = va >> 12, va & 0xFFF
            p = self._page(pg)
            if p is None or len(p) < 4096:
                return None
            k = min(n, 4096 - off)
            out += p[off:off + k]
            va, n = va + k, n - k
        return out


class DumpReader:
    def __init__(self, dump):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import golden  # noqa: E402
        self.d = golden.GoldenDump(golden.resolve_dump(dump))

    def read(self, va, n):
        try:
            b = self.d.read(va, n)
        except Exception:  # noqa: BLE001
            return None
        return b if b is not None and len(b) == n else None


def u32(r, va):
    b = r.read(va, 4)
    return None if b is None else struct.unpack(">I", b)[0]


def u8(r, va):
    b = r.read(va, 1)
    return None if b is None else b[0]


def tstring(r, p, cap=160):
    """(len, text) of the TString at p, or (None, None)."""
    if not p or u8(r, p + 4) != 4:
        return None, None
    n = u32(r, p + 12)
    if n is None or n > 1 << 24:
        return None, None
    b = r.read(p + 16, min(n, cap)) or b""
    return n, b.decode("latin-1")


_SHAPE = re.compile(r"0x[0-9a-fA-F]+|[0-9]+(?:\.[0-9]+)?")


def shape(s):
    """A string's shape: numbers -> '#', so 'obj_123' and 'obj_124' are one shape."""
    return _SHAPE.sub("#", s)


def counters(r):
    """{mgr, L, G, gc_threshold, nblocks, runstring} (any field None if unreadable). runstring = the game's count of
    FmScript_RunString calls (AptVmInvokeAction 82176EB8), each of which collects the whole heap first."""
    mgr = u32(r, VA_SCRIPT_MGR)
    L = u32(r, mgr + 0x0C) if mgr else None
    G = u32(r, L + 0x10) if L else None
    return {"mgr": mgr, "L": L, "G": G, "gc_threshold": u32(r, G + 0x20) if G else None,
            "nblocks": u32(r, G + 0x24) if G else None, "runstring": u32(r, VA_RUNSTRING_COUNT)}


def census(r, recent=400, top=60, examples=3, max_objects=MAX_OBJECTS, hot=8):
    """The whole heap: per-type counts and bytes, string shapes, closures by prototype, tables by shape, the newest
    `recent` rootgc objects. Returns a JSON-able dict."""
    c = counters(r)
    out = {"counters": c, "ok": False}
    G = c["G"]
    if not G:
        out["error"] = "no G (script manager not created yet)"
        return out
    dummy = G + 64
    by_tt, bytes_tt = Counter(), Counter()
    torn = 0
    # strings
    hash_, nuse, size = u32(r, G), u32(r, G + 4), u32(r, G + 8)
    shapes, shape_bytes, shape_ex = Counter(), Counter(), defaultdict(list)
    nstr = 0
    if hash_ and size and size < 1 << 24:
        heads = r.read(hash_, 4 * size)
        for i in range(size if heads else 0):
            p = struct.unpack_from(">I", heads, 4 * i)[0]
            hops = 0
            while p and hops < 100000:
                n, s = tstring(r, p)
                if n is None:
                    torn += 1
                    break
                nstr += 1
                by_tt["string"] += 1
                bytes_tt["string"] += n + 17
                k = shape(s)
                shapes[k] += 1
                shape_bytes[k] += n + 17
                if len(shape_ex[k]) < examples:
                    shape_ex[k].append(s[:80])
                p = u32(r, p)
                hops += 1
    # rootgc
    protos = {}

    def proto_name(pr):
        if pr in protos:
            return protos[pr]
        src = u32(r, pr + 32)
        _, s = tstring(r, src) if src else (None, None)
        line = u32(r, pr + 60)
        protos[pr] = "%s:%s" % (s if s is not None else "?", line)
        return protos[pr]

    lclos, cclos, tables, newest = Counter(), Counter(), Counter(), []
    lclos_proto, all_protos = {}, []
    p, nobj = u32(r, G + 0x0C), 0
    while p and nobj < max_objects:
        tt = u8(r, p + 4)
        name = TT.get(tt)
        if name is None:
            torn += 1
            break
        nobj += 1
        by_tt[name] += 1
        desc, nbytes = name, 0
        if tt == 5:
            sa, ls, node, mt = u32(r, p + 28) or 0, u8(r, p + 7) or 0, u32(r, p + 16), u32(r, p + 8)
            nbytes = 32 + 8 * sa + (0 if node == dummy else 20 << ls)
            desc = "table array=%d hash=%d%s" % (sa, 0 if node == dummy else 1 << ls, " meta" if mt else "")
            tables[desc] += 1
        elif tt == 6:
            isc, nup = u8(r, p + 6), u8(r, p + 7) or 0
            if isc:
                nbytes = 16 + 8 * nup
                desc = "Cclosure f=%08X nup=%d" % (u32(r, p + 12) or 0, nup)
                cclos[desc] += 1
            else:
                nbytes = 24 + 4 * nup
                pr = u32(r, p + 12) or 0
                desc = "Lclosure %s nup=%d" % (proto_name(pr), nup)
                lclos[desc] += 1
                lclos_proto.setdefault(desc, pr)
        elif tt == 9:
            nbytes = 72 + 4 * (u32(r, p + 44) or 0) + 8 * (u32(r, p + 40) or 0) + 4 * (u32(r, p + 52) or 0)
            desc = "proto %s" % proto_name(p)
            all_protos.append(p)
        elif tt == 10:
            nbytes = 20
        bytes_tt[name] += nbytes
        if len(newest) < recent:
            newest.append({"va": "%08X" % p, "what": desc, "bytes": nbytes})
        p = u32(r, p)
    # userdata
    p, nud = u32(r, G + 0x10), 0
    while p and nud < max_objects:
        if u8(r, p + 4) != 7:
            torn += 1
            break
        nud += 1
        by_tt["userdata"] += 1
        bytes_tt["userdata"] += 16 + (u32(r, p + 12) or 0)
        p = u32(r, p)
    out.update({
        "ok": True, "torn": torn, "strt": {"size": size, "nuse": nuse, "walked": nstr},
        "rootgc_walked": nobj, "rootudata_walked": nud,
        "by_type": dict(by_tt), "bytes_by_type_est": dict(bytes_tt),
        "bytes_est_total": sum(bytes_tt.values()),
        "string_shapes": [{"shape": k, "n": v, "bytes": shape_bytes[k], "ex": shape_ex[k]}
                          for k, v in shapes.most_common(top)],
        "string_shapes_distinct": len(shapes),
        "lclosures": lclos.most_common(top), "cclosures": cclos.most_common(top), "tables": tables.most_common(top),
        "newest": newest,
        "hot_protos": hot_protos(r, lclos, lclos_proto, all_protos, hot),
    })
    return out


def proto_info(r, pr, kmax=48):
    """A Proto's line, sizes, constants (strings / numbers) and child prototypes' lines."""
    if not pr or u8(r, pr + 4) != 9:
        return None
    k, sizek, sizep, kids = u32(r, pr + 8), u32(r, pr + 40) or 0, u32(r, pr + 52) or 0, u32(r, pr + 16)
    consts = []
    for i in range(min(sizek, kmax)):
        tt = u32(r, k + 8 * i) if k else None
        v = r.read(k + 8 * i + 4, 4) if k else None
        if tt == 4 and v:
            consts.append(tstring(r, struct.unpack(">I", v)[0])[1])
        elif tt == 3 and v:
            consts.append(round(struct.unpack(">f", v)[0], 4))
        else:
            consts.append("tt%s" % tt)
    children = []
    for i in range(min(sizep, 64)):
        c = u32(r, kids + 4 * i) if kids else None
        children.append({"va": "%08X" % (c or 0), "line": u32(r, c + 60) if c else None})
    return {"va": "%08X" % pr, "line": u32(r, pr + 60), "sizecode": u32(r, pr + 44), "sizek": sizek, "sizep": sizep,
            "nups": u8(r, pr + 68), "numparams": u8(r, pr + 69), "consts": consts, "children": children}


def hot_protos(r, lclos, lclos_proto, all_protos, n):
    """The n most repeated Lua closures' prototypes, each with its PARENT prototype (the proto whose p[] holds it:
    the function whose every run makes these closures) and that parent's own parent."""
    if n <= 0:
        return []
    kids_of = {}
    for pp in all_protos:
        sizep, kids = u32(r, pp + 52) or 0, u32(r, pp + 16)
        for i in range(min(sizep, 4096)):
            c = u32(r, kids + 4 * i) if kids else None
            if c:
                kids_of.setdefault(c, pp)
    out = []
    for desc, cnt in lclos.most_common(n):
        pr = lclos_proto.get(desc)
        parent = kids_of.get(pr)
        grand = kids_of.get(parent) if parent else None
        out.append({"closure": desc, "n": cnt, "proto": proto_info(r, pr), "parent": proto_info(r, parent),
                    "grandparent": proto_info(r, grand, kmax=16)})
    return out


def cmd_dump(a):
    res = census(DumpReader(a.dump), recent=a.recent, top=a.top)
    res["source"] = {"dump": a.dump}
    text = json.dumps(res, indent=1)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    print_summary(res)
    return 0 if res.get("ok") else 2


def print_summary(res, top=15):
    c = res["counters"]
    print("nblocks %s  threshold %s  G %s" % (c.get("nblocks"), c.get("gc_threshold"),
                                             "%08X" % c["G"] if c.get("G") else None))
    if not res.get("ok"):
        print("  ", res.get("error"))
        return
    print("strings %s (strt nuse %s size %s), rootgc %s, udata %s, torn %s, est bytes %s" % (
        res["strt"]["walked"], res["strt"]["nuse"], res["strt"]["size"], res["rootgc_walked"],
        res["rootudata_walked"], res["torn"], res["bytes_est_total"]))
    print("  by type:", res["by_type"])
    print("  bytes  :", res["bytes_by_type_est"])
    for s in res["string_shapes"][:top]:
        print("  str %6d %8d B  %r  e.g. %r" % (s["n"], s["bytes"], s["shape"][:60], s["ex"][:2]))
    for k, v in res["lclosures"][:top]:
        print("  Lcl %6d  %s" % (v, k))
    for k, v in res["tables"][:top]:
        print("  tab %6d  %s" % (v, k))


def cmd_live(a):
    """One census of a running game or Xenia through tools/oracle/metro_watch.py's read-only page agent."""
    import frida
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "oracle"))
    import metro_watch
    sess = frida.attach(a.pid)
    script = sess.create_script("var SIDE = %s, MODULE = %s;\n" % (json.dumps(a.side), json.dumps(a.module)) + metro_watch.AGENT)
    script.load()
    init = script.exports_sync.init()
    if not init.get("ok"):
        print("init failed:", init)
        return 2

    def fetch(pgs):
        rr = script.exports_sync.pages(list(pgs))
        data, got, off = bytes.fromhex(rr["hex"]), {}, 0
        for pg, n in rr["table"]:
            got[pg] = data[off:off + n] if n else None
            off += n
        return got
    try:
        res = census(PageReader(fetch), recent=a.recent, top=a.top)
    finally:
        script.unload()
        sess.detach()
    res["source"] = {"pid": a.pid, "side": a.side, "membase": init.get("membase")}
    if a.out:
        Path(a.out).write_text(json.dumps(res, indent=1), encoding="utf-8")
    print_summary(res)
    for h in res.get("hot_protos", []):
        print("HOT %6d %s" % (h["n"], h["closure"]))
        for k in ("proto", "parent", "grandparent"):
            if h.get(k):
                x = h[k]
                print("   %-11s %s line %s code %s k %s p %s params %s consts %s" % (
                    k, x["va"], x["line"], x["sizecode"], x["sizek"], x["sizep"], x["numparams"], x["consts"][:24]))
    return 0 if res.get("ok") else 2


def cmd_diff(a):
    A, B = (json.loads(Path(p).read_text(encoding="utf-8")) for p in (a.a, a.b))
    print("A:", a.a)
    print_summary(A, 0)
    print("B:", a.b)
    print_summary(B, 0)

    def table(key, field="n"):
        da = {(x["shape"] if isinstance(x, dict) else x[0]): (x[field] if isinstance(x, dict) else x[1]) for x in A[key]}
        db = {(x["shape"] if isinstance(x, dict) else x[0]): (x[field] if isinstance(x, dict) else x[1]) for x in B[key]}
        rows = sorted(set(da) | set(db), key=lambda k: -(db.get(k, 0) - da.get(k, 0)))
        print("== %s: B - A (top %d)" % (key, a.top))
        for k in rows[:a.top]:
            print("  %+8d  A %7d  B %7d  %r" % (db.get(k, 0) - da.get(k, 0), da.get(k, 0), db.get(k, 0), k[:90]))
    for t in ("by_type", "bytes_by_type_est"):
        print("== %s: A %s | B %s" % (t, A[t], B[t]))
    table("string_shapes")
    table("lclosures")
    table("cclosures")
    table("tables")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump")
    d.add_argument("--dump", default="sound_gameplay")
    d.add_argument("--out", default="")
    d.add_argument("--recent", type=int, default=400)
    d.add_argument("--top", type=int, default=60)
    lv = sub.add_parser("live")
    lv.add_argument("--pid", type=int, required=True)
    lv.add_argument("--side", choices=("ours", "xenia"), required=True)
    lv.add_argument("--module", default="SupermanReturns.exe")
    lv.add_argument("--out", default="")
    lv.add_argument("--recent", type=int, default=400)
    lv.add_argument("--top", type=int, default=60)
    f = sub.add_parser("diff")
    f.add_argument("a")
    f.add_argument("b")
    f.add_argument("--top", type=int, default=40)
    a = ap.parse_args(argv)
    return {"dump": cmd_dump, "diff": cmd_diff, "live": cmd_live}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
