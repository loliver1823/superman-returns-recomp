#!/usr/bin/env python3
"""hot_lines.py - iter-466 hfr: line-level self time of ONE thread from SR_HOST_PROFILE_RAWPC=1
profiles (docs/PERFORMANCE.md 2.5).

    python tools/perf/hot_lines.py <perf dir> --thread <name substring or tid> [--from N] [--to M]
           [--top 40] [--inlines]

Reads hprof_<n>.txt (n in [from, to]: pick the files of the era you want; each file is
SR_HOST_PROFILE_SECS long) and aggregates the `self` raw-PC samples of the matching thread by
function + file:line (llvm-symbolizer over the exe's PDB; --inlines attributes to the innermost
inlined frame). Other modules resolve to their nearest export (module!name).
"""
import argparse
import glob
import os
import re
import struct
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolize import Symbolizer, LLVM_SYMBOLIZER  # noqa: E402


def llvm_lines(exe, rvas, inlines):
    base = 0x140000000
    try:
        data = open(exe, 'rb').read(4096)
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    except (OSError, struct.error):
        pass
    inp = ''.join('0x%x\n' % (base + r) for r in rvas)
    args = [LLVM_SYMBOLIZER, '--obj=' + exe, '--functions=linkage', '--output-style=LLVM']
    if not inlines:
        args.append('--no-inlines')
    p = subprocess.run(args, input=inp, capture_output=True, text=True)
    out, cur = [], []
    for ln in p.stdout.splitlines():
        if not ln.strip():
            if cur:
                out.append(cur)
                cur = []
            continue
        cur.append(ln.strip())
    if cur:
        out.append(cur)
    res = []
    for blk in out:
        fn = blk[0] if blk else '??'
        loc = blk[1] if len(blk) > 1 else '?'
        loc = re.sub(r'^.*[\\/]', '', loc)
        res.append('%s @ %s' % (fn[:90], loc))
    while len(res) < len(rvas):
        res.append('??')
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('perf_dir')
    ap.add_argument('--thread', required=True)
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=10 ** 9)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--inlines', action='store_true')
    a = ap.parse_args()
    samples, mods, total = Counter(), {}, 0
    for f in sorted(glob.glob(os.path.join(a.perf_dir, 'hprof_*.txt'))):
        n = int(re.search(r'hprof_(\d+)', f).group(1))
        if n < a.lo or n > a.hi:
            continue
        tids = set()
        for ln in open(f, encoding='utf-8', errors='replace'):
            if ln.startswith('mod '):
                _, idx, b, sz, path = ln.rstrip('\n').split(' ', 4)
                mods[int(idx)] = (path, int(b, 16), int(sz, 16))
            elif ln.startswith('thread '):
                parts = ln.split()
                if a.thread in parts[1] or a.thread in ln.split('name=')[1].split(' run=')[0]:
                    tids.add(parts[1])
            elif ln.startswith('self '):
                _, tid, cnt, key = ln.split()
                if tid in tids:
                    m, r = key.split(':')
                    samples[(int(m), int(r, 16) & 0x7FFFFFFF)] += int(cnt)   # bit 31 = raw-pc flag
                    total += int(cnt)
    if not total:
        print('no samples for', a.thread)
        return 1
    sym = Symbolizer(mods)
    exe_keys = [k for k in samples if mods.get(k[0], ('',))[0].lower().endswith('.exe')]
    names = {}
    if exe_keys:
        exe = mods[exe_keys[0][0]][0]
        for k, nm in zip(exe_keys, llvm_lines(exe, [k[1] for k in exe_keys], a.inlines)):
            names[k] = nm
    agg = Counter()
    for k, c in samples.items():
        agg[names.get(k) or sym.sym(*k)] += c
    print('thread %s: %d self samples' % (a.thread, total))
    for nm, c in agg.most_common(a.top):
        print('%6.2f%%  %s' % (100.0 * c / total, nm))
    return 0


if __name__ == '__main__':
    sys.exit(main())
