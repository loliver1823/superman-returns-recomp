#!/usr/bin/env python3
"""coverage.py - what the golden-call differential exercised, from the oracle's <tag>.<mode>.cov.json
files: distinct instructions / functions / conditional-branch directions / mnemonics, the share of
a static closure (tools/recomp_fn_profile.py --closure --json) that ran, and the kernel strong-def
overrides (docs/OVERRIDES.json) and hand-spliced guest bodies (tools/regen/splice_list.txt) that
sat on the executed path (the recomp side runs those, not pure codegen).

    python coverage.py [--out-dir archive/golden_call/out] [--closure closure.json] [--mode xenia] set...
"""
import bisect
import collections
import json
import os
import re
import sys

import argparse
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ap = argparse.ArgumentParser()
ap.add_argument("sets", nargs="*", default=["gold", "mq", "lc", "cp"])
ap.add_argument("--out-dir", default=os.path.join(ROOT, "archive", "golden_call", "out"))
ap.add_argument("--closure", default=os.path.join(ROOT, "archive", "v1_invest", "mopp", "closure.json"))
ap.add_argument("--mode", default="xenia")
A = ap.parse_args()
sets, mode, OUTD = A.sets, A.mode, A.out_dir

starts = sorted(int(m.group(1), 16) for m in re.finditer(r"\{ 0x([0-9A-F]{8}), ", open(os.path.join(ROOT, "guest", "ppc", "ppc_func_mapping.cpp")).read()))
closure = json.load(open(A.closure)) if os.path.exists(A.closure) else []
cl_fns = {int(f["va"], 16) for f in closure if f.get("file")}
cl_mn = collections.Counter()

pcs, br, mn = set(), {}, collections.Counter()
steps = 0
for s in sets:
    c = json.load(open(os.path.join(OUTD, "%s.%s.cov.json" % (s, mode))))
    pcs.update(c["pcs"])
    for k, v in c["branches"].items():
        t = br.setdefault(k, [0, 0]); t[0] += v[0]; t[1] += v[1]
    mn.update(c["mnemonics"])
    steps += sum(c["steps"].values())


def fn_of(pc):
    i = bisect.bisect_right(starts, pc) - 1
    return starts[i] if i >= 0 else None


fns = collections.Counter(fn_of(p) for p in pcs)
print("sets:", sets, " instructions executed (dynamic):", steps)
print("distinct instruction addresses executed:", len(pcs))
print("distinct functions entered:", len(fns))
print("static closure functions executed: %d / %d" % (len(cl_fns & set(fns)), len(cl_fns)))
missing = sorted(cl_fns - set(fns))
both = sum(1 for v in br.values() if v[0] and v[1])
one = sum(1 for v in br.values() if bool(v[0]) != bool(v[1]))
print("conditional branches executed: %d  (both directions: %d, one direction only: %d)" % (len(br), both, one))
print("distinct mnemonics executed: %d" % len(mn))
print("  top:", ", ".join("%s:%d" % kv for kv in mn.most_common(12)))
fp = {k: v for k, v in mn.items() if re.match(r"^(v|f|lv|stv|lfs|stfs)", k)}
print("  FP/VMX mnemonics:", ", ".join("%s:%d" % kv for kv in sorted(fp.items())))
ov = json.load(open(os.path.join(ROOT, "docs", "OVERRIDES.json")))
ov_hit = sorted("%s(%s:%s)" % (k, v.get("file"), v.get("class")) for k, v in ov.items() if k.startswith("sub_") and int(k[4:], 16) in fns)
print("kernel strong-def overrides on the executed path (%d):" % len(ov_hit), ", ".join(ov_hit))
spl = set()
sp = os.path.join(ROOT, "tools", "regen", "splice_list.txt")
if os.path.exists(sp):
    for l in open(sp):
        l = l.strip().split()
        if l and l[0].startswith(("sub_", "0x")):
            try:
                spl.add(int(l[0].replace("sub_", "").replace("0x", ""), 16))
            except ValueError:
                pass
spl_hit = sorted("%08X" % f for f in fns if f in spl)
print("hand-spliced guest bodies on the executed path (%d):" % len(spl_hit), " ".join(spl_hit))
json.dump({"functions": ["%08X" % f for f in sorted(fns)], "closure_missing": ["%08X" % f for f in missing]},
          open(os.path.join(OUTD, "coverage_functions.json"), "w"), indent=0)
print("closure functions NOT executed (%d): see %s" % (len(missing), os.path.join(OUTD, "coverage_functions.json")))
