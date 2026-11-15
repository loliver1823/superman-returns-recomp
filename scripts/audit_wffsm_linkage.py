#!/usr/bin/env python3
"""iter-455l [WFFSM] linkage audit (fail-closed, SELF-CALIBRATING).

The WFFSM observer integrates into two PRE-EXISTING strong wrappers
(sub_8224CDC0 in apt_as_watch.cpp, sub_8278C448 in apt_parse_trace.cpp) and
adds two NEW ones (sub_821F7C38, sub_82167808 in wffsm_obs.cpp). The director's
verdict requires an audit proving EXACTLY ONE strong installed symbol per
target across the whole link — a duplicate strong definition (the failure mode
the original spec would have caused) must fail loudly.

Calibration: the mangled global-hook template is derived from the
known-installed hook sub_82709470 (alloc_overlap_watch.cpp.obj, strong T).
For each target the audit scans EVERY object in SupermanReturns.dir and
requires exactly one strong 'T' definition, located in the EXPECTED object.

Fail-closed contract (exit 2, named reason) on: missing objects/llvm-nm,
calibration ambiguity, weak-only definitions, zero or duplicate strong
definitions, or a strong definition in an unexpected object.

Usage (from the repo root): python scripts/audit_wffsm_linkage.py
"""
import glob
import os
import shutil
import subprocess
import sys

TARGETS = {
    'sub_8224CDC0': 'apt_as_watch.cpp.obj',
    'sub_8278C448': 'apt_parse_trace.cpp.obj',
    'sub_821F7C38': 'wffsm_obs.cpp.obj',
    'sub_82167808': 'wffsm_obs.cpp.obj',
}
CAL_SYM = 'sub_82709470'
CAL_OBJ = 'alloc_overlap_watch.cpp.obj'
OBJ_ROOT = 'build/CMakeFiles/SupermanReturns.dir'

def fail(msg):
    print(f'WFFSM LINKAGE AUDIT FAIL: {msg}', file=sys.stderr)
    sys.exit(2)

def nm_defined(nm, obj):
    try:
        out = subprocess.run([nm, '--defined-only', obj], capture_output=True,
                             text=True, timeout=60)
    except Exception as e:
        fail(f'llvm-nm failed to run on {obj}: {e}')
    if out.returncode != 0:
        fail(f'llvm-nm exited {out.returncode} on {obj}: '
             f'{out.stderr.strip()[:200]}')
    syms = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            syms[parts[-1]] = parts[-2]
    return syms

def main():
    nm = shutil.which('llvm-nm')
    if not nm:
        cands = glob.glob('C:/Program Files/LLVM/bin/llvm-nm.exe')
        nm = cands[0] if cands else None
    if not nm:
        fail('llvm-nm not found on PATH or in C:/Program Files/LLVM/bin')

    objs = glob.glob(f'{OBJ_ROOT}/**/*.obj', recursive=True)
    if not objs:
        fail(f'no objects under {OBJ_ROOT} - build first')

    cal_objs = [o for o in objs if os.path.basename(o) == CAL_OBJ]
    if len(cal_objs) != 1:
        fail(f'calibration object {CAL_OBJ}: found {len(cal_objs)}')
    cal_syms = nm_defined(nm, cal_objs[0])
    cal_defs = [s for s in cal_syms if s.startswith('?' + CAL_SYM + '@@')]
    if len(cal_defs) != 1:
        fail(f'calibration symbol {CAL_SYM} ambiguous/absent: {sorted(cal_defs)}')
    if cal_syms[cal_defs[0]] != 'T':
        fail(f'calibration symbol {cal_defs[0]} type {cal_syms[cal_defs[0]]!r}, '
             f'expected strong T')
    template = cal_defs[0].replace(CAL_SYM, '{}')
    print(f'calibrated template from {CAL_SYM}: {template.format("<name>")}')

    # one nm pass per object, reused across targets
    per_obj = {}
    for o in objs:
        per_obj[o] = nm_defined(nm, o)

    bad = []
    for sym, want_obj in TARGETS.items():
        want = template.format(sym)
        strong, weak = [], []
        for o, syms in per_obj.items():
            t = syms.get(want)
            if t == 'T':
                strong.append(os.path.basename(o))
            elif t in ('W', 'w'):
                weak.append(os.path.basename(o))
        if len(strong) != 1:
            bad.append((sym, f'expected exactly ONE strong T, found '
                             f'{len(strong)}: {strong} (weak: {weak})'))
        elif strong[0] != want_obj:
            bad.append((sym, f'strong definition in {strong[0]}, expected '
                             f'{want_obj}'))
    if bad:
        for sym, why in bad:
            print(f'WFFSM LINKAGE AUDIT FAIL: {sym}: {why}', file=sys.stderr)
        sys.exit(2)
    print(f'linkage audit OK ({len(objs)} objects scanned)')
    for sym, obj in TARGETS.items():
        print(f'  {sym}: exactly one strong T, in {obj}')
    sys.exit(0)

if __name__ == '__main__':
    main()
