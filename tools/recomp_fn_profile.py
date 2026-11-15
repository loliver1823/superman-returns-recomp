#!/usr/bin/env python3
"""recomp_fn_profile.py - per-function instruction-class profile of the RECOMPILED guest code.

Reads guest/ppc/ppc_recomp.*.cpp (the generated recomp; every lowered instruction carries its
PPC mnemonic as a `// mnem operands` comment) and, for each requested function, reports:
  - the file it lives in and its instruction count;
  - the counts of the "codegen-risky" instruction classes (VMX compares / record forms, FP
    estimate + select + fused ops, conversions, rounding, dot products, D3D pack/unpack, mftb);
  - KNOWN-BUG hits: instruction forms with a known recompiler / helper defect
    (vcmpgtuh./vcmpgtub. do not update CR6 - recompiler.cpp; vrfin uses roundevenf, which
    rounded .5 ties away from even in guest/math_helpers.cpp before the fix batch);
  - the direct callees (bl targets, save/restore helpers dropped), indirect calls (bctrl) and
    jump tables (bctr lowered to switch).
With --closure it walks the direct-call graph from the roots (BFS, --depth levels) so the
static reach of an entry point is profiled in one go. Indirect calls (vtables, the Havok
collision dispatcher) are NOT followed - add their targets as extra roots.

    python tools/recomp_fn_profile.py 0x825CD4C0 0x8258B920
    python tools/recomp_fn_profile.py --closure --depth 3 0x8258B920 --json out.json

Stdlib only. The recomp is regenerated in place; results describe the tree on disk now.
"""
import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PPC_DIR = os.path.join(ROOT, "guest", "ppc")

FN_RE = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
INSTR_RE = re.compile(r"^\t// ([a-z][a-z0-9_.]*)\s*(.*)$")
BL_RE = re.compile(r"^bl\s+0x([0-9a-f]+)")

# Instruction classes where lowering bugs have bitten (or could), grouped for the report.
CLASSES = collections.OrderedDict([
    ("vcmp_record", re.compile(r"^vcmp.*\.$")),            # updates CR6
    ("vcmp", re.compile(r"^vcmp.*[^.]$")),
    ("fcmp", re.compile(r"^fcmp")),
    ("fsel", re.compile(r"^fsel")),
    ("vsel", re.compile(r"^vsel")),
    ("estimate", re.compile(r"^(frsqrte|fres|vrsqrtefp|vrefp|vexptefp|vlogefp)")),
    ("fused_fp", re.compile(r"^(fmadd|fmsub|fnmadd|fnmsub)")),
    ("fused_vmx", re.compile(r"^(vmaddfp|vnmsubfp|vmaddcfp)")),
    ("dot", re.compile(r"^(vmsum3fp|vmsum4fp|vdot)")),
    ("minmax", re.compile(r"^(vmaxfp|vminfp)")),
    ("round_vmx", re.compile(r"^vrfi")),
    ("convert_vmx", re.compile(r"^(vctsxs|vctuxs|vcfsx|vcfux|vcsxwfp|vcuxwfp|vcfpsxws|vcfpuxws)")),
    ("convert_fp", re.compile(r"^(fctiw|fctid|fcfid|frsp)")),
    ("d3d_pack", re.compile(r"^(vpkd3d|vupkd3d)")),
    ("fsqrt", re.compile(r"^fsqrt")),
    ("fpscr", re.compile(r"^(mffs|mtfsf|mtfsb|mcrfs|mtvscr|mfvscr)")),
    ("mftb", re.compile(r"^mftb")),
])
KNOWN_BUGS = {
    "vcmpgtuh.": "no CR6 update (recompiler.cpp vcmpgtu[bh]. record form)",
    "vcmpgtub.": "no CR6 update (recompiler.cpp vcmpgtu[bh]. record form)",
    "vrfin": "roundevenf rounds .5 ties away from even (guest/math_helpers.cpp, pre-fix)",
}
HELPER_PREFIXES = (0x8279E, 0x8279F)   # __savegprlr_/__restgprlr_/__savefpr_/__savevmx_ family


def load_functions(ppc_dir):
    """{va: {file, lines(list of (mnem, operands, following-code))}} for every recomp function."""
    fns = {}
    for name in sorted(os.listdir(ppc_dir)):
        if not (name.startswith("ppc_recomp.") and name.endswith(".cpp")):
            continue
        cur = None
        with open(os.path.join(ppc_dir, name), encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                m = FN_RE.match(ln)
                if m:
                    cur = {"file": name, "ins": [], "switch": 0}
                    fns[int(m.group(1), 16)] = cur
                    continue
                if cur is None:
                    continue
                m = INSTR_RE.match(ln)
                if m:
                    cur["ins"].append((m.group(1), m.group(2)))
                elif ln.startswith("\tswitch ("):
                    cur["switch"] += 1
    return fns


def profile(va, f):
    mn = collections.Counter(m for m, _ in f["ins"])
    classes = collections.OrderedDict()
    for cname, rx in CLASSES.items():
        hits = {m: n for m, n in mn.items() if rx.match(m)}
        if hits:
            classes[cname] = hits
    callees = []
    for m, ops in f["ins"]:
        if m == "bl":
            b = BL_RE.match("bl " + ops)
            if b:
                t = int(b.group(1), 16)
                if (t >> 12) not in HELPER_PREFIXES and t not in callees:
                    callees.append(t)
    return {
        "va": "0x%08X" % va,
        "file": f["file"],
        "n_ins": len(f["ins"]),
        "classes": classes,
        "known_bug_hits": {m: mn[m] for m in KNOWN_BUGS if mn.get(m)},
        "callees": ["0x%08X" % t for t in callees],
        "indirect_calls": mn.get("bctrl", 0),
        "jump_tables": f["switch"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("va", nargs="+", help="guest function VAs (hex)")
    ap.add_argument("--closure", action="store_true", help="follow direct bl callees")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--ppc-dir", default=PPC_DIR)
    ap.add_argument("--json", help="write the full result as JSON")
    a = ap.parse_args()
    fns = load_functions(a.ppc_dir)
    order, seen = [], set()
    frontier = [(int(v, 16), 0) for v in a.va]
    while frontier:
        va, d = frontier.pop(0)
        if va in seen:
            continue
        seen.add(va)
        if va not in fns:
            order.append({"va": "0x%08X" % va, "missing": True, "depth": d})
            continue
        p = profile(va, fns[va])
        p["depth"] = d
        order.append(p)
        if a.closure and d < a.depth:
            frontier.extend((int(c, 16), d + 1) for c in p["callees"])
    for p in order:
        if p.get("missing"):
            print("%s  (not a recomp function entry)" % p["va"])
            continue
        cls = "; ".join("%s: %s" % (c, ",".join("%s x%d" % kv for kv in sorted(h.items())))
                        for c, h in p["classes"].items())
        print("%s d%d %-18s ins=%-5d bctrl=%-2d jt=%d" % (p["va"], p["depth"], p["file"], p["n_ins"],
                                                        p["indirect_calls"], p["jump_tables"]))
        if cls:
            print("    ops: " + cls)
        if p["known_bug_hits"]:
            print("    KNOWN-BUG FORMS: " + ", ".join("%s x%d (%s)" % (m, n, KNOWN_BUGS[m])
                                                     for m, n in p["known_bug_hits"].items()))
        if p["callees"]:
            print("    calls: " + " ".join(p["callees"]))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(order, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
