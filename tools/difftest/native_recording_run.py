#!/usr/bin/env python3
"""Actual SDK 82110C78 differential and compiled faults; bounded CPU scope only.

python tools/difftest/native_recording_run.py --out archive/<fresh-bank>
One build slot, four jobs, 8 GiB free; no game/GPU worker. Helpers keep their
existing contracts. No kick token delivery, sealing, live or release acceptance.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from native_memory_test_run import stop_windows_tree

CASES = ('plain', 'deferred', 'whole', 'both', 'pre-failed', 'coherent-failed', 'kick-failed', 'append-failed',
         'no-gpu', 'no-gpu-deferred', 'pix', 'zero-root', 'cpu-mask', 'full-pointers', 'direct', 'direct-deferred', 'all-ones-range')
EQUALITY = {f'{name}: {route} equals actual SDK' for name in CASES for route in ('dispatch', 'direct')}
FAILURE_CASES = {'pre-failed', 'coherent-failed', 'kick-failed', 'append-failed'}


def equalities(cases):
    return {f'{name}: {route} equals actual SDK' for name in cases for route in ('dispatch', 'direct')}


ALLOWED = {
    # Every fixture writes the frame backchain, including pre-existing failure.
    'frame': EQUALITY,
    'old_descriptor': equalities(set(CASES) - FAILURE_CASES - {'no-gpu'}),
    'rollback_limit': equalities(FAILURE_CASES),
    'root_view': equalities(set(CASES) - {'pre-failed', 'coherent-failed', 'kick-failed', 'zero-root'}),
    'kick_increment': equalities(set(CASES) - FAILURE_CASES - {'no-gpu', 'no-gpu-deferred'}),
    'pix_condition': equalities(set(CASES) - FAILURE_CASES - {'no-gpu', 'no-gpu-deferred'}),
    # 820FDA80's optional idle prefix tests flags & 0xC0FFFFFF. Plain flags
    # 4 and 5 both suppress it; 0x23000000 and 0x23000001 differ. Only the
    # cpu-mask fixture exposes removing the kick caller's mandatory low bit.
    'callback_flags': equalities({'cpu-mask'}),
}
REQUIRED = {
    'frame': {'plain: dispatch equals actual SDK', 'pre-failed: direct equals actual SDK'},
    'old_descriptor': {'plain: dispatch equals actual SDK', 'deferred: direct equals actual SDK'},
    'rollback_limit': {'pre-failed: dispatch equals actual SDK', 'append-failed: direct equals actual SDK'},
    'root_view': {'plain: dispatch equals actual SDK', 'append-failed: direct equals actual SDK'},
    'kick_increment': {'plain: dispatch equals actual SDK', 'direct: direct equals actual SDK'},
    'pix_condition': {'pix: dispatch equals actual SDK', 'plain: direct equals actual SDK'},
    'callback_flags': equalities({'cpu-mask'}),
}


def grade(code, raw, gate_off=False, defect=None):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    summary = re.findall(r'^\[NATIVE_RECORDING_TEST\] (\d+) passed, (\d+) failed$', '\n'.join(lines), re.M)
    expected = 123 if gate_off else 106
    if summary != [(str(len(good)), str(len(bad)))] or len(good) + len(bad) != expected:
        return False
    if len(set(good + bad)) != expected:
        return False
    if defect is None:
        return code == 0 and not bad
    return not gate_off and code == 1 and REQUIRED[defect] <= set(bad) <= ALLOWED[defect]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def restore_source(source, original, expected, quiescent):
    """Do not overwrite concurrent edits or restore while cleanup is unproved."""
    if not quiescent:
        raise RuntimeError('Process cleanup unproved; source restoration is unsafe')
    if source.read_bytes() != expected:
        raise RuntimeError('Concurrent source edit; refusing restoration')
    if expected != original:
        source.write_bytes(original)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_recording.cpp'
    original = source.read_bytes()
    (out / 'native_recording.original.cpp').write_bytes(original)
    paths = ('kernel/native_recording.cpp', 'kernel/native_recording.hpp', 'kernel/native_memory.hpp',
             'kernel/native_transport.cpp', 'kernel/native_transport.hpp', 'kernel/native_flush.cpp', 'kernel/native_flush.hpp',
             'kernel/gpu_ringless.cpp', 'kernel/gpu_write_watch.cpp', 'kernel/guest_context.cpp', 'kernel/stubs.cpp', 'kernel/kpcr.cpp',
             'kernel/p2_trace.cpp', 'kernel/p2_trace.hpp', 'guest/ppc/ppc_recomp.2.cpp',
             'tools/difftest/native_recording_test.cpp', 'tools/difftest/native_recording_run.py',
             'tools/difftest/test_native_recording_runner.py', 'tools/difftest/native_memory_test_run.py',
             'tools/difftest/difftest_main.cpp', 'CMakeLists.txt')
    result = dict(scope='Complete CPU recording-end/kick controller, with actual existing SDK helpers. No generation/token delivery, sealing, native-zero or release acceptance.',
                  source_sha256={p: sha(root / p) for p in paths}, mutations=[])
    quiescent = True
    expected_source = original

    def save():
        result['process_quiescent'] = quiescent
        (out / 'recording_matrix.json').write_text(json.dumps(result, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env.get('PATH', '')
    env.update(SR_NATIVE_FLUSH='0', SR_NATIVE_SEGMENTLESS='0', SR_NATIVE_RINGLESS='0', SR_PM4_RING='0',
               SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0', SR_NATIVE_TRANSPORT='0')

    def run(tag, command, timeout, gate_off=False, defect=None):
        nonlocal quiescent
        command = [str(x) for x in command]
        p = subprocess.Popen(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        quiescent = False
        stdout = stderr = b''
        row = dict(command=command, pid=p.pid, exit=None, terminal_observed=False)
        try:
            try:
                stdout, stderr = p.communicate(timeout=timeout)
                code = p.returncode
            except subprocess.TimeoutExpired as error:
                stdout, stderr = error.output or b'', error.stderr or b''
                stop_windows_tree(p)  # Raises if any tracked child remains.
                stdout, stderr = p.communicate(timeout=10)
                code = 124
            quiescent = True  # Set only after observed completion / checked cleanup.
            row.update(exit=code, terminal_observed=True)
        except BaseException as error:
            row['error'] = str(error)
            raise
        finally:
            (out / (tag + '_stdout.txt')).write_bytes(stdout)
            (out / (tag + '_stderr.txt')).write_bytes(stderr)
            row.update(stdout_sha256=hashlib.sha256(stdout).hexdigest(), stderr_sha256=hashlib.sha256(stderr).hexdigest())
            (out / (tag + '_exit.json')).write_text(json.dumps(row, indent=2) + '\n')
            save()
        if '--native-recording-test' in command:
            row.update(valid=grade(code, stdout, gate_off, defect),
                       summary=re.findall(r'\[NATIVE_RECORDING_TEST\] (\d+) passed, (\d+) failed', stdout.decode(errors='replace')))
        (out / (tag + '_exit.json')).write_text(json.dumps(row, indent=2) + '\n')
        print(json.dumps({'tag': tag, 'exit': code, 'valid': row.get('valid'), 'summary': row.get('summary')}), flush=True)
        return row, stdout

    def exe_hash(tag):
        # File opens may stall independently of builds. Keep the read bounded
        # in its own child; failed cleanup must prevent restoration / rebuild.
        code = "import hashlib,sys; h=hashlib.sha256(); f=open(sys.argv[1],'rb'); h.update(f.read()); print(h.hexdigest())"
        row, raw = run(tag + '_hash', [sys.executable, '-c', code, root / 'build/difftest.exe'], 30)
        digest = raw.decode(errors='replace').strip()
        if row['exit'] != 0 or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise RuntimeError('Executable hash failed; inspect retained output')
        return digest

    def build(tag):
        guard = """$ErrorActionPreference='Stop'
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Less than 8 GiB free RAM' }
if (Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(SupermanReturns|xenia.*|difftest)\\.exe$' }) { throw 'Game or test active' }
"""
        checked, _ = run(tag + '_guard', ['powershell', '-NoProfile', '-Command', guard], 30)
        if checked['exit'] != 0:
            raise RuntimeError('Build resource guard failed')
        row, _ = run(tag, [sys.executable, 'tools/build_slot.py', '--owner', 'native-recording-controls', '--capacity', '1', '--',
                        'cmake', '--build', 'build', '--target', 'difftest', '-j4'], 300)
        if row['exit'] != 0:
            raise RuntimeError('Build failed')
        row['exe_sha256'] = exe_hash(tag)
        return row

    def test(tag, gate_off=False, integrated=False, defect=None):
        env['SR_NATIVE_REPLAY_KICK'] = '0' if gate_off else '1'
        env['SR_NATIVE_TRANSPORT'] = '1' if integrated else '0'
        row, _ = run(tag, [root / 'build/difftest.exe', '--native-recording-test', 'gate-off' if gate_off else 'native'],
                  30, gate_off, defect)
        row['exe_sha256'] = exe_hash(tag)
        if not row['valid']:
            raise RuntimeError(f'{tag} invalid; inspect retained outputs')
        return row

    mutations = (
        ('frame', b'work.r1.u32 = stack - 0xB0u;', b'work.r1.u32 = stack - 0xC0u;', 1),
        ('old_descriptor', b'ctx.r3.u64 + 0x32DCu', b'ctx.r3.u64 + 0x32C8u', 2),
        ('rollback_limit', b'rd(dev + 0x32F4u) - 160u', b'rd(dev + 0x32F4u) - 164u', 1),
        ('root_view', b'uint64_t(physical(root)) - 0x40000000ull', b'uint64_t(physical(root))', 1),
        ('kick_increment', b'work.r7.u64 = 1u; work.r8.u64', b'work.r7.u64 = 0u; work.r8.u64', 1),
        ('pix_condition', b'if (rd(dev + 0x4DF4u) ||', b'if (rd(dev + 0x4DF4u) &&', 1),
        ('callback_flags', b'ctx.r5.u64 | 1u', b'ctx.r5.u64', 1),
    )
    try:
        result['positive_build'] = build('positive_build')
        for key, off, integrated in (('positive', False, False), ('gate_off', True, False),
                                     ('integrated', False, True), ('integrated_gate_off', True, True)):
            result[key] = test(key, off, integrated)
        for name, before, after, count in mutations:
            if original.count(before) != count:
                raise RuntimeError('Production anchor changed; refusing mutation')
            if not quiescent or source.read_bytes() != original:
                raise RuntimeError('Unsafe mutation or concurrent source edit')
            changed = original.replace(before, after)
            row = dict(name=name, mutated_sha256=hashlib.sha256(changed).hexdigest())
            result['mutations'].append(row)
            source.write_bytes(changed)
            expected_source = changed
            try:
                row['build'] = build(name + '_build')
                row['test'] = test(name + '_test', defect=name)
                row['gate_off'] = test(name + '_gate_off', gate_off=True)
            finally:
                restore_source(source, original, expected_source, quiescent)
                expected_source = original
                row['source_restored'] = True
                save()
    finally:
        result['valid'] = False
        result['all_sources_restored'] = False
        try:
            restore_source(source, original, expected_source, quiescent)
            expected_source = original
            result['all_sources_restored'] = all(sha(root / p) == v for p, v in result['source_sha256'].items())
            if result['all_sources_restored']:
                result['restored_build'] = build('restored_build')
                result['restored'] = test('restored')
                result['restored_integrated'] = test('restored_integrated', integrated=True)
            result['valid'] = bool(quiescent and result['all_sources_restored']
                and all(result.get(key, {}).get('valid') for key in
                        ('positive', 'gate_off', 'integrated', 'integrated_gate_off', 'restored', 'restored_integrated'))
                and len(result['mutations']) == len(mutations)
                and all(row.get('source_restored') and row.get('test', {}).get('valid')
                        and row.get('gate_off', {}).get('valid') for row in result['mutations']))
        except BaseException as error:
            result['restoration_error'] = str(error)
            raise
        finally:
            save()
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
