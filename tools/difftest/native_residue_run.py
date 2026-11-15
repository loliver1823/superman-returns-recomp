#!/usr/bin/env python3
"""p2.transport RESIDUE offline proof: the actual SDK bodies against SR_NATIVE_IDLE_WAIT / SR_NATIVE_INVAL_RANGE /
SR_NATIVE_LOCK_COHER, with compiled faults.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name tres python tools/difftest/native_residue_run.py --out archive/tres/residue_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode (native, gate-off, CPPM4, the PM4 output, composed with SR_NATIVE_BRACKET - its
bracket cycles native in both arms, with the residue gates on and off - and with SR_NATIVE_TRANSPORT, segmentless-off,
P2 trace on); each compiled fault (kernel/native_flush.cpp only) rebuilt
and required to fail its named assertions without crashing (a hang ends through the suite's own watchdog as a named
failure), and to pass its gate-off control; then the source restored byte for byte, rebuilt and the positive modes
rerun. No live, pacing or release acceptance is claimed by this matrix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[NATIVE_RESIDUE_TEST\] (\d+) passed, (\d+) failed$', re.M)
GATES = 'SR_NATIVE_IDLE_WAIT+SR_NATIVE_INVAL_RANGE+SR_NATIVE_LOCK_COHER'
OFF = {'SR_NATIVE_IDLE_WAIT': '0', 'SR_NATIVE_INVAL_RANGE': '0', 'SR_NATIVE_LOCK_COHER': '0'}

# mode tag -> (test argument, environment overrides). Every mode runs the per-present A/B of the three gates
# (SR_AB_TOGGLE, SR_AB_PRESENT=1): odd presents the SDK arm, even the native arm.
MODES = {
    'native': ('native', {}),
    'gate_off': ('gate-off', OFF),
    'cppm4': ('native', {'SR_NATIVE_FLUSH_CPPM4': '1'}),
    'pm4out': ('native', {'SR_NATIVE_FLUSH_PM4': '1', 'SR_NATIVE_CONST_DEDUP_CHECK': '1'}),
    'bracket': ('native', {'SR_NATIVE_BRACKET': '1'}),
    'bracket_gate_off': ('gate-off', dict(OFF, SR_NATIVE_BRACKET='1')),
    'transport': ('native', {'SR_NATIVE_TRANSPORT': '1'}),
    'transport_gate_off': ('gate-off', dict(OFF, SR_NATIVE_TRANSPORT='1')),
    'segmentless_off': ('segmentless-off', {'SR_NATIVE_SEGMENTLESS': '0'}),
    'p2trace': ('native', {'SR_P2_TRACE': '1'}),
}

SDK_ARM = re.compile(r': SDK arm: ')


def A(case, text):
    return f'{case}: {text}'


DEVICE = 'device, fence block and resource equal the actual SDK'
CMD = 'command buffer equals the actual SDK outside the span'
RING = 'ring descriptors equal the actual SDK'
COHER = 'COHER applied: status, size, base as the SDK\'s packets set them, its wait run'
CONSUMER = 'consumer registers (WAIT_UNTIL, COHER) and waits equal the actual SDK\'s'
SPAN = 'the SDK wrote exactly the expected PM4 where the native span lies'
HOST = 'the status kRkInvalidate cannot carry travels as kRkCoherHost (kind 22), applied once'
MERGED = 'another thread: dev+0x2D18 = the merged range {end, base} (both endpoints, from the SDK\'s empty value)'
RANGE_APPLIED = 'the next close\'s INVALIDATE applied that range (page base, page-rounded size)'
BR_TAKEN = 'taken natively inside the bracket: each residue call one record span'
BR_KIND22 = 'kind 22 (the texture lock\'s status) consumed in both BS passes (depth, colour): applied twice'
TAKEN = {1: 'taken natively: one record span standing for the SDK\'s 1 packet(s)',
         4: 'taken natively: one record span standing for the SDK\'s 4 packet(s)',
         3: 'taken natively: one record span standing for the SDK\'s 3 packet(s)'}

# name, anchor, replacement, the test mode it runs in, assertions that must fail, whether SDK-arm assertions may fail.
MUTATIONS = (
    ('idle_value', b'const uint8_t kWaitUntil3dIdle[4] = {0x00u, 0x02u, 0x00u, 0x00u};',
     b'const uint8_t kWaitUntil3dIdle[4] = {0x00u, 0x01u, 0x00u, 0x00u};', 'native',
     {A('idle-plain', 'WAIT_UNTIL applied: 0x5C8 = 0x00020000'), A('idle-plain', CONSUMER)}, False),
    ('idle_no_fence_wait', b'  if (!ctx.cr0.eq) {\n    ctx.r5.s64 = 4;', b'  if (false) {\n    ctx.r5.s64 = 4;', 'native',
     {A('idle-late-fence', 'returned only after the consumer applied the fence (the SDK\'s wait)'),
      A('idle-plain', 'returned with fb+0 at S (the fence wait passed)')}, False),
    ('idle_no_spin', b'    ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);\n  } while (!ctx.cr6.eq);',
     b'    ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);\n  } while (false);', 'native',
     {A('idle-spin', 'returned only after dev+0x2A74 was released (the SDK\'s spin)')}, False),
    # A cursor 4 bytes short of the span: observed (run 1) as idle-plain's native arm never returning from its wait -
    # the watchdog's assertion, which ends the suite there (exit 1). The cause inside the close was not traced.
    ('idle_cursor', b'    ctx.r3.u32 = at + 8u;  // the stwu chain', b'    ctx.r3.u32 = at + 4u;  // the stwu chain', 'native',
     {A('idle-plain', 'both arms completed (no hang)')}, False),
    ('idle_owner', b'  if (why >= 0 || !owner) {  // the guest body writes its WAIT_UNTIL on any thread',
     b'  if (why >= 0) {  // the guest body writes its WAIT_UNTIL on any thread', 'native',
     {A('idle-other-thread', 'declined at the top (reason 5), the guest body whole')}, False),
    ('inval_no_wait_until',
     b'return e.run(0x5C8u, 1u, kWaitUntil3dIdle) && e.coherency(0x03000000u, ctx.r9.u32, ctx.r10.u32);',
     b'return e.coherency(0x03000000u, ctx.r9.u32, ctx.r10.u32);', 'native',
     {A('inval-owner', SPAN)}, False),
    ('inval_status', b'e.coherency(0x03000000u, ctx.r9.u32, ctx.r10.u32);', b'e.coherency(0x01000000u, ctx.r9.u32, ctx.r10.u32);',
     'native', {A('inval-owner', COHER), A('inval-owner', CONSUMER)}, False),
    ('lock_size_base', b'return e.coherency(ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);',
     b'return e.coherency(ctx.r8.u32, ctx.r10.u32, ctx.r9.u32);', 'native',
     {A('lock-vb', COHER), A('lock-vb', CONSUMER)}, False),
    ('lock_status_const', b'return e.coherency(ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);',
     b'return e.coherency(0x03000000u, ctx.r9.u32, ctx.r10.u32);', 'native',
     {A('lock-texture', COHER), A('lock-texture', HOST)}, False),
    ('coher_route', b'  if (status != 0x03000000u && status != 0x01000000u) {\n    ensureResidueHandlers();',
     b'  if (false) {\n    ensureResidueHandlers();', 'native',
     {A('lock-texture', COHER), A('lock-texture', HOST)}, False),
    ('host_apply', b'  eng.writeReg(0x0A31u, w[0]);\n  eng.writeReg(0x0A2Fu, w[1]);',
     b'  eng.writeReg(0x0A31u, w[0] | 1u);\n  eng.writeReg(0x0A2Fu, w[1]);', 'native',
     {A('lock-texture', COHER), A('lock-texture', CONSUMER)}, False),
    ('host_render', b'const uint32_t v[11] = {0x00000A31u, w[0], 0x00010A2Fu, w[1], w[2],',
     b'const uint32_t v[11] = {0x00000A31u, w[0], 0x00010A2Fu, w[2], w[1],', 'cppm4',
     {A('lock-texture', COHER), A('lock-texture', CONSUMER)}, False),
    ('late_recheck', b'  if (segmentlessActive() && !rsGuestMode(d) && be32(d + 40) == at) {',
     b'  if (segmentlessActive() && be32(d + 40) == at) {', 'native',
     {A('idle-late-no-gpu', 'refused late at the record point: the SDK\'s own dwords written')}, False),
    ('late_literal', b'             PPC_STORE_U32(at + 8u, ctx.r10.u32);', b'             PPC_STORE_U32(at + 8u, ctx.r11.u32);',
     'native', {A('idle-late-no-gpu', 'command buffer equals the actual SDK')}, False),
    ('lock_kick', b'    cr6.compare<uint32_t>(r11.u32, ctx.r10.u32, xer);\n    if (!cr6.gt) goto loc_820F3AE0;',
     b'    cr6.compare<uint32_t>(r11.u32, ctx.r10.u32, xer);\n    if (true) goto loc_820F3AE0;', 'native',
     {A('lock-vb-kick', RING)}, False),
    ('inval_merge', b'    if (!cr6.lt) goto loc_820F42D8;', b'    if (true) goto loc_820F42D8;', 'native',
     {A('inval-other-thread', MERGED), A('inval-other-thread', RANGE_APPLIED)}, False),
    # The merge's END update dropped (Astra P2): from the SDK's empty value the end stays 0 - visible only because the
    # fixture seeds 0x00000000FFFFFFFF (the all-ones seed kept end 0xFFFFFFFF either way).
    ('inval_merge_end', b'    if (!cr6.gt) goto loc_820F42EC;', b'    if (true) goto loc_820F42EC;', 'native',
     {A('inval-other-thread', MERGED), A('inval-other-thread', RANGE_APPLIED)}, False),
    ('lock_merge_end', b'    if (!cr6.gt) goto loc_820F3A7C;', b'    if (true) goto loc_820F3A7C;', 'native',
     {A('lock-other-thread', MERGED), A('lock-other-thread', RANGE_APPLIED)}, False),
    # The residue refusing a pinned segment (a bracket open): the bracket cycles' spans are gone - composed with BRACKET.
    ('pinned_decline', b'  if (rsGuestMode(d)) return kRsMode;\n',
     b'  if (rsGuestMode(d) || be32(d + 0x3290u)) return kRsMode;\n', 'bracket',
     {A('bracket residue', BR_TAKEN), A('bracket residue', BR_KIND22), A('bracket pending', BR_TAKEN)}, False),
    ('lock_count', b'loc_820F3BEC:\n    r11.s64 = 256;', b'loc_820F3BEC:\n    r11.s64 = 512;', 'native',
     {A('lock-vb', DEVICE)}, False),
    ('ab_window', b'  if (ab >= 0 && !sr::abt::on(ab)) return kRsAb;\n', b'', 'native',
     {A('idle-plain', 'SDK arm: the guest body (declined in the A/B OFF window)')}, True),
    ('requires_segmentless',
     b'  if (!g_on || !segmentlessActive() || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return kRsSeg;',
     b'  if (!g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return kRsSeg;', 'segmentless_off',
     {A('idle-plain', 'segmentless off: declined at the top, the guest body whole')}, False),
    ('entry_check', b'  if ((d[10808] & 0x02u) || sr::gfx::livetrace::armed() || sr::native::dev::on()) return kRsEntry;',
     b'  if (sr::gfx::livetrace::armed() || sr::native::dev::on()) return kRsEntry;', 'native',
     {A('idle-tile-pred', 'declined at the top (reason 4), the guest body whole')}, False),
    ('mode_check', b'  if (rsGuestMode(d)) return kRsMode;\n', b'', 'native',
     {A('idle-no-gpu', 'declined at the top (reason 3), the guest body whole')}, False),
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
    parser.add_argument('--modes-only', action='store_true', help='the positive modes only (no mutation)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original
    (out / 'native_flush.original.cpp.sha256').write_text(sha(original) + '\n')
    tracked = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/native_record_format.hpp',
               'kernel/cs_timeout_underflow_fix.cpp', 'kernel/native_brackets.gen.cpp',
               'tools/difftest/native_residue_test.inc', 'tools/difftest/native_residue_run.py',
               'tools/difftest/difftest_main.cpp')
    result = dict(scope='Offline CPU proof of p2.transport RESIDUE (SR_NATIVE_IDLE_WAIT, SR_NATIVE_INVAL_RANGE, '
                  'SR_NATIVE_LOCK_COHER): the actual SDK 820FDCB8 / 820F4280 / 820F3988 (via 820F3FA0, 820F40F0 and '
                  'directly) / 820FDD30 against the native ports and one emulated in-order consumer. No live, pacing '
                  'or release acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[])
    only = {x for x in args.only.split(',') if x}
    selected = [] if args.modes_only else [m for m in MUTATIONS if not only or m[0] in only]

    def save():
        (out / 'residue_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE=GATES, SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1',
                    SR_NATIVE_IDLE_WAIT='1', SR_NATIVE_INVAL_RANGE='1', SR_NATIVE_LOCK_COHER='1',
                    SR_NATIVE_BRACKET='0', SR_NATIVE_TRANSPORT='0')

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
        code, raw = run(tag, [root / 'build/difftest.exe', '--native-residue-test', arg], env, 900)
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

    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
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
                missing = sorted(required - set(bad))
                sdk_failures = [b for b in bad if SDK_ARM.search(b)]
                trow['missing_required'] = missing
                # A detected defect: the suite ran to its summary (or its watchdog's), exited 1, every named assertion
                # failed, and the SDK arm's own assertions held (unless the defect is the A/B window itself).
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                 and not missing and (sdk_ok or not sdk_failures))
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
                result['restored'] = {m: positive('restored_' + m, m) for m in ('native', 'gate_off', 'cppm4', 'pm4out', 'bracket')}
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
