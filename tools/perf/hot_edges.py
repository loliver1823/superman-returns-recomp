#!/usr/bin/env python3
"""hot_edges.py <perf dir> --thread NAME --from N --to M [--top 400] [--out edges.json]
Guest call edges (caller __imp__sub_X -> callee __imp__sub_Y adjacent on sampled host stacks) of
one thread over a window of host-profile files (perf_run --hz N + SR_HOST_PROFILE_RAWPC=1), with
the edge's inclusive weight (samples whose stack holds the pair) and the callee's host code size
and self share. iter-467 guestcode: the input of the direct-call experiment
(tools/regen/direct_calls.py); frames of kernel strong-def overrides are not __imp__ symbols, so
an overridden callee never shows up as an edge here (direct_calls.py still re-checks the registry).
"""
import argparse, bisect, glob, json, os, re, subprocess
from collections import Counter
import pefile

ap = argparse.ArgumentParser()
ap.add_argument('perf_dir')
ap.add_argument('--thread', required=True)
ap.add_argument('--from', dest='lo', type=int, default=0)
ap.add_argument('--to', dest='hi', type=int, default=10**9)
ap.add_argument('--top', type=int, default=400)
ap.add_argument('--out')
a = ap.parse_args()

stacks, mods = Counter(), {}
for f in sorted(glob.glob(os.path.join(a.perf_dir, 'hprof_*.txt'))):
    n = int(re.search(r'hprof_(\d+)', f).group(1))
    if n < a.lo or n > a.hi:
        continue
    tids = set()
    for ln in open(f, encoding='utf-8', errors='replace'):
        if ln.startswith('mod '):
            _, idx, b, sz, path = ln.rstrip('\n').split(' ', 4)
            mods[int(idx)] = path
        elif ln.startswith('thread '):
            if a.thread in ln.split('name=')[1].split(' run=')[0]:
                tids.add(ln.split()[1])
        elif ln.startswith('stk '):
            p = ln.split()
            if p[1] in tids and p[2] == 'R':
                stacks[p[4]] += int(p[3])
exe = [p for i, p in mods.items() if p.lower().endswith('.exe')][0]
exeidx = [i for i, p in mods.items() if p.lower().endswith('.exe')][0]
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [s for s, _ in funcs]


def fn(key):
    m, r = key.split(':')
    if int(m) != exeidx:
        return None
    r = int(r, 16) & 0x7FFFFFFF
    i = bisect.bisect_right(starts, r) - 1
    return funcs[i][0] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None


total = sum(stacks.values())
edge, selfc, eself = Counter(), Counter(), Counter()
for s, c in stacks.items():
    fr = [fn(k) for k in s.split(';')]
    if fr and fr[0] is not None:
        selfc[fr[0]] += c
        if len(fr) > 1 and fr[1] is not None and fr[1] != fr[0]:
            eself[(fr[1], fr[0])] += c
    seen = set()
    for i in range(len(fr) - 1):
        callee, caller = fr[i], fr[i + 1]
        if callee is None or caller is None or callee == caller:
            continue
        seen.add((caller, callee))
    for e in seen:
        edge[e] += c
keys = sorted({x for e in edge for x in e})
inp = ''.join('0x%x\n' % (0x140000000 + k) for k in keys)
out = subprocess.run([r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe', '--obj=' + exe, '--functions=linkage',
                      '--no-inlines'], input=inp, capture_output=True, text=True).stdout.split('\n\n')
name = {k: o.strip().split('\n')[0] for k, o in zip(keys, out)}
size = dict(funcs)
rx = re.compile(r'^__imp__sub_([0-9A-F]{8})$')
rows = []
for (caller, callee), c in edge.most_common():  # (all edges; --top caps the guest->guest ones kept)
    mc, me = rx.match(name.get(caller, '')), rx.match(name.get(callee, ''))
    if not (mc and me):
        continue
    rows.append({'caller': 'sub_' + mc.group(1), 'callee': 'sub_' + me.group(1), 'weight': c,
                 'pct': round(100.0 * c / total, 3), 'edge_self_pct': round(100.0 * eself[(caller, callee)] / total, 3),
                 'callee_self_pct': round(100.0 * selfc[callee] / total, 3),
                 'callee_bytes': size[callee] - callee})
rows.sort(key=lambda r: -r['edge_self_pct'])
rows = rows[:a.top]
print('thread %s: %d samples, %d guest->guest edges kept (of %d edges)' % (a.thread, total, len(rows), len(edge)))
for r in rows[:30]:  # sorted by the time spent in the callee's own body when called from this caller
    print('  edge self %5.2f%%  incl %6.2f%%  %s -> %s  (callee self %.2f%%, %d bytes)' % (
        r['edge_self_pct'], r['pct'], r['caller'], r['callee'], r['callee_self_pct'], r['callee_bytes']))
if a.out:
    json.dump({'source': os.path.abspath(a.perf_dir), 'thread': a.thread, 'files': [a.lo, a.hi], 'samples': total,
               'exe': exe, 'edges': rows}, open(a.out, 'w'), indent=1)
