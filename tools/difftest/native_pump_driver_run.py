#!/usr/bin/env python3
"""Bounded actual-SDK replay-driver differential and compiled defect controls.

python tools/difftest/native_pump_driver_run.py --out archive/<fresh-bank>
Serial capacity-one builds, four jobs, 8 GiB free; no game or GPU worker.
Generation binding, final sealing, native-zero and release proof remain separate.
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

CHECKS=115


def equalities(*names):
    return {f'{name}: {route} equals actual SDK' for name in names for route in ('dispatch','direct')}


REQUIRED={
    'empty_return': equalities('empty driver'),
    'resume_source': equalities('resume driver','pause and kick resume driver'),
    'repeat_width': equalities('wide repeat driver'),
    'repeat_zero': equalities('zero repeat driver'),
    'context_width': equalities('full context pointer driver'),
    'handler_context': equalities('handler switch driver'),
    'driver_frame': equalities('one repeat driver','full stack pointer driver'),
    'late_handler': equalities('handler switch iteration 0','handler switch iteration 1'),
}
ALLOWED={name:set(labels) for name,labels in REQUIRED.items()}
ALLOWED['empty_return']|={f'contended driver route {route} preserves empty return and lock state' for route in (1,2)}
ALLOWED['driver_frame']=equalities('zero repeat driver','one repeat driver','three repeats driver','wide repeat driver',
    'full context pointer driver','full stack pointer driver','handler switch driver') | {
        f'participants order {order}: {route} equals actual SDK' for order in (1,2) for route in ('dispatch','direct')}
ALLOWED['driver_frame']|=equalities('handler switch iteration 0','handler switch iteration 1')


def grade(code,raw,defect=None):
    lines=raw.decode('utf-8',errors='replace').replace('\r\n','\n').splitlines()
    good=[line[6:] for line in lines if line.startswith('  ok  ')]
    bad=[line[6:] for line in lines if line.startswith('FAIL: ')]
    summary=re.findall(r'\[NATIVE_PUMP_DRIVER_TEST\] (\d+) passed, (\d+) failed','\n'.join(lines))
    if summary!=[(str(len(good)),str(len(bad)))] or len(good)+len(bad)!=CHECKS or len(set(good+bad))!=CHECKS:
        return False
    return (code==0 and not bad) if defect is None else (code==1 and REQUIRED[defect]<=set(bad)<=ALLOWED[defect])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',required=True,type=Path)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    source=root/'kernel/native_pump.cpp';original=source.read_bytes()
    (out/'native_pump.original.cpp').write_bytes(original)
    paths=('kernel/native_pump.cpp','kernel/native_pump.hpp','kernel/alloc_overlap_watch.cpp','kernel/native_memory.hpp',
        'kernel/guest_context.cpp','kernel/gpu_write_watch.cpp','kernel/gpu_ringless.cpp','kernel/native_transport.cpp',
        'kernel/stubs.cpp','kernel/kpcr.cpp','guest/ppc/ppc_recomp.2.cpp',
        'tools/difftest/native_pump_driver_test.cpp','tools/difftest/native_pump_driver_run.py',
        'tools/difftest/test_native_pump_driver_runner.py','tools/difftest/native_memory_test_run.py',
        'tools/difftest/difftest_main.cpp','CMakeLists.txt')
    evidence=dict(scope='CPU replay-driver semantics and rendezvous only; no generation binding, final sealing, native-zero or release acceptance.',
        source_sha256={name:digest(root/name) for name in paths},mutations=[])
    quiescent=True

    def save():
        evidence['process_quiescent']=quiescent
        (out/'driver_matrix.json').write_text(json.dumps(evidence,indent=2)+'\n')

    def run(tag,command,env,timeout=300):
        nonlocal quiescent
        command=[str(value) for value in command]
        p=subprocess.Popen(command,cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE);quiescent=False
        try:stdout,stderr=p.communicate(timeout=timeout);code=p.returncode;quiescent=True
        except subprocess.TimeoutExpired:
            stop_windows_tree(p);quiescent=True;stdout,stderr=p.communicate(timeout=10);code=124
        (out/(tag+'_stdout.txt')).write_bytes(stdout);(out/(tag+'_stderr.txt')).write_bytes(stderr)
        row=dict(command=command,exit=code,exe_sha256=digest(root/'build/difftest.exe'),
            stdout_sha256=hashlib.sha256(stdout).hexdigest(),stderr_sha256=hashlib.sha256(stderr).hexdigest())
        print(json.dumps(dict(tag=tag,exit=code)),flush=True);return row,stdout

    def build(tag):
        guard="""$ErrorActionPreference='Stop'
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Less than 8 GiB free RAM' }
if (Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(SupermanReturns|xenia.*|difftest)\\.exe$' }) { throw 'Game or test active' }
"""
        checked=subprocess.run(['powershell','-NoProfile','-Command',guard],cwd=root,capture_output=True,timeout=30)
        (out/(tag+'_guard_stdout.txt')).write_bytes(checked.stdout);(out/(tag+'_guard_stderr.txt')).write_bytes(checked.stderr)
        if checked.returncode:raise RuntimeError('Build resource guard failed')
        env=dict(os.environ);env['PATH']=r'C:\Program Files\LLVM\bin;'+env.get('PATH','')
        row,_=run(tag,[sys.executable,'tools/build_slot.py','--owner','native-driver-controls','--capacity','1','--',
            'cmake','--build','build','--target','difftest','-j','4'],env);return row

    def test(tag,defect=None,enabled=True,integrated=False):
        env={k:v for k,v in os.environ.items() if not k.upper().startswith('SR_')}
        env.update(SR_NATIVE_REPLAY_DRIVER='1' if enabled else '0',SR_NATIVE_REPLAY_MARKERS='1' if integrated else '0',
            SR_NATIVE_TRANSPORT='1' if integrated else '0',SR_NATIVE_RINGLESS='0',SR_NATIVE_SEGMENTLESS='0',SR_NATIVE_FLUSH='0',
            SR_PM4_RING='0',SR_PM4_RING_EXEC='0',SR_PM4_CP='0',SR_ALLOW_FORCING='0',SR_INSTANCE_ID='native-driver-host')
        row,raw=run(tag,[root/'build/difftest.exe','--native-pump-driver-test','native' if enabled else 'gate-off'],env,30)
        row['summary']=re.findall(r'\[NATIVE_PUMP_DRIVER_TEST\] (\d+) passed, (\d+) failed',raw.decode(errors='replace'))
        row['valid']=grade(row['exit'],raw,defect);return row

    def mutate(before,after,count=1):
        if original.count(before)!=count:raise ValueError('Production anchor changed; refusing mutation')
        return original.replace(before,after)

    cases=[
        ('empty_return',mutate(b'return true;  // SDK returns the incoming context pointer unchanged.',
            b'ctx.r3.u64 = 0u; return true;  // SDK returns the incoming context pointer unchanged.')),
        ('resume_source',mutate(b'cursor = rd(state + 0x50u);',b'cursor = 0u;')),
        ('repeat_width',mutate(b'writeMarkerWord(state + 0x1Cu, header & 0xFFFFu);',
            b'writeMarkerWord(state + 0x1Cu, header & 0xFFu);')),
        ('repeat_zero',mutate(b'if (!(header & 0x80000000u)) {',
            b'if (!(header & 0x80000000u)) {\n      if (!(header & 0xFFFFu)) { cursor += ((header >> 16) & 0x7FFFu) + 4u; continue; }')),
        ('context_width',mutate(b'work.r3.u64 = ctx.r3.u64;',b'work.r3.u64 = ctx.r3.u32;',2)),
        ('handler_context',mutate(b'writeMarkerWord(state + 0x14u, rd(uint32_t(cursor) + 8u));',
            b'writeMarkerWord(state + 0x14u, rd(uint32_t(cursor) + 4u));')),
        ('driver_frame',mutate(b'work.r1.u32 = stack - 0x80u;',b'work.r1.u32 = stack - 0x90u;')),
        ('late_handler',mutate(b'PPC_LOOKUP_FUNC(base, handler)(work, base);',
            b'PPC_LOOKUP_FUNC(base, rd(state + 0x10u))(work, base);',2)),
    ]
    try:
        evidence['positive_build']=build('positive_build')
        if evidence['positive_build']['exit']:raise RuntimeError('Positive build failed')
        for tag,enabled,integrated in [('positive',True,False),('gate_off',False,False),('integrated',True,True),('integrated_gate_off',False,True)]:
            evidence[tag]=test(tag,enabled=enabled,integrated=integrated)
            if not evidence[tag]['valid']:raise RuntimeError('Positive SDK differential failed')
        save()
        for name,changed in cases:
            if source.read_bytes()!=original:raise RuntimeError('Concurrent source edit; refusing mutation')
            row=dict(name=name,mutated_sha256=hashlib.sha256(changed).hexdigest());evidence['mutations'].append(row)
            source.write_bytes(changed)
            try:
                row['build']=build(name+'_build')
                if row['build']['exit']:raise RuntimeError('Defect did not compile')
                row['test']=test(name,name);row['gate_off']=test(name+'_gate_off',enabled=False)
                if not row['test']['valid'] or not row['gate_off']['valid']:raise RuntimeError('Defect specificity or gate-off failed')
            finally:
                if not quiescent or source.read_bytes()!=changed:raise RuntimeError('Unsafe restoration or concurrent source edit')
                source.write_bytes(original);row['source_restored']=True;save()
    finally:
        evidence['all_sources_restored']=all(digest(root/name)==value for name,value in evidence['source_sha256'].items())
        if quiescent and evidence['all_sources_restored']:
            evidence['restored_build']=build('restored_build')
            if not evidence['restored_build']['exit']:
                evidence['restored']=test('restored');evidence['restored_integrated']=test('restored_integrated',integrated=True)
        evidence['valid']=bool(all(evidence.get(tag,{}).get('valid') for tag in ('positive','gate_off','integrated','integrated_gate_off','restored','restored_integrated'))
            and len(evidence['mutations'])==len(cases) and evidence['all_sources_restored'] and quiescent
            and all(row.get('test',{}).get('valid') and row.get('gate_off',{}).get('valid') and row.get('source_restored') for row in evidence['mutations']))
        save()
    return 0 if evidence['valid'] else 1


if __name__=='__main__':raise SystemExit(main())
