#!/usr/bin/env python3
"""compare.py -- L6 differential CPU-test: diff recomp vs golden (the SECOND half).

Runs the recomp side (the `difftest` executable built from tools/difftest/difftest_main.cpp)
on a case file, then masked-diffs its register deltas against the captured golden
(golden/<fn>.golden.json, produced by golden_capture.py). See docs/DIFFTEST_DESIGN.md §4
for the divergence vocabulary.

MASKED DIFF (DIFFTEST_DESIGN.md §4):
  * Output registers (default r3, r4)  -> GATING. A mismatch is a definite recomp bug.
  * Memory writes (Tier 1)             -> GATING. deltas.mem is byte-diffed vs golden.
  * cr / xer                           -> advisory (warn, don't fail).
  * volatile scratch (r9..r12, etc.)   -> ignored entirely.

Usage:
  python compare.py --fn sub_82670774                          # auto: cases/ + golden/
  python compare.py --cases cases/sub_82670774.cases.json \
                    --golden golden/sub_82670774.golden.json
  python compare.py --fn sub_82670774 --difftest ../../build/difftest.exe
  python compare.py --all                                      # every golden/*.golden.json

Exit code 0 = all gated fields match; 1 = at least one divergence; 2 = harness error.
Stdlib only.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES_DIR = os.path.join(HERE, 'cases')
GOLDEN_DIR = os.path.join(HERE, 'golden')
# Default location of the built recomp-side runner.
REPO_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_DIFFTEST = [
    os.path.join(REPO_ROOT, 'build', 'difftest.exe'),
    os.path.join(REPO_ROOT, 'build', 'difftest'),
]

# Registers we gate on (the ABI output set). Everything else is advisory/ignored.
GATED_REGS = ('r3', 'r4')


def _hexbytes(s):
    """A hex byte-string ('11', '0100', '0xdeadbeef') -> list of int byte values."""
    if s.lower().startswith('0x'):
        s = s[2:]
    if len(s) % 2:
        s = s[:-1]
    return [int(s[i:i + 2], 16) for i in range(0, len(s), 2)]


def mem_bytemap(delta_mem):
    """Flatten a deltas.mem dict { "<VA hex>": "<hex bytes>" } into { int_VA: byte }.

    This makes the comparison independent of how the two sides happened to SEGMENT the
    changed bytes into runs: only the per-address post-byte matters (DIFFTEST_DESIGN §4 #2:
    "a written VA differs in value, OR in which bytes were written").
    """
    out = {}
    for va, hexs in (delta_mem or {}).items():
        base = int(va, 16) if str(va).lower().startswith('0x') else int(va, 16)
        for i, b in enumerate(_hexbytes(hexs)):
            out[base + i] = b
    return out


def find_difftest(explicit):
    if explicit:
        return explicit
    for p in DEFAULT_DIFFTEST:
        if os.path.exists(p):
            return p
    return None


def norm(v):
    """Normalize a hex/int reg value to a canonical 8-wide lowercase 0x string."""
    if isinstance(v, str):
        v = int(v, 16) if v.lower().startswith('0x') else int(v, 0)
    return '0x' + format(v & 0xFFFFFFFF, '08x')


def run_recomp(difftest, cases_path):
    """Invoke the recomp runner on a case file; return {case_id: {reg: val}} deltas."""
    try:
        out = subprocess.run([difftest, '--cases', cases_path],
                             capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        raise SystemExit(f'compare: failed to run {difftest}: {e}')
    if out.returncode not in (0, 1):
        sys.stderr.write(out.stderr)
        raise SystemExit(f'compare: {difftest} exited {out.returncode}')
    try:
        results = json.loads(out.stdout)
    except ValueError as e:
        sys.stderr.write(out.stdout)
        raise SystemExit(f'compare: cannot parse recomp output as JSON: {e}')
    # Keep the whole recomp object per case (status/purity/exception + deltas) so the
    # comparator can gate negative (faulted) cases as well as register deltas.
    by_id = {}
    for r in results:
        by_id[r.get('case_id')] = r
    return by_id


def load_golden(golden_path):
    with open(golden_path, encoding='utf-8') as fh:
        goldens = json.load(fh)
    return {g.get('case_id'): g for g in goldens}


def compare_one(fn, cases_path, golden_path, difftest):
    recomp = run_recomp(difftest, cases_path)
    golden = load_golden(golden_path)

    n_pass = n_fail = 0
    for cid, g in golden.items():
        if cid not in recomp:
            print(f'  [MISS] {fn}:{cid}  recomp produced no result')
            n_fail += 1
            continue
        robj = recomp[cid]
        rstatus = robj.get('status', 'ok')

        # NEGATIVE case: golden expects a clean fault (non-pure fn the SEH guard caught).
        if g.get('status') == 'faulted':
            if rstatus == 'faulted':
                exc = robj.get('exception', '?')
                print(f'  [PASS] {fn}:{cid}  faulted (SEH caught {exc}, '
                      f'purity={robj.get("purity","?")})')
                n_pass += 1
            else:
                print(f'  [FAIL] {fn}:{cid}  expected fault but recomp status={rstatus} '
                      f'(SEH guard did not fire / fn ran)')
                n_fail += 1
            continue

        # POSITIVE case: a fault is a hard failure (the function should have run cleanly).
        if rstatus == 'faulted':
            print(f'  [FAIL] {fn}:{cid}  recomp FAULTED ({robj.get("exception","?")}) '
                  f'on a case expected to run; purity={robj.get("purity","?")}')
            n_fail += 1
            continue

        gregs = {k: norm(v) for k, v in g.get('deltas', {}).get('regs', {}).items()}
        rregs = {k: norm(v) for k, v in robj.get('deltas', {}).get('regs', {}).items()}
        diffs = []
        for reg in GATED_REGS:
            if reg in gregs:  # golden expects this output reg
                want = gregs[reg]
                got = rregs.get(reg)
                if got != want:
                    diffs.append(f'{reg}: recomp={got} golden={want}')

        # MEMORY GATING (Tier 1, DIFFTEST_DESIGN §4 #2): the recomp's memory delta must
        # match the golden's byte-for-byte. We flatten both deltas to {VA: byte} maps so
        # the comparison is independent of run segmentation, then diff over the UNION of
        # touched addresses -- a byte the golden expects but the recomp didn't write (and
        # vice versa) is a mismatch, exactly like a register divergence.
        gmem = mem_bytemap(g.get('deltas', {}).get('mem', {}))
        rmem = mem_bytemap(robj.get('deltas', {}).get('mem', {}))
        for addr in sorted(set(gmem) | set(rmem)):
            want = gmem.get(addr)
            got = rmem.get(addr)
            if want != got:
                ws = '0x%02x' % want if want is not None else '--'
                gs = '0x%02x' % got if got is not None else '--'
                diffs.append('mem[0x%08x]: recomp=%s golden=%s' % (addr, gs, ws))

        if diffs:
            print(f'  [FAIL] {fn}:{cid}  ' + '; '.join(diffs))
            n_fail += 1
        else:
            shown = ' '.join(f'{r}={gregs[r]}' for r in GATED_REGS if r in gregs)
            if gmem:
                shown += (' ' if shown else '') + 'mem(%dB)' % len(gmem)
            purity = robj.get('purity', '?')
            note = '' if purity == 'leaf' else f'  [purity={purity}!]'
            print(f'  [PASS] {fn}:{cid}  {shown}  (oracle={g.get("oracle","?")}){note}')
            n_pass += 1
    return n_pass, n_fail


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fn', help='function name; uses cases/<fn>.cases.json + golden/<fn>.golden.json')
    ap.add_argument('--cases', help='explicit cases file')
    ap.add_argument('--golden', help='explicit golden file')
    ap.add_argument('--difftest', help='path to the built difftest runner (default: build/difftest[.exe])')
    ap.add_argument('--all', action='store_true', help='compare every golden/*.golden.json')
    args = ap.parse_args(argv)

    difftest = find_difftest(args.difftest)
    if not difftest:
        raise SystemExit('compare: difftest runner not found. Build it:\n'
                         '  cmake --build build --target difftest\n'
                         'or pass --difftest <path>.')

    jobs = []  # (fn, cases_path, golden_path)
    if args.all:
        for gp in sorted(glob.glob(os.path.join(GOLDEN_DIR, '*.golden.json'))):
            fn = os.path.basename(gp)[:-len('.golden.json')]
            cp = os.path.join(CASES_DIR, f'{fn}.cases.json')
            if os.path.exists(cp):
                jobs.append((fn, cp, gp))
    elif args.fn:
        jobs.append((args.fn,
                     args.cases or os.path.join(CASES_DIR, f'{args.fn}.cases.json'),
                     args.golden or os.path.join(GOLDEN_DIR, f'{args.fn}.golden.json')))
    elif args.cases and args.golden:
        fn = os.path.basename(args.cases).split('.')[0]
        jobs.append((fn, args.cases, args.golden))
    else:
        raise SystemExit('compare: provide --fn, (--cases and --golden), or --all')

    total_pass = total_fail = 0
    for fn, cp, gp in jobs:
        if not os.path.exists(cp):
            print(f'[SKIP] {fn}: cases file {cp} missing'); continue
        if not os.path.exists(gp):
            print(f'[SKIP] {fn}: golden file {gp} missing (run golden_capture.py)'); continue
        print(f'== {fn} ==')
        p, f = compare_one(fn, cp, gp, difftest)
        total_pass += p
        total_fail += f

    print(f'\n{total_pass} pass, {total_fail} fail '
          f'({"OK" if total_fail == 0 else "DIVERGENCE"})  oracle vs recomp [{os.path.basename(difftest)}]')
    return 1 if total_fail else 0


if __name__ == '__main__':
    sys.exit(main())
