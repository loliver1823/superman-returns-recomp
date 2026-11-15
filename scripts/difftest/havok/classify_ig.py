#!/usr/bin/env python3
"""classify_ig.py - compare hkCharacterProxy::integrate outcomes (recomp vs oracle) per case:
final phantom position, proxy velocity, manifold size + body set, collector hit counts; and report
how many cases the building actually stopped / deflected the capsule (the collision the user
reports as missing).

    python classify_ig.py <out>/ig.recomp.out <out>/ig.oracle.<mode>.out ig.script
"""
import struct
import sys
import collections
import math

import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "golden_call"))
from classify import cases  # noqa: E402


def parse(lines):
    r = {"pos": None, "vel": None, "man": None, "bodies": None, "cast": None, "start": None, "err": None, "changed": []}
    for l in lines:
        if l.startswith("ORACLE-ERROR"):
            r["err"] = l
        elif l.startswith("D 022E1AB0"):
            b = bytes.fromhex(l.split()[2])
            r["pos"] = struct.unpack(">3f", b[48:60])
        elif l.startswith("D 0236E3E0"):
            b = bytes.fromhex(l.split()[2])
            r["vel"] = struct.unpack(">3f", b[16:28])
        elif l.startswith("DARR 0236E454"):
            p = l.split()
            n = int(p[2][2:])
            data = bytes.fromhex(p[3]) if len(p) > 3 else b""
            r["man"] = n
            r["bodies"] = sorted(struct.unpack(">I", data[48 * i + 40:48 * i + 44])[0] for i in range(n))
        elif l.startswith("DARR 70000B10"):
            r["cast"] = int(l.split()[2][2:])
        elif l.startswith("DARR 70000C10"):
            r["start"] = int(l.split()[2][2:])
        elif l.startswith("CHANGED"):
            r["changed"].append(l.split()[1])
    return r


def script_inputs(path):
    """start position + velocity written by each case (W 022E1AB0 / W 0236E3F0)."""
    out, cur = {}, None
    for l in open(path):
        l = l.strip()
        if l.startswith("CASE "):
            cur = l[5:]
            out[cur] = {}
        elif l.startswith("W 022E1AB0") and cur:
            b = bytes.fromhex(l.split()[2])
            out[cur]["p0"] = struct.unpack(">3f", b[48:60])
        elif l.startswith("W 0236E3F0") and cur:
            out[cur]["v0"] = struct.unpack(">3f", bytes.fromhex(l.split()[2])[:12])
    return out


def main():
    R, order = cases(sys.argv[1])
    O, _ = cases(sys.argv[2])
    ins = script_inputs(sys.argv[3]) if len(sys.argv) > 3 else {}
    cls = collections.Counter()
    maxdp = maxdv = 0.0
    worst = []
    blocked = building_contact = 0
    for c in order:
        a, b = parse(R[c]), parse(O.get(c, []))
        if b["err"]:
            cls["ORACLE-ERROR"] += 1
            continue
        if R[c] == O.get(c):
            cls["SAME"] += 1
        elif a["man"] != b["man"] or a["bodies"] != b["bodies"] or a["cast"] != b["cast"] or a["start"] != b["start"]:
            cls["STRUCT"] += 1
            worst.append(("STRUCT", c, a["man"], b["man"], a["bodies"], b["bodies"], a["cast"], b["cast"], a["start"], b["start"]))
        else:
            cls["FLOAT"] += 1
        if a["pos"] and b["pos"]:
            dp = max(abs(x - y) for x, y in zip(a["pos"], b["pos"]))
            dv = max(abs(x - y) for x, y in zip(a["vel"], b["vel"]))
            if dp > maxdp:
                maxdp = dp
            maxdv = max(maxdv, dv)
        i = ins.get(c, {})
        if "p0" in i and "v0" in i and a["pos"]:
            free = [i["p0"][k] + i["v0"][k] / 30.0 for k in range(3)]
            if math.dist(free, a["pos"]) > 0.01:
                blocked += 1
            if c.split()[1] != "golden":
                body = int(c.split()[1], 16) + 0x14
                if body in (a["bodies"] or []):
                    building_contact += 1
    print("classes:", dict(cls))
    print("max |pos recomp - oracle| = %.3g m, max |vel diff| = %.3g m/s" % (maxdp, maxdv))
    print("cases where the capsule did NOT reach its free-flight target (>1 cm): %d; cases whose final manifold holds the target building: %d" % (blocked, building_contact))
    for w in worst[:10]:
        print("  ", w)


if __name__ == "__main__":
    main()
