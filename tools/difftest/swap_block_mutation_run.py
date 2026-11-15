"""Execute production SWAP wiring and protected-metadata defect controls.

Usage: python tools/difftest/swap_block_mutation_run.py --out archive/<fresh-bank>
       --matrix archive/<passed-bank>/swap_block_unit_matrix.json
Mutates only the exact owned source snippets, restores their original bytes,
and rebuilds through build_slot. Close game processes before running.
This proves offline test sensitivity; it does not accept live or full SDK swap.
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


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    matrix = json.loads(args.matrix.read_text(encoding='utf-8'))
    if not matrix.get('unchanged_exe') or any(not row['valid'] for row in matrix['rows']):
        parser.error('matrix must record a passed, unchanged executable')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = out / 'swap_block_mutations.json'
    env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
    env.update(matrix['base_gates'])
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env['PATH']
    # Expected access violations must terminate without showing Windows dialogs.
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    sources = {name: (root / name).read_bytes() for name in ('kernel/stubs.cpp','kernel/native_flush.cpp')}
    evidence = dict(scope='Offline defect sensitivity only; no live/full-SDK acceptance.',
                    tool_sha256=sha(Path(__file__).read_bytes()),
                    original_sources={name:sha(data) for name,data in sources.items()},
                    gates=matrix['base_gates'], rows=[])

    def save():
        receipt.write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')

    def run(name, command, settings=None, timeout=300):
        child_env = dict(env)
        child_env.update(settings or {})
        with (out/f'{name}_stdout.txt').open('xb') as stdout, (out/f'{name}_stderr.txt').open('xb') as stderr:
            code = subprocess.run(command,cwd=root,env=child_env,stdout=stdout,stderr=stderr,timeout=timeout).returncode
        result = dict(exit=code,stdout_sha256=sha((out/f'{name}_stdout.txt').read_bytes()),
                      stderr_sha256=sha((out/f'{name}_stderr.txt').read_bytes()))
        print(json.dumps(dict(name=name,**result)),flush=True)
        return result

    build = [sys.executable,'tools/build_slot.py','--owner','release-swap-block-controls','--capacity','1',
             '--','cmake','--build','build','--target','difftest','-j','4']
    test = [str(root/'build/difftest.exe'),'--swap-block-test','normal']
    guard = """$ErrorActionPreference='Stop'
if (@(Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe' OR Name='xenia_canary.exe'").Count) { throw 'Wait for games before building' }
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Build RAM headroom insufficient' }
"""
    def build_checked(name):
        subprocess.run(['powershell','-NoProfile','-Command',guard],check=True,capture_output=True)
        return run(name,build)

    def replace(data, old, new):
        if data.count(old) != 1:
            raise ValueError('Source snippet changed; refusing mutation')
        return data.replace(old,new)

    stubs = sources['kernel/stubs.cpp']
    flush = sources['kernel/native_flush.cpp']
    wiring = replace(stubs,b'if (sr::native::flush::swapBlockOn() &&',
                     b'if (false && sr::native::flush::swapBlockOn() &&')
    unsafe_root = replace(stubs,
        b'ReadProcessMemory(GetCurrentProcess(), first, bytes, sizeof(bytes), &copied)',
        b'(std::memcpy(bytes, first, sizeof(bytes)), copied = sizeof(bytes), true)')
    unsafe_device = replace(flush,b'uint8_t device[0x5000u];',
                            b'const uint8_t* device = gateSpan(dev, 0x5000u);')
    unsafe_device = replace(unsafe_device,b'!gateRead(dev, device, sizeof(device)) || blockVA',b'!device || blockVA')
    cases = [('removed_wiring','kernel/stubs.cpp',wiring,None),
             ('unsafe_root_cell','kernel/stubs.cpp',unsafe_root,
              b'[SWAP_BLOCK_CONTROL] export metadata VA=82000000 swap=30 protected=1'),
             ('cached_device_read','kernel/native_flush.cpp',unsafe_device,
              b'[SWAP_BLOCK_CONTROL] warmed-cache device protected=1 warmed=1')]
    try:
        for name, source_name, mutated, marker in cases:
            source = root/source_name
            if any((root/n).read_bytes() != data for n,data in sources.items()):
                raise ValueError('Concurrent source edit; refusing mutation')
            row = dict(name=name,source=source_name,mutated_sha256=sha(mutated))
            evidence['rows'].append(row)
            source.write_bytes(mutated)
            try:
                row['build'] = build_checked(name+'_build')
                if row['build']['exit']:
                    raise RuntimeError('Mutant failed to build')
                row['exe_sha256'] = sha((root/'build/difftest.exe').read_bytes())
                row['test'] = run(name+'_test',test,timeout=45)
                output = (out/f'{name}_test_stdout.txt').read_bytes()
                if marker is None:
                    row['detected'] = row['test']['exit'] == 1 and \
                        len(re.findall(rb'FAIL: production SWAP base=.*',output)) == 8 and \
                        b'[SWAP_BLOCK_TEST] 59 passed, 8 failed' in output
                else:
                    row['detected'] = (row['test']['exit'] & 0xFFFFFFFF) == 0xC0000005 and \
                        marker in output and b'[SWAP_BLOCK_TEST]' not in output
                row['gate_off_test'] = run(name+'_gate_off_test',test,{'SR_NATIVE_SWAP_BLOCK':'0'},timeout=45)
                off_output = (out/f'{name}_gate_off_test_stdout.txt').read_bytes()
                if name == 'unsafe_root_cell':
                    row['gate_off_valid'] = (row['gate_off_test']['exit'] & 0xFFFFFFFF) == 0xC0000005 and \
                        marker in off_output and b'[SWAP_BLOCK_TEST]' not in off_output
                else:
                    row['gate_off_valid'] = row['gate_off_test']['exit'] == 0 and \
                        b'[SWAP_BLOCK_TEST] 67 passed, 0 failed' in off_output
            finally:
                if source.read_bytes() != mutated:
                    raise ValueError('Concurrent edit; refusing to overwrite source')
                source.write_bytes(sources[source_name])
                row['source_restored'] = source.read_bytes() == sources[source_name]
                save()
    finally:
        evidence['all_sources_restored'] = all((root/n).read_bytes() == data for n,data in sources.items())
        if evidence['all_sources_restored']:
            evidence['restored_build'] = build_checked('restored_build')
            if evidence['restored_build']['exit'] == 0:
                evidence['restored_test'] = run('restored_test',test,timeout=45)
                evidence['restored_test_valid'] = evidence['restored_test']['exit'] == 0 and \
                    b'[SWAP_BLOCK_TEST] 67 passed, 0 failed' in (out/'restored_test_stdout.txt').read_bytes()
        save()
    valid = len(evidence['rows']) == 3 and all(row['detected'] and row['gate_off_valid'] and
                row['source_restored'] for row in evidence['rows']) and evidence['all_sources_restored'] and \
                evidence.get('restored_test_valid',False)
    print(json.dumps(dict(valid=valid,receipt=str(receipt))),flush=True)
    return int(not valid)


if __name__ == '__main__':
    raise SystemExit(main())
