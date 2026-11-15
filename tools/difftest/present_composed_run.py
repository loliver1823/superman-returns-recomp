#!/usr/bin/env python3
"""p2.transport COMPOSED present offline proof (transport-composed lane, 2026-10-14): InsertFence 820FDD30 + the vblank
gate 82111EC8 + Swap 82112050 (its 820F0048 re-emit included) through production dispatch with the transport kinds
composed, against the all-SDK oracle; with compiled faults.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --name tcmp python tools/difftest/present_composed_run.py --out archive/tcmp/present_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode - all OFF (the production route must equal the SDK byte for byte), each kind
alone (GATE, SWAP_BLOCK + SWAP, DEVCREATE, CALLBACK), GATE + SWAP, GATE + SWAP + DEVCREATE, the composed env (every
transport kind + DEVCREATE, SHADER_CONST_RUNS off: the orchestrator's 2026-10-14 decision) by the strong symbols and
by the production dispatch table, composed under CPPM4 and under the PM4 output, composed without GATE (CALLBACK takes
the gate's callback block), composed with segmentless off (every leaf declines), composed and all OFF with the baked
rows 5 / 3b pinned off; then each compiled fault (kernel/native_flush.cpp only) rebuilt and required to fail its named
assertions in its mode, never crash, and pass its control mode (all OFF; for the rows 5 / 3b faults, whose leaves run in
all OFF too, all OFF with rows 5 / 3b pinned off - the fault inert there); the source restored byte for byte, rebuilt
and the positive modes rerun. No live, pacing or release acceptance is claimed by this matrix.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

SUMMARY = re.compile(r'^\[PRESENT_COMPOSED_TEST\] (\d+) passed, (\d+) failed', re.M)
KINDS = ('SR_NATIVE_CALLBACK', 'SR_NATIVE_GATE', 'SR_NATIVE_SWAP_BLOCK', 'SR_NATIVE_SWAP', 'SR_NATIVE_BRACKET',
         'SR_NATIVE_IDLE_WAIT', 'SR_NATIVE_INVAL_RANGE', 'SR_NATIVE_LOCK_COHER', 'SR_NATIVE_TRANSPORT',
         'SR_NATIVE_DEVCREATE')
OFF = {k: '0' for k in KINDS}
COMPOSED = {k: '1' for k in KINDS}
ROWS53B_OFF = {'SR_NATIVE_TILERECTS': '0', 'SR_NATIVE_TILEPRED': '0'}  # the baked rows 5 / 3b (not transport kinds)


def only(*on):
    env = dict(OFF)
    env.update({k: '1' for k in on})
    return env


# mode tag -> (route argument, environment overrides on top of the base env)
MODES = {
    'all_off': ('direct', OFF),
    'gate': ('direct', only('SR_NATIVE_GATE')),
    'swap': ('direct', only('SR_NATIVE_SWAP_BLOCK', 'SR_NATIVE_SWAP')),
    'devcreate': ('direct', only('SR_NATIVE_DEVCREATE')),
    'callback': ('direct', only('SR_NATIVE_CALLBACK')),
    'gate_swap': ('direct', only('SR_NATIVE_GATE', 'SR_NATIVE_SWAP_BLOCK', 'SR_NATIVE_SWAP')),
    'gate_swap_devcreate': ('direct', only('SR_NATIVE_GATE', 'SR_NATIVE_SWAP_BLOCK', 'SR_NATIVE_SWAP',
                                           'SR_NATIVE_DEVCREATE')),
    'composed': ('direct', COMPOSED),
    'composed_indirect': ('indirect', COMPOSED),
    'composed_cppm4': ('direct', dict(COMPOSED, SR_NATIVE_FLUSH_CPPM4='1')),
    'composed_pm4out': ('direct', dict(COMPOSED, SR_NATIVE_FLUSH_PM4='1', SR_NATIVE_CONST_DEDUP_CHECK='1')),
    'composed_no_gate': ('direct', dict(COMPOSED, SR_NATIVE_GATE='0')),
    # off segmentless every transport leaf declines; rows 5 / 3b would write ring-path records this pin harness does
    # not model, so they are held off there
    'composed_segmentless_off': ('direct', dict(COMPOSED, SR_NATIVE_SEGMENTLESS='0', **ROWS53B_OFF)),
    # the earlier configuration (rows 5 / 3b pinned off): the composition holds either way
    'composed_rows53b_off': ('direct', dict(COMPOSED, **ROWS53B_OFF)),
    'all_off_rows53b_off': ('direct', dict(OFF, **ROWS53B_OFF)),
}

ONCE = 'each producer call converted exactly once'
ORACLE = 'the scoped SDK oracle runs every body the SDK\'s (no leaf took a call)'
APPLY = 'the CP\'s effects equal the SDK\'s, twice'
DEVICE = 'device, list, ABI and return equal the SDK\'s'
SCOPE = 'ShaderConstRunsOracle: the gate, its callback and the re-emit decline inside the scope'
ROWS_SCOPE = {'PresentGateOracle: rows 5 / 3b (820F0208, 8210D400) decline inside the scope',
              'ShaderConstRunsOracle: rows 5 / 3b (820F0208, 8210D400) decline inside the scope'}


def named(case, text, offsets=(0, 4)):
    """Assertion-name prefixes (the test appends counts / suffixes): matched with startswith."""
    return {f'{case} offset={o}: {text}' for o in offsets}


# name, anchor, replacement, the mode it runs in, assertion-name prefixes that must fail.
MUTATIONS = (
    # the defect this lane fixed: devcreate ignored the scoped SDK oracles, so the composed oracle's Swap ran a native
    # 820F0048 inside a guest-closed segment
    ('devcreate_oracle',
     b'  return on && g_on && segmentlessActive() && !g_presentGateOracleDepth && !g_shaderConstRunsOracleDepth &&\n'
     b'         (s_ab < 0 || sr::abt::on(s_ab));',
     b'  return on && g_on && segmentlessActive() &&\n         (s_ab < 0 || sr::abt::on(s_ab));',
     'composed', named('plain', ORACLE) | named('scaler-gamma', ORACLE)),
    # SWAP refusing a device the gate just flagged (0x2A39 bit 0x20): GATE and SWAP would not compose
    ('swap_after_gate', b'(snapshot[0x2A39u] & 0x52u)', b'(snapshot[0x2A39u] & 0x72u)', 'gate_swap',
     named('plain', ONCE) | named('immediate', ONCE)),
    # a gate that records its block and then reports a decline: the override runs the SDK body too (double
    # application: two callbacks, the CALLBACK leaf converting the second)
    ('gate_double', b'  g_gateCalls.fetch_add(1, std::memory_order_relaxed);\n  return true;',
     b'  g_gateCalls.fetch_add(1, std::memory_order_relaxed);\n  return false;', 'composed',
     named('plain', ONCE) | named('plain', APPLY)),
    # Swap's 820F0048 re-emit routed around DEVCREATE: the composed transport would leave its packets as PM4
    ('reemit_bypass', b'  call(sub_820F0048);', b'  { ShaderConstRunsOracle bypass; call(sub_820F0048); }',
     'composed', named('plain', ONCE)),
    # the gate and the callback opening an Entry inside ShaderConstRunsOracle (whose Entry owns the arena spinlock)
    ('gate_oracle_scope',
     b'g_presentGateOracleDepth || g_shaderConstRunsOracleDepth)\n    return false;',
     b'g_presentGateOracleDepth)\n    return false;', 'composed', {SCOPE}),
    ('callback_oracle_scope',
     b'  if (g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;\n  if (!g_on || !segmentlessActive())',
     b'  if (g_presentGateOracleDepth) return false;\n  if (!g_on || !segmentlessActive())', 'composed_no_gate', {SCOPE}),
    # the baked rows 5 / 3b ignoring the scoped SDK oracles (the state before the 2026-10-14 decision): row 5 also
    # turns the oracle's own Swap re-emit native, row 3b is reached only by the scope-contract calls
    ('tilerects_oracle_scope',
     b'if (!tileRectsOn() || !g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;',
     b'if (!tileRectsOn() || !g_on) return false;', 'all_off',
     ROWS_SCOPE | named('plain', ORACLE) | named('scaler-gamma', ORACLE), 'all_off_rows53b_off'),
    ('tilepred_oracle_scope',
     b'if (!tilePredOn() || !g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;',
     b'if (!tilePredOn() || !g_on) return false;', 'all_off', ROWS_SCOPE, 'all_off_rows53b_off'),
)


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
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    parser.add_argument('--no-mutations', action='store_true', help='the positive modes only')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original
    tracked = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/stubs.cpp', 'kernel/hle_api_observe.cpp',
               'kernel/alloc_overlap_watch.cpp', 'tools/difftest/present_composed_test.cpp',
               'tools/difftest/present_composed_run.py', 'tools/difftest/difftest_main.cpp')
    result = dict(scope='Offline CPU proof of the composed present: InsertFence + the vblank gate + Swap (its 820F0048 '
                  're-emit) with the transport kinds composed, against the scoped all-SDK oracle; recording-pin '
                  'segments consumed by an in-order emulated CP (mock vblank / callback completion). No live, pacing '
                  'or release acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[])
    names = {x for x in args.only.split(',') if x}
    selected = [] if args.no_mutations else [m for m in MUTATIONS if not names or m[0] in names]

    def save():
        (out / 'present_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    # swap_sdk_test_run.py's base (the async CP the VdSwap export and swapLeaf require) plus the gate runner's (no dry
    # CHECK, no stress, no A/B). Rows 5 / 3b stay at their baked default (ON): since the orchestrator's 2026-10-14
    # decision they decline inside both scoped SDK oracles, so the oracle's 820F0048 -> 820F0208 stays the SDK's while
    # the production route takes them (the earlier pins are the composed_rows53b_off mode)
    base_env.update(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0',
                    SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_CONST_DEDUP='0', SR_NATIVE_CONST_DEDUP_CHECK='0',
                    SR_NATIVE_SHADER_CONST_RUNS='0',
                    SR_NATIVE_EXEC='0', SR_NATIVE_SEG_STRESS='0', SR_NATIVE_REPLAY_CALLBACK='0', SR_AB_TOGGLE='',
                    SR_PM4_CP='1', SR_PM4_LIVE='0', SR_ALLOW_FORCING='0', SR_MODS='0', SR_DEVTOOLS='0',
                    SR_HIGHFPS='0', SR_API_OBSERVE='0', SR_API_PRESENT_BLOCK='0', SR_HDR_ADAPT_DT='0', SR_LOG='dev')

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
        for attempt in range(3):  # the shared worker's sccache drops a connection now and then (os error 10054)
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
        code, raw = run(tag, [root / 'build/difftest.exe', '--present-composed-test', arg], env, 600)
        good, bad, summary = parse(raw)
        row = dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                   failures=bad)
        return row, good, bad, summary

    def positive(tag, mode):
        row, good, bad, summary = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')])
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
            save()
        if not all(r['valid'] for r in result['modes'].values()):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, before, after, mode, required, *rest in selected:
            control = rest[0] if rest else 'all_off'  # the mode the fault must leave valid
            if crlf:
                before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
            if source.read_bytes() != original:
                raise RuntimeError('concurrent source edit; refusing mutation')
            changed = original.replace(before, after)
            mrow = dict(name=name, mode=mode, control=control, required=sorted(required), mutated_sha256=sha(changed))
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                trow, good, bad, summary = test(name + '_test', mode)
                missing = sorted(r for r in required if not any(b.startswith(r) for b in bad))
                trow['missing_required'] = missing
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))] and bad and
                                 len(good) + len(bad) == result['modes'][mode]['passed'] and not missing)
                mrow['test'] = trow
                grow = positive(name + '_' + control, control)
                mrow['all_off'] = grow  # (the control row, named by `control`)
                mrow['detected'] = bool(trow['valid'] and grow['valid'])
                print(json.dumps({'mutation': name, 'failed': len(bad), 'missing': missing,
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
                result['restored'] = {m: positive('restored_' + m, m) for m in ('all_off', 'composed', 'gate_swap')}
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and len(result['mutations']) == len(selected)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and (not selected or (result.get('restored') and all(r['valid'] for r in result['restored'].values()))))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
