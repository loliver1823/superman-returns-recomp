#!/usr/bin/env python3
"""scan_candidates.py -- find pure-leaf guest functions in the recomp source.

A "pure leaf" (Tier 0, see docs/DIFFTEST_DESIGN.md) is a recompiled function whose
body contains NONE of: a call (direct sub_X(ctx,base) or PPC_CALL*/PPC_CALL_INDIRECT*),
a register-spill thunk (__savegprlr/__restgprlr/save*/rest*), a guest memory access
(PPC_LOAD_U*/PPC_STORE_U*/PPC_MM_*), ppc_trap, or a raw `.long` data island.

These are the ideal first differential-test targets: the Ghidra PowerPC emulator is a
trustworthy oracle for them, and any divergence is an integer/bitfield codegen bug.

Tier 1 (memory-touching leaves) can be listed with --allow-mem: still no calls/spills,
but PPC_LOAD/STORE permitted.

Usage:
  python scan_candidates.py [--recomp-dir guest/ppc] [--allow-mem]
                            [--min-ops 5] [--max-ops 30] [--limit N] [--json]

Stdlib only.
"""
import argparse
import glob
import json
import os
import re
import sys

FN_RE = re.compile(r'PPC_FUNC_IMPL\(__imp__(sub_[0-9A-Fa-f]+)\)\s*\{')
# instruction comments look like:  // mullw r11,r3,r4
OP_RE = re.compile(r'//\s+([a-z][a-z0-9.]+)')
CALL_RE = re.compile(r'\bsub_[0-9A-Fa-f]+\(ctx')

ARITH_OPS = {
    'add', 'addi', 'addic', 'addis', 'subf', 'subfic', 'subfc', 'subfe', 'neg',
    'mullw', 'mulli', 'mulhw', 'mulhwu', 'mulld', 'divw', 'divwu', 'divd', 'divdu',
    'rlwinm', 'rlwimi', 'rlwnm', 'rldicl', 'rldicr', 'rldic', 'rldimi',
    'slw', 'srw', 'sraw', 'srawi', 'sld', 'srd', 'srad', 'sradi',
    'and', 'andi', 'andis', 'or', 'ori', 'oris', 'xor', 'xori', 'xoris',
    'nand', 'nor', 'eqv', 'andc', 'orc',
    'extsb', 'extsh', 'extsw', 'cntlzw', 'cntlzd', 'clrlwi', 'clrrwi',
    'cmpw', 'cmpwi', 'cmplw', 'cmplwi', 'cmpd', 'cmpdi', 'cmpld', 'cmpldi',
}

SPILL_MARKERS = ('__savegprlr', '__restgprlr', 'savefpr', 'restfpr', 'savevmx', 'restvmx')


def split_functions(text):
    """Yield (name, body) for each PPC_FUNC_IMPL in a file."""
    idxs = [(m.start(), m.group(1)) for m in FN_RE.finditer(text)]
    for i, (start, name) in enumerate(idxs):
        end = idxs[i + 1][0] if i + 1 < len(idxs) else len(text)
        yield name, text[start:end]


def classify(body, allow_mem=False):
    """Return (is_candidate, ops_list) for a function body."""
    if '.long' in body:
        return False, []
    has_call = bool(CALL_RE.search(body)) or 'PPC_CALL_INDIRECT_FUNC' in body or 'PPC_CALL_FUNC' in body
    has_spill = any(m in body for m in SPILL_MARKERS)
    has_trap = 'ppc_trap' in body
    has_mem = ('PPC_STORE_U' in body) or ('PPC_LOAD_U' in body) or ('PPC_MM_' in body)
    if has_call or has_spill or has_trap:
        return False, []
    if has_mem and not allow_mem:
        return False, []
    ops = [m.group(1) for m in OP_RE.finditer(body)]
    return True, ops


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recomp-dir', default=None,
                    help='dir with ppc_recomp.*.cpp (default: <repo>/guest/ppc)')
    ap.add_argument('--allow-mem', action='store_true',
                    help='Tier 1: permit PPC_LOAD/STORE (still no calls/spills)')
    ap.add_argument('--min-ops', type=int, default=5)
    ap.add_argument('--max-ops', type=int, default=30)
    ap.add_argument('--require-arith', action='store_true', default=True,
                    help='require at least one arithmetic/bitfield op (default on)')
    ap.add_argument('--limit', type=int, default=0, help='0 = no limit')
    ap.add_argument('--json', action='store_true', help='emit JSON instead of a table')
    args = ap.parse_args(argv)

    recomp_dir = args.recomp_dir
    if recomp_dir is None:
        here = os.path.dirname(os.path.abspath(__file__))
        repo = os.path.abspath(os.path.join(here, '..', '..'))
        recomp_dir = os.path.join(repo, 'guest', 'ppc')

    files = sorted(glob.glob(os.path.join(recomp_dir, 'ppc_recomp.*.cpp')))
    if not files:
        print(f'no ppc_recomp.*.cpp under {recomp_dir}', file=sys.stderr)
        return 1

    results = []
    for f in files:
        text = open(f, encoding='utf-8', errors='replace').read()
        for name, body in split_functions(text):
            ok, ops = classify(body, allow_mem=args.allow_mem)
            if not ok:
                continue
            nops = len(ops)
            if not (args.min_ops <= nops <= args.max_ops):
                continue
            if args.require_arith and not any(o.split('.')[0] in ARITH_OPS for o in ops):
                continue
            va = '0x' + name[len('sub_'):].upper()
            results.append({
                'guest_va': va, 'fn': name, 'file': os.path.basename(f),
                'n_ops': nops, 'ops': ops,
            })

    results.sort(key=lambda r: (r['n_ops'], r['guest_va']))
    if args.limit:
        results = results[:args.limit]

    if args.json:
        json.dump(results, sys.stdout, indent=2)
        sys.stdout.write('\n')
    else:
        tier = 'Tier 1 (mem-touching leaves)' if args.allow_mem else 'Tier 0 (pure-arith leaves)'
        print(f'# {tier}: {len(results)} candidates')
        for r in results:
            print(f"{r['n_ops']:3d}  {r['guest_va']}  {r['fn']:20s}  {r['file']:20s}  "
                  f"{' '.join(r['ops'][:12])}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
