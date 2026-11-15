#!/usr/bin/env python3
"""suites_compare.py - compare two difftest suite runs' exit codes (the summary.tsv a run_suites PowerShell runner
writes: suite <TAB> exit code <TAB> seconds per line), e.g. a lane's run against the dev baseline.

    python tools/difftest/suites_compare.py <baseline summary.tsv> <current summary.tsv> [--out compare.json]
           [--extra native-residue-test[,coher-sentinel-test] ...] [--expect-suites 74]

Prints and writes {suites_*, exit0_*, differences, identical, nonzero}; `identical` = the same suites with the same exit
codes. Suites named with --extra (repeatable or comma-separated; the leading -- of a suite name is optional, since
argparse reads a bare --name as an option) are reported apart (a lane's new suite) and left out of the comparison. Exit 0 when
identical, 1 when they differ, 2 when either input is not evidence: missing / empty, a non-blank line that is not
`--suite <TAB> exit code <TAB> seconds` (an integer exit code or TIMEOUT), a suite named twice, or (--expect-suites) a
compared suite count other than the one stated. Nothing is written for an input error.
"""
import argparse
import json
import re
import sys

ROW = re.compile(r'^(--[a-z0-9][a-z0-9-]*)\t(-?\d+|TIMEOUT)\t\d+$')


class BadSummary(Exception):
    pass


def read(path):
    rows = {}
    try:
        with open(path, encoding='utf-8-sig') as f:
            lines = f.read().splitlines()
    except (OSError, UnicodeError) as e:  # invalid UTF-8 is unreadable input too (exit 2, not a traceback)
        raise BadSummary('%s: unreadable (%s)' % (path, e))
    for k, line in enumerate(lines, 1):
        if not line.strip():
            continue
        m = ROW.match(line)
        if not m:
            raise BadSummary('%s:%d: not a summary row: %r' % (path, k, line[:80]))
        if m.group(1) in rows:
            raise BadSummary('%s:%d: suite %s named twice' % (path, k, m.group(1)))
        rows[m.group(1)] = m.group(2)
    if not rows:
        raise BadSummary('%s: no suite rows (empty or not a summary)' % path)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('baseline')
    ap.add_argument('current')
    ap.add_argument('--out')
    ap.add_argument('--extra', action='append', default=[],
                    help='a suite to report apart: native-residue-test or --extra=--native-residue-test; repeatable, comma lists')
    ap.add_argument('--expect-suites', type=int, help='the compared suite count both inputs must have')
    a = ap.parse_args()
    try:
        base, cur = read(a.baseline), read(a.current)
        names = ['--' + n.strip().lstrip('-') for v in a.extra for n in v.split(',') if n.strip().lstrip('-')]
        extra = {s: cur.pop(s, None) for s in names}
        for s in names:
            base.pop(s, None)
        if a.expect_suites is not None:
            for name, rows in (('baseline', base), ('current', cur)):
                if len(rows) != a.expect_suites:
                    raise BadSummary('%s: %d compared suites, %d expected' % (name, len(rows), a.expect_suites))
    except BadSummary as e:
        print('suites_compare: input error: %s' % e, file=sys.stderr)
        return 2
    diffs = [{'suite': s, 'baseline': base.get(s), 'current': cur.get(s)}
             for s in sorted(set(base) | set(cur)) if base.get(s) != cur.get(s)]
    res = {'baseline': a.baseline, 'current': a.current, 'suites_baseline': len(base), 'suites_current': len(cur),
           'exit0_baseline': sum(1 for v in base.values() if v == '0'),
           'exit0_current': sum(1 for v in cur.values() if v == '0'), 'differences': diffs, 'identical': not diffs,
           'nonzero': {s: v for s, v in sorted(cur.items()) if v != '0'}, 'extra': extra}
    text = json.dumps(res, indent=1)
    print(text)
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')
    return 0 if not diffs else 1


if __name__ == '__main__':
    raise SystemExit(main())
