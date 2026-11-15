"""The boot gaps' mutants (native/boot-gaps): each compiled defect in the record forms (and in the ramp builders' kernel
export VdGetCurrentDisplayGamma) must fail the offline proof, and its control must pass - the same build with the
defect's gate =0, or (the export, which has no gate) the swap suite, which cannot see it - so each assertion is shown to
bite in the code it claims to cover.

Usage (the cloud worker or any tree with a configured build/): python tools/difftest/boot_gaps_mutation_run.py
       --out archive/<fresh-bank> [--jobs 6]
Requires a passing tools/difftest/boot_gaps_run.py of the same sources first (its receipt is not read: run it). Every
mutant edits one source snippet (refused unless found exactly once), rebuilds difftest, runs its detecting row and its
control row, and restores the exact source bytes (even on failure); a final rebuild and boot_gaps_run.py of the restored
tree close the receipt. Writes boot_gaps_mutations.json. Exit 0 = every mutant detected, every control passed, the sources
restored and the restored matrix valid. Offline sensitivity only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boot_gaps_run as bgr  # noqa: E402  (the matrix's own base gates and the swap suite's pins)

FLUSH, EMIT, STUBS = 'kernel/native_flush.cpp', 'kernel/native_display_emit.cpp', 'kernel/stubs.cpp'
BOOT, SWAP, REENTRY = ['--boot-gaps-test'], ['--swap-sdk-test', 'direct'], ['--swap-sdk-test', 'reentry']
DISPLAY_OFF, MODE_OFF, LITERAL = {'SR_NATIVE_DISPLAY': '0'}, {'SR_NATIVE_MODE_STATE': '0'}, {'SR_NATIVE_FLUSH_PM4': '1'}
SW, SW_OFF = dict(bgr.SWAP), dict(bgr.SWAP, SR_NATIVE_DISPLAY='0')  # the swap rows run on the swap suite's pins
SW_REENTRY = dict(bgr.SWAP, SR_NATIVE_SHADER_CONST_RUNS='1')         # its reentry mode: the nested shader-const runs
HANG = None  # a detecting row whose defect deadlocks: detected by the bounded timeout (exit 124), no summary
# (name, file, [(old, new)], detecting argv, detecting env, the FAIL text the detection must show (HANG: the timeout),
#  control argv, control env)
MUTANTS = [
    ('mode_wiring_removed', FLUSH,
     [(b'  if (sr::native::flush::modeStateOn() && sr::native::flush::modeStateLeaf(ctx, base)) return;\n', b'')],
     BOOT, {}, b'FAIL: mode state 82103238', BOOT, MODE_OFF),
    ('mode_bits_shifted', FLUSH,
     [(b'& ~0x3000u) | ((mode & 3u) << 12);', b'& ~0x3000u) | ((mode & 3u) << 13);')],
     BOOT, {}, b'FAIL: mode state 82103238', BOOT, MODE_OFF),
    ('mode_kick_dropped', FLUSH,
     [(b'  bool modeStatePackets() {\n    if (!kickPoint()) return false;\n', b'  bool modeStatePackets() {\n')],
     BOOT, {}, b'FAIL: mode state 82103238 mode=0 kick=1', BOOT, MODE_OFF),
    ('persist_rmw_mask', FLUSH,
     [(b'e.regRmw(0x1E4Eu, 0xFFFFFFFEu, 0u);', b'e.regRmw(0x1E4Eu, 0xFFFFFFFFu, 0u);')],
     SWAP, SW, b'FAIL: real closed segments', SWAP, SW_OFF),
    ('persist_bit_kept', FLUSH,
     [(b'    d[0x2A39u] &= ~0x10u;\n    displayNotePersist();', b'    displayNotePersist();')],
     SWAP, SW, b'FAIL: SDK swap fmt=', SWAP, SW_OFF),
    ('persist_ramp_skipped', FLUSH,
     [(b'  if (persist && (d[0x2A39u] & 0x10u)) call(sub_82105DD0, dev + 0x3370u, 1u);\n', b'')],
     SWAP, SW, b'FAIL: SDK swap fmt=', SWAP, SW_OFF),
    ('persist_ungated', FLUSH,
     [(b'const bool persist = (snapshot[0x2A39u] & 0x10u) != 0u && displayActive();',
       b'const bool persist = (snapshot[0x2A39u] & 0x10u) != 0u;')],
     SWAP, SW_OFF, b'FAIL: the persisted display with SR_NATIVE_DISPLAY off declines', SWAP, SW),
    # the reserve's segment switch under the arena lock: without the nested-oracle guard the reentrant allocator's
    # nested shader-const runs open a second Entry and wait on the lock this one holds (sw1: the reentry row hung)
    ('ramp_unguarded', EMIT,
     [(b'  ShaderConstRunsOracle nested;\n  const int reserved = e.reserve(wide ? 1413u : 2309u);',
       b'  const int reserved = e.reserve(wide ? 1413u : 2309u);')],
     REENTRY, SW_REENTRY, HANG, REENTRY, dict(SW_REENTRY, SR_NATIVE_DISPLAY='0')),
    ('ramp_table_packing', EMIT,
     [(b'const uint32_t r7 = ((be16(p0 + 2u * i) << 10) & 0x03FF0000u) | be16(p1 + 2u * i);',
       b'const uint32_t r7 = ((be16(p0 + 2u * i) << 9) & 0x03FF0000u) | be16(p1 + 2u * i);')],
     BOOT, {}, b'FAIL: gamma ramp 821056E0', BOOT, DISPLAY_OFF),
    ('ramp_pwl_halves', EMIT,
     [(b'    w[2u + 3u * i] = (be16(p1 + 4u * i + 2u) << 16) | be16(p1 + 4u * i);',
       b'    w[2u + 3u * i] = (be16(p1 + 4u * i) << 16) | be16(p1 + 4u * i + 2u);')],
     BOOT, {}, b'FAIL: gamma ramp 821057C0', BOOT, DISPLAY_OFF),
    ('ramp_apply_mode', EMIT,
     [(b'  eng.writeReg(0x1921u, w[0]);', b'  eng.writeReg(0x1921u, w[0] ^ 1u);')],
     BOOT, {}, b'FAIL: gamma ramp 8210', BOOT, DISPLAY_OFF),
    ('ramp_render_function', EMIT,
     [(b'0x00001925u, v, 0xC0054500u, 7u, 0x1925u, v,', b'0x00001925u, v, 0xC0054500u, 3u, 0x1925u, v,')],
     BOOT, LITERAL, b'FAIL: gamma ramp 821056E0', BOOT, dict(LITERAL, **DISPLAY_OFF)),
    ('scaler_prelude_arg', EMIT,
     [(b'const uint32_t r7b = (rd(dev + 0x4E00u) - f94) >> 1;', b'const uint32_t r7b = (rd(dev + 0x4E00u) - f94) >> 2;')],
     BOOT, {}, b'FAIL: scaler 8210A788', BOOT, DISPLAY_OFF),
    ('scaler_wait_dropped', EMIT,
     [(b'  eng.applyWaitRegMem(wait, 5u);\n  eng.writeReg(0x1852u, 0u);', b'  eng.writeReg(0x1852u, 0u);')],
     BOOT, {}, b'FAIL: scaler 8210A788', BOOT, DISPLAY_OFF),
    ('scaler_wiring_removed', EMIT,
     [(b'  if (sr::native::flush::displayGate() && sr::native::flush::scalerLeaf(ctx, base)) return;\n', b'')],
     BOOT, {}, b'FAIL: scaler 8210A788', BOOT, DISPLAY_OFF),
    ('scaler_program_tail', EMIT,
     [(b'    dispWrite32(at + 76u + 4u * program, kScalerTail[0]);', b'    dispWrite32(at + 76u + 4u * program, 0u);')],
     BOOT, {}, b'FAIL: scaler 8210A788', BOOT, DISPLAY_OFF),
    ('gamma_prefix_or', EMIT,
     [(b'!e.regRmw(0x1841u, 0xFFFFF8FFu, wide ? 0x100u : 0u)', b'!e.regRmw(0x1841u, 0xFFFFF8FFu, 0x100u)')],
     BOOT, {}, b'FAIL: set gamma 82111CB8 mode=00120000', BOOT, DISPLAY_OFF),
    ('gamma_default_ramp', EMIT,
     [(b'        const uint32_t r10 = (r9 / 0x7Fu) & 0xFFFFu;', b'        const uint32_t r10 = (r9 / 0x7Eu) & 0xFFFFu;')],
     BOOT, {}, b'FAIL: set gamma 82111CB8 mode=28280136 ramp=default', BOOT, DISPLAY_OFF),
    # VdGetCurrentDisplayGamma (no gate: a kernel export) - its control is the swap suite on the same build, whose
    # persisted-display Swaps run the builders through the export in both arms and cannot see either defect (a stray
    # word in the builder's frame, the type's branch taken by both arms alike): only the canary check bites
    ('gamma_stub_overrun', STUBS,
     [(b'  if (r3 != 0) writeGuest32(r3, kDisplayGammaType);',
       b'  if (r3 != 0) { writeGuest32(r3, kDisplayGammaType); writeGuest32(r3 + 4u, 0u); }')],
     BOOT, {}, b'FAIL: VdGetCurrentDisplayGamma', SWAP, SW),
    ('gamma_stub_type', STUBS,
     [(b'constexpr uint32_t kDisplayGammaType = 2u;', b'constexpr uint32_t kDisplayGammaType = 1u;')],
     BOOT, {}, b'FAIL: VdGetCurrentDisplayGamma', SWAP, SW),
]
BASE = bgr.BASE
SUMMARY = bgr.SUMMARY
HANG_TIMEOUT = 150  # s: a healthy reentry row takes ~60 s on the worker


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--only', action='append', default=[], help='run only these mutants (repeatable)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    exe = root / 'build/difftest.exe'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = out / 'boot_gaps_mutations.json'
    names = {FLUSH, EMIT, STUBS}
    sources = {n: (root / n).read_bytes() for n in names}
    evidence = dict(scope='Offline defect sensitivity of the boot-gaps record forms; no live acceptance.',
                    tool_sha256=digest(Path(__file__).read_bytes()), original_exe_sha256=digest(exe.read_bytes()),
                    original_sources={n: digest(d) for n, d in sources.items()}, rows=[])

    def save():
        receipt.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')

    def run(name, command, settings=None, timeout=600):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SR_')}
        env.update(BASE)
        env.update(settings or {})
        with (out / f'{name}_stdout.txt').open('xb') as so, (out / f'{name}_stderr.txt').open('xb') as se:
            try:
                code = subprocess.run(command, cwd=root, env=env, stdout=so, stderr=se, timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                code = 124
                se.write(b'\nBounded timeout\n')
        result = dict(command=command, settings=settings or {}, exit=code,
                      stdout_sha256=digest((out / f'{name}_stdout.txt').read_bytes()))
        print(json.dumps(dict(name=name, **result)), flush=True)
        return result

    def build(name):
        return run(name, ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], timeout=3000)

    selected = [m for m in MUTANTS if not args.only or m[0] in args.only]
    try:
        for name, source_name, edits, argv, env, needle, control_argv, control_env in selected:
            if any((root / n).read_bytes() != d for n, d in sources.items()):
                raise ValueError('concurrent source edit; refusing mutation')
            mutated = sources[source_name]
            crlf = b'\r\n' in mutated  # a checkout with CRLF line ends (the worker's): the snippets' \n as \r\n
            for old, new in edits:
                if crlf:
                    old, new = old.replace(b'\n', b'\r\n'), new.replace(b'\n', b'\r\n')
                if mutated.count(old) != 1:
                    raise ValueError('%s: snippet not found exactly once - refusing mutation' % name)
                mutated = mutated.replace(old, new)
            row = dict(name=name, source=source_name, mutated_sha256=digest(mutated))
            evidence['rows'].append(row)
            path = root / source_name
            path.write_bytes(mutated)
            try:
                row['build'] = build(name + '_build')
                if row['build']['exit']:
                    raise RuntimeError('%s: the mutant failed to build' % name)
                row['exe_sha256'] = digest(exe.read_bytes())
                row['test'] = run(name + '_test', [str(exe)] + argv, env,
                                  timeout=HANG_TIMEOUT if needle is HANG else 600)
                text = (out / f'{name}_test_stdout.txt').read_bytes()
                summary = SUMMARY.findall(text)
                row['summary'] = [dict(passed=int(p), failed=int(f)) for p, f in summary]
                if needle is HANG:  # the deadlock: killed by the bound, no summary printed
                    row['detected'] = row['test']['exit'] == 124 and not summary
                else:
                    row['detected'] = row['test']['exit'] == 1 and len(summary) == 1 and int(summary[0][1]) > 0 and \
                        needle in text
                row['control'] = run(name + '_control', [str(exe)] + control_argv, control_env)
                ctext = (out / f'{name}_control_stdout.txt').read_bytes()
                csum = SUMMARY.findall(ctext)
                row['control_valid'] = row['control']['exit'] == 0 and len(csum) == 1 and int(csum[0][1]) == 0 and \
                    b'FAIL:' not in ctext
                if not row['detected'] or not row['control_valid']:
                    raise RuntimeError('%s: the defect was not detected or its control failed' % name)
            finally:
                if path.read_bytes() != mutated:
                    raise ValueError('concurrent edit; refusing to overwrite the source')
                path.write_bytes(sources[source_name])
                row['source_restored'] = path.read_bytes() == sources[source_name]
                save()
    finally:
        evidence['all_sources_restored'] = all((root / n).read_bytes() == d for n, d in sources.items())
        if evidence['all_sources_restored']:
            evidence['restored_build'] = build('restored_build')
            if evidence['restored_build']['exit'] == 0:
                evidence['restored_exe_sha256'] = digest(exe.read_bytes())
                evidence['restored_matrix'] = run('restored_matrix', [sys.executable, 'tools/difftest/boot_gaps_run.py',
                                                                      '--out', str(out / 'restored_matrix')])
                evidence['restored_matrix_valid'] = evidence['restored_matrix']['exit'] == 0
        save()
    valid = len(evidence['rows']) == len(selected) and all(
        r.get('detected') and r.get('control_valid') and r.get('source_restored') for r in evidence['rows']) and \
        evidence['all_sources_restored'] and evidence.get('restored_matrix_valid', False)
    evidence['valid'] = valid
    save()
    print(json.dumps(dict(valid=valid, receipt=str(receipt))), flush=True)
    return int(not valid)


if __name__ == '__main__':
    raise SystemExit(main())
