"""Execute native permission checks, safety defects and restored SDK contracts.

python tools/difftest/native_memory_test_run.py --out archive/<fresh-bank>
Requires a built difftest. Serial build_slot rebuilds, no game launches. Exact
header restoration waits for confirmed process termination. Failed cleanup
leaves the mutant in place and stops without rebuilding; original bytes remain
in native_memory.original.hpp for recovery after the process tree has stopped.
No speed, pixels or Native0 proof.
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

def digest(data):
    return hashlib.sha256(data).hexdigest()

def grade_defect(name, code, raw):
    """Accept only the named defect's failures, with complete 36-check accounting."""
    raw = raw.replace(b'\r\n', b'\n')
    failures = re.findall(rb'^  FAIL: (.+)$', raw, re.MULTILINE)
    passes = re.findall(rb'^  ok  (.+)$', raw, re.MULTILINE)
    summary = re.findall(rb'\[NATIVE_MEMORY_TEST\] (\d+) passed, (\d+) failed', raw)
    invalid = b'successful query with invalid writable-looking fields uses complete fallback'
    query_failed = b'failed query with valid writable-looking fields uses complete fallback'
    bad = b'valid query reporting a bad writable page rejects'
    protected = {('mixed protected page%d protection%X rejects, guard/protection unchanged, restore succeeds' %
                  (page, protection)).encode() for page in (1, 3) for protection in (1, 2, 0x104)}
    allowed = {
        'ignored_validity': {invalid, b'real nonresident committed pages accept through fallback',
                             b'RW to protected to RW sees fresh permissions',
                             b'recommitted interior accepts fresh range'},
        'ignored_query_failure': {query_failed},
        'first_page_only': protected | {b'uncommitted interior rejects complete range'},
        'ignored_bad_page': {bad},
    }
    required = {'ignored_validity': {invalid}, 'ignored_query_failure': {query_failed},
                'first_page_only': {label for label in protected if b'page1 ' in label},
                'ignored_bad_page': {bad}}
    return code == 1 and len(summary) == 1 and len(passes) + len(failures) == 36 and \
        tuple(map(int, summary[0])) == (len(passes), len(failures)) and \
        bool(required[name].intersection(failures)) and set(failures).issubset(allowed[name])

