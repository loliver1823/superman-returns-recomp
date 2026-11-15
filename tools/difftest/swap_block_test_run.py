"""Run the actual VdSwap export and pin-transport matrix in isolated processes.

Usage: python tools/difftest/swap_block_test_run.py --out archive/<fresh-bank>
These no-game tests do not establish live swap, pacing or complete 82112050.
"""
import argparse
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
    parser.add_argument('--exe', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    exe = (args.exe or root / 'build/difftest.exe').resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = out / 'swap_block_unit_matrix.json'
    if receipt.exists() or list(out.glob('*_stdout.txt')):
        parser.error('choose a fresh output bank to preserve existing evidence')
    sources = ('kernel/stubs.cpp','kernel/native_flush.cpp','kernel/native_flush.hpp',
               'gfx/pm4_engine.cpp','gfx/pm4_engine.hpp','tools/difftest/swap_block_test.cpp')
    matrix = dict(exe=str(exe), exe_sha256=digest(exe), tool_sha256=digest(Path(__file__)),
                  source_sha256={name:digest(root/name) for name in sources},
                  scope='Actual export and offline recording pin, not live or full SDK swap acceptance.', rows=[])
    cases = [('record', {}, 'normal'),
             ('literal', {'SR_NATIVE_FLUSH_PM4': '1'}, 'normal'),
             ('reconstructed', {'SR_NATIVE_FLUSH_CPPM4': '1'}, 'normal'),
             ('swap_off', {'SR_NATIVE_SWAP_BLOCK': '0'}, 'normal'),
             ('flush_off', {'SR_NATIVE_FLUSH': '0'}, 'normal'),
             ('segmentless_off', {'SR_NATIVE_SEGMENTLESS': '0'}, 'normal'),
             ('unexpected_write_control', {}, 'fault-bytes')]
    for name, settings, mode in cases:
        env = {k: v for k, v in os.environ.items() if not k.startswith('SR_')}
        env.update(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_SWAP_BLOCK='1',
                   SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_CHECK='0',
                   SR_NATIVE_CONST_DEDUP='0', SR_NATIVE_SEG_STRESS='0', SR_NATIVE_EXEC='0',
                   SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0', SR_NATIVE_SHADER_CONST_RUNS='0',
                   SR_NATIVE_DEVCREATE='0', SR_PM4_CP='1', SR_PM4_LIVE='0', SR_ALLOW_FORCING='0',
                   SR_MODS='0', SR_DEVTOOLS='0', SR_HIGHFPS='0', SR_LOG='dev')
        env.update(settings)
        command = [str(exe), '--swap-block-test', mode]
        try:
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, timeout=45)
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', (error.stderr or b'') + b'\nBounded test timeout\n'
        for stream, data in [('stdout',stdout), ('stderr',stderr)]:
            with (out / f'{name}_{stream}.txt').open('xb') as file:
                file.write(data)
        summary = re.findall(rb'\[SWAP_BLOCK_TEST\] (\d+) passed, (\d+) failed',stdout)
        expected_failure = mode == 'fault-bytes'
        expected = (b'59',b'8') if expected_failure else (b'67',b'0')
        valid = code == int(expected_failure) and summary == [expected]
        valid &= stdout.count(b'production SWAP base=') == 8
        valid &= all(marker in stdout for marker in (
            b'[SWAP_BLOCK_CONTROL] export metadata VA=82000000 swap=30 protected=1',
            b'[SWAP_BLOCK_CONTROL] export metadata VA=70654000 swap=60 protected=1',
            b'[SWAP_BLOCK_CONTROL] warmed-cache device protected=1 warmed=1'))
        row = dict(name=name, command=command, exit=code, valid=valid, expected_failure=expected_failure,
                   summary=[dict(passed=int(p),failed=int(f)) for p,f in summary], gates=settings,
                   stdout_sha256=digest(out/f'{name}_stdout.txt'), stderr_sha256=digest(out/f'{name}_stderr.txt'))
        matrix['rows'].append(row)
        receipt.write_text(json.dumps(matrix,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(row),flush=True)
    matrix['unchanged_exe'] = digest(exe) == matrix['exe_sha256']
    matrix['unchanged_sources'] = all(digest(root/name) == value for name,value in matrix['source_sha256'].items())
    matrix['base_gates'] = {k:v for k,v in env.items() if k.startswith('SR_') and k not in settings}
    receipt.write_text(json.dumps(matrix,indent=2)+'\n',encoding='utf-8')
    return int(not matrix['unchanged_exe'] or not matrix['unchanged_sources'] or any(not row['valid'] for row in matrix['rows']))


if __name__ == '__main__':
    raise SystemExit(main())
