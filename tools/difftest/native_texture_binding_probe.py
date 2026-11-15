#!/usr/bin/env python3
"""Prove that production compact texture indices affect actual replay pixels.

    python tools/difftest/native_texture_binding_probe.py --bank archive/<built-candidate> --out archive/<fresh-probe>

Builds an isolated copy of the actual backend with ordinary texture indices
rotated after snapshot construction. The healthy replay must remain exact; the
faulty replay must change pixels while preserving draw hashes and complete
GPU/VFETCH summaries. No checkout source/binary mutation or live game acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import native_draw_controls as shared
import translated_constant_controls as controls

ROOT = Path(__file__).resolve().parents[2]
ANCHOR = '  std::memcpy(shared.data(), textures.indices.data(), sizeof(textures.indices));'
ROTATION = (ANCHOR + '\n'
    '  // Isolated defect: rotate actual ordinary texture bindings.\n'
    '  std::array<uint32_t, 16> wrong;\n'
    '  for (uint32_t i = 0; i < 16; ++i) wrong[i] = (textures.indices[i] + 1u) % 16u;\n'
    '  std::memcpy(shared.data(), wrong.data(), sizeof(wrong));')


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def source_rotation(raw):
    """Preserve UTF-8 bytes and the source's uniform LF or CRLF line endings."""
    text = raw.decode('utf-8')
    crlf = b'\r\n' in raw
    normalized = text.replace('\r\n', '\n')
    require('\r' not in normalized and (not crlf or raw.count(b'\r\n') == raw.count(b'\n')),
            'Source has mixed/unsupported line endings')
    mutated = shared.replace_once(normalized, ANCHOR, ROTATION)
    expected = mutated.replace('\n', '\r\n').encode('utf-8') if crlf else mutated.encode('utf-8')
    # build_mutant writes text using the host newline convention. Checking the
    # actual written bytes below catches an unsupported host/source combination.
    return mutated, expected


def declared_link_inputs(ninja_text, build_root):
    """Pin all explicit objects, resources and static/import libraries."""
    block = re.search(r'^build SupermanReturns\.exe: ([^\n]+)\n((?:  [^\n]*\n)+)', ninja_text, re.M)
    require(block is not None, 'Production Ninja link target missing')
    inputs = block[1].split(' ', 1)[1].split(' |', 1)[0]
    properties = dict(re.findall(r'^  (\w+) = (.*)$', block[2], re.M))
    arguments = shared.command_args(inputs + ' ' + properties['LINK_LIBRARIES'])
    files, implicit = {}, []
    for argument in arguments:
        if argument.startswith('-l'):
            implicit.append(argument)
        elif argument.lower().endswith(('.obj', '.res', '.lib')):
            path = Path(argument)
            path = path if path.is_absolute() else build_root / path
            require(path.is_file(), 'Declared link input missing: ' + str(path))
            files[str(path.resolve())] = shared.digest(path)
    require(any(p.lower().endswith('.obj') for p in files) and
            any(p.lower().endswith('.lib') for p in files), 'Declared link inputs empty/incomplete')
    return files, implicit


def supplied_environment(env):
    return {key: value for key, value in env.items() if key.upper().startswith('SR_')}


