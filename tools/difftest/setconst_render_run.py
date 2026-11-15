#!/usr/bin/env python3
"""P2-B item 5 offline proof: the kRkSetConstMem record against the SDK, and the CP's render-back of it under the
CPPM4 split (Astra round 1 on native/setconst-bake). Suite: difftest --setconst-render-test (setconst_render_test.inc).

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name scb python tools/difftest/setconst_render_run.py --out archive/scb/setconst_render
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode - records (the record vs the SDK's packet, applied through nativeRecord; the
render-back counter must not move), cppm4 (SR_NATIVE_FLUSH_CPPM4=1: the CP renders the record to PM4 itself and
executes it; the counter counts it), pm4 (SR_NATIVE_FLUSH_PM4=1: the producer-side literal span equals the SDK's
bytes) - and, across the two processes, the CP's PM4 (cppm4) equal to the producer's (pm4) for every case, both with
the pad NOPs stripped. Then compiled faults in kernel/native_flush.cpp's cpRecordPm4 - no CP case for kRkSetConstMem
(the opaque fallback runs the record through nativeRecord again) and a wrong render (the body's length taken as the
header's count in bytes, not dwords) - each rebuilt and required to fail its named assertions in cppm4 without
crashing, never the SDK arm's, while records and pm4 (which do not reach the CP's case) still pass whole; then the
source restored byte for byte, rebuilt, and the positive modes rerun.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[SETCONST_RENDER_TEST\] (\d+) passed, (\d+) failed$', re.M)
BYTES = re.compile(r'^\[SETCONST_RENDER\] (\S+) (sdk|cp|producer)=([0-9A-F]+)$', re.M)
MODES = {
    'records': {},
    'cppm4': {'SR_NATIVE_FLUSH_CPPM4': '1'},
    'pm4': {'SR_NATIVE_FLUSH_PM4': '1'},
}
CASES = ('vs-c100-x4-pad0', 'ps-c100-x4-pad4-pred', 'vs-c96-x1-pad8', 'ps-c101-x2-pad12-pred')
RENDERS = 'the CP renders the record to PM4 itself (one setconst, no opaque fallback to nativeRecord)'
EQUALS = "the CP's PM4 equals the SDK's bytes without the pad NOPs"
COUNTS = 'cpPm4 counts the record the CP rendered back and executed'

NO_CASE = (b'        case kRkSetConstMem: {  // P2-B item 5: {header, offset, bodyVA}; the header\'s count = the body\'s dwords',
           b'        case kRkKinds + 1u: {  // MUTANT: the CP has no kRkSetConstMem case (the opaque fallback runs it)')
WRONG_LEN = (b'          const uint32_t nb = 4u * ((p[0] >> 16) & 0x3FFFu);',
             b'          const uint32_t nb = (p[0] >> 16) & 0x3FFFu;  // MUTANT: the count taken as bytes')
# name, replacement, the cppm4 assertions that must fail (the records / pm4 controls must pass whole)
MUTATIONS = (
    ('no_cp_case', NO_CASE, {f'{c}: {a}' for c in CASES for a in (RENDERS, EQUALS, COUNTS)}),
    ('wrong_render', WRONG_LEN, {f'{c}: {EQUALS}' for c in CASES}),
)
SDK_ARM = re.compile(r': SDK arm: ')


def parse(raw):
    text = raw.decode(errors='replace').replace('\r\n', '\n')
    lines = text.splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    data = {}
    for case, kind, value in BYTES.findall(text):
        data.setdefault(case, {})[kind] = value
    return good, bad, SUMMARY.findall(text), data


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original
    for name, (before, _after), _req in MUTATIONS:  # every anchor exactly once before anything is built
        if original.count(before) != 1:
            raise SystemExit(f'{name}: anchor occurs {original.count(before)} times in kernel/native_flush.cpp')
    tracked = ('kernel/native_flush.cpp', 'kernel/native_record_format.hpp', 'gfx/pm4_engine.cpp', 'gfx/pm4_engine.hpp',
               'tools/difftest/setconst_render_test.inc', 'tools/difftest/setconst_render_run.py',
               'tools/difftest/difftest_main.cpp', 'tools/difftest/suite_bare.inc')
    result = dict(scope='Offline CPU proof of P2-B item 5: 82113010 through production dispatch, the SDK body against the '
                  'native Entry (default ON, SR_NATIVE_SETCONSTMEM unset), the record applied by the CP through '
                  'nativeRecord and through the CPPM4 render-back, the producer-side literal split. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, cross={}, mutations=[])

    def save():
        (out / 'setconst_render_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE='SR_NATIVE_SETCONSTMEM', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1',
                    SR_NATIVE_FLUSH='1')

    def run(tag, command, env, timeout):
        try:
            p = subprocess.run([str(c) for c in command], cwd=root, env=env, capture_output=True, timeout=timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        return code, stdout

    def build(tag):
        # The shared worker's sccache server sometimes drops a connection (os error 10054): retried twice, recorded.
        for attempt in range(3):
            code, raw = run(tag if not attempt else f'{tag}_retry{attempt}',
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], base_env, 1800)
            if code == 0 or b'sccache: error' not in raw:
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag, mode):
        env = dict(base_env)
        env.update(MODES[mode])
        code, raw = run(tag, [root / 'build/difftest.exe', '--setconst-render-test', mode], env, 600)
        good, bad, summary, data = parse(raw)
        return dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad), good, bad, summary, data

    def positive(tag, mode, datas=None):
        row, good, bad, summary, data = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good) and set(data) == set(CASES))
        if datas is not None:
            datas[mode] = data
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    def cross(datas):
        """The CP's PM4 (cppm4) equals the producer's literal split (pm4) for every case; both match the same SDK."""
        rows = {}
        for case in CASES:
            cp = datas.get('cppm4', {}).get(case, {})
            pr = datas.get('pm4', {}).get(case, {})
            rows[case] = dict(cp=cp.get('cp'), producer=pr.get('producer'),
                              equal=bool(cp.get('cp')) and cp.get('cp') == pr.get('producer'),
                              same_sdk=bool(cp.get('sdk')) and cp.get('sdk') == pr.get('sdk'))
        return dict(cases=rows, valid=all(r['equal'] and r['same_sdk'] for r in rows.values()))

    totals = {}
    try:
        result['positive_build'] = build('positive_build')
        datas = {}
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode, datas)
            totals[mode] = result['modes'][mode]['passed']
            save()
        result['cross'] = cross(datas)
        print(json.dumps({'cross': result['cross']['valid']}), flush=True)
        save()
        if not (all(r['valid'] for r in result['modes'].values()) and result['cross']['valid']):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, (before, after), required in MUTATIONS:
            if crlf:
                before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
            changed = original.replace(before, after)
            if source.read_bytes() != original:
                raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, required=sorted(required), mutated_sha256=sha(changed), tests={})
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                trow, good, bad, summary, _data = test(f'{name}_cppm4', 'cppm4')
                missing = sorted(required - set(bad))
                trow['missing_required'] = missing
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                 and len(good) + len(bad) == totals['cppm4'] and not missing
                                 and not [b for b in bad if SDK_ARM.search(b)])
                mrow['tests']['cppm4'] = trow
                mrow['controls'] = {m: positive(f'{name}_{m}', m) for m in ('records', 'pm4')}
                mrow['detected'] = bool(trow['valid'] and all(r['valid'] for r in mrow['controls'].values()))
                print(json.dumps({'mutation': name, 'failed': trow['failed'], 'missing': missing,
                                  'detected': mrow['detected']}), flush=True)
            finally:
                source.write_bytes(original)
                mrow['source_restored'] = source.read_bytes() == original
                save()
    finally:
        result['valid'] = False
        if source.read_bytes() != original:
            source.write_bytes(original)
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                rdatas = {}
                result['restored'] = {m: positive('restored_' + m, m, rdatas) for m in MODES}
                result['restored_cross'] = cross(rdatas)
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and result['cross'].get('valid') and len(result['mutations']) == len(MUTATIONS)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and result.get('restored') and all(r['valid'] for r in result['restored'].values())
                and result.get('restored_cross', {}).get('valid'))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
