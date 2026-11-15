#!/usr/bin/env python3
"""p2.transport BRACKET offline proof: the actual SDK Z-pass bracket against SR_NATIVE_BRACKET, with compiled faults.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name tbr python tools/difftest/native_bracket_run.py --out archive/tbr/bracket_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode (native, gate-off, CPPM4, SR_NATIVE_TRANSPORT composed, its gate-off,
segmentless-off, P2 trace on); each compiled fault (kernel/native_flush.cpp only) rebuilt and required to fail its
named assertions, never crash, and pass in its gate-off control; then the source restored byte for byte, rebuilt and
the positive modes rerun. No live, sealing or release acceptance is claimed by this matrix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

SUMMARY = re.compile(r'^\[NATIVE_BRACKET_TEST\] (\d+) passed, (\d+) failed$', re.M)

# mode tag -> (test argument, environment overrides). The fixture always runs the per-present A/B
# (SR_AB_TOGGLE=SR_NATIVE_BRACKET, SR_AB_PRESENT=1): odd presents the SDK arm, even the native arm.
MODES = {
    'native': ('native', {}),
    'gate_off': ('gate-off', {'SR_NATIVE_BRACKET': '0'}),
    'cppm4': ('native', {'SR_NATIVE_FLUSH_CPPM4': '1'}),
    'transport': ('native', {'SR_NATIVE_TRANSPORT': '1'}),
    'transport_gate_off': ('gate-off', {'SR_NATIVE_BRACKET': '0', 'SR_NATIVE_TRANSPORT': '1'}),
    'segmentless_off': ('segmentless-off', {'SR_NATIVE_SEGMENTLESS': '0'}),
    'p2trace': ('native', {'SR_P2_TRACE': '1'}),
}

SDK_ARM = re.compile(r': SDK arm ')


def A(case, text):
    return f'{case}: {text}'


REPLAY = 'replay sequence equals the actual SDK pump (BS values, segments)'
CMD = 'command buffer, projections and lists equal the actual SDK'
DEVICE = 'device equals the actual SDK (dev+0x2A74 compared apart)'
STACK = 'stack window, return and ABI registers equal the actual SDK'

# name, anchor, replacement, the test mode it runs in, assertions that must fail, whether SDK-arm assertions may fail.
MUTATIONS = (
    ('colour_value', b'const uint32_t v = (flags & 0x80000000u) ? 0x2AAAAAAAu :',
     b'const uint32_t v = (flags & 0x80000000u) ? 0x15555555u :', 'native',
     {A('plain', REPLAY), A('records', REPLAY)}, False),
    # the colour pass's 0x85 jumps to the 0x86: the records between them (EndZPass's neutral scissor) replay once
    ('colour_skips_neutral', b'        if (flags & 0x80000000u) k = p174;', b'        if (false) k = p174;', 'native',
     {A('neutral', REPLAY), A('post-kick', REPLAY)}, False),
    ('colour_pass', b'        if (flags & 0x80000000u) k = p170;', b'        if (false) k = p170;', 'native',
     {A('plain', REPLAY)}, False),
    ('kick_increment', b'  work.r7.u64 = 0u;  // the increment: no pump work', b'  work.r7.u64 = 1u;  // the increment: no pump work',
     'native', {A('plain', 'dev+0x2A74 never moved (nothing to drain)')}, False),
    ('coherency_sentinel', b'const bool range = be64(d + 0x2D18u) != 0x00000000FFFFFFFFull;',
     b'const bool range = be64(d + 0x2D18u) != ~0ull;', 'native', {A('plain', CMD), A('all-ones-range', CMD)}, False),
    ('root_view', b'const uint32_t view = root ? brPhys(root) - 0x40000000u : 0u;',
     b'const uint32_t view = root ? brPhys(root) : 0u;', 'native', {A('plain', CMD)}, False),
    ('retire', b'    if (!c.sp) continue;\n    g_segById.erase(c.sp->id);', b'    if (c.sp) continue;\n    g_segById.erase(c.sp->id);',
     'native', {A('plain', 'bound entries retired; the native arm never used the deferred lookup the SDK pump needed'),
                A('kick-failed', 'failed bracket dropped as the SDK drops it; its entries retired')}, False),
    ('failure_rollback', b'sr::kernel::writeGuestBE32(dev + 0x30u, brRd(dev + 0x32F4u) - 160u);',
     b'sr::kernel::writeGuestBE32(dev + 0x30u, brRd(dev + 0x32F4u) - 164u);', 'native',
     {A('kick-failed', DEVICE), A('pre-failed', DEVICE)}, False),
    ('walk_link', b'      at = word + 4u;  // a software link', b'      return false;  // a software link', 'native',
     {A('list-overflow', 'BRACKET_END published')}, False),
    ('begin_ab_window', b'  if (ab >= 0 && !sr::abt::on(ab)) {\n    g_brBeginAbOff',
     b'  if (false) {\n    g_brBeginAbOff', 'native',
     {A('plain', 'SDK arm kicked the pump (dev+0x2A74 1, interpreter to the terminator, EndSync drained it)')}, True),
    ('consumer_bs', b'      eng.applyBinReg(2u, value);\n    }\n    if (tr::g_on) tr::event(tr::kBs, 1u',
     b'      eng.applyBinReg(0u, value);\n    }\n    if (tr::g_on) tr::event(tr::kBs, 1u', 'native',
     {A('plain', 'consumer effects equal the actual SDK (its kick block apart)')}, False),
    ('bind_sequence', b'tokens[k].n != c.dwords || tokens[k].phys != c.phys ||', b'', 'native',
     {A('duplicated-record', 'declined to the SDK body (reason 6)')}, False),
    ('poison', b'    g_brOpen->poisoned = true;  // its list record has no parts', b'    g_brOpen->poisoned = false;  // its list record has no parts',
     'native', {A('poisoned', 'declined to the SDK body (reason 1)')}, False),
    ('frame', b'  const uint32_t frame = stack - 0xB0u;', b'  const uint32_t frame = stack - 0xC0u;', 'native',
     {A('plain', STACK)}, False),
    ('published_check',
     b'if (!c.sp->deferred || c.sp->published || c.sp->started || !g_segById.count(c.sp->id)) return decline(kBrPublished);',
     b'if (false) return decline(kBrPublished);', 'native',
     {A('already-published', 'declined to the SDK body (reason 7)')}, False),
    ('requires_segmentless',
     b'  if (!g_on || !segmentlessActive() || g_pm4Out || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth ||\n      !bracketConsumer() || !dev',
     b'  if (!g_on || g_pm4Out || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth ||\n      !bracketConsumer() || !dev',
     'segmentless_off', {'segmentless off: every begin declined, no bracket opened'}, False),
    # mixed per-present traffic: a native end while an OFF bracket's pump work is outstanding chains the closes after
    # it into a list no pump walks (the live tbr_abp wedge, 2026-10-14)
    ('pump_pending', b'if (!gateRead(dev + 0x2A74u, pumpWork, 4u) || be32(pumpWork)) return decline(kBrPending);',
     b'if (!gateRead(dev + 0x2A74u, pumpWork, 4u)) return decline(kBrPending);', 'native',
     {"mixed alternate-lag: every close's marker reached the consumer (no segment lost)",
      'mixed alternate-lag2: dev+0x2A74 drained to 0'}, False),
)


def parse(raw):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    summary = SUMMARY.findall('\n'.join(lines))
    return good, bad, summary


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original
    (out / 'native_flush.original.cpp.sha256').write_text(sha(original) + '\n')
    tracked = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/native_recording.cpp',
               'kernel/native_record_format.hpp', 'tools/difftest/native_bracket_test.inc',
               'tools/difftest/native_bracket_run.py', 'tools/difftest/difftest_main.cpp')
    result = dict(scope='Offline CPU proof of p2.transport BRACKET (SR_NATIVE_BRACKET): the actual SDK 82110B30 / '
                  '82110C78 / 82110460 / 82110768 bracket path against the native begin / end and the consumer replay. '
                  'No live, sealing or release acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[])
    only = {x for x in args.only.split(',') if x}
    selected = [m for m in MUTATIONS if not only or m[0] in only]

    def save():
        (out / 'bracket_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    # the mem.resources bake (native/mem-native a1fda568, 2026-10-17) made the native resource route the dev default, where the GPU write tracker is off: these cases test the page model's tracker, so they run at SR_MEM_RESOURCES=0 (the oracle arm); the native default's rows are composed_suites_run.py's *_native and native_route_admission_run.py
    base_env['SR_MEM_RESOURCES'] = '0'
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE='SR_NATIVE_BRACKET', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1',
                    SR_NATIVE_FLUSH='1', SR_NATIVE_BRACKET='1', SR_NATIVE_TRANSPORT='0')

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
        code, raw = run(tag, [root / 'build/difftest.exe', '--native-bracket-test', arg], env, 600)
        good, bad, summary = parse(raw)
        row = dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary,
                   stdout_sha256=sha(raw), failures=bad)
        return row, good, bad, summary

    def positive(tag, mode):
        row, good, bad, summary = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good))
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    expected_total = {}
    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            row = positive('positive_' + mode, mode)
            result['modes'][mode] = row
            expected_total[MODES[mode][0]] = expected_total.get(MODES[mode][0], row['passed'])
            save()
        if not all(r['valid'] for r in result['modes'].values()):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, before, after, mode, required, sdk_ok in selected:
            if crlf:  # the worker's checkout may carry CRLF line ends
                before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
            if source.read_bytes() != original:
                raise RuntimeError('concurrent source edit; refusing mutation')
            changed = original.replace(before, after)
            mrow = dict(name=name, mode=mode, required=sorted(required), mutated_sha256=sha(changed))
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                trow, good, bad, summary = test(name + '_test', mode)
                total = expected_total[MODES[mode][0]]
                missing = sorted(required - set(bad))
                sdk_failures = [b for b in bad if SDK_ARM.search(b)]
                trow['missing_required'] = missing
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                 and len(good) + len(bad) == total and not missing and (sdk_ok or not sdk_failures))
                mrow['test'] = trow
                grow = positive(name + '_gate_off', 'gate_off')
                mrow['gate_off'] = grow
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
                result['restored'] = {m: positive('restored_' + m, m) for m in ('native', 'gate_off', 'cppm4', 'transport')}
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and len(result['mutations']) == len(selected)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and (not selected or all(r['valid'] for r in result.get('restored', {}).values()) and result.get('restored')))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
