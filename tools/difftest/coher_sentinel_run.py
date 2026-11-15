#!/usr/bin/env python3
"""native/coher-sentinel offline proof: the native segmentless close's coherency range (dev+0x2D18) against the SDK.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name cs python tools/difftest/coher_sentinel_run.py --out archive/cs/coher_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode (records = the default path, pm4 = the records written as PM4 so an emulated
CP applies every request, segmentless-off = both arms the guest close); compiled faults in kernel/native_flush.cpp -
the comparison reverted to all ones, the restore reverted to all ones, and both (the exact pre-fix code) - each rebuilt,
required to fail its named assertions in the native modes without crashing, and to pass the segmentless-off control;
then the source restored byte for byte, rebuilt and the positive modes rerun.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[COHER_SENTINEL_TEST\] (\d+) passed, (\d+) failed$', re.M)

MODES = {
    'records': ('records', {}),
    'pm4': ('pm4', {'SR_NATIVE_FLUSH_PM4': '1'}),
    'segmentless_off': ('segmentless-off', {'SR_NATIVE_SEGMENTLESS': '0'}),
}

COMPARE = (b'const bool hasRange = be64(d + 0x2D18u) != kCoherEmpty;',
           b'const bool hasRange = be64(d + 0x2D18u) != ~0ull;')
# The restore value of coherencyBlock's swap. The CAS line itself also appears in BRACKET's end (82110C78's port, which
# restores the same SDK value), so the anchor is the value's declaration, which names coherencyBlock's kCoherEmpty:
# all ones is the pre-fix restore.
RESTORE = (b'const long long empty = static_cast<long long>(_byteswap_uint64(kCoherEmpty));',
           b'const long long empty = -1LL;')


def A(case, text):
    return f'{case}: {text}'


TAILS = 'tail allocations and ring write index equal the SDK\'s after every step'
COUNTERS = 'native close range blocks (coherRange) equal the SDK\'s, none reaching 4 GB (coherTop 0)'
RANGE = 'dev+0x2D18 equals the SDK\'s after every step'
KICK = 'every kick\'s coherency block (count, requests) equals the SDK\'s'
CP = 'the emulated CP\'s coherency counters equal the SDK\'s; top 0, page 0'

# name, replacements, {mode: assertions that must fail}
MUTATIONS = (
    ('compare_all_ones', (COMPARE,),
     {'records': {A('fresh-reset', TAILS), A('fresh-reset', COUNTERS)},
      'pm4': {A('fresh-reset', COUNTERS), A('fresh-reset', CP)}}),
    ('restore_all_ones', (RESTORE,),
     {'records': {A('non-owner-merge', RANGE), A('non-owner-merge', KICK)},
      'pm4': {A('non-owner-merge', RANGE), A('non-owner-merge', CP)}}),
    ('pre_fix', (COMPARE, RESTORE),
     {'records': {A('fresh-reset', COUNTERS), A('kick-after-reset', KICK), A('kick-after-reset', RANGE)},
      'pm4': {A('fresh-reset', CP), A('kick-after-reset', CP), A('cycle', CP)}}),
)
SDK_ARM = re.compile(r': SDK arm ')


def parse(raw):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    return good, bad, SUMMARY.findall('\n'.join(lines))


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
    # Every fault's anchor must occur exactly once before anything is built (a duplicate elsewhere in the file - the
    # CAS line BRACKET's end shares - would otherwise only surface after the positive matrix).
    for name, repls, _ in MUTATIONS:
        for before, _after in repls:
            if b'\r\n' in original:
                before = before.replace(b'\n', b'\r\n')
            if original.count(before) != 1:
                raise SystemExit(f'{name}: anchor occurs {original.count(before)} times in kernel/native_flush.cpp')
    tracked = ('kernel/native_flush.cpp', 'gfx/pm4_engine.cpp', 'gfx/pm4_engine.hpp', 'kernel/gpu_command_processor.cpp', 'kernel/gpu_submit.cpp',
               'tools/difftest/coher_sentinel_test.inc', 'tools/difftest/coher_sentinel_run.py',
               'tools/difftest/difftest_main.cpp')
    result = dict(scope='Offline CPU proof of the native close\'s coherency range sentinel (native/coher-sentinel): the '
                  'actual SDK 820FD8C0 / 820FD3B8 / 820F4280 against the native segmentless close. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[])

    def save():
        (out / 'coher_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE='SR_NATIVE_SEGMENTLESS', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1',
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
        arg, extra = MODES[mode]
        env = dict(base_env)
        env.update(extra)
        code, raw = run(tag, [root / 'build/difftest.exe', '--coher-sentinel-test', arg], env, 600)
        good, bad, summary = parse(raw)
        return dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad), good, bad, summary

    def positive(tag, mode):
        row, good, bad, summary = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good))
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    totals = {}
    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
            totals[mode] = result['modes'][mode]['passed']
            save()
        if not all(r['valid'] for r in result['modes'].values()):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        crlf = b'\r\n' in original
        for name, repls, required in MUTATIONS:
            changed = original
            for before, after in repls:
                if crlf:
                    before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
                if changed.count(before) != 1:
                    raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
                changed = changed.replace(before, after)
            if source.read_bytes() != original:
                raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, required={m: sorted(v) for m, v in required.items()}, mutated_sha256=sha(changed),
                        tests={})
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                ok = True
                for mode, req in required.items():
                    trow, good, bad, summary = test(f'{name}_{mode}', mode)
                    missing = sorted(req - set(bad))
                    trow['missing_required'] = missing
                    trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                     and len(good) + len(bad) == totals[mode] and not missing
                                     and not [b for b in bad if SDK_ARM.search(b)])
                    mrow['tests'][mode] = trow
                    ok = ok and trow['valid']
                mrow['control'] = positive(f'{name}_segmentless_off', 'segmentless_off')
                mrow['detected'] = bool(ok and mrow['control']['valid'])
                print(json.dumps({'mutation': name, 'failed': {m: t['failed'] for m, t in mrow['tests'].items()},
                                  'missing': {m: t['missing_required'] for m, t in mrow['tests'].items()},
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
                result['restored'] = {m: positive('restored_' + m, m) for m in MODES}
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and len(result['mutations']) == len(MUTATIONS)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and result.get('restored') and all(r['valid'] for r in result['restored'].values()))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
