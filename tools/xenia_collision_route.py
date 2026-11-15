#!/usr/bin/env python3
"""xenia_collision_route.py - the Xenia (oracle) side of the iter-463 collision fixed-route test.
Launches the stock oracle Xenia on the retail ISO, attaches tools/xenia_collision_route.js
(observation only), writes every message to <out>/xenia_route.jsonl, and writes <out>/anchor.txt
when the guest requests "screens/hud/main_hud" - the anchor tools/xenia_pad_tape.py waits for
before it plays the route tape (the same tape our build replays through SR_INPUT_REPLAY).

Run order (one emulator/game process at a time; no SupermanReturns.exe running):
  1. python tools/xenia_pad_tape.py --tape <route.inpr> --anchor-file <out>/anchor.txt --pre "..."
     (Python with vgamepad; starts the virtual pad BEFORE Xenia)
  2. <python-with-frida> tools/xenia_collision_route.py --out <out> [--seconds 420]
  3. python tools/xenia_collision_route.py --summarize <out>   (any Python; also run by step 2)

    --target x,y,z  --aabb x0,y0,z0,x1,y1,z1  --radius 40   (default: Havok_Box01, see
    tools/gen_collision_route_tape.py)
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time

# The Xenia Canary exe and your own disc image: --xenia / --iso, or SR_XENIA_EXE / SR_GAME_ISO.
XENIA = os.environ.get("SR_XENIA_EXE", "")
ISO = os.environ.get("SR_GAME_ISO", "")
HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = [-2281.447, 1176.876, -365.4114]
AABB = [[-2301.17, 1171.95, -375.39], [-2262.25, 1374.14, -354.79]]


def summarize(out):
    path = os.path.join(out, "xenia_route.jsonl")
    ticks, inside, tele, events, secs = [], [], [], [], []
    for l in open(path):
        m = json.loads(l)
        t = m.get("t")
        if t == "tick":
            ticks.append(m)
        elif t == "inside":
            inside.append(m)
        elif t == "teleport":
            tele.append(m)
        elif t == "event":
            events.append(m)
        elif t == "sec":
            secs.append(m)
    res = route_verdict(ticks)
    res.update({"inside_lines": len(inside), "teleports": len(tele), "events": [e["ev"] for e in events][:50],
                "loco_seen": sorted({s.get("loco") for s in secs if s.get("loco")})})
    print(json.dumps(res, indent=1))
    return res


def route_verdict(ticks):
    """shared by both sides (the recomp log is parsed into the same tick dicts by
    tools/collision_route_verdict.py)."""
    near = [t for t in ticks if t.get("loco") == "FLY"]
    contact = [t for t in near if t.get("manT", 0) > 0]
    deep = [t for t in near if t.get("depth", -1) > 4.0 and t.get("manT", 0) == 0]
    paths = sorted({t.get("path") for t in near})
    return {"near_ticks": len(ticks), "near_ticks_FLY": len(near), "target_contact_ticks": len(contact),
            "deep_inside_no_contact_ticks": len(deep), "max_depth": max((t["depth"] for t in near), default=None),
            "paths": paths, "first_contact": contact[0] if contact else None, "first_deep": deep[0] if deep else None}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    ap.add_argument("--summarize")
    ap.add_argument("--seconds", type=float, default=420.0)
    ap.add_argument("--xenia", default=XENIA)
    ap.add_argument("--iso", default=ISO)
    ap.add_argument("--target", default=",".join(map(str, TARGET)))
    ap.add_argument("--aabb", default=",".join(map(str, AABB[0] + AABB[1])))
    ap.add_argument("--radius", type=float, default=40.0)
    a = ap.parse_args()
    if not a.xenia or not a.iso:
        sys.exit("give --xenia and --iso (or set SR_XENIA_EXE and SR_GAME_ISO)")
    if a.summarize:
        summarize(a.summarize)
        return 0
    import frida  # noqa: E402  (only the run needs it)
    os.makedirs(a.out, exist_ok=True)
    anchor = os.path.join(a.out, "anchor.txt")
    if os.path.exists(anchor):
        raise SystemExit("%s exists: use a fresh --out" % anchor)
    jl = open(os.path.join(a.out, "xenia_route.jsonl"), "w")
    # iter-464 bgruns: automated runs never take the foreground (tools/bg_window.py).
    import bg_window  # noqa: E402
    # Owner rule: automated runs never play sound through his speakers (--mute; the APU still runs).
    proc = bg_window.popen_background([a.xenia, "--log_file=" + os.path.join(os.path.abspath(a.out), "xenia.log"),
                                       "--mute=true", a.iso])
    bg_window.guard_background(proc.pid, prev_foreground=proc.sr_prev_foreground)
    time.sleep(5)
    sess = frida.attach(proc.pid)
    script = sess.create_script(open(os.path.join(HERE, "xenia_collision_route.js")).read())

    def on_message(msg, _data):
        if msg.get("type") != "send":
            jl.write(json.dumps({"t": "frida", "msg": msg}) + "\n")
            return
        p = msg["payload"]
        jl.write(json.dumps(p) + "\n")
        jl.flush()
        if p.get("t") == "anchor" and not os.path.exists(anchor):
            open(anchor, "w").write(str(time.time()))
            print("anchor: main_hud requested")
        elif p.get("t") in ("superman", "target", "attached", "attach-error"):
            print(p)

    script.on("message", on_message)
    script.load()
    t = [float(x) for x in a.target.split(",")]
    b = [float(x) for x in a.aabb.split(",")]
    script.exports_sync.configure(t, [b[:3], b[3:]], a.radius)
    t0 = time.time()
    try:
        while time.time() - t0 < a.seconds and proc.poll() is None:
            time.sleep(1)
    finally:
        try:
            sess.detach()
        except Exception:
            pass
        if proc.poll() is None:
            proc.terminate()
        jl.close()
    summarize(a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
