#!/usr/bin/env python3
"""The native producers' admission on the native resource route, offline (lane I integrate, 2026-10-17;
docs/RELEASE_COMPOSITION.md 9.6 "the native producers' admission").

Since the mem.resources bake (native/mem-native a1fda568) the development build's default is the native resource route, where
the GPU write tracker is off (kernel/gpu_write_watch.cpp enabled(): false under memres::resourceMode() outside the
frozen-pass A/B's dual mode) and no page is ever tracker-owned - only the page model (SR_MEM_RESOURCES=0, the oracle arm)
calls protectForValidation. The admission of the native producers (gateWritable / mappedRange / writable:
writableRange || gww::cpuWritableRange) is then writableRange alone. The three suites with tracker-owned cases carry a
native statement for that route (the bracket's tracked-lists-native, swap tracker mode's "native route:" cases, the
pair's tracked ranges): the tracker off, a page protected through the tracker's API not admitted (the end declines once
at a list's site / the leaf declines with cursorWrite before any store), ordinary writable pages admitted, an unowned
read-only page declined. Their tracker cases run pinned to SR_MEM_RESOURCES=0.

Run on the cloud worker (never on the desktop), from the synced worktree root, after building the difftest target:
    python tools/cloud/sr_cloud.py run --worktree . --name <tree> -- "python tools/difftest/native_route_admission_run.py --out archive/<bank>"
Positive: each suite on the native default (`_native`) and at SR_MEM_RESOURCES=0, with composed_suites_run.py's
reference environments - every row exit 0, N passed, 0 failed. Mutant `tracker_on_native`: enabled() keeps the tracker
on the native route (the resourceMode clause dropped); each native row must fail its named native-route assertions (and
only native-route ones), each =0 row must stay whole; then the source is restored byte for byte, rebuilt and the
positives rerun. CPU only: no game, live, pacing or release acceptance. Exit 0 = valid.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import composed_suites_run as C  # noqa: E402  (the suites' reference environments, one source)

SUMMARY = re.compile(r'^\[([A-Z_]+_TEST)\] (\d+) passed, (\d+) failed', re.M)
MR0 = dict(SR_MEM_RESOURCES='0')
OFF = 'native route: the GPU write tracker is off (gww::enabled() false)'
# tag -> (composed_suites_run row, extra env, assertions the mutant must fail, the prefix every mutant failure carries)
ROWS = {
    'bracket_native': ('bracket_native', {}, {'tracked-lists-native: ' + OFF}, ('tracked-lists-native: ',)),
    'swap_tracker_native': ('swap_sdk_tracker_native', {}, {OFF}, ('native route: ',)),
    'pair_native': ('transport_pair_native', {}, {OFF}, ('native route: ',)),
    'bracket_mr0': ('bracket_native', MR0, None, ()),
    'swap_tracker_mr0': ('swap_sdk_tracker_native', MR0, None, ()),
    'pair_mr0': ('transport_pair_native', MR0, None, ()),
}
SOURCE = 'kernel/gpu_write_watch.cpp'
ANCHOR = b'  if (sr::gfx::memres::resourceMode() && !sr::gfx::memres::resourceAbDual()) return false;\n'
FAULT = b'  // MUTANT tracker_on_native: the tracker answers on the native route too\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def parse(raw):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [ln[6:] for ln in lines if ln.startswith('  ok  ')]
    bad = [ln[6:] for ln in lines if ln.startswith('FAIL: ')]
    return good, bad, SUMMARY.findall('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--no-mutations', action='store_true')
    a = ap.parse_args()
    root = HERE.parents[1]
    out = a.out if a.out.is_absolute() else (root / a.out)
    out.mkdir(parents=True, exist_ok=False)
    src = root / SOURCE
    original = src.read_bytes()
    crlf = b'\r\n' in original
    anchor, fault = (ANCHOR.replace(b'\n', b'\r\n'), FAULT.replace(b'\n', b'\r\n')) if crlf else (ANCHOR, FAULT)
    if original.count(anchor) != 1:
        raise SystemExit('production anchor changed in %s; refusing' % SOURCE)
    clean = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    clean['PATH'] = r'C:\Program Files\LLVM\bin;' + clean.get('PATH', '')
    res = dict(scope=__doc__.split('\n')[0], source_sha256=sha(original), rows={}, mutation=None)

    def save():
        (out / 'native_route_admission.json').write_text(json.dumps(res, indent=2) + '\n')

    def run(tag, cmd, env, timeout):
        try:
            p = subprocess.run([str(c) for c in cmd], cwd=root, env=env, capture_output=True, timeout=timeout)
            code, so, se = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            code, so, se = 124, e.stdout or b'', e.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(so)
        (out / (tag + '_stderr.txt')).write_bytes(se)
        return code, so

    def build(tag):
        for attempt in range(3):  # the shared worker's sccache drops a connection now and then
            code, raw = run(tag if not attempt else '%s_retry%d' % (tag, attempt),
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(a.jobs)], clean, 1800)
            if code == 0 or b'sccache: error' not in raw:
                break
        print(json.dumps({'build': tag, 'exit': code}), flush=True)
        if code:
            raise RuntimeError('%s failed (see %s_stdout.txt)' % (tag, tag))

    def test(tag, name):
        row, extra, _, _ = ROWS[name]
        argv, ref = C.SUITES[row][0], C.SUITES[row][1]
        env = dict(clean)
        env.update(ref)
        env.update(extra)
        code, raw = run(tag, [root / 'build/difftest.exe'] + argv, env, 900)
        good, bad, summary = parse(raw)
        return dict(exit=code, passed=len(good), failed=len(bad), summary=summary, failures=bad[:40]), good, bad

    def positive(tag, name):
        r, good, bad = test(tag, name)
        r['valid'] = bool(r['exit'] == 0 and good and not bad and r['summary'] and
                          all(int(f) == 0 for _, _, f in r['summary']))
        print(json.dumps({'test': tag, 'summary': r['summary'], 'valid': r['valid']}), flush=True)
        return r

    try:
        build('positive_build')
        res['rows'] = {n: positive('positive_' + n, n) for n in ROWS}
        save()
        if a.no_mutations or not all(r['valid'] for r in res['rows'].values()):
            res['valid'] = all(r['valid'] for r in res['rows'].values()) and a.no_mutations
            return 0 if res['valid'] else 1
        src.write_bytes(original.replace(anchor, fault))
        m = res['mutation'] = dict(name='tracker_on_native', mutated_sha256=sha(src.read_bytes()), rows={})
        try:
            build('tracker_on_native_build')
            detected = True
            for n, (_, _, must, prefixes) in ROWS.items():
                r, good, bad = test('tracker_on_native_' + n, n)
                if must is None:  # the =0 rows: the page model is untouched by the mutant
                    r['valid'] = bool(r['exit'] == 0 and not bad and good)
                else:
                    missing = sorted(x for x in must if x not in bad)
                    stray = sorted(b for b in bad if not b.startswith(prefixes))
                    r.update(missing_required=missing, stray_failures=stray[:20])
                    r['valid'] = bool(r['exit'] != 0 and bad and not missing and not stray)
                detected &= r['valid']
                m['rows'][n] = r
                print(json.dumps({'mutation': m['name'], 'test': n, 'failed': r['failed'],
                                  'missing': r.get('missing_required'), 'stray': r.get('stray_failures', [])[:3],
                                  'valid': r['valid']}), flush=True)
            m['detected'] = bool(detected)
        finally:
            src.write_bytes(original)
            m['source_restored'] = src.read_bytes() == original
            save()
        build('restored_build')
        res['restored'] = {n: positive('restored_' + n, n) for n in ROWS}
        res['valid'] = bool(all(r['valid'] for r in res['rows'].values()) and m.get('detected') and
                            m.get('source_restored') and all(r['valid'] for r in res['restored'].values()))
        return 0 if res['valid'] else 1
    finally:
        if src.read_bytes() != original:
            src.write_bytes(original)
        res.setdefault('valid', False)
        save()
        print(json.dumps({'valid': res['valid']}), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
