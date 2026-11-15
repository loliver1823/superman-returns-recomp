#!/usr/bin/env python3
"""metro_watch.py - step-4 lane F (rog-oracle): watch metro (mini) events in a RUNNING Xenia or our build, read only.

    py -3.13 tools/oracle/metro_watch.py --pid <pid> --side xenia|ours --out <run>/metro.jsonl
        [--module SupermanReturns.exe] [--every 2] [--quit-file <f>] [--max-seconds 1800]

Every --every s it builds a fresh guest page image through its OWN frida session (tools/devtools/xenia_peek.py's
read-only page agent: every page range-checked with Process.findRangeByAddress; Xenia: membase 0x100000000 and the
0xE0000000 view's +0x1000; ours: the exe's g_debugGuestBase export) and reads, with tools/devtools/skipexec.World
(the dev tools' reader, unchanged):
  - engaged: BaseWorldFlow's DAS value miniEventEngaged (the event engine's own "a mini event is running" flag; the
    level-skip failure verdict reads it, DEVTOOLS.md);
  - flows: the world-flow manager's 8 slots (an event loads its own world flow: its name is the event's name);
  - activated: DAS objectiveActivated, health: the metro health object (+4);
  - lua: the script heap's counters (tools/logic_audit/lua_heap.py counters: nblocks = live bytes, GCthreshold, runstr =
    the game's FmScript_RunString count - each run collects the whole heap first), and
    every --lua-census-every s (default 60, 0 = off) a whole-heap census (lua_heap.census: strings by shape, closures
    by prototype, tables, the newest objects) as one JSON line in lua_census.jsonl beside --out (logic audit, Fable
    ruling 7: the same instrument on both sides).
One JSON line per poll ({"t": epoch s, ...} or {"t", "refusal": code} while the base flow is not current, e.g. a
load); a {"change": ...} line whenever engaged / flows change. The reads are NOT atomic (pages fetched while the game
runs): a field can tear, so a change is recorded only when two consecutive polls agree.
OBSERVATION ONLY: it never writes guest memory and never blocks the guest. Exits when the process exits, on
--quit-file, or after --max-seconds.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOLS, "devtools"))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "logic_audit"))

AGENT = r"""
var MEMBASE = null;
var XENIA = (SIDE === 'xenia');
function hostAddr(va) { va = va >>> 0; var p = MEMBASE.add(va); return (XENIA && va >= 0xE0000000) ? p.add(0x1000) : p; }
function ok(a, n) {
  var r = Process.findRangeByAddress(a);
  if (r === null || r.protection.indexOf('r') !== 0) return false;
  return a.add(n).compare(r.base.add(r.size)) <= 0;
}
rpc.exports = {
  init: function () {
    if (XENIA) { MEMBASE = ptr('0x100000000'); return { ok: true, membase: MEMBASE.toString() }; }
    var m = Process.getModuleByName(MODULE);
    var s = m.findExportByName('g_debugGuestBase');
    if (s === null) return { ok: false, error: 'g_debugGuestBase export not found' };
    var b = s.readPointer();
    if (b.isNull()) return { ok: false, error: 'g_debugGuestBase is null (harness not started)' };
    MEMBASE = b;
    return { ok: true, membase: MEMBASE.toString() };
  },
  pages: function (pgs) {
    var table = [], chunks = [], total = 0;
    pgs.forEach(function (pg) {
      var a = hostAddr(pg * 4096), p = null;
      if (ok(a, 4096)) p = new Uint8Array(a.readByteArray(4096));
      table.push([pg, p === null ? 0 : p.length]);
      if (p !== null) { chunks.push(p); total += p.length; }
    });
    var blob = new Uint8Array(total), off = 0;
    chunks.forEach(function (c) { blob.set(c, off); off += c.length; });
    return { table: table, hex: Array.prototype.map.call(blob, function (b) { return ('0' + b.toString(16)).slice(-2); }).join('') };
  }
};
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--side", choices=("xenia", "ours"), required=True)
    ap.add_argument("--module", default="SupermanReturns.exe")
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=float, default=2.0)
    ap.add_argument("--quit-file", default=None)
    ap.add_argument("--max-seconds", type=float, default=1800.0)
    ap.add_argument("--lua-census-every", type=float, default=60.0,
                    help="seconds between whole-heap Lua censuses (lua_census.jsonl beside --out); 0 = off")
    a = ap.parse_args()
    import frida
    import skipexec
    import lua_heap
    from xenia_skip import FetchImage
    out = open(a.out, "a", encoding="utf-8")
    census_path = os.path.join(os.path.dirname(os.path.abspath(a.out)), "lua_census.jsonl")

    def emit(rec):
        out.write(json.dumps(rec, default=str) + "\n")
        out.flush()

    sess = frida.attach(a.pid)
    detached = []
    sess.on("detached", lambda reason, *_: detached.append(str(reason)))
    script = sess.create_script("var SIDE = %s, MODULE = %s;\n" % (json.dumps(a.side), json.dumps(a.module)) + AGENT)
    script.load()
    t0 = time.time()
    init = None
    while time.time() - t0 < 120 and not detached:
        init = script.exports_sync.init()
        if init.get("ok"):
            break
        time.sleep(1.0)
    emit({"t": time.time(), "what": "attached", "side": a.side, "pid": a.pid, "init": init})
    if not init or not init.get("ok"):
        return 2

    def fetch(pgs):
        r = script.exports_sync.pages(list(pgs))
        data = bytes.fromhex(r["hex"])
        got, off = {}, 0
        for pg, n in r["table"]:
            got[pg] = data[off:off + n] if n else None
            off += n
        return got

    last = None       # the last state recorded as a change
    pending = None    # a new state seen once (needs a second agreeing poll)
    next_census = time.time()
    try:
        while not detached and time.time() - t0 < a.max_seconds:
            if a.quit_file and os.path.exists(a.quit_file):
                break
            tp = time.time()
            rec = {"t": round(tp, 3)}
            try:
                w = skipexec.World(FetchImage(fetch, {"side": a.side, "mode": "metro_watch (non-atomic)"}))
                rec["flows"] = w.loaded()
                try:
                    rec["engaged"] = w.das_val("miniEventEngaged")
                    rec["activated"] = w.das_val("objectiveActivated")
                except skipexec.Refusal as e:
                    rec["refusal"] = getattr(e, "code", str(e))
                try:
                    rec["health"] = w.metro().get("health")
                except skipexec.Refusal:
                    pass
            except Exception as e:  # noqa: BLE001  (a torn read, a page gone, the process exiting)
                rec["error"] = "%s: %s" % (type(e).__name__, e)
            try:
                lc = lua_heap.counters(lua_heap.PageReader(fetch, run=1))
                rec["lua"] = {"nblocks": lc["nblocks"], "thr": lc["gc_threshold"], "runstr": lc["runstring"]}
                if a.lua_census_every > 0 and lc["nblocks"] and time.time() >= next_census:
                    next_census = time.time() + a.lua_census_every
                    tc = time.time()
                    cen = lua_heap.census(lua_heap.PageReader(fetch, run=16))
                    cen.update({"t": round(tc, 3), "walk_s": round(time.time() - tc, 2), "side": a.side})
                    with open(census_path, "a", encoding="utf-8") as cf:
                        cf.write(json.dumps(cen, default=str) + "\n")
            except Exception as e:  # noqa: BLE001
                rec["lua_error"] = "%s: %s" % (type(e).__name__, e)
            emit(rec)
            state = (rec.get("engaged"), tuple(rec.get("flows") or ()))
            if "error" not in rec and "refusal" not in rec and state != last:
                if pending == state:
                    emit({"t": rec["t"], "change": {"engaged": state[0], "flows": list(state[1]),
                                                    "from": None if last is None else
                                                    {"engaged": last[0], "flows": list(last[1])}}})
                    last, pending = state, None
                else:
                    pending = state
            elif state == last:
                pending = None
            time.sleep(max(0.0, a.every - (time.time() - tp)))
    finally:
        emit({"t": time.time(), "what": "end", "detached": detached})
        try:
            script.unload()
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
        out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
