#!/usr/bin/env python3
"""Actual native_flush writer/registry/consumer tests and compiled lifetime defects.

python tools/difftest/native_segment_ownership_run.py --out archive/<fresh-bank>
Serial capacity-one builds, four jobs, 8 GiB free; no game or GPU worker.
Tests only ownership and completion. Exact pump identity, expected pass counts,
hard capacity admission and live release acceptance remain separate obligations.
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

CHECKS = 43
ALLOWED = {
    'arena_source': {
        'production writer consumer executes emitted fence after arena reuse',
        'production writer completion belongs to captured record',
        'production writer reclaim audit survives overwritten header',
        'arena reuse before close cannot replace the first consumer\'s bytes',
        'second consumer uses emission bytes after arena wrap and delayed publication',
        'first completed pass still owes the unpublished second pass',
        'execution completion is published after the last part returns',
        'record reclaim audit reads stable completion independent of arena header',
        'owned production consumers report no bad or torn records'},
    'premature_completion': {
        'started consumer is not complete while execution is blocked',
        'registry retains a consumer through delayed execution and bracket expiry'},
    'owed_pass_collection': {
        'capacity collection retains an entry which owes its second pass',
        'capacity pressure retains and reports all live unpublished consumers',
        'capacity pressure does not discard unfinished passes',
        'owed second pass remains publishable after capacity collection'},
    'early_arena_audit': {
        'production writer completion belongs to captured record',
        'old owned execution cannot mark a reused arena slot complete',
        'record and arena audit remain unfinished while execution is blocked',
        'execution completion is published after the last part returns'},
    'producer_lock': {'consumer can finish while producer owns the arena lock'},
    'writer_capture': {
        'production writer capture site owns bytes and registers reclaim audit',
        'production writer consumer executes emitted fence after arena reuse',
        'production writer completion belongs to captured record',
        'production writer reclaim audit survives overwritten header',
        'owned production consumers report no bad or torn records'},
}
REQUIRED = {
    'arena_source': {'arena reuse before close cannot replace the first consumer\'s bytes',
                     'second consumer uses emission bytes after arena wrap and delayed publication'},
    'premature_completion': {'started consumer is not complete while execution is blocked'},
    'owed_pass_collection': {'capacity collection retains an entry which owes its second pass'},
    'early_arena_audit': {'record and arena audit remain unfinished while execution is blocked'},
    'producer_lock': {'consumer can finish while producer owns the arena lock'},
    'writer_capture': {'production writer capture site owns bytes and registers reclaim audit',
                       'production writer consumer executes emitted fence after arena reuse'},
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def grade(code, raw, defect=None):
    lines = raw.decode('utf-8', errors='replace').replace('\r\n', '\n').splitlines()
    good = sum(line.startswith('  ok  ') for line in lines)
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    summary = re.findall(r'\[NATIVE_SEGMENT_OWNERSHIP_TEST\] (\d+) passed, (\d+) failed', '\n'.join(lines))
    if summary != [(str(good), str(len(bad)))] or good + len(bad) != CHECKS:
        return False
    if defect is None:
        return code == 0 and not bad
    return code == 1 and REQUIRED[defect] <= set(bad) <= ALLOWED[defect]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root/'kernel/native_flush.cpp'
    original = source.read_bytes()
    (out/'native_flush.original.cpp').write_bytes(original)
    paths = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'gfx/pm4_engine.cpp',
             'tools/difftest/native_segment_ownership_test.inc',
             'tools/difftest/native_segment_ownership_run.py',
             'tools/difftest/test_native_segment_ownership_runner.py',
             'tools/difftest/difftest_main.cpp', 'CMakeLists.txt')
    evidence = dict(scope='Record ownership and consumer completion only; not full pump, hard capacity or release acceptance.',
                    source_sha256={p:digest(root/p) for p in paths}, mutations=[])
    quiescent = True
    def save():
        evidence['process_quiescent'] = quiescent
        (out/'ownership_matrix.json').write_text(json.dumps(evidence, indent=2)+'\n')
    def run(tag, command, env=None, timeout=300):
        nonlocal quiescent
        command = [str(x) for x in command]
        process = subprocess.Popen(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        quiescent = False
        try:
            stdout, stderr = process.communicate(timeout=timeout)
            code = process.returncode
            quiescent = True
        except subprocess.TimeoutExpired:
            stop_windows_tree(process)
            quiescent = True
            stdout, stderr = process.communicate(timeout=20)
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
        checked = subprocess.run(['powershell','-NoProfile','-Command',guard], cwd=root, capture_output=True, timeout=30)
        (out/(tag+'_guard_stdout.txt')).write_bytes(checked.stdout)
        (out/(tag+'_guard_stderr.txt')).write_bytes(checked.stderr)
        if checked.returncode:
            raise RuntimeError('Build resource guard failed')
        env = dict(os.environ)
        env['PATH'] = r'C:\Program Files\LLVM\bin;'+env.get('PATH','')
        row, _ = run(tag,[sys.executable,'tools/build_slot.py','--owner','native-segment-controls',
                         '--capacity','1','--','cmake','--build','build','--target','difftest','-j','4'],env)
        return row
    def test(tag, defect=None):
        env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
        env.update(SR_NATIVE_SEGMENTLESS='1',SR_NATIVE_FLUSH='1',SR_NATIVE_FLUSH_CPPM4='0')
        row, raw = run(tag,[root/'build/difftest.exe','--native-segment-ownership-test'],env,30)
        row['summary'] = re.findall(r'\[NATIVE_SEGMENT_OWNERSHIP_TEST\] (\d+) passed, (\d+) failed',raw.decode(errors='replace'))
        row['valid'] = grade(row['exit'],raw,defect)
        return row
    def mutate(before, after):
        if original.count(before) != 1:
            raise ValueError('Production anchor changed; refusing mutation')
        return original.replace(before,after)
    cases = [
        ('arena_source', mutate(b'cpApplyRecord(eng, part.pos, part.record.get());',b'cpApplyRecord(eng, part.pos);')),
        ('premature_completion',mutate(b'return finished >= passes && finished == published && started == finished;',
                                      b'return started >= passes && started == published;')),
        ('owed_pass_collection',mutate(b'                            e.complete();',
                                      b'                            e.finished == e.published;')),
        ('early_arena_audit',mutate(b'  const uint32_t* w = reinterpret_cast<const uint32_t*>(r + sizeof(RecHdr));',
                                   b'  __atomic_store_n(&reinterpret_cast<RecHdr*>(g_arena + (pos % kRecArena))->exec, 1u, __ATOMIC_RELEASE);\n'
                                   b'  const uint32_t* w = reinterpret_cast<const uint32_t*>(r + sizeof(RecHdr));')),
        ('producer_lock',mutate(b'    owned->completed.store(true, std::memory_order_release);',
                               b'    std::lock_guard<SpinLock> lk(g_mu);\n'
                               b'    owned->completed.store(true, std::memory_order_release);')),
        ('writer_capture',mutate(b'g_segFeet.push_back(SegFoot{spanStart + 4u, c + 4u, pos, captureSegmentRecord(r, bytes, pos)});',
                                b'g_segFeet.push_back(SegFoot{spanStart + 4u, c + 4u, pos, nullptr});')),
    ]
    try:
        evidence['positive_build'] = build('positive_build')
        if evidence['positive_build']['exit']:
            raise RuntimeError('Positive build failed')
        evidence['positive'] = test('positive')
        save()
        if not evidence['positive']['valid']:
            raise RuntimeError('Positive ownership fixture failed')
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
                if not row['test']['valid']:
                    raise RuntimeError('Defect specificity failed')
            finally:
                if not quiescent or source.read_bytes() != changed:
                    raise RuntimeError('Unsafe restoration or concurrent edit')
                source.write_bytes(original)
                row['source_restored'] = True
                save()
    finally:
        evidence['all_sources_restored'] = all(digest(root/p)==v for p,v in evidence['source_sha256'].items())
        if quiescent and evidence['all_sources_restored']:
            evidence['restored_build'] = build('restored_build')
            if not evidence['restored_build']['exit']:
                evidence['restored'] = test('restored')
        evidence['valid'] = bool(evidence.get('positive',{}).get('valid') and evidence.get('restored',{}).get('valid')
                                 and len(evidence['mutations'])==len(cases) and evidence['all_sources_restored'] and quiescent
                                 and all(x.get('test',{}).get('valid') and x.get('source_restored') for x in evidence['mutations']))
        save()
    return 0 if evidence['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
