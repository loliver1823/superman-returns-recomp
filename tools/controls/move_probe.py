#!/usr/bin/env python3
"""iter-464 controls lane: the MOVE PROBE - a 30 Hz timeline of Superman's state in live Xenia
(the oracle) while a pad script plays, to see what the original game does with an input
sequence (e.g. "does Block + a stick flick evade in the air?"). Read-only: it never writes guest
memory (docs/PC_CONTROLS.md 5.4).

    py -3.13 tools/controls/move_probe.py --pid <xenia pid> --script <pad script> --out <jsonl>
    py -3.13 tools/controls/move_probe.py --ours --pid <our game pid> --script <script> --out <jsonl>
(--ours: our build; drive it with tools/controls/harness_play.py in parallel and give the probe a
script of `mark` / `sleep` lines only.)

Samples (every 33 ms, Frida agent): Superman's position (actor +0x20) and facing (quaternion
+0x10), his mover class (vt of *(actor+0x60)+4: LOCO / FLY / ANIM ...), move-def index (+0x60),
flight slot (+0x90), proxy velocity, and player 1's input-context type (IgcContextManager slot 0
+8). The actor is found by its vtable 0x82009798 + the name "SUPERMAN" (actor +0x84 -> +0x24).

Pad script = lines for tools/xenia_pad.py (the held-state tokens: HOLD:B, LX=1, WAIT:150, ...),
plus:
    mark <label>        a marker in the timeline
    sleep <ms>          wait without sending anything
    pad <tokens...>     send tokens to the pad (one pad_cmd.txt write; the pad plays them in order)
Each `pad` line waits for the pad to consume its file before the next line.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PAD_CMD = os.path.join(os.path.dirname(HERE), "pad_cmd.txt")

AGENT = r"""
var MEMBASE = ptr('0x100000000');
function H(va) { return MEMBASE.add(va >>> 0); }
function initBase(ours) {
  if (!ours) return MEMBASE.toString();
  var mod = Process.enumerateModules()[0];
  var sym = mod.findExportByName('g_debugGuestBase');
  if (sym === null) throw new Error('g_debugGuestBase export not found in ' + mod.name);
  MEMBASE = sym.readPointer();
  return MEMBASE.toString();
}
function ok(va, n) {
  var a = H(va), r = Process.findRangeByAddress(a);
  return r !== null && r.protection.indexOf('r') === 0 && a.add(n).compare(r.base.add(r.size)) <= 0;
}
function u32(va) { if (!ok(va, 4)) return 0; var b = new Uint8Array(H(va).readByteArray(4)); return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0; }
function f32(va) { if (!ok(va, 4)) return NaN; var d = new DataView(H(va).readByteArray(4)); return d.getFloat32(0, false); }
function vec(va, n) { var o = []; for (var i = 0; i < n; i++) o.push(f32(va + 4 * i)); return o; }
function isSup(a) {
  if (u32(a) !== 0x82009798) return false;
  var s = u32(u32(a + 0x84) + 0x24);
  if (!ok(s, 9)) return false;
  var b = new Uint8Array(H(s).readByteArray(9));
  return String.fromCharCode.apply(null, Array.prototype.slice.call(b, 0, 8)) === 'SUPERMAN' && b[8] === 0;
}
var ACTOR = 0, LAST_SCAN = 0;
function findActor() {
  if (ACTOR && isSup(ACTOR)) return ACTOR;
  if (isSup(0x00719ED0)) { ACTOR = 0x00719ED0; return ACTOR; }
  var now = Date.now();
  if (now - LAST_SCAN < 2000) return 0;   // the scan is heavy: at most every 2 s
  LAST_SCAN = now;
  var ranges = [[0x00400000, 0x02000000]];
  for (var k = 0; k < ranges.length; k++) {
    for (var va = ranges[k][0]; va < ranges[k][1]; va += 0x100000) {
      if (!ok(va, 0x100000)) continue;
      var hits = Memory.scanSync(H(va), 0x100000, '82 00 97 98');
      for (var i = 0; i < hits.length; i++) {
        var a = hits[i].address.sub(MEMBASE).toUInt32();
        if ((a & 3) === 0 && isSup(a)) { ACTOR = a; return a; }
      }
    }
  }
  return 0;
}
var VT = {0x8200EDF4: 'LOCO', 0x8200ED88: 'FLY', 0x8200ED28: 'ANIM', 0x8200EDBC: 'GRND', 0x8200ED58: 'CPTR', 0x8200F030: 'WHLD', 0x82008D50: 'NONE'};
function sample() {
  var a = findActor(), o = {t: Date.now()};
  var mgr = u32(0x8296D884), slots = mgr ? u32(mgr + 32) : 0;
  o.ctx = slots ? u32(slots + 8) : -1;
  if (!a) { o.actor = 0; return o; }
  o.actor = a;
  o.pos = vec(a + 0x20, 3);
  o.q = vec(a + 0x10, 4);
  var comp = u32(u32(a + 0x60) + 4), vt = u32(comp);
  o.mover = VT[vt] || ('0x' + vt.toString(16));
  o.def = u32(comp + 0x60);
  o.slot = o.mover === 'FLY' ? u32(comp + 0x90) : -1;
  o.vel = vec(u32(u32(a + 0x70) + 4) + 0x10, 3);
  return o;
}
var timer = null;
rpc.exports = {
  base: function (ours) { return initBase(ours); },
  setActor: function (va) { if (isSup(va)) { ACTOR = va; return true; } return false; },
  start: function (ms) { if (timer === null) timer = setInterval(function () { send(sample()); }, ms); return findActor(); },
  stop: function () { if (timer !== null) { clearInterval(timer); timer = null; } },
  one: function () { return sample(); }
};
"""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--script", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--period-ms", type=int, default=33)
    ap.add_argument("--log", default="",
                    help="our game's log: the actor comes from its [PC_CONTROLS] Superman actor= line")
    ap.add_argument("--ours", action="store_true",
                    help="our SupermanReturns.exe (guest base from its g_debugGuestBase export)")
    a = ap.parse_args(argv)
    import frida
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    out = open(a.out, "a")
    t0 = time.time()

    def rec(obj):
        obj.setdefault("wall", round(time.time() - t0, 3))
        out.write(json.dumps(obj) + "\n")
        out.flush()

    sess = frida.attach(a.pid)
    script = sess.create_script(AGENT)
    script.on("message", lambda m, d: rec(m["payload"]) if m.get("type") == "send" else rec({"agent": m}))
    script.load()
    base = script.exports_sync.base(bool(a.ours))
    rec({"mark": "membase", "base": base, "side": "ours" if a.ours else "xenia"})
    if a.log:
        import re
        import threading

        def follow():
            pos = 0
            while True:
                try:
                    with open(a.log, 'rb') as f:
                        f.seek(pos)
                        chunk = f.read()
                        pos += len(chunk)
                    m = re.search(rb'Superman actor=0x([0-9A-Fa-f]{8})', chunk)
                    if m and script.exports_sync.set_actor(int(m.group(1), 16)):
                        rec({"mark": "actor from log", "actor": int(m.group(1), 16)})
                        return
                except (OSError, frida.InvalidOperationError):
                    pass
                time.sleep(0.5)
        threading.Thread(target=follow, daemon=True).start()
    actor = script.exports_sync.start(a.period_ms)
    rec({"mark": "probe start", "actor": actor})
    print("move_probe: actor=0x%08X" % actor, flush=True)
    try:
        for raw in open(a.script):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            cmd, _, rest = line.partition(" ")
            if cmd == "mark":
                rec({"mark": rest})
                print("mark:", rest, flush=True)
            elif cmd == "sleep":
                time.sleep(int(rest) / 1000.0)
            elif cmd == "pad":
                with open(PAD_CMD + ".tmp", "w") as f:
                    f.write(rest)
                os.replace(PAD_CMD + ".tmp", PAD_CMD)
                rec({"pad": rest})
                deadline = time.time() + 30
                while os.path.exists(PAD_CMD) and time.time() < deadline:
                    time.sleep(0.01)
            else:
                sys.exit("move_probe: unknown line: " + line)
    finally:
        try:
            script.exports_sync.stop()
            time.sleep(0.1)
            script.unload()
            sess.detach()
        except Exception:
            pass
        out.close()


if __name__ == "__main__":
    main()
