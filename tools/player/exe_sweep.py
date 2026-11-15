#!/usr/bin/env python3
"""exe_sweep.py - C1 removed-gate sweep, binary side: is any environment name the release configuration removed
(tools/player/gate_sweep.py's list) still in the release exe?

    python tools/player/exe_sweep.py --removed <removed.json> --release <release exe> [--default <default exe>]
           [--allow NAME[,NAME...]] [--out <names.tsv>]

getenv() takes its name as a literal, so a removed name that is not in the release exe as a C string has no reader
there, however the source reads it (a helper, a table, a macro). A removed name that IS in the release exe is a kept
reader, a log text or a table entry: each must be explained, then listed in --allow. Run it where the exes are (the
cloud worker: `python tools/cloud/sr_cloud.py run --name <tree> -- python tools/player/exe_sweep.py ...`).

Inputs: --removed - gate_sweep.py --json output ({"removed": {name: [files]}, ...}); --release (and optionally
  --default, the SR_ORACLE=1 build of the same tree, as the scan's control) - the exes, read as bytes.
Method: each removed name followed by NUL is counted wherever it occurs in the exe's bytes, whatever byte precedes it
  (so SR_FOO_BAR does not count as SR_FOO). Fail-closed: the byte before a string in .rdata can be any value - an
  alignment pad or the tail of other data reads as 'A' or '8' as often as as NUL - so a match is never dropped for
  its predecessor; a longer literal that merely ends in a removed name (XSR_FOO) counts too, and is explained like any
  other hit. A UTF-16 or computed name is not seen.
Outputs: stdout - one line per removed name `NAME<TAB>default=N<TAB>release=N`, then the summary: removed names, the
  ones present in the release exe (allowed / NOT allowed), --allow names absent from the release exe (stale), and -
  with --default - the names in neither exe (their absence from the release proves nothing: difftest-only names,
  names only in log text that the compiler folded). --out: the same rows as TSV.
Exit: 0 = no removed name is in the release exe outside --allow; 1 = some are (listed as NOT allowed); 2 = could not
  run (an unreadable exe or JSON, a JSON without "removed", an empty removed list).
"""
import argparse
import json
import sys

def scan(path, names):
    with open(path, 'rb') as fh:
        data = fh.read()
    return {n: data.count(n.encode('ascii') + b'\x00') for n in names}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--removed', required=True, help='gate_sweep.py --json output')
    ap.add_argument('--release', required=True, help='the release (SR_ORACLE=0) exe')
    ap.add_argument('--default', help='the default (SR_ORACLE=1) exe of the same tree: the control')
    ap.add_argument('--allow', default='', help='comma-separated removed names explained as kept in the release')
    ap.add_argument('--out', help='write the per-name rows here as TSV')
    a = ap.parse_args()
    try:
        with open(a.removed, encoding='utf-8') as fh:
            removed = json.load(fh)['removed']
        if not removed:
            raise ValueError('the removed list is empty')
        if not all(isinstance(n, str) and n.isascii() for n in removed):
            raise ValueError('a removed name is not an ASCII string')
        rel = scan(a.release, removed)
        dflt = scan(a.default, removed) if a.default else None
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f'exe_sweep: cannot run: {e!r}', file=sys.stderr)
        return 2
    allow = {x.strip() for x in a.allow.split(',') if x.strip()}
    rows, present, neither = [], [], []
    for n in sorted(removed):
        d = dflt[n] if dflt is not None else '-'
        r = rel[n]
        rows.append((n, d, r))
        if r:
            present.append(n)
        elif dflt is not None and not d:
            neither.append(n)
        print(f'{n}\tdefault={d}\trelease={r}')
    bad = [n for n in present if n not in allow]
    stale = sorted(n for n in allow if not rel.get(n))
    print(f'removed names {len(removed)}; in the release exe: {len(present)} - allowed {len(present) - len(bad)}: '
          f"{' '.join(n for n in present if n in allow) or '-'}; NOT allowed {len(bad)}: {' '.join(bad) or '-'}")
    print(f"--allow names absent from the release exe (stale): {' '.join(stale) or '-'}")
    if dflt is not None:
        print(f'in neither exe (the scan cannot tell): {len(neither)}: {" ".join(neither) or "-"}')
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as fh:
            fh.write('name\tdefault\trelease\n')
            fh.writelines(f'{n}\t{d}\t{r}\n' for n, d, r in rows)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
