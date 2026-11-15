"""Run the boot gaps' offline proof (native/boot-gaps, docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE"): the display
emitters (GpuRingEmitGammaRamp 821056E0 / ...Wide 821057C0, GpuSetGammaRamp 82111CB8, GpuScalerEmitCommandBuffer
8210A788), the mode state GpuRingEmitModeStateReg 82103238 and the first Swap's persisted display, each against its SDK
body, under every gate arm.

Usage: python tools/difftest/boot_gaps_run.py --out archive/<fresh-bank>
Writes boot_gaps_matrix.json (the rows, the exe and source hashes) and per-row stdout / stderr. Exit 0 when every row
holds its expected form. No game or graphics worker is launched: offline only - it proves the leaves against the SDK
bodies on fixture devices, not live boot, pacing or the census.

Rows:
  direct        --boot-gaps-test, the defaults (every gate ON: records)            every section, gaps 0
  literal       SR_NATIVE_FLUSH_PM4=1 (the records rendered back to literal PM4) the renderers byte for byte vs the SDK
  reconstructed SR_NATIVE_FLUSH_CPPM4=1 (the CP renders and walks them)           the consumer through the walker
  display_off   SR_NATIVE_DISPLAY=0                                               the guest bodies, no leaf counted
  mode_off      SR_NATIVE_MODE_STATE=0                                            82103238's guest body
  seg_off       SR_NATIVE_SEGMENTLESS=0                                           no leaf (the transport is required)
  swap          --swap-sdk-test direct                                            the persisted-display Swaps + the rest,
                                                                                  no literal PM4 (gaps 0)
  swap_literal  --swap-sdk-test direct, SR_NATIVE_FLUSH_PM4=1                     the first Swap's bytes vs the SDK
  swap_reconstructed --swap-sdk-test direct, SR_NATIVE_FLUSH_CPPM4=1              the CP renders the records back
  swap_display_off --swap-sdk-test direct, SR_NATIVE_DISPLAY=0                    the persisted display declines (flags2A39)
The swap rows run on the swap suite's own pins (tools/difftest/swap_sdk_test_run.py `base`: SWAP / SWAP_BLOCK / FLUSH /
SEGMENTLESS on, the present gate, callback, construns, dedup and exec off - the contract its fixtures are written for)
with SR_NATIVE_DEVCREATE at its default (ON): 820F0048's per-present re-emit a record too, so the persisted-display
Swap's segments must hold no literal PM4 at all. (Its SR_PM4_CP=1 matters: BASE's SR_PM4_LIVE=0 alone leaves the GPU CP
inactive, so SWAP declines every present at its export check - 110 / 98 on this tree, the gates on or off.)
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SOURCES = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/native_display_emit.cpp',
           'kernel/native_record_format.hpp', 'kernel/stubs.cpp', 'tools/difftest/swap_sdk_test.cpp',
           'tools/difftest/boot_gaps_run.py')
# The defaults the rows start from: every gate as the default build has it (unset), the oracles and diagnostics off.
BASE = dict(SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_CHECK='0', SR_PM4_LIVE='0',
            SR_ALLOW_FORCING='0', SR_MODS='0', SR_DEVTOOLS='0', SR_HIGHFPS='0', SR_LOG='dev')
# The swap suite's pins (swap_sdk_test_run.py `base`) but SR_NATIVE_DEVCREATE, left at its default (ON).
SWAP = dict(SR_NATIVE_SWAP='1', SR_NATIVE_SWAP_BLOCK='1', SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1',
            SR_NATIVE_CONST_DEDUP='0', SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0', SR_NATIVE_SHADER_CONST_RUNS='0',
            SR_NATIVE_EXEC='0', SR_PM4_CP='1', SR_API_OBSERVE='0', SR_API_PRESENT_BLOCK='0')
# (name, suite argv, env, expected section lines {marker: count}, expected summary (passed, failed) or None); every row:
# exit 0, one summary line with 0 failed and passed > 0 (or exactly the expected pair)
DIRECT = {b'mode state 82103238': 8, b'gamma ramp 8210': 8, b'set gamma 82111CB8': 6, b'scaler 8210A788': 8,
          b'VdGetCurrentDisplayGamma writes': 1}
SWAP_ON = {b'SDK swap fmt=': 32, b'boot gaps fmt=': 32}
ROWS = [
    ('direct', ['--boot-gaps-test'], {}, DIRECT, (39, 0)),
    ('literal', ['--boot-gaps-test'], {'SR_NATIVE_FLUSH_PM4': '1'}, DIRECT, (39, 0)),
    ('reconstructed', ['--boot-gaps-test'], {'SR_NATIVE_FLUSH_CPPM4': '1'}, DIRECT, (39, 0)),
    ('display_off', ['--boot-gaps-test'], {'SR_NATIVE_DISPLAY': '0'}, DIRECT, (39, 0)),
    ('mode_off', ['--boot-gaps-test'], {'SR_NATIVE_MODE_STATE': '0'}, DIRECT, (39, 0)),
    ('seg_off', ['--boot-gaps-test'], {'SR_NATIVE_SEGMENTLESS': '0', 'SR_NATIVE_TILERECTS': '0'}, DIRECT, (39, 0)),
    ('swap', ['--swap-sdk-test', 'direct'], SWAP, SWAP_ON, (208, 0)),
    ('swap_literal', ['--swap-sdk-test', 'direct'], dict(SWAP, SR_NATIVE_FLUSH_PM4='1'), SWAP_ON, (208, 0)),
    ('swap_reconstructed', ['--swap-sdk-test', 'direct'], dict(SWAP, SR_NATIVE_FLUSH_CPPM4='1'), SWAP_ON, (208, 0)),
    ('swap_display_off', ['--swap-sdk-test', 'direct'], dict(SWAP, SR_NATIVE_DISPLAY='0'),
     {b'SDK swap fmt=': 22, b'boot gaps fmt=': 22, b'the persisted display with SR_NATIVE_DISPLAY off declines': 1},
     (149, 0)),
]
SUMMARY = re.compile(rb'\[(?:BOOT_GAPS|SWAP_SDK)_TEST\] (\d+) passed, (\d+) failed')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--exe', type=Path, help='difftest.exe (default: build/difftest.exe of this tree)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    exe = (args.exe or root / 'build/difftest.exe').resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = out / 'boot_gaps_matrix.json'
    if receipt.exists() or list(out.glob('*_stdout.txt')):
        parser.error('choose a fresh evidence bank')
    matrix = dict(exe=str(exe), exe_sha256=digest(exe), source_sha256={s: digest(root / s) for s in SOURCES},
                  base_gates=BASE, rows=[])
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    for name, argv, settings, sections, totals in ROWS:
        env = {k: v for k, v in os.environ.items() if not k.startswith('SR_')}
        env.update(BASE)
        env.update(settings)
        command = [str(exe)] + argv
        try:
            r = subprocess.run(command, cwd=root, env=env, capture_output=True, timeout=180)
            code, stdout, stderr = r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired as e:
            code, stdout, stderr = 124, e.stdout or b'', (e.stderr or b'') + b'\nBounded timeout\n'
        for stream, data in (('stdout', stdout), ('stderr', stderr)):
            with (out / f'{name}_{stream}.txt').open('xb') as f:
                f.write(data)
        summary = SUMMARY.findall(stdout)
        counts = {k.decode(): stdout.count(k) for k in sections}
        valid = code == 0 and len(summary) == 1 and int(summary[0][1]) == 0 and int(summary[0][0]) > 0 and \
            all(stdout.count(k) == n for k, n in sections.items()) and b'FAIL:' not in stdout and \
            (totals is None or (int(summary[0][0]), int(summary[0][1])) == totals)
        row = dict(name=name, command=command, gates=settings, exit=code, valid=valid,
                   summary=[dict(passed=int(p), failed=int(f)) for p, f in summary], sections=counts,
                   expected_sections={k.decode(): n for k, n in sections.items()},
                   expected_summary=dict(passed=totals[0], failed=totals[1]) if totals else None,
                   stdout_sha256=digest(out / f'{name}_stdout.txt'), stderr_sha256=digest(out / f'{name}_stderr.txt'))
        matrix['rows'].append(row)
        receipt.write_text(json.dumps(matrix, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(row), flush=True)
    matrix['unchanged_exe'] = digest(exe) == matrix['exe_sha256']
    matrix['unchanged_sources'] = all(digest(root / s) == h for s, h in matrix['source_sha256'].items())
    matrix['valid'] = matrix['unchanged_exe'] and matrix['unchanged_sources'] and all(r['valid'] for r in matrix['rows'])
    receipt.write_text(json.dumps(matrix, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(valid=matrix['valid'], receipt=str(receipt))), flush=True)
    return 0 if matrix['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