def stop_windows_tree(process):
    """Keep the timed-out root alive until taskkill can stop its descendants too."""
    script = '''$ErrorActionPreference='Stop'
$tracked = [System.Collections.Generic.HashSet[int]]::new()
[void]$tracked.Add(%d)
$tree = @(Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId)
do {
  $added = $false
  foreach ($entry in $tree) {
    if ($tracked.Contains([int]$entry.ParentProcessId) -and $tracked.Add([int]$entry.ProcessId)) { $added = $true }
  }
} while ($added)
& taskkill.exe /PID %d /T /F 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Timed-out process tree could not be stopped' }
$remaining = @(Get-CimInstance Win32_Process | Where-Object { $tracked.Contains([int]$_.ProcessId) })
if ($remaining.Count) { throw 'Timed-out descendants remain; source restoration is unsafe' }
''' % (process.pid, process.pid)
    subprocess.run(['powershell','-NoProfile','-Command',script],check=True,capture_output=True,timeout=30)
    process.wait(timeout=10)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    header = root / 'kernel/native_memory.hpp'
    original = header.read_bytes()
    (out/'native_memory.original.hpp').write_bytes(original)
    env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env['PATH']
    if os.name == 'nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
    exe = root / 'build/difftest.exe'
    sources = ('kernel/native_memory.hpp','kernel/native_flush.cpp','tools/difftest/native_memory_test.cpp',
               'tools/difftest/native_memory_test_run.py','tools/difftest/test_native_memory_runner.py',
               'tools/difftest/difftest_main.cpp','CMakeLists.txt')
    evidence = dict(scope='Fresh native output permission admission; no live or performance acceptance.',
                    source_sha256={name:digest((root/name).read_bytes()) for name in sources},
                    original_exe_sha256=digest(exe.read_bytes()), original_header_sha256=digest(original), mutations=[])
    def save():
        (out/'native_memory_matrix.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
    process_quiescent = True
    def run(tag, command, timeout=120):
        nonlocal process_quiescent
        with (out/(tag+'_stdout.txt')).open('xb') as stdout, (out/(tag+'_stderr.txt')).open('xb') as stderr:
            process = subprocess.Popen(command,cwd=root,env=env,stdout=stdout,stderr=stderr)
            process_quiescent = False
            try:
                code = process.wait(timeout=timeout)
                process_quiescent = True
            except subprocess.TimeoutExpired:
                stop_windows_tree(process)
                process_quiescent = True
                code = 124
                stderr.write(b'\nBounded timeout; process tree stopped and checked\n')
        raw = (out/(tag+'_stdout.txt')).read_bytes()
        row = dict(command=[str(x) for x in command],exit=code,exe_sha256=digest(exe.read_bytes()),
                   stdout_sha256=digest(raw),stderr_sha256=digest((out/(tag+'_stderr.txt')).read_bytes()))
        print(json.dumps(dict(tag=tag,**row)),flush=True)
        return row,raw
    def test(tag):
        row,raw = run(tag,[exe,'--native-memory-test'],30)
        summary = re.findall(rb'\[NATIVE_MEMORY_TEST\] (\d+) passed, (\d+) failed',raw)
        row['summary'] = [[int(a),int(b)] for a,b in summary]
        row['valid'] = row['exit'] == 0 and summary == [(b'36',b'0')]
        return row,raw
    def build(tag):
        guard = '''$ErrorActionPreference='Stop'
if (@(Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe' OR Name='xenia_canary.exe'").Count) { throw 'Wait for games before building' }
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Build RAM headroom insufficient' }
'''
        subprocess.run(['powershell','-NoProfile','-Command',guard],check=True,capture_output=True)
        row,_ = run(tag,[sys.executable,'tools/build_slot.py','--owner','native-memory-controls','--capacity','1',
                         '--','cmake','--build','build','--target','difftest','-j','4'],300)
        return row
    def replace(old,new):
        if original.count(old) != 1: raise ValueError('Source contract changed; refusing mutation')
        return original.replace(old,new)
    evidence['positive'],_ = test('positive')
    save()
    if not evidence['positive']['valid']: return 1
    cases = [
        ('ignored_validity', replace(b'if (!attributes.Valid) return writableVirtualRange(pointer, bytes);',b'/* ignore page validity */')),
        ('ignored_query_failure', replace(b'if (!query(GetCurrentProcess(), pages, DWORD(count * sizeof(pages[0]))))\n    return writableVirtualRange(pointer, bytes);',
                                         b'(void)query(GetCurrentProcess(), pages, DWORD(count * sizeof(pages[0])));')),
        ('first_page_only', replace(b'for (size_t i = 0; i < count; ++i) {\n    const auto attributes',b'for (size_t i = 0; i < 1u; ++i) {\n    const auto attributes')),
        ('ignored_bad_page', replace(b'attributes.Bad || !writableProtection',b'!writableProtection')),
    ]
    try:
        for name,mutated in cases:
            if header.read_bytes() != original: raise ValueError('Concurrent source edit; refusing mutation')
            row = dict(name=name,mutated_sha256=digest(mutated))
            evidence['mutations'].append(row)
            header.write_bytes(mutated)
            try:
                row['build'] = build(name+'_build')
                if row['build']['exit']: raise RuntimeError('Defect failed to build')
                row['test'],raw = test(name+'_test')
                row['detected'] = grade_defect(name, row['test']['exit'], raw)
                if not row['detected']: raise RuntimeError('Expected admission defect was not detected')
            finally:
                if not process_quiescent: raise RuntimeError('Process tree not confirmed stopped; refusing source restoration')
                if header.read_bytes() != mutated: raise ValueError('Concurrent edit; refusing to overwrite source')
                header.write_bytes(original)
                row['source_restored'] = header.read_bytes() == original
                save()
    finally:
        evidence['all_sources_restored'] = all(digest((root/name).read_bytes()) == value for name,value in evidence['source_sha256'].items())
        evidence['process_quiescent'] = process_quiescent
        if evidence['all_sources_restored'] and process_quiescent:
            evidence['restored_build'] = build('restored_build')
            if evidence['restored_build']['exit'] == 0:
                evidence['restored'],_ = test('restored')
                evidence['sdk_restored'],_ = run('sdk_restored',[sys.executable,'tools/difftest/swap_sdk_test_run.py','--out',str(out/'sdk_restored')],90)
        evidence['valid'] = len(evidence['mutations']) == 4 and all(r.get('detected') and r.get('source_restored') for r in evidence['mutations']) and \
            evidence['all_sources_restored'] and evidence.get('restored',{}).get('valid',False) and evidence.get('sdk_restored',{}).get('exit') == 0
        save()
    print(json.dumps(dict(valid=evidence['valid'],receipt=str(out/'native_memory_matrix.json'))),flush=True)
    return int(not evidence['valid'])

if __name__ == '__main__': raise SystemExit(main())