def run_witness(exe, frame_dir, env, folder, attempt, active):
    """Persist actual supplied spawn inputs, rather than reserved empty paths."""
    provenance = {'scope': 'Inputs supplied to the pinned direct-process runner; not an OS observation',
                  'arguments': [str(exe)], 'cwd': str(frame_dir),
                  'exe_sha256': shared.digest(exe), 'supplied_sr_environment': supplied_environment(env),
                  'runner_helper_sha256': shared.digest(ROOT / 'tools/difftest/translated_constant_controls.py'),
                  'terminal_observed': False}
    path = Path(attempt['ProvenanceJson'])
    shared.write_json(path, provenance)
    try:
        result = controls.run(exe, frame_dir, env, folder, active)
        provenance.update(result)
        shared.write_json(path, provenance)
        return result, provenance['supplied_sr_environment'], shared.digest(path)
    except BaseException as error:
        provenance['error'] = repr(error)
        if active['proc'] is not None:
            provenance.update(pid=active['proc'].pid,
                              terminal_observed=active['proc'].poll() is not None)
        shared.write_json(path, provenance)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--out', type=Path, help='Fresh probe directory; prior evidence is never overwritten')
    args = parser.parse_args()
    bank = args.bank.resolve()
    build = json.loads((bank / 'build_receipt.json').read_text(encoding='utf-8-sig'))
    replay = json.loads((bank / 'replay/receipt.json').read_text(encoding='utf-8-sig'))
    require(build['exit_code'] == 0 and build['terminal_observed'] is True and
            replay['matched_expected'] is True and replay['slot_release_exit_code'] == 0,
            'Candidate build/replay not successful terminal evidence')
    guards = [(ROOT, build['source_pins']), (bank / 'source_snapshot', build['source_pins']),
              (bank / 'binary', build['binary_pins']), (ROOT, build['owner_wip_pins'])]
    require(all(shared.digest(base / p) == h for base, pins in guards for p, h in pins.items()),
            'Candidate provenance changed')
    out = args.out.resolve() if args.out else bank / 'production_texture_probe'
    out.mkdir(exist_ok=False)
    os.environ['SR_MAIN_ROOT'] = str(ROOT.parent / 'dev')
    os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / 'archive/native_recovery_20261012/temp')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$',
                     (ROOT / 'build/CMakeCache.txt').read_text(encoding='utf-8'), re.M).group(1)
    link_pins, implicit_libraries = declared_link_inputs(
        (ROOT / 'build/build.ninja').read_text(encoding='utf-8'), ROOT / 'build')
    build_config = ['build/CMakeCache.txt', 'build/build.ninja', 'build/CMakeFiles/rules.ninja',
                    'app/windows.manifest']
    config_pins = {str(ROOT / p): shared.digest(ROOT / p) for p in build_config}
    config_pins[str(Path(ninja))] = shared.digest(Path(ninja))
    commands = subprocess.run([ninja, '-t', 'commands', 'SupermanReturns'], cwd=ROOT / 'build',
        capture_output=True, text=True, encoding='utf-8', check=True, timeout=30).stdout.splitlines()
    compile_line = next(line for line in commands if
                        'gfx/d3d12_backend.cpp.obj' in line and ' -c ' in line)
    compiler = Path(shared.command_args(compile_line)[0])
    config_pins[str(compiler)] = shared.digest(compiler)
    shared.write_json(out / 'build_inputs.json', {'explicit_link_input_pins': link_pins,
        'build_config_pins': config_pins, 'implicit_sdk_library_options_unpinned': implicit_libraries})
    source, expected_source = source_rotation((ROOT / 'gfx/d3d12_backend.cpp').read_bytes())
    mutant = controls.build_mutant('rotated_compact_indices', 'SupermanReturns',
                                  'gfx/d3d12_backend.cpp', source, out, ninja)
    mutant_folder = mutant.parent
    require((mutant_folder / 'd3d12_backend.cpp').read_bytes() == expected_source,
            'Mutant changes bytes beyond the single declared rotation')
    mutant_build = json.loads((mutant_folder / 'build_receipt.json').read_text(encoding='utf-8'))
    require(mutant_build['compile_arguments'][0] == str(compiler), 'Mutant compiler differs')
    produced_pins = {str(p): shared.digest(p) for p in (
        mutant_folder / 'd3d12_backend.cpp', mutant_folder / 'mutant.obj',
        mutant_folder / 'control.rsp', mutant_folder / 'build_receipt.json', mutant)}
    produced_pins.update({str(mutant_folder / name): pin for name, pin in mutant_build['dll_pins'].items()})
    harness = ['tools/difftest/native_texture_binding_probe.py',
               'tools/difftest/translated_constant_controls.py',
               'tools/difftest/native_draw_controls.py', 'tools/build_slot.py',
               'tools/game_slot.py', 'tools/sr_artifacts.ps1',
               'tools/difftest/test_native_texture_binding_probe.py']
    receipt = {'scope': 'Production compact ordinary-texture binding sensitivity in one deterministic movie replay; no delayed-production ownership/exhaustion, full-game fidelity or Native0 acceptance',
               'candidate_build_sha256': shared.digest(bank / 'build_receipt.json'),
               'candidate_replay_sha256': shared.digest(bank / 'replay/receipt.json'),
               'mutant_binary_sha256': shared.digest(mutant),
               'explicit_link_input_pins': link_pins, 'build_config_pins': config_pins,
               'implicit_sdk_library_options_unpinned': implicit_libraries,
               'produced_mutant_pins': produced_pins, 'single_source_delta_verified': True,
               'harness_pins': {p: shared.digest(ROOT / p) for p in harness}, 'runs': []}
    shutil.copy2(Path(__file__), out / 'runner_at_execution.py')
    entry = replay['comparisons'][0]; trace = entry['trace']
    reference = bank / 'replay' / (trace + '_proof.png')
    reference_draws = bank / 'replay' / (trace + '_draws.txt')
    require(shared.digest(reference) == entry['proof_sha256'] and
            shared.digest(reference_draws) == entry['draws_sha256'], 'Reference proof differs')
    trace_path = Path(entry['gates']['SR_GPU_TRACE_REPLAY'])
    trace_pin = shared.digest(trace_path)
    require(replay['trace_pins'].get(str(trace_path)) == trace_pin, 'Trace input changed')
    receipt['reference_pins'] = {str(reference): shared.digest(reference),
        str(reference_draws): shared.digest(reference_draws), str(trace_path): trace_pin}
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]; owner = 'production_texture_probe'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--min-free-gb', '12',
                                    '--max-hours', '0.05', '--poll', '5'],
                             capture_output=True, text=True, timeout=190)
    shared.write_json(out / 'slot_acquire.json', {'exit_code': acquire.returncode,
        'stdout': acquire.stdout, 'stderr': acquire.stderr})
    require(acquire.returncode == 0, 'Game slot unavailable')
    active = {'proc': None}
    try:
        for name, exe in [('healthy', bank / 'binary/SupermanReturns.exe'), ('rotated', mutant)]:
            folder = out / name; folder.mkdir()
            frame_dir, env, attempt = controls.production_inputs(folder, entry)
            # Windows environment names are case-insensitive. The shared
            # factory strips uppercase SR_ names; remove any inherited aliases.
            declared = set(entry['gates']) | {'SR_MAIN_ROOT', 'SR_INSTANCE_ID', 'SR_LOG', 'SR_REPLAY_DRAW_HASH'}
            env = {k: v for k, v in env.items() if not k.upper().startswith('SR_') or k in declared}
            result, gates, provenance_pin = run_witness(exe, frame_dir, env, folder, attempt, active)
            raw = (folder / 'stdout.log').read_text(encoding='utf-8', errors='replace') + '\n' + (folder / 'stderr.log').read_text(encoding='utf-8', errors='replace')
            result.update(controls.grade_replay(raw, result['exit_code'], result['timed_out'],
                          result['terminal_observed'], entry['summaries']))
            require(result['matched_expected'], 'Incomplete replay: ' + name)
            from PIL import Image
            bmp = frame_dir / 'replay_frame_1.bmp'
            Image.open(bmp).convert('RGB').save(folder / 'proof.png')
            result.update(controls.compare_images(folder / 'proof.png', reference))
            result.update(name=name, raw_attempt=attempt, gates=gates, provenance_sha256=provenance_pin,
                          source_bmp_sha256=shared.digest(bmp),
                          proof_sha256=shared.digest(folder / 'proof.png'),
                          draws_sha256=shared.digest(folder / 'draws.txt'),
                          draw_hashes_equal=shared.digest(folder / 'draws.txt') == entry['draws_sha256'])
            result['matched_expected'] = result['matched_expected'] and result['draw_hashes_equal'] and (
                result['different_pixels'] == 0 and result['max_difference'] == 0 if name == 'healthy' else
                result['different_pixels'] > 0 and result['max_difference'] > 1)
            receipt['runs'].append(result); shared.write_json(out / 'receipt.json', receipt)
            print(name + ' matched=' + str(result['matched_expected']) +
                  ' different_pixels=' + str(result['different_pixels']), flush=True)
            require(result['matched_expected'], 'Production binding control is not sensitive: ' + name)
    finally:
        receipt['slot_release_exit_code'] = shared.release_slot(slot, owner, out, active['proc']).returncode
        receipt['candidate_pins_unchanged'] = all(shared.digest(base / p) == h for base, pins in guards for p, h in pins.items())
        receipt['harness_pins_unchanged'] = all(shared.digest(ROOT / p) == h for p, h in receipt['harness_pins'].items())
        receipt['reference_pins_unchanged'] = all(shared.digest(Path(p)) == h for p, h in receipt['reference_pins'].items())
        receipt['mutant_binary_unchanged'] = shared.digest(mutant) == receipt['mutant_binary_sha256']
        receipt['link_inputs_unchanged'] = all(shared.digest(Path(p)) == h for p, h in link_pins.items())
        receipt['build_config_unchanged'] = all(shared.digest(Path(p)) == h for p, h in config_pins.items())
        receipt['produced_mutant_unchanged'] = all(shared.digest(Path(p)) == h for p, h in produced_pins.items())
        receipt['provenance_witnesses_unchanged'] = all(
            shared.digest(Path(r['raw_attempt']['ProvenanceJson'])) == r['provenance_sha256'] for r in receipt['runs'])
        paired = [{k: v for k, v in r['gates'].items() if k != 'SR_REPLAY_DRAW_HASH'} for r in receipt['runs']]
        receipt['paired_supplied_inputs_equal'] = len(paired) == 2 and paired[0] == paired[1]
        receipt['matched_expected'] = len(receipt['runs']) == 2 and all(r['matched_expected'] for r in receipt['runs']) and receipt['slot_release_exit_code'] == 0 and all(
            receipt[k] for k in ('candidate_pins_unchanged', 'harness_pins_unchanged', 'reference_pins_unchanged',
                'mutant_binary_unchanged', 'link_inputs_unchanged', 'build_config_unchanged',
                'produced_mutant_unchanged', 'provenance_witnesses_unchanged', 'paired_supplied_inputs_equal'))
        shared.write_json(out / 'receipt.json', receipt)
    return 0 if receipt['matched_expected'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
