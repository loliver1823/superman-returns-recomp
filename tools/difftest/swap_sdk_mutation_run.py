"""Verify SDK swap wiring and Astra's callback fixes with executed defects.

Usage: python tools/difftest/swap_sdk_mutation_run.py --out archive/<fresh-bank>
       --matrix archive/<passed-bank>/swap_sdk_unit_matrix.json
Restores exact source bytes and rebuilds through build_slot, even on failure.
Offline sensitivity only; no live pacing, whole SDK or Native0 acceptance.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    matrix = json.loads(args.matrix.read_text(encoding='utf-8'))
    # 13 rows since 2026-10-14 (swap_sdk_test_run.py's tracker case); a 12-row matrix is the earlier runner's
    if len(matrix['rows']) not in (12, 13) or not matrix.get('unchanged_exe') or not matrix.get('unchanged_sources') or \
            any(not row['valid'] for row in matrix['rows']):
        parser.error('requires a complete passed matrix with unchanged sources and executable')
    exe = root / 'build/difftest.exe'
    if digest(exe.read_bytes()) != matrix['exe_sha256'] or \
            any(digest((root/name).read_bytes()) != value for name,value in matrix['source_sha256'].items()):
        parser.error('current sources or executable differ from the passed matrix')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = out / 'swap_sdk_mutations.json'
    env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
    env.update(matrix['base_gates'])
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env['PATH']
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    sources = {name:(root/name).read_bytes() for name in ('kernel/hle_api_observe.cpp','kernel/native_flush.cpp')}
    evidence = dict(scope='Offline SDK swap defect sensitivity only; no live or Native0 acceptance.',
                    tool_sha256=digest(Path(__file__).read_bytes()), original_exe_sha256=matrix['exe_sha256'],
                    original_sources={name:digest(data) for name,data in sources.items()}, rows=[])

    def save():
        receipt.write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')

    def run(name, command, settings=None, timeout=300):
        child_env = dict(env)
        child_env.update(settings or {})
        with (out/f'{name}_stdout.txt').open('xb') as stdout, (out/f'{name}_stderr.txt').open('xb') as stderr:
            try:
                code = subprocess.run(command,cwd=root,env=child_env,stdout=stdout,stderr=stderr,timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                code = 124
                stderr.write(b'\nBounded timeout\n')
        result = dict(command=command,settings=settings or {},exit=code,
                      stdout_sha256=digest((out/f'{name}_stdout.txt').read_bytes()),
                      stderr_sha256=digest((out/f'{name}_stderr.txt').read_bytes()))
        print(json.dumps(dict(name=name,**result)),flush=True)
        return result

    build = [sys.executable,'tools/build_slot.py','--owner','release-swap-sdk-controls','--capacity','1',
             '--','cmake','--build','build','--target','difftest','-j','4']
    guard = """$ErrorActionPreference='Stop'
