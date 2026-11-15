#!/usr/bin/env python3
"""classify.py - classify recomp-vs-oracle divergences per case:
  ULP     same records, same shape/key chains, same counts; only float bits differ (max ulp shown)
  STRUCT  different number of contact records, different (shape, key) chains, or different
          collector counts: a HIT-SET difference (the class that could lose a building)
  OTHER   anything else (CHANGED pages, r3, DARR ...)
For STRUCT cases prints the near-threshold context (distances / fractions involved).

    python classify.py out/<tag>.recomp.out out/<tag>.oracle.<mode>.out [--show 20]
"""
import argparse
import re
import struct
import collections

PREC = re.compile(r"P c=(\w+) pos=([\w,]+) nd=([\w,]+) A=(\S*) B=(\S*)")


def cases(path):
    out, order, cur = {}, [], None
    for l in open(path):
        l = l.rstrip("\n")
        if l.startswith("CASE "):
            cur = l[5:]
            order.append(cur)
            out[cur] = []
        elif cur is not None:
            out[cur].append(l)
    return out, order


def f(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def ulp(a, b):
    if a == b:
        return 0
    def key(u):
        return u if u < 0x80000000 else -(u & 0x7FFFFFFF)
    if (a & 0x7F800000) == 0x7F800000 or (b & 0x7F800000) == 0x7F800000:
        return 1 << 30
    return abs(key(a) - key(b))


def records(lines):
    recs, other = [], []
    for l in lines:
        m = PREC.match(l)
        if m:
            recs.append((m.group(1), [int(x, 16) for x in m.group(2).split(",")],
                         [int(x, 16) for x in m.group(3).split(",")], m.group(4), m.group(5)))
        else:
            other.append(l)
    return recs, other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recomp")
    ap.add_argument("oracle")
    ap.add_argument("--show", type=int, default=15)
    a = ap.parse_args()
    R, order = cases(a.recomp)
    O, _ = cases(a.oracle)
    cls = collections.Counter()
    ulps = collections.Counter()
    structs = []
    others = []
    absd = {"pos": 0.0, "normal": 0.0, "dist": 0.0}
    for c in order:
        r, o = R.get(c, []), O.get(c, [])
        if r == o:
            cls["SAME"] += 1
            continue
        rr, ro = records(r)
        orr, oo = records(o)
        if any(l.startswith("ORACLE-ERROR") for l in oo):
            cls["ORACLE-ERROR"] += 1
            others.append((c, [l for l in oo if l.startswith("ORACLE-ERROR")][:1]))
            continue
        same_struct = len(rr) == len(orr) and all(x[0] == y[0] and x[3] == y[3] and x[4] == y[4] for x, y in zip(rr, orr))
        # collector dumps: +12 count must match; +4 earlyOut may differ by ulps
        dump_ok = True
        mu = 0
        if len(ro) != len(oo):
            dump_ok = False
        else:
            for x, y in zip(ro, oo):
                if x == y:
                    continue
                if x.startswith("D 700001") and y.startswith("D 700001"):
                    hx, hy = x.split()[2], y.split()[2]
                    if hx[16:] != hy[16:] or hx[:8] != hy[:8]:
                        dump_ok = False
                    else:
                        mu = max(mu, ulp(int(hx[8:16], 16), int(hy[8:16], 16)))
                else:
                    dump_ok = False
        if same_struct and dump_ok:
            for x, y in zip(rr, orr):
                for k, (u, v) in enumerate(zip(x[1][:3] + x[2], y[1][:3] + y[2])):
                    fu, fv = f(u), f(v)
                    d = abs(fu - fv) if fu == fu and fv == fv else float("inf")
                    kind = "pos" if k < 3 else "normal" if k < 6 else "dist"
                    absd[kind] = max(absd[kind], d)
                    if kind == "dist":
                        mu = max(mu, ulp(u, v))
            cls["ULP"] += 1
            ulps[min(mu, 1 << 20) if mu < 1 << 30 else "inf/nan"] += 1
        elif not same_struct:
            cls["STRUCT"] += 1
            structs.append((c, rr, orr, ro, oo))
        else:
            cls["OTHER"] += 1
            others.append((c, [x for x, y in zip(ro, oo) if x != y][:2] + [y for x, y in zip(ro, oo) if x != y][:2]))
    print("classes:", dict(cls))
    buckets = collections.Counter()
    for k, v in ulps.items():
        if k == "inf/nan":
            buckets[k] += v
        else:
            b = 1
            while b < k:
                b *= 2
            buckets["<=%d" % b] += v
    print("ULP-class max |recomp - oracle|: pos %.3g m, normal %.3g, dist/fraction %.3g" % (absd["pos"], absd["normal"], absd["dist"]))
    print("ULP-only cases by max ulp of dist/fraction (incl. collector earlyOut):", dict(sorted(buckets.items(), key=lambda kv: str(kv[0]))))
    for c, rr, orr, ro, oo in structs[:a.show]:
        print("STRUCT", c, "recomp %d recs / oracle %d recs" % (len(rr), len(orr)))
        rs = {(x[0], x[4]) for x in rr}
        os_ = {(x[0], x[4]) for x in orr}
        for x in rr:
            tag = "" if (x[0], x[4]) in os_ else "  <- only recomp"
            print("   R c=%s B=%s dist/frac=%.7g%s" % (x[0], x[4], f(x[2][3]), tag))
        for x in orr:
            tag = "" if (x[0], x[4]) in rs else "  <- only oracle"
            print("   O c=%s B=%s dist/frac=%.7g%s" % (x[0], x[4], f(x[2][3]), tag))
    for c, ls in others[:a.show]:
        print("OTHER", c, ls)


if __name__ == "__main__":
    main()
