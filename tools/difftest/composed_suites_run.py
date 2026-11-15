#!/usr/bin/env python3
"""p2.transport COMPOSITION offline proof (transport-composed lane, 2026-10-14): every difftest suite that reaches a
transport kind, run in its own configured reference mode AND with every transport kind composed.

Run on the cloud worker (never on the desktop), from the synced worktree root, after building the difftest target:
    python tools/cloud/sr_cloud.py run --worktree . --name tcmp -- "python tools/difftest/composed_suites_run.py --out archive/tcmp/suites"
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. It builds nothing.

THE COMPOSED ENV (the orchestrator's decision, 2026-10-14): CALLBACK, GATE, SWAP_BLOCK + SWAP, BRACKET, IDLE_WAIT /
INVAL_RANGE / LOCK_COHER, the transport pair and DEVCREATE all 1; SHADER_CONST_RUNS 0 (p1.row3_construns'). Each suite
keeps its runner's own base env - in particular its SR_AB_TOGGLE marker: five suites (bracket, residue, census,
coher-sentinel, setconst-render) re-launch themselves with every caller SR_* stripped unless SR_AB_TOGGLE equals their
toggle (tools/difftest/suite_bare.inc), which would silently drop the composed gates. A suite whose runner fixes
SEGMENTLESS / FLUSH off (the pair, the pump driver, the recording controller) keeps that: its fixture is built on the
guest close, so the segmentless leaves stay inert there - recorded per suite as `inert`.

A row is valid when the process exits 0 and every summary line reads "N passed, 0 failed" with N > 0. The one
exception is named per row (`allow`, with its reason): a case written for an SDK path a composed kind removes (the
census suite's SDK Z-pass kick cases under BRACKET) may fail there, only those, and a companion row holding that one
gate at the suite's pin (`pins`) must pass whole. The reference and composed counts are both recorded; a composed count
below the reference is reported (a fixture branch that stopped running), never silently accepted. A `reference_only`
row (swap_sdk_bare, 2026-10-14) runs a suite exactly as the bare suite lists do - no SR_ variable - and has no composed
arm. Exit 0 = every row valid. Read-only apart from --out.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

SUMMARY = re.compile(rb'\[([A-Z_]+_TEST)\] (\d+) passed, (\d+) failed')
KINDS = ('SR_NATIVE_CALLBACK', 'SR_NATIVE_GATE', 'SR_NATIVE_SWAP_BLOCK', 'SR_NATIVE_SWAP', 'SR_NATIVE_BRACKET',
         'SR_NATIVE_IDLE_WAIT', 'SR_NATIVE_INVAL_RANGE', 'SR_NATIVE_LOCK_COHER', 'SR_NATIVE_TRANSPORT',
         'SR_NATIVE_DEVCREATE')
COMPOSED = dict({k: '1' for k in KINDS}, SR_NATIVE_SHADER_CONST_RUNS='0')
RESIDUE = 'SR_NATIVE_IDLE_WAIT+SR_NATIVE_INVAL_RANGE+SR_NATIVE_LOCK_COHER'

# Each runner's base env (copied from the runner named; the composed row adds COMPOSED on top).
BRACKET = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
               SR_AB_TOGGLE='SR_NATIVE_BRACKET', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1',
               SR_NATIVE_BRACKET='1', SR_NATIVE_TRANSPORT='0')                       # native_bracket_run.py
RESIDUE_BASE = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE=RESIDUE, SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1',
                    SR_NATIVE_IDLE_WAIT='1', SR_NATIVE_INVAL_RANGE='1', SR_NATIVE_LOCK_COHER='1',
                    SR_NATIVE_BRACKET='0', SR_NATIVE_TRANSPORT='0')                  # native_residue_run.py
CENSUS = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0', SR_NATIVE_SEGMENTLESS='1',
              SR_NATIVE_FLUSH='1', SR_NATIVE_IDLE_WAIT='1', SR_NATIVE_INVAL_RANGE='1', SR_NATIVE_LOCK_COHER='1',
              SR_NATIVE_BRACKET='0', SR_NATIVE_TRANSPORT='0', SR_NATIVE_BRACKETS='1',
              SR_AB_TOGGLE='SR_NATIVE_BRACKETS')                                     # native_census_run.py 'residue'
COHER = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
             SR_AB_TOGGLE='SR_NATIVE_SEGMENTLESS', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1',
             SR_NATIVE_FLUSH='1')                                                    # coher_sentinel_run.py
SETCONST = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                SR_AB_TOGGLE='SR_NATIVE_SETCONSTMEM', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1',
                SR_NATIVE_FLUSH='1')                                                 # setconst_render_run.py
GATE = dict(SR_NATIVE_FLUSH='1', SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_SEG_STRESS='0',
            SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_CONST_DEDUP='0',
            SR_NATIVE_CONST_DEDUP_CHECK='0', SR_AB_TOGGLE='', SR_NATIVE_SHADER_CONST_RUNS='0', SR_NATIVE_GATE='1',
            SR_NATIVE_CALLBACK='1', SR_NATIVE_DEVCREATE='0', SR_ALLOW_FORCING='0', SR_HDR_ADAPT_DT='0',
            SR_LOG='dev')                                                            # present_gate_sdk_test_run.py
SWAP = dict(SR_NATIVE_SWAP='1', SR_NATIVE_SWAP_BLOCK='1', SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1',
            SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_CONST_DEDUP='0',
            SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0',
            SR_NATIVE_SHADER_CONST_RUNS='0', SR_NATIVE_DEVCREATE='0', SR_NATIVE_EXEC='0', SR_PM4_CP='1',
            SR_PM4_LIVE='0', SR_ALLOW_FORCING='0', SR_MODS='0', SR_DEVTOOLS='0', SR_HIGHFPS='0',
            SR_API_OBSERVE='0', SR_API_PRESENT_BLOCK='0', SR_LOG='dev')              # swap_sdk_test_run.py
# (no rows 5 / 3b pins, as the swap runner since 2026-10-14: SWAP on, the oracle is scoped, where those baked leaves
# decline; SWAP off, the oracle is the unscoped production route, where they take Swap's re-emit as the route does.)
BLOCK = dict(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_SWAP_BLOCK='1', SR_NATIVE_FLUSH_PM4='0',
             SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_CONST_DEDUP='0', SR_NATIVE_SEG_STRESS='0',
             SR_NATIVE_EXEC='0', SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0', SR_NATIVE_SHADER_CONST_RUNS='0',
             SR_NATIVE_DEVCREATE='0', SR_PM4_CP='1', SR_PM4_LIVE='0', SR_ALLOW_FORCING='0', SR_MODS='0',
             SR_DEVTOOLS='0', SR_HIGHFPS='0', SR_LOG='dev')                          # swap_block_test_run.py
PAIR = dict(SR_NATIVE_RINGLESS='0', SR_NATIVE_SEGMENTLESS='0', SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0',
            SR_ALLOW_FORCING='0', SR_INSTANCE_ID='native-transport-host', SR_NATIVE_TRANSPORT='1')  # native_transport
DRIVER = dict(SR_NATIVE_REPLAY_DRIVER='1', SR_NATIVE_REPLAY_MARKERS='1', SR_NATIVE_TRANSPORT='1',
              SR_NATIVE_RINGLESS='0', SR_NATIVE_SEGMENTLESS='0', SR_NATIVE_FLUSH='0', SR_PM4_RING='0',
              SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
              SR_INSTANCE_ID='native-driver-host')                                  # native_pump_driver_run.py integrated
RECORDING = dict(SR_NATIVE_FLUSH='0', SR_NATIVE_SEGMENTLESS='0', SR_NATIVE_RINGLESS='0', SR_PM4_RING='0',
                 SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0', SR_NATIVE_TRANSPORT='0',
                 SR_NATIVE_REPLAY_KICK='1')                                         # native_recording_run.py 'positive'
# present_composed_run.py's base (no rows 5 / 3b pins: its oracle is scoped, the route takes them).
PRESENT = dict(SWAP, SR_NATIVE_CONST_DEDUP_CHECK='0', SR_NATIVE_SEG_STRESS='0', SR_NATIVE_REPLAY_CALLBACK='0',
               SR_AB_TOGGLE='', SR_HDR_ADAPT_DT='0')
EXECUTOR = dict(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_PM4='0',
                SR_NATIVE_EXEC='0', SR_NATIVE_TRANSPORT='0', SR_PM4_CP='0', SR_PM4_RING_EXEC='0',
                SR_ALLOW_FORCING='0')                                                # native_record_executor_run.py

# the page model (the write tracker's route): the tracker-owned cases' pin since the mem.resources bake
MR0 = dict(SR_MEM_RESOURCES='0')

# tag -> (difftest arguments, reference env, inert note or None)
SUITES = {
    # The mem.resources bake (native/mem-native a1fda568, 2026-10-17) made the native resource route the dev default:
    # the GPU write tracker is off there and no page is tracker-owned. The suites with tracker-owned cases (bracket's
    # tracked-lists, swap tracker mode, the pair's tracked ranges) run twice: pinned to the page model
    # (SR_MEM_RESOURCES=0, the oracle arm - the tracker cases whole) and on the native default (`_native`: every other
    # case, the tracker cases replaced by the native statement - the tracker off, the admission writableRange alone;
    # docs/RELEASE_COMPOSITION.md 9.6). tools/difftest/native_route_admission_run.py holds its mutant.
    'bracket': (['--native-bracket-test', 'native'], dict(BRACKET, **MR0), None),
    'bracket_native': (['--native-bracket-test', 'native'], BRACKET, None),
    'bracket_cppm4': (['--native-bracket-test', 'native'], dict(BRACKET, SR_NATIVE_FLUSH_CPPM4='1', **MR0), None),
    'bracket_cppm4_native': (['--native-bracket-test', 'native'], dict(BRACKET, SR_NATIVE_FLUSH_CPPM4='1'), None),
    'residue': (['--native-residue-test', 'native'], RESIDUE_BASE, None),
    'residue_cppm4': (['--native-residue-test', 'native'], dict(RESIDUE_BASE, SR_NATIVE_FLUSH_CPPM4='1'), None),
    'residue_p2trace': (['--native-residue-test', 'native'], dict(RESIDUE_BASE, SR_P2_TRACE='1'), None),
    # The census suite's zpass-kick and pump-driver cases attribute the SDK Z-pass kick's PM4 (its INTERRUPT packets,
    # the pump it leaves, the pump driver 82110840 running it): with BRACKET native the kick is a record and emits
    # none of it, so their preconditions cannot hold (the suite pins SR_NATIVE_BRACKET=0 itself). Exactly those two
    # cases may fail in the full composed row; census_no_bracket (the composed env, BRACKET at the suite's pin) must
    # pass whole.
    'census': (['--native-census-test', 'residue'], CENSUS,
               {'allow': ('zpass-kick: ', 'pump-driver: '),
                'why': 'the SDK Z-pass kick / its pump attribution cases: no SDK kick with BRACKET native'}),
    'census_no_bracket': (['--native-census-test', 'residue'], CENSUS, {'pins': {'SR_NATIVE_BRACKET': '0'}}),
    'coher_sentinel': (['--coher-sentinel-test', 'records'], COHER, None),
    'setconst_render': (['--setconst-render-test', 'records'], SETCONST, None),
    'present_gate_sdk_leaf': (['--present-gate-sdk-test', 'leaf'], GATE, None),
    'present_gate_sdk_direct': (['--present-gate-sdk-test', 'direct'], GATE, None),
    'present_gate_sdk_indirect': (['--present-gate-sdk-test', 'indirect'], GATE, None),
    'present_gate': (['--present-gate-test'], GATE, None),
    'swap_sdk_direct': (['--swap-sdk-test', 'direct'], SWAP, None),
    'swap_sdk_leaf': (['--swap-sdk-test', 'leaf'], SWAP, None),
    'swap_sdk_indirect': (['--swap-sdk-test', 'indirect'], SWAP, None),
    'swap_sdk_tracker': (['--swap-sdk-test', 'tracker'], dict(SWAP, **MR0), None),
    'swap_sdk_tracker_native': (['--swap-sdk-test', 'tracker'], SWAP, None),
    # The suite exactly as dev's bare suite list runs it (no SR_ variable: every default, SWAP off - the route is the
    # unscoped SDK body, the baked rows 5 / 3b on). Its reference arm only: the composed arm of a no-SR_ environment
    # would lack the swap runner's async-CP base. 5dc52749 broke this run (22 failures) and no lane row caught it.
    'swap_sdk_bare': (['--swap-sdk-test'], {}, {'reference_only': True,
                                                'why': 'the bare suite, as tools/difftest suite lists run it'}),
    'swap_block': (['--swap-block-test', 'normal'], BLOCK, None),
    'callback': (['--callback-test'], dict(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_ALLOW_FORCING='0'),
                 'calls the guest 820FDA80 body directly: no leaf in its path'),
    'transport_pair': (['--native-transport-test', 'native'], dict(PAIR, **MR0),
                       'segmentless off by its runner: the segmentless leaves are inert; the pair itself is ON'),
    'transport_pair_native': (['--native-transport-test', 'native'], PAIR,
                              'segmentless off by its runner: the segmentless leaves are inert; the pair itself is ON'),
    'pump_driver_integrated': (['--native-pump-driver-test', 'native'], DRIVER,
                               'flush / segmentless off by its runner: only the pair and the driver are live'),
    'recording': (['--native-recording-test', 'native'], RECORDING,
                  'flush / segmentless off by its runner: only the pair is live'),
    'record_adapter': (['--native-record-adapter-test'], EXECUTOR, None),
    'record_failure': (['--native-record-failure-test'], EXECUTOR, None),
    'segment_ownership': (['--native-segment-ownership-test'], EXECUTOR, None),
    'seg_tile': (['--seg-tile-test'], dict(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_ALLOW_FORCING='0'),
                 None),
    'present_composed_direct': (['--present-composed-test', 'direct'], PRESENT, None),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--only', default='', help='comma-separated suite tags (default: all)')
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    exe = root / 'build' / 'difftest.exe'
    if not exe.exists():
        raise SystemExit('build/difftest.exe missing: build the difftest target first')
    wanted = {x for x in args.only.split(',') if x}
    result = dict(scope='Every transport-reaching difftest suite in its reference mode and with every transport kind '
                  'composed (+ DEVCREATE, SHADER_CONST_RUNS off). Offline CPU only: no live, pacing or release '
                  'acceptance.', exe_sha256=sha(exe.read_bytes()), composed=COMPOSED, rows={})
    clean = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}

    def run(tag, argv, env):
        e = dict(clean)
        e.update(env)
        try:
            p = subprocess.run([str(exe)] + argv, cwd=root, env=e, capture_output=True, timeout=args.timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        summaries = [(t.decode(), int(p_), int(f)) for t, p_, f in SUMMARY.findall(stdout)]
        valid = code == 0 and bool(summaries) and all(p_ > 0 and f == 0 for _, p_, f in summaries)
        fails = [ln[6:].decode(errors='replace') for ln in stdout.replace(b'\r\n', b'\n').split(b'\n')
                 if ln.startswith(b'FAIL: ')]
        return dict(argv=argv, env={k: v for k, v in env.items()}, exit=code, summaries=summaries, valid=valid,
                    passed=sum(p_ for _, p_, _ in summaries), failed=sum(f for _, _, f in summaries),
                    failures=fails[:40], stdout_sha256=sha(stdout))

    for tag, (argv, ref, note) in SUITES.items():
        if wanted and tag not in wanted:
            continue
        opt = note if isinstance(note, dict) else {'inert': note}
        reference = run(tag + '_reference', argv, ref)
        if opt.get('reference_only'):  # the bare row: graded as it is run outside this matrix, no composed arm
            row = dict(reference=reference, composed=None, reference_only=True, why=opt.get('why'),
                       valid=reference['valid'])
            result['rows'][tag] = row
            print(json.dumps({'suite': tag, 'reference': reference['summaries'], 'composed': None,
                              'valid': row['valid']}), flush=True)
            (out / 'composed_suites.json').write_text(json.dumps(result, indent=2) + '\n')
            continue
        env = dict(ref)
        env.update(COMPOSED)
        env.update(opt.get('pins', {}))  # a gate held at the suite's own pin in this row (named in the row)
        composed = run(tag + '_composed', argv, env)
        allow = opt.get('allow', ())
        if allow and not composed['valid']:
            # only the named cases may fail, and at least one assertion still passes; the row says which failed
            fails = composed['failures']
            composed['allowed_failures'] = fails
            composed['valid'] = bool(composed['passed'] > 0 and fails and len(fails) == composed['failed'] and
                                     all(f.startswith(allow) for f in fails))
        row = dict(reference=reference, composed=composed, inert=opt.get('inert'), pins=opt.get('pins', {}),
                   allow=list(allow), why=opt.get('why'), info=opt.get('info'),
                   fewer_assertions=composed['passed'] < reference['passed'],
                   valid=reference['valid'] and composed['valid'])
        result['rows'][tag] = row
        print(json.dumps({'suite': tag, 'reference': reference['summaries'], 'composed': composed['summaries'],
                          'valid': row['valid']}), flush=True)
        (out / 'composed_suites.json').write_text(json.dumps(result, indent=2) + '\n')
    result['valid'] = bool(result['rows']) and all(r['valid'] for r in result['rows'].values() if not r.get('info'))
    (out / 'composed_suites.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
