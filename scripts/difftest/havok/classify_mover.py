#!/usr/bin/env python3
"""classify_mover.py - read gen_mover_cases.py outputs: recomp vs oracle agreement per case, and what
the mover DID (so "is collision applied" is answered from the game's own code, per case):

  per tick: entity position (actor+0x20), proxy velocity, PRXY flags (+0x7C supported, +0x88
  off-camera-skip taken), manifold bodies (hkCharacterProxy +0x74, 48-byte entries, collidable B at
  +40; body = collidable - 0x14).
  outcome for wall cases (target body + approach axis/side from the case name, start from the W
  lines, AABB from <script>.meta.json):
    BLOCKED       the target body entered the manifold and the capsule centre stayed in front of
                  the face (penetration <= 0.35 m past the AABB face)
    INSET-STOP    contact with the target, stopped deeper than the AABB face (wall inset in the
                  list/MOPP geometry) and not at the free-flight position
    PASS-THROUGH  ended > 5 m past the face with no contact with the target at any tick (walls
                  sit up to ~3 m inside the AABB of list/MOPP buildings)
    NO-REACH      never got within the capsule radius of the face
    OTHER         anything else (printed)

    python classify_mover.py <script> <recomp.out> [<oracle.out>] [--verbose]
(a `path` set prints, per re-traced case, how many travelled tick segments cross the target's shape)
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "golden_call"))
from classify import cases  # noqa: E402


def f4(hexs):
    return struct.unpack(">4f", bytes.fromhex(hexs)[:16])


def script_cases(path):
    out, cur = {}, None
    for l in open(path):
        l = l.rstrip("\n")
        if l.startswith("CASE "):
            cur = l[5:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(l)
    return out


def parse_out(lines, meta):
    actor, po, proxy = meta["actor"], meta["prxy_obj"], meta["proxy"]
    ticks, cur = [], None
    err = None
    for l in lines:
        if l.startswith("ORACLE-ERROR") or l.startswith("NOFUNC"):
            err = l
        if not l.startswith("D ") and not l.startswith("DARR "):
            continue
        p = l.split()
        va = int(p[1], 16)
        if l.startswith("D ") and va == actor + 0x20:
            cur = {"pos": f4(p[2])[:3]}
            ticks.append(cur)
        elif l.startswith("D ") and va == proxy + 0x10 and cur is not None:
            cur["vel"] = f4(p[2])[:3]
        elif l.startswith("D ") and va == po + 0x7C and cur is not None:
            b = bytes.fromhex(p[2])
            cur["supported"] = b[0]
            cur["reduced"] = struct.unpack(">I", b[8:12])[0]
            cur["skip"] = struct.unpack(">I", b[12:16])[0]
        elif l.startswith("D ") and va == po + 0x0C and cur is not None:
            b = bytes.fromhex(p[2])
            cur["b0E"], cur["b0F"] = b[2], b[3]
        elif l.startswith("DARR") and va == proxy + 0x74 and cur is not None:
            n = int(p[2][2:])
            data = bytes.fromhex(p[3]) if len(p) > 3 else b""
            cur["man"] = sorted(struct.unpack(">I", data[48 * i + 40:48 * i + 44])[0] - 0x14 for i in range(n))
    return ticks, err


def start_of(script_lines, meta):
    p0 = v0 = None
    for l in script_lines:
        if l.startswith("W %08X " % (meta["actor"] + 0x20)):
            p0 = struct.unpack(">3f", bytes.fromhex(l.split()[2])[:12])
        elif l.startswith("W %08X " % (meta["proxy"] + 0x10)):
            v0 = struct.unpack(">3f", bytes.fromhex(l.split()[2])[:12])
    return p0, v0


def outcome(name, ticks, p0, v0, meta):
    parts = name.split()
    tgt = next((x for x in parts if x.startswith("0x")), None)
    axs = next((x for x in parts if x.startswith("ax")), None)
    if not tgt or not axs or not ticks or p0 is None:
        return None, {}
    ax, side = int(axs[2]), axs[3] == "+"
    lo, hi = meta["bodies"][tgt]["aabb"]
    face = lo[ax] if side else hi[ax]
    sgn = 1.0 if side else -1.0
    tb = int(tgt, 16)
    pens = [(t["pos"][ax] - face) * sgn for t in ticks]
    contact = any(tb in t.get("man", []) for t in ticks)
    free = p0[ax] + v0[ax] * len(ticks) / 30.0 if v0 else None
    fin = ticks[-1]["pos"][ax]
    at_free = free is not None and abs(fin - free) < 0.01 * len(ticks)
    skips = sum(1 for t in ticks if t.get("skip") == 1)
    info = {"pen_final": round(pens[-1], 3), "pen_max": round(max(pens), 3), "contact": contact,
            "free_flight": at_free, "skips": skips, "speed": round(sum(x * x for x in v0) ** 0.5, 1) if v0 else None,
            "other_bodies": sorted({"0x%08X" % b for t in ticks for b in t.get("man", []) if b != tb})}
    if contact and max(pens) <= 0.35:
        return "BLOCKED", info
    if contact and not at_free:
        return "INSET-STOP", info
    if not contact and pens[-1] > 5.0:
        return "PASS-THROUGH", info
    if max(pens) < -0.3:
        return "NO-REACH", info
    return "OTHER", info


def path_summary(order, R, O):
    """path sets (gen_mover_cases.py path): per source case, the tick segments whose castRay hit the
    target building's own shape."""
    agg = collections.OrderedDict()
    agree = collections.Counter()
    for c in order:
        src, seg = c[5:].split(" | ")
        hit = any(l.startswith("D 700007F0") and int(l.split()[2][:2], 16) for l in R[c])
        frac = next((struct.unpack(">f", bytes.fromhex(l.split()[2])[20:24])[0] for l in R[c] if l.startswith("D 70000780")), None)
        a = agg.setdefault(src, [0, 0, []])
        a[0] += 1
        if hit:
            a[1] += 1
            a[2].append("%s@%.2f" % (seg, frac))
        if O is not None:
            agree["SAME" if R[c] == O.get(c) else "DIFF"] += 1
    for src, (n, h, segs) in agg.items():
        print("%-58s segments=%3d hit=%3d %s" % (src[:58], n, h, " ".join(segs[:4])))
    if O is not None:
        print("recomp vs oracle:", dict(agree))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    verbose = "--verbose" in sys.argv
    script, rout = args[0], args[1]
    oout = args[2] if len(args) > 2 else None
    meta = json.load(open(os.path.splitext(script)[0] + ".meta.json"))
    S = script_cases(script)
    R, order = cases(rout)
    O = cases(oout)[0] if oout else None
    if order and order[0].startswith("path "):
        return path_summary(order, R, O)
    agree = collections.Counter()
    oc = collections.Counter()
    by = collections.defaultdict(collections.Counter)
    maxdp = [0.0]
    for c in order:
        ticks, err = parse_out(R[c], meta)
        p0, v0 = start_of(S.get(c, []), meta)
        kind, info = outcome(c, ticks, p0, v0, meta)
        if O is not None:
            agree["SAME" if R[c] == O.get(c) else "DIFF"] += 1
            ot, oerr = parse_out(O.get(c, []), meta)
            okind, _ = outcome(c, ot, p0, v0, meta)
            if okind != kind:
                agree["OUTCOME-DIFF"] += 1
                print("OUTCOME DIFF %s: recomp %s oracle %s" % (c, kind, okind))
            if oerr:
                agree["ORACLE-ERROR"] += 1
            if len(ot) != len(ticks):
                agree["TICKCOUNT-DIFF"] += 1
            for a, b in zip(ticks, ot):
                d = max(abs(x - y) for x, y in zip(a["pos"], b["pos"]))
                maxdp[0] = max(maxdp[0], d)
                if a.get("skip") != b.get("skip") or a.get("man") != b.get("man"):
                    agree["GATE/MANIFOLD-SET-DIFF"] += 1
        grp = " ".join(c.split()[:2]) if c.split()[0] in ("mv", "gate") else c.split()[0]
        if kind:
            oc[kind] += 1
            by[grp][kind] += 1
        if verbose or kind in ("PASS-THROUGH", "OTHER") or err or c.startswith("gate skip") or c.startswith("gate ndc"):
            extra = ""
            if ticks:
                t = ticks[-1]
                extra = "pos=%s vel=%s sup=%s red=%s skip=%s man=%s" % (
                    tuple(round(x, 3) for x in t["pos"]), tuple(round(x, 2) for x in t.get("vel", ())),
                    t.get("supported"), t.get("reduced"), t.get("skip"), ["0x%08X" % b for b in t.get("man", [])])
            r3 = [l for l in R[c] if l.startswith("r3=")]
            print("%-60s %-12s %s %s %s %s" % (c[:60], kind or "", info if kind else "", extra, " ".join(r3), err or ""))
    if O is not None:
        print("recomp vs oracle:", dict(agree), "max |pos diff| over all ticks = %.4g m" % maxdp[0])
    print("outcomes:", dict(oc))
    for g in sorted(by):
        print("  %-12s %s" % (g, dict(by[g])))


if __name__ == "__main__":
    main()