if (@(Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe' OR Name='xenia_canary.exe'").Count) { throw 'Wait for games before building' }
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Build RAM headroom insufficient' }
"""

    # build_slot.py's slot directories live under SR_MAIN_ROOT (default: the desktop's dev checkout); the cloud worker
    # names its own tree there (`$env:SR_MAIN_ROOT='C:/sr/wt/<tree>'`). Only the build sees it: the tests stay SR_-free.
    slot_root = {'SR_MAIN_ROOT': os.environ['SR_MAIN_ROOT']} if os.environ.get('SR_MAIN_ROOT') else {}

    def build_checked(name):
        subprocess.run(['powershell','-NoProfile','-Command',guard],check=True,capture_output=True)
        return run(name,build,slot_root)

    def replace_once(data, old, new):
        if data.count(old) != 1:
            raise ValueError('Source snippet changed; refusing mutation')
        return data.replace(old,new)

    observer = sources['kernel/hle_api_observe.cpp']
    flush = sources['kernel/native_flush.cpp']
    removed = replace_once(observer,
        b'if (!sr::native::flush::swapLeaf(ctx, base)) __imp__sub_82112050(ctx, base);',
        b'__imp__sub_82112050(ctx, base);')
    # Limit guard mutations to this SDK body, leaving other entries intact.
    start = flush.index(b'bool swapLeaf(')
    end = flush.index(b'bool swapBlockLeaf(',start)
    sdk = flush[start:end]
    # three since native/boot-gaps: the front write's, the scaler reset's and the persisted display's REG_RMW
    if sdk.count(b'ShaderConstRunsOracle nested;') != 3:
        raise ValueError('Expected the three SDK callback guards')
    unguarded = flush[:start] + sdk.replace(b'ShaderConstRunsOracle nested;',b'/* removed callback guard */') + flush[end:]
    stale_fb = replace_once(flush,
        b'swap(reserved + 4u, frame + 0x80u, sr::kernel::readGuestU32BE(dev + 0x2A10u) + 8u,',
        b'swap(reserved + 4u, frame + 0x80u, be32(snapshot + 0x2A10u) + 8u,')
    stale_rect = replace_once(flush,
        b'std::memcpy(rectangle, sr::kernel::guestVAToHost(rect), sizeof(rectangle));',
        b'/* retain stale admission rectangle */')
    stale_epilog = replace_once(flush,
        b'else work.r4.u64 = 0u;',
        b'/* retain preceding helper r4 on zero previous fence */')
    cases = [('removed_wiring','kernel/hle_api_observe.cpp',removed,'direct',{}),
             ('removed_callback_guards','kernel/native_flush.cpp',unguarded,'reentry',{'SR_NATIVE_SHADER_CONST_RUNS':'1'}),
             ('stale_writeback','kernel/native_flush.cpp',stale_fb,'mutating',{}),
             ('stale_rectangle','kernel/native_flush.cpp',stale_rect,'mutating',{}),
             ('stale_epilog_input','kernel/native_flush.cpp',stale_epilog,'direct',{})]
    try:
        for name,source_name,mutated,mode,settings in cases:
            if any((root/n).read_bytes() != data for n,data in sources.items()):
                raise ValueError('Concurrent source edit; refusing mutation')
            source = root/source_name
            row = dict(name=name,source=source_name,mutated_sha256=digest(mutated))
            evidence['rows'].append(row)
            source.write_bytes(mutated)
            try:
                row['build'] = build_checked(name+'_build')
                if row['build']['exit']:
                    raise RuntimeError('Mutant failed to build')
                row['exe_sha256'] = digest(exe.read_bytes())
                command = [str(exe),'--swap-sdk-test',mode]
                row['test'] = run(name+'_test',command,settings,45)
                output = (out/f'{name}_test_stdout.txt').read_bytes()
                summary = re.findall(rb'\[SWAP_SDK_TEST\] (\d+) passed, (\d+) failed',output)
                if name == 'removed_callback_guards':
                    row['detected'] = row['test']['exit'] == 124 and b'original SDK executes' in output and not summary
                # boot gaps (native/boot-gaps): 32 SDK swaps (the five persisted-display cases at two offsets); the
                # removed wiring also fails the 8 persisted-display Swaps' "boot gaps" check (no native persist taken)
                elif name == 'removed_wiring':
                    row['detected'] = row['test']['exit'] == 1 and summary == [(b'168',b'40')] and \
                        output.count(b'FAIL: production route') == 32 and output.count(b'FAIL: boot gaps fmt=') == 8
                elif name == 'stale_epilog_input':
                    row['detected'] = row['test']['exit'] == 1 and summary == [(b'184',b'24')] and \
                        output.count(b'FAIL: actual SDK and native caller invoke epilog') == 24 and \
                        output.count(b'FAIL:') == 24
                else:
                    row['detected'] = row['test']['exit'] == 1 and len(summary) == 1 and \
                        int(summary[0][1]) > 0 and b'FAIL: SDK swap fmt=' in output
                row['gate_off'] = run(name+'_gate_off',command,dict(settings,SR_NATIVE_SWAP='0',SR_NATIVE_SWAP_BLOCK='0'),45)
                row['gate_off_valid'] = row['gate_off']['exit'] == 0 and \
                    b'[SWAP_SDK_TEST] 208 passed, 0 failed' in (out/f'{name}_gate_off_stdout.txt').read_bytes()
                if not row['detected'] or not row['gate_off_valid']:
                    raise RuntimeError('Expected defect or gate-off control did not hold')
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
                evidence['restored_matrix'] = run('restored_matrix',
                    [sys.executable,'tools/difftest/swap_sdk_test_run.py','--out',str(out/'restored_matrix')],timeout=90)
                evidence['restored_matrix_valid'] = evidence['restored_matrix']['exit'] == 0
        save()
    valid = len(evidence['rows']) == len(cases) and all(row['detected'] and row['gate_off_valid'] and
                row['source_restored'] for row in evidence['rows']) and evidence['all_sources_restored'] and \
                evidence.get('restored_matrix_valid',False)
    print(json.dumps(dict(valid=valid,receipt=str(receipt))),flush=True)
    return int(not valid)


if __name__ == '__main__':
    raise SystemExit(main())
