#!/usr/bin/env python3
"""collision_route_verdict.py - PASS / FAIL of the iter-463 collision fixed-route test, from OUR run
log ([COLL_ROUTE] / [COLL_MOVER] lines of kernel/collision_watch.cpp, route mode: SR_COLL_ROUTE=1
or the legacy SR_COLLISION_MOVER_WATCH=1)
and the XENIA run (xenia_route.jsonl of tools/xenia_collision_route.py). Both carry the same per-tick
fields for Superman's MoveTowardIdealPos calls near the target building.

Per side:
  INCONCLUSIVE  no near tick with loco=FLY (the take-off or the heading assumption failed; see the
                tape's ASSUMPTIONS in tools/gen_collision_route_tape.py) - rerun, not a verdict
  STOPPED       the target body entered the manifold (manT > 0) and the capsule centre never went
                > 4 m inside the target AABB without contact
  THROUGH       some FLY tick has the centre > 4 m inside the AABB with manT == 0 (walls sit <= ~3 m
                inside the AABB of list/MOPP buildings; golden-call: Box01's -z wall stops the
                centre at z = -374.81, 0.58 m inside)
  NO-REACH      neither (never touched it)
Combined:
  xenia STOPPED, ours THROUGH  -> FAIL: port bug on this route (the report names the first deep
                                 tick: path integrate/skip/keyframed, ovT = target in the phantom
                                 overlap list, loco, and any TELEPORT / SETPOS / filterInfo event)
  both THROUGH                -> designed or H-D (the original lets him through here too)
  both STOPPED                -> PASS (no repro on this route; compare the stop coordinates)
  xenia THROUGH, ours STOPPED -> unexpected divergence, record it
  anything INCONCLUSIVE / NO-REACH -> rerun (DOGMA: one roll is a sample)

    python tools/collision_route_verdict.py --ours <superman_returns_runN.log> --xenia <out>/xenia_route.jsonl
"""
import argparse
import json
import re

TICK = re.compile(r"\[COLL_ROUTE\] t=(\d+) loco=(\S+) def=(\d+) sub=(\d+) b0E=(\w+) b0F=(\w+) path=(\S+) "
                  r"in=\(([-\d.e+ ]+)\) out=\(([-\d.e+ ]+)\) v=\(([-\d.e+ ]+)\) man=(\d+) manT=(\d+) "
                  r"ovT=(\d+) ovN=(\d+) depth=([-\d.e+]+)")
EVENT = re.compile(r"\[COLL_MOVER\] (TELEPORT|SETPOS|SET_PHYSREP_MODE|DEACTIVATE|SET_FILTERINFO|NO-INTEGRATE)(.*)")


def vec(s):
    return [float(x) for x in s.split()]


def ours_ticks(path):
    ticks, events = [], []
    for l in open(path, errors="replace"):
        m = TICK.search(l)
        if m:
            g = m.groups()
            ticks.append({"tick": int(g[0]), "loco": g[1], "def": int(g[2]), "sub": int(g[3]), "path": g[6],
                          "in": vec(g[7]), "out": vec(g[8]), "v": vec(g[9]), "man": int(g[10]),
                          "manT": int(g[11]), "ovT": int(g[12]), "ovN": int(g[13]), "depth": float(g[14])})
            continue
        e = EVENT.search(l)
        if e:
            events.append(e.group(1) + e.group(2)[:120])
    return ticks, events


def xenia_ticks(path):
    ticks, events = [], []
    for l in open(path):
        m = json.loads(l)
        if m.get("t") == "tick":
            ticks.append(m)
        elif m.get("t") in ("teleport", "event"):
            events.append("%s %s" % (m.get("ev", "TELEPORT"), {k: m[k] for k in m if k not in ("t",)}))
    return ticks, events


def side(ticks):
    fly = [t for t in ticks if t.get("loco") == "FLY"]
    if not fly:
        return "INCONCLUSIVE", {"near_ticks": len(ticks), "locos": sorted({t.get("loco") for t in ticks})}
    contact = [t for t in fly if t["manT"] > 0]
    deep = [t for t in fly if t["depth"] > 4.0 and t["manT"] == 0]
    info = {"near_ticks_FLY": len(fly), "contact_ticks": len(contact), "deep_no_contact_ticks": len(deep),
            "max_depth": round(max(t["depth"] for t in fly), 3), "paths": sorted({t["path"] for t in fly}),
            "max_speed": round(max((sum(x * x for x in t["v"]) ** 0.5 for t in fly), default=0), 2),
            "first_contact_out": contact[0]["out"] if contact else None,
            "first_deep": deep[0] if deep else None,
            "not_integrate_ticks": sum(1 for t in fly if t["path"] != "integrate"),
            "target_not_in_overlap_ticks": sum(1 for t in fly if t["depth"] > -1.0 and not t["ovT"])}
    if deep:
        return "THROUGH", info
    if contact:
        return "STOPPED", info
    return "NO-REACH", info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours")
    ap.add_argument("--xenia")
    a = ap.parse_args()
    res = {}
    if a.ours:
        t, ev = ours_ticks(a.ours)
        k, info = side(t)
        res["ours"] = {"outcome": k, **info, "events": ev[:40]}
    if a.xenia:
        t, ev = xenia_ticks(a.xenia)
        k, info = side(t)
        res["xenia"] = {"outcome": k, **info, "events": ev[:40]}
    o, x = res.get("ours", {}).get("outcome"), res.get("xenia", {}).get("outcome")
    if o and x:
        if "INCONCLUSIVE" in (o, x) or "NO-REACH" in (o, x):
            v = "RERUN (%s / %s)" % (o, x)
        elif x == "STOPPED" and o == "THROUGH":
            v = "FAIL: port bug on this route"
        elif x == "THROUGH" and o == "THROUGH":
            v = "BOTH THROUGH: designed or H-D (the original does it too)"
        elif x == "STOPPED" and o == "STOPPED":
            v = "PASS: both stopped by the wall"
        else:
            v = "UNEXPECTED: xenia THROUGH, ours STOPPED"
        res["verdict"] = v
    print(json.dumps(res, indent=1, default=str))


if __name__ == "__main__":
    main()
