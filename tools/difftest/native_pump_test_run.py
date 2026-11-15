#!/usr/bin/env python3
"""Retail software-marker replay differential and compiled defect controls.

python tools/difftest/native_pump_test_run.py --out archive/<fresh-bank>
Capacity-one builds, four jobs, 8 GiB free; no game or GPU worker. Tests the
marker body, dispatch, and cursor scope, not generation ownership, outer-driver
retirement, native GPU submission, pixels, or release acceptance.
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

CHECKS = 134


def equality_checks(*cases):
    return {f'{name}: {route} equals actual SDK' for name in cases for route in ('dispatch', 'direct')}


IDENTITY_CASES = ('identical descriptors', 'software link', 'depth neutral colour post',
                  'tile depth replay', 'tile colour entry', 'pause untaken', 'full cursor return', 'ring wrap',
                  'zero dword descriptor')
PIX_CHECKS = {f'actual publication route {route} retains scope through SDK PIX callbacks' for route in (1, 2)}
ALLOWED = {
    'header_size': equality_checks('header copy'),
    'depth_flag': equality_checks('depth neutral colour post', 'tile depth replay', 'tile colour entry'),
    'cursor_truncated': equality_checks('full cursor return'),
    'pause_stride': equality_checks('pause taken', 'pause untaken', 'full state pause return') |
                    {'pause untaken: dispatch cursor scope', 'pause untaken: direct cursor scope'},
    'l2_clear': equality_checks('L2 lock clear unlock'),
    'record_identity': {f'{name}: {route} cursor scope' for name in IDENTITY_CASES
                        for route in ('dispatch', 'direct')} | {'nested publication restores exact outer cursor'} | PIX_CHECKS,
    'scope_restore': {'nested publication restores exact outer cursor'},
    'coverage_link': equality_checks('coverage linked rectangles'),
    'spin_skip': {'spin route 1 waits for actual producer publication',
                  'spin route 2 waits for actual producer publication'},
    'scope_shortened': PIX_CHECKS,
    'descriptor_mask': equality_checks('identical descriptors', 'ring wrap') | PIX_CHECKS,
}
REQUIRED = {name: set(labels) for name, labels in ALLOWED.items()}
REQUIRED['depth_flag'] = equality_checks('depth neutral colour post')
REQUIRED['pause_stride'] = equality_checks('pause untaken')
REQUIRED['record_identity'] = {'identical descriptors: dispatch cursor scope',
                               'identical descriptors: direct cursor scope'}


def grade(code, raw, defect=None):
    lines = raw.decode('utf-8', errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    summary = re.findall(r'\[NATIVE_PUMP_TEST\] (\d+) passed, (\d+) failed', '\n'.join(lines))
    if summary != [(str(len(good)), str(len(bad)))] or len(good)+len(bad) != CHECKS:
        return False
    if len(set(good+bad)) != CHECKS:
        return False
    if defect is None:
        return code == 0 and not bad
    return code == 1 and REQUIRED[defect] <= set(bad) <= ALLOWED[defect]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root/'kernel/native_pump.cpp'
    original = source.read_bytes()
    (out/'native_pump.original.cpp').write_bytes(original)
    paths = ('kernel/native_pump.cpp', 'kernel/native_pump.hpp', 'kernel/native_memory.hpp',
             'kernel/guest_context.cpp', 'kernel/gpu_write_watch.cpp', 'kernel/gpu_ringless.cpp',
             'kernel/native_transport.cpp', 'guest/ppc/ppc_recomp.2.cpp',
             'tools/difftest/native_pump_test.cpp', 'tools/difftest/native_pump_test_run.py',
             'tools/difftest/test_native_pump_runner.py', 'tools/difftest/native_memory_test_run.py',
             'tools/difftest/difftest_main.cpp', 'CMakeLists.txt')
    evidence = dict(scope='SDK marker control flow and scoped cursor only; no generation, retirement, GPU or release acceptance.',
                    source_sha256={name:digest(root/name) for name in paths}, mutations=[])
    quiescent = True

    def save():
        evidence['process_quiescent'] = quiescent
        (out/'native_pump_matrix.json').write_text(json.dumps(evidence, indent=2)+'\n')

    def run(tag, command, env=None, timeout=300):
        nonlocal quiescent
        command = [str(value) for value in command]
        process = subprocess.Popen(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        quiescent = False
        try:
            stdout, stderr = process.communicate(timeout=timeout)
            code = process.returncode
            quiescent = True
        except subprocess.TimeoutExpired:
            stop_windows_tree(process)
            quiescent = True
            stdout, stderr = process.communicate(timeout=10)
            code = 124
        (out/(tag+'_stdout.txt')).write_bytes(stdout)
        (out/(tag+'_stderr.txt')).write_bytes(stderr)
        row = dict(command=command, exit=code, exe_sha256=digest(root/'build/difftest.exe'),
                   stdout_sha256=hashlib.sha256(stdout).hexdigest(), stderr_sha256=hashlib.sha256(stderr).hexdigest())
        print(json.dumps(dict(tag=tag, exit=code)), flush=True)
        return row, stdout

    def build(tag):
        guard = """$ErrorActionPreference='Stop'
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Less than 8 GiB free RAM' }
if (Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(SupermanReturns|xenia.*|difftest)\\.exe$' }) { throw 'Game or test process is active' }
"""
        checked = subprocess.run(['powershell', '-NoProfile', '-Command', guard], cwd=root,
                                 capture_output=True, timeout=30)
        (out/(tag+'_guard_stdout.txt')).write_bytes(checked.stdout)
        (out/(tag+'_guard_stderr.txt')).write_bytes(checked.stderr)
        if checked.returncode:
            raise RuntimeError('Build resource guard failed')
        env = dict(os.environ)
        env['PATH'] = r'C:\Program Files\LLVM\bin;'+env.get('PATH', '')
        row, _ = run(tag, [sys.executable, 'tools/build_slot.py', '--owner', 'native-pump-controls',
                          '--capacity', '1', '--', 'cmake', '--build', 'build', '--target', 'difftest', '-j', '4'], env)
        return row

    def test(tag, defect=None, enabled=True):
        env = {k:v for k,v in os.environ.items() if not k.upper().startswith('SR_')}
        env.update(SR_NATIVE_REPLAY_MARKERS='1' if enabled else '0', SR_NATIVE_RINGLESS='0',
                   SR_NATIVE_SEGMENTLESS='0', SR_NATIVE_TRANSPORT='0', SR_NATIVE_FLUSH='0',
                   SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                   SR_INSTANCE_ID='native-pump-host')
        row, raw = run(tag, [root/'build/difftest.exe', '--native-pump-test',
                            'native' if enabled else 'gate-off'], env, 30)
        row['summary'] = re.findall(r'\[NATIVE_PUMP_TEST\] (\d+) passed, (\d+) failed', raw.decode(errors='replace'))
        row['valid'] = grade(row['exit'], raw, defect)
        return row

    def mutate(before, after):
        if original.count(before) != 1:
            raise ValueError('Production anchor changed; refusing mutation')
        return original.replace(before, after)

    cases = [
        ('header_size', mutate(b'uint32_t(cursor) + 4u), 248u);', b'uint32_t(cursor) + 4u), 244u);')),
        ('depth_flag', mutate(b'const uint32_t flags = rd(state + 0x28u) & ~0x40000000u;',
                              b'const uint32_t flags = rd(state + 0x28u) & ~0x20000000u;')),
        ('cursor_truncated', mutate(b'uint64_t cursor = ctx.r3.u64;', b'uint64_t cursor = ctx.r3.u32;')),
        ('pause_stride', mutate(b'const uint32_t mask = rd(uint32_t(cursor) + 4u);\n        cursor += 8u;',
                                b'const uint32_t mask = rd(uint32_t(cursor) + 4u);\n        cursor += 4u;')),
        ('l2_clear', mutate(b'std::memset(K::guestVAToHost(0x7F100000u), 0, 0x20000u);',
                            b'std::memset(K::guestVAToHost(0x7F100000u), 1, 0x20000u);')),
        ('record_identity', mutate(b'PublicationScope scope(uint32_t(cursor), state);',
                                   b'PublicationScope scope(state, state);')),
        ('scope_restore', mutate(b'~PublicationScope() { publication = previous; }',
                                 b'~PublicationScope() { publication = {}; }')),
        ('coverage_link', mutate(b'            list = rd(at);', b'            list = 0xC0000000u;')),
        ('spin_skip', mutate(b'case 0x83u: break;', b'case 0x83u: cursor += 4u; break;')),
        ('scope_shortened', mutate(b'        sub_820FD218(work, base);',
                                   b'        publication = {};\n        sub_820FD218(work, base);')),
        ('descriptor_mask', mutate(b'header & 0xFFFFFFu', b'header & 0xFFFFu')),
    ]
    try:
        evidence['positive_build'] = build('positive_build')
        if evidence['positive_build']['exit']:
            raise RuntimeError('Positive build failed')
        evidence['positive'] = test('positive')
        evidence['gate_off'] = test('gate_off', enabled=False)
        save()
        if not evidence['positive']['valid'] or not evidence['gate_off']['valid']:
            raise RuntimeError('Positive SDK differential failed')
        for name, changed in cases:
            if source.read_bytes() != original:
                raise RuntimeError('Concurrent source edit; refusing mutation')
            row = dict(name=name, mutated_sha256=hashlib.sha256(changed).hexdigest())
            evidence['mutations'].append(row)
            source.write_bytes(changed)
            try:
                row['build'] = build(name+'_build')
                if row['build']['exit']:
                    raise RuntimeError('Defect did not compile')
                row['test'] = test(name, name)
                row['gate_off'] = test(name+'_gate_off', enabled=False)
                if not row['test']['valid'] or not row['gate_off']['valid']:
                    raise RuntimeError('Defect specificity or gate-off failed')
            finally:
                if not quiescent or source.read_bytes() != changed:
                    raise RuntimeError('Unsafe restoration or concurrent edit')
                source.write_bytes(original)
                row['source_restored'] = True
                save()
    finally:
        evidence['all_sources_restored'] = all(digest(root/name)==value for name,value in evidence['source_sha256'].items())
        if quiescent and evidence['all_sources_restored']:
            evidence['restored_build'] = build('restored_build')
            if not evidence['restored_build']['exit']:
                evidence['restored'] = test('restored')
        evidence['valid'] = bool(evidence.get('positive', {}).get('valid') and evidence.get('gate_off', {}).get('valid')
                                 and evidence.get('restored', {}).get('valid') and len(evidence['mutations'])==len(cases)
                                 and evidence['all_sources_restored'] and quiescent
                                 and all(row.get('test', {}).get('valid') and row.get('gate_off', {}).get('valid')
                                         and row.get('source_restored') for row in evidence['mutations']))
        save()
    return 0 if evidence['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
