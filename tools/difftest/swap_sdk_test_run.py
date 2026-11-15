"""Run normal SDK swap contracts, production wiring and disabled paths.

Usage: python tools/difftest/swap_sdk_test_run.py --out archive/<fresh-bank>
No game or graphics worker is launched; this cannot prove live pacing or Native0.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    exe = root / 'build/difftest.exe'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = out / 'swap_sdk_unit_matrix.json'
    if receipt.exists() or list(out.glob('*_stdout.txt')):
        parser.error('choose a fresh evidence bank')
    sources = ('kernel/native_flush.cpp','kernel/native_flush.hpp','kernel/native_memory.hpp','kernel/hle_api_observe.cpp',
               'kernel/native_display_emit.cpp','tools/difftest/swap_sdk_test.cpp','tools/difftest/swap_sdk_test_run.py')
    matrix = dict(exe=str(exe),exe_sha256=digest(exe),source_sha256={s:digest(root/s) for s in sources},rows=[])
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    cases = [('direct', {}, 'direct'),('leaf', {}, 'leaf'),('indirect', {}, 'indirect'),
             ('shader_reentry', {'SR_NATIVE_SHADER_CONST_RUNS':'1'}, 'reentry'),
             ('mutating_allocator', {}, 'mutating'),
             ('literal', {'SR_NATIVE_FLUSH_PM4':'1'}, 'direct'),
             ('reconstructed', {'SR_NATIVE_FLUSH_CPPM4':'1'}, 'direct'),
             ('sdk_off', {'SR_NATIVE_SWAP':'0','SR_NATIVE_SWAP_BLOCK':'0'}, 'direct'),
             ('block_off', {'SR_NATIVE_SWAP_BLOCK':'0'}, 'direct'),
             ('flush_off', {'SR_NATIVE_FLUSH':'0'}, 'direct'),
             ('segmentless_off', {'SR_NATIVE_SEGMENTLESS':'0','SR_NATIVE_TILERECTS':'0'}, 'direct'),
             # the mem.resources bake (native/mem-native a1fda568, 2026-10-17) made the native resource route the dev default, where the GPU write tracker is off: these cases test the page model's tracker, so they run at SR_MEM_RESOURCES=0 (the oracle arm); the native default's rows are composed_suites_run.py's *_native and native_route_admission_run.py
             ('tracker', {'SR_MEM_RESOURCES':'0'}, 'tracker'),
             ('fence_control', {}, 'fault-fence')]
    # The baked rows 5 / 3b (SR_NATIVE_TILERECTS / _TILEPRED) run at their default (ON) - no pins since 2026-10-14: SWAP
    # on, the oracle is scoped (where they decline); SWAP off, the oracle is the unscoped production route (where they
    # take Swap's re-emit as the route does). One pin stays, row 5 in segmentless_off only: with SEGMENTLESS=0 its
    # record is a marker-path span, whose 16-byte NOP marker carries the record's arena position (native_flush.cpp,
    # the marker's third word), advancing 0x28 per record - two sequential executions cannot write equal bytes (the
    # transport-fix lane's sweep, 2026-10-14: unpinned or 3b-pinned 103/22, first difference at 90642038 oracle
    # 00000000 / actual 00000028; row 5 pinned 125/0). That case's subject is SWAP's segmentless prerequisite.
    # tracker: the tcmp1 diagnosis's tracker-owned reserved blocks and the post-swap fallbacks (28 more assertions).
    base = dict(SR_NATIVE_SWAP='1',SR_NATIVE_SWAP_BLOCK='1',SR_NATIVE_FLUSH='1',SR_NATIVE_SEGMENTLESS='1',
                SR_NATIVE_FLUSH_PM4='0',SR_NATIVE_FLUSH_CPPM4='0',SR_NATIVE_FLUSH_CHECK='0',
                SR_NATIVE_CONST_DEDUP='0',SR_NATIVE_GATE='0',
                SR_NATIVE_CALLBACK='0',SR_NATIVE_SHADER_CONST_RUNS='0',SR_NATIVE_DEVCREATE='0',SR_NATIVE_EXEC='0',
                SR_PM4_CP='1',SR_PM4_LIVE='0',SR_ALLOW_FORCING='0',SR_MODS='0',SR_DEVTOOLS='0',SR_HIGHFPS='0',
                SR_API_OBSERVE='0',SR_API_PRESENT_BLOCK='0',SR_LOG='dev')
    # boot gaps (native/boot-gaps, 2026-10-16): SR_NATIVE_DISPLAY (default ON, the display leaves on the native transport)
    # adds the persisted-display cases (5, at two offsets: 32 SDK swaps, each with its "boot gaps" check) and the leaves'
    # byte cases; with the transport off (flush_off / segmentless_off) the leaves stand down: 22 SDK swaps, the persisted
    # display declining (flags2A39) as before. The fault-fence control fails every SDK swap's route check (32).
    expected = dict(tracker=[(b'236',b'0')],fence_control=[(b'176',b'32')],flush_off=[(b'149',b'0')],
                    segmentless_off=[(b'149',b'0')])
    swaps = dict(flush_off=22,segmentless_off=22)
    for name,settings,mode in cases:
        env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
        env.update(base); env.update(settings)
        command = [str(exe),'--swap-sdk-test',mode]
        try:
            result = subprocess.run(command,cwd=root,env=env,capture_output=True,timeout=45)
            code,stdout,stderr = result.returncode,result.stdout,result.stderr
        except subprocess.TimeoutExpired as error:
            code,stdout,stderr = 124,error.stdout or b'',(error.stderr or b'') + b'\nBounded timeout\n'
        for stream,data in [('stdout',stdout),('stderr',stderr)]:
            with (out/f'{name}_{stream}.txt').open('xb') as file:
                file.write(data)
        summary = re.findall(rb'\[SWAP_SDK_TEST\] (\d+) passed, (\d+) failed',stdout)
        control = name == 'fence_control'
        valid = code == int(control) and summary == expected.get(name,[(b'208',b'0')])
        valid &= stdout.count(b'SDK swap fmt=') == swaps.get(name,32)
        row = dict(name=name,command=command,gates=settings,exit=code,valid=valid,expected_failure=control,
                   summary=[dict(passed=int(p),failed=int(f)) for p,f in summary],
                   stdout_sha256=digest(out/f'{name}_stdout.txt'),stderr_sha256=digest(out/f'{name}_stderr.txt'))
        matrix['rows'].append(row)
        receipt.write_text(json.dumps(matrix,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(row),flush=True)
        if not valid:
            matrix['stopped_on_failure'] = name
            break
    matrix['base_gates'] = base
    matrix['unchanged_exe'] = digest(exe) == matrix['exe_sha256']
    matrix['unchanged_sources'] = all(digest(root/s) == h for s,h in matrix['source_sha256'].items())
    receipt.write_text(json.dumps(matrix,indent=2)+'\n',encoding='utf-8')
    return int(len(matrix['rows']) != len(cases) or not matrix['unchanged_exe'] or
               not matrix['unchanged_sources'] or any(not r['valid'] for r in matrix['rows']))


if __name__ == '__main__':
    raise SystemExit(main())
