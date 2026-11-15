#!/usr/bin/env python3
"""Compile isolated translated-draw input and production-wiring defect controls.

Build SupermanReturns and sr_translated_constants_test through build_slot first.
Run with --bank <fresh evidence directory> --reference <passing replay directory>.
Use --family geometry for vertex/index controls, or samplers for descriptor
retention and actual point/linear production-binding controls.
The reference must contain menu_pressstart_454107ED_9415_proof.png. Sources and
normal binaries are never changed. Compiles use the built targets' exact argv;
the production response file is reconstructed from Ninja's declared link rule.
Builds are serial; all GPU processes hold one game slot. The ownership mutant
aborts with its queue delayed before any dangling GPU execution.
This is bounded wiring/defect-sensitivity evidence, never native-zero acceptance.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import native_draw_controls as shared

ROOT = Path(__file__).resolve().parents[2]
TRACE = 'menu_pressstart_454107ED_9415'
TAG = '[TRANSLATED_CONSTANT_TEST]'
RETAIN_CHECK = 'both_constant_versions_survive_producer_and_recording_destruction'
ABORT_LINE = 'ABORT before executing dangling constants'
FULL_CHECKS = (
    'aligned_exact_fetched_extent', 'aliases_and_shared_fetch_offsets',
    'short_vs_refuses_without_replacing_output', 'short_ps_refuses_without_replacing_output',
    'short_shared_refuses_without_replacing_output', 'short_fetch_refuses_without_replacing_output',
    'uninitialized_arena_preserves_output', 'unopened_recording_refuses_binding',
    'empty_snapshot_refuses_binding', 'closed_recording_refuses_binding',
    'retained_snapshot_prevents_page_budget_reuse', 'abandoned_snapshot_releases_page',
    'decoded_snapshot_has_no_fetch_binding', 'float_extents_pack_blocks_and_keep_aliases',
    'empty_unaligned_or_oversized_extent_refuses', 'unfitting_reserve_moves_snapshot_to_a_new_page',
    'fitting_reserve_stays_on_the_page_inside_the_resource', 'actual_queue_still_delayed', RETAIN_CHECK,
    'float_shared_aliases_render_exact_after_source_mutation', 'both_versions_retire_after_checked_completion',
    'actual_queue_still_delayed', RETAIN_CHECK,
    'fetch_aliases_and_full_extents_render_exact_after_source_mutation',
    'both_versions_retire_after_checked_completion',
    'actual_queue_still_delayed', RETAIN_CHECK,
    'compact_extents_render_exact_after_source_mutation', 'both_versions_retire_after_checked_completion',
    'actual_queue_still_delayed', RETAIN_CHECK,
    'compact_extents_with_fetch_render_exact_after_source_mutation',
    'both_versions_retire_after_checked_completion',
)
ALIAS_FAILURES = (
    'aliases_and_shared_fetch_offsets', 'float_extents_pack_blocks_and_keep_aliases',
    'float_shared_aliases_render_exact_after_source_mutation',
    'fetch_aliases_and_full_extents_render_exact_after_source_mutation',
    'compact_extents_render_exact_after_source_mutation',
    'compact_extents_with_fetch_render_exact_after_source_mutation',
)
SAMPLER_TAG = '[TRANSLATED_SAMPLERS_TEST]'
SAMPLER_RETAIN = 'sampler_retained_after_cache_and_recording_destruction'
SAMPLER_ABORT = 'ABORT dangling recorded sampler before submission'
SAMPLER_CHECKS = (
    'all_sixteen_indices_preserve_filter_address_and_graphics_options',
    'invalid_anisotropy_preserves_recipes', 'nonfinite_lod_bias_rejected',
    'out_of_range_lod_bias_rejected', 'null_device_rejected',
    'default_table_reused_with_sixteen_descriptors', 'unchanged_options_reuse_distinct_version',
    'invalid_selection_preserves_published_output', 'return_to_defaults_reuses_original_version',
    'foreign_device_rejected_without_output_change', 'all_retained_versions_count_toward_budget',
    'version_budget_failure_preserves_output', 'released_old_version_returns_budget',
    'returned_budget_admits_new_immutable_version', 'empty_descriptor_version_rejected',
    'queued_version_survives_more_than_seven_option_changes',
    'closed_recording_rejects_descriptor_retention', 'actual_queue_still_delayed', SAMPLER_RETAIN,
    'all_wrap_clamp_indices_exact_pixels_after_owner_destruction',
    'all_point_linear_indices_exact_pixels_after_owner_destruction',
    'lod_bias_selects_expected_mip_between_actual_a_b_a_heap_switches',
    'sampler_version_retired_after_checked_completion',
)


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf-8')


def grade_fixture(stdout, stderr, name, exit_code, timed_out, terminal=True):
    """Reconcile the independently specified full sequence or guarded prefix."""
    tagged = [line for line in (stdout + '\n' + stderr).splitlines() if line.startswith(TAG)]
    checks = [match.groups() for line in tagged
              if (match := re.fullmatch(re.escape(TAG) + r' (PASS|FAIL) (.+)', line))]
    summaries = [tuple(map(int, match.groups())) for line in tagged if (match := re.fullmatch(
        re.escape(TAG) + r' RESULT checks=(\d+) passed=(\d+) failed=(\d+)', line))]
    aborted = name == 'missing_owner'
    sequence = FULL_CHECKS[:FULL_CHECKS.index(RETAIN_CHECK) + 1] if aborted else FULL_CHECKS
    expected_failures = (RETAIN_CHECK,) if aborted else ALIAS_FAILURES if name == 'wrong_alias' else ()
    expected_exit = 0 if name == 'native' else 1
    failures = [label for status, label in checks if status == 'FAIL']
    counts = (len(checks), len(checks) - len(failures), len(failures))
    errors = []
    if name not in ('native', 'missing_owner', 'wrong_alias'):
        errors.append('unknown fixture case')
    if not terminal or timed_out or exit_code != expected_exit:
        errors.append('terminal exit differs from expected or run timed out')
    if tuple(label for _, label in checks) != sequence:
        errors.append('check names, order or scope differ from fixture protocol')
    if Counter(failures) != Counter(expected_failures):
        errors.append('failure names or multiplicities differ from expected')
    expected_lines = [TAG + ' ' + status + ' ' + label for status, label in checks]
    if aborted:
        if summaries or tagged != expected_lines or stderr.splitlines() != [ABORT_LINE]:
            errors.append('guarded abort must end the exact prefix without a summary')
    elif summaries != [counts] or tagged != expected_lines + [
            TAG + ' RESULT checks=%d passed=%d failed=%d' % counts]:
        errors.append('exactly one final summary must reconcile all PASS/FAIL records')
    return {'check_names': [label for _, label in checks], 'actual_counts': counts,
            'failures': failures, 'summaries': summaries, 'grading_errors': errors,
            'matched_expected': not errors}


def grade_sampler_fixture(stdout, stderr, name, exit_code, timed_out, terminal=True):
    """Require the independently named sampler protocol or its safe abort."""
    lines = [line for line in (stdout + '\n' + stderr).splitlines() if line.startswith(SAMPLER_TAG)]
    checks = [m.groups() for line in lines if (m := re.fullmatch(
        re.escape(SAMPLER_TAG) + r' (PASS|FAIL) (.+)', line))]
    aborted = name == 'missing_sampler_owner'
    sequence = SAMPLER_CHECKS[:16] if aborted else SAMPLER_CHECKS
    failures = [label for status, label in checks if status == 'FAIL']
    expected = ('queued_version_survives_more_than_seven_option_changes',) if aborted else ()
    counts = (len(checks), len(checks) - len(failures), len(failures))
    expected_lines = [SAMPLER_TAG + ' ' + status + ' ' + label for status, label in checks]
    expected_lines += [SAMPLER_TAG + ' ' + SAMPLER_ABORT] if aborted else [
        SAMPLER_TAG + ' RESULT checks=%d passed=%d failed=%d' % counts]
    errors = []
    if name not in ('native', 'missing_sampler_owner') or not terminal or timed_out or exit_code != (1 if aborted else 0):
        errors.append('unknown sampler case or unsuccessful terminal protocol')
    if tuple(label for _, label in checks) != sequence or Counter(failures) != Counter(expected):
        errors.append('sampler names/order/failure multiplicity differ from protocol')
    if lines != expected_lines or stderr.strip():
        errors.append('sampler summary/abort does not reconcile or setup error present')
    return {'check_names': [label for _, label in checks], 'actual_counts': counts,
            'failures': failures, 'grading_errors': errors, 'matched_expected': not errors}


def replay_summaries(raw):
    return re.findall(r'(\[(?:GPU:REPLAY|VFETCH)\] SUMMARY[^\r\n]*)', raw)


def grade_replay(raw, exit_code, timed_out, terminal, expected):
    summaries = replay_summaries(raw)
    gpu = [line for line in summaries if line.startswith('[GPU:REPLAY]')]
    fetch = [line for line in summaries if line.startswith('[VFETCH] SUMMARY vsParsed=')]
    errors = []
    if not terminal or timed_out or exit_code != 0:
        errors.append('replay lacks a successful terminal exit')
    if len(gpu) != 1 or len(fetch) != 1 or 'frames=1 ' not in gpu[0] or 'parse=CLEAN-EOF' not in gpu[0]:
        errors.append('complete one-frame GPU/VFETCH summary missing')
    if not expected or summaries != expected:
        errors.append('complete replay summary differs from reference')
    return {'summaries': summaries, 'grading_errors': errors, 'matched_expected': not errors}


def load_reference(reference):
    receipt = json.loads((reference / 'receipt.json').read_text(encoding='utf-8-sig'))
    entries = [entry for entry in receipt['comparisons'] if entry['trace'] == TRACE]
    if len(entries) != 1:
        raise ValueError('Reference must identify exactly one title trace')
    entry = entries[0]
    proof = reference / (TRACE + '_proof.png')
    if shared.digest(proof) != entry['proof_sha256']:
        raise ValueError('Reference proof hash differs')
    stdout, stderr = reference / (TRACE + '.out'), reference / (TRACE + '.err')
    if shared.digest(stdout) != entry['raw_sha256'] or shared.digest(stderr) != entry['stderr_sha256']:
        raise ValueError('Reference raw-log hashes differ')
    grade = grade_replay(stdout.read_text(errors='replace') + '\n' + stderr.read_text(errors='replace'),
                         entry['exit_code'], entry['timed_out'], entry['terminal_observed'], entry['summaries'])
    if not grade['matched_expected'] or entry['different_pixels'] != 0 or not entry['draw_hashes_equal']:
        raise ValueError('Reference lacks passing complete replay evidence')
    return entry


def prepare_link(target, lines, folder, original, replacement):
    wrapped = next(line for line in lines if ' -o ' + target + '.exe ' in line and ' -c ' not in line)
    body = wrapped.split(' /C "cd . && ', 1)[1].rsplit(' && ', 1)[0]
    argv = shared.command_args(body)
    responses = [part for part in argv if part.startswith('@')]
    if responses:
        if len(responses) != 1 or target != 'SupermanReturns':
            raise ValueError('Unexpected response-file linker layout')
        ninja = (ROOT / 'build/build.ninja').read_text(encoding='utf-8')
        block = re.search(r'^build SupermanReturns\.exe: ([^\n]+)\n((?:  [^\n]*\n)+)', ninja, re.M)
        if not block:
            raise ValueError('Production Ninja target missing')
        inputs = block[1].split(' ', 1)[1].split(' |', 1)[0]
        properties = dict(re.findall(r'^  (\w+) = (.*)$', block[2], re.M))
        rules = (ROOT / 'build/CMakeFiles/rules.ninja').read_text(encoding='utf-8')
        rule = re.search(r'^rule CXX_EXECUTABLE_LINKER__SupermanReturns_Release\n((?:  [^\n]*\n)+)', rules, re.M)
        if not rule or 'rspfile_content = $in $LINK_PATH $LINK_LIBRARIES' not in rule[1]:
            raise ValueError('Unrecognized production response rule')
        content = inputs + ' ' + properties['LINK_PATH'] + ' ' + properties['LINK_LIBRARIES']
        # Clang's response lexer treats backslashes as escapes, including in a
        # Windows absolute path. Use a quoted forward-slash path in the file.
        content = shared.replace_once(content, original, '"' + replacement.as_posix() + '"')
        response = folder / 'control.rsp'
        response.write_text(content, encoding='utf-8')
        argv[argv.index(responses[0])] = '@' + str(response)
    else:
        argv[argv.index(original)] = str(replacement)
    argv[argv.index('-o') + 1] = str(folder / (target + '.exe'))
    return ['/implib:' + str(folder / 'control.lib') if part.startswith('/implib:') else
            '/pdb:' + str(folder / 'control.pdb') if part.startswith('/pdb:') else part for part in argv]


def build_mutant(name, target, source_name, source, bank, ninja):
    folder = bank / name
    folder.mkdir()
    mutated = folder / Path(source_name).name
    mutated.write_text(source, encoding='utf-8')
    lines = subprocess.run([ninja, '-t', 'commands', target], cwd=ROOT / 'build',
                           capture_output=True, text=True, check=True).stdout.splitlines()
    suffix = source_name + '.obj'
    compile_line = next(line for line in lines if suffix in line and ' -c ' in line)
    argv = shared.command_args(compile_line)
    output = folder / 'mutant.obj'
    for flag, value in [('-MT', str(output)), ('-MF', str(output) + '.d'),
                        ('-o', str(output)), ('-c', str(mutated))]:
        argv[argv.index(flag) + 1] = value
    argv.insert(1, '-I' + str(ROOT / 'gfx'))
    original = next(part for part in shared.command_args(compile_line) if part.endswith(suffix))
    link = prepare_link(target, lines, folder, original, output)
    receipt = {'name': name, 'source_sha256': shared.digest(mutated), 'compile_arguments': argv,
               'link_arguments': link}
    for stage, command in [('compile', argv), ('link', link)]:
        with (folder / (stage + '.log')).open('wb') as log:
            result = subprocess.run([sys.executable, str(ROOT / 'tools/build_slot.py'), '--owner',
                'translated_constant_control_' + name, '--capacity', '1', '--'] + command,
                cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, timeout=180)
        receipt[stage + '_exit_code'] = result.returncode
        receipt[stage + '_log_sha256'] = shared.digest(folder / (stage + '.log'))
        save(folder / 'build_receipt.json', receipt)
        if result.returncode:
            raise RuntimeError(name + ' ' + stage + ' failed')
    exe = folder / (target + '.exe')
    receipt['binary_sha256'] = shared.digest(exe)
    if target == 'SupermanReturns':
        receipt['dll_pins'] = {}
        for dll in (ROOT / 'build').glob('*.dll'):
            shutil.copy2(dll, folder / dll.name)
            receipt['dll_pins'][dll.name] = shared.digest(folder / dll.name)
    save(folder / 'build_receipt.json', receipt)
    print('Built ' + name, flush=True)
    return exe


def run(exe, cwd, env, folder, active):
    start = time.monotonic()
    with (folder / 'stdout.log').open('wb') as out, (folder / 'stderr.log').open('wb') as err:
        proc = subprocess.Popen([str(exe)], cwd=cwd, env=env, stdout=out, stderr=err)
        active['proc'] = proc
        try:
            code = proc.wait(timeout=120)
            timed_out = False
        except subprocess.TimeoutExpired:
            timed_out = True
            termination = shared.terminate_child(proc)
            save(folder / 'child_termination.json', {'pid': proc.pid, **termination})
            code = termination['exit_code']
    return {'pid': proc.pid, 'exit_code': code, 'terminal_observed': proc.poll() is not None,
            'timed_out': timed_out, 'elapsed_seconds': time.monotonic() - start,
            'stdout_sha256': shared.digest(folder / 'stdout.log'),
            'stderr_sha256': shared.digest(folder / 'stderr.log')}


def production_inputs(folder, reference):
    def quote(value):
        return "'" + str(value).replace("'", "''") + "'"
    command = '. ' + quote(ROOT / 'tools/sr_artifacts.ps1') + '; New-SrAttemptRoot -Out ' + quote(folder / 'raw') + ' -Attempt 1 | ConvertTo-Json -Depth 4'
    attempt = json.loads(subprocess.check_output(['powershell', '-NoProfile', '-Command', command], text=True))
    env = {key: value for key, value in os.environ.items() if not key.startswith('SR_')}
    env.update(reference['gates'])
    # Both paired processes use explicit identical inputs. Only their binary and
    # diagnostic output paths differ. SR_LOG keeps completion evidence even when
    # no instance is inherited; the old player-profile control lacked summaries.
    env.update(SR_MAIN_ROOT=str(ROOT.parent / 'dev'), SR_INSTANCE_ID='translated_constant_production_pair',
               SR_LOG='dev', SR_REPLAY_DRAW_HASH=str(folder / 'draws.txt'))
    return Path(attempt['FrameDir']), env, attempt


def compare_images(actual, expected):
    import numpy as np
    from PIL import Image
    image = np.asarray(Image.open(actual).convert('RGB'), dtype=np.int16)
    reference = np.asarray(Image.open(expected).convert('RGB'), dtype=np.int16)
    if image.shape != reference.shape:
        raise ValueError('Replay image dimensions differ')
    delta = np.abs(image - reference)
    return {'different_pixels': int(np.any(delta, axis=2).sum()), 'max_difference': int(delta.max())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--only-production', action='store_true',
                        help='Run only the matched production pair; keep retained fixture runs')
    parser.add_argument('--family', choices=('constants', 'geometry', 'samplers'), default='constants')
    args = parser.parse_args()
    bank, reference = args.bank.resolve(), args.reference.resolve()
    bank.mkdir(parents=True, exist_ok=True)
    if any(bank.iterdir()):
        raise ValueError('Use a fresh control bank')
    reference_entry = load_reference(reference)
    shutil.copy2(Path(__file__), bank / 'tool_source_at_execution.py')
    cache = (ROOT / 'build/CMakeCache.txt').read_text(encoding='utf-8')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M)[1]
    files = ['gfx/translated_constants.hpp', 'gfx/translated_constants.cpp', 'gfx/d3d12_backend.hpp',
             'gfx/d3d12_backend.cpp', 'tools/difftest/translated_constants_test.cpp',
             'tools/difftest/translated_constant_controls.py', 'tools/difftest/native_draw_controls.py']
    if args.family == 'samplers':
        files += ['gfx/native_draw.hpp', 'gfx/native_draw.cpp', 'gfx/translated_samplers.hpp',
                  'gfx/translated_samplers.cpp', 'tools/difftest/translated_samplers_test.cpp']
    pins = {name: shared.digest(ROOT / name) for name in files}
    source = (ROOT / 'gfx/translated_constants.cpp').read_text(encoding='utf-8')
    owner = shared.replace_once(source, 'const HRESULT hr = recording.keep(m_storage);',
                               'if (!recording.commands()) return E_ABORT;\n  const HRESULT hr = S_OK; // Defect: no owner retained.')
    alias = shared.replace_once(source, 'case 0: case 3: return 0;',
                                'case 0: return 0;\n    case 3: return m_vsBytes;  // Defect: dynamic VS alias reads PS.')
    production = (ROOT / 'gfx/d3d12_backend.cpp').read_text(encoding='utf-8')
    production = shared.replace_once(production, '  ++m_nativeConstantDraws;',
        '  if (b.psHash == 0x2679C172F83DE85Dull) { // Defect: route tonemap PS to VS constants.\n'
        '    m_nativeRecording->commands()->SetGraphicsRootConstantBufferView(1, constants.address(0));\n'
        '    m_nativeRecording->commands()->SetGraphicsRootConstantBufferView(4, constants.address(0));\n'
        '  }\n  ++m_nativeConstantDraws;')
    cases = [('missing_owner', 'sr_translated_constants_test', 'gfx/translated_constants.cpp', owner),
             ('wrong_alias', 'sr_translated_constants_test', 'gfx/translated_constants.cpp', alias),
             ('wrong_production_binding', 'SupermanReturns', 'gfx/d3d12_backend.cpp', production)]
    if args.family == 'geometry':
        backend = (ROOT / 'gfx/d3d12_backend.cpp').read_text(encoding='utf-8')
        stride = shared.replace_once(backend,
            'const D3D12_VERTEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), b.xlatStride};',
            'const D3D12_VERTEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), 0}; // Defect: repeat first vertex.')
        index = shared.replace_once(backend,
            'const D3D12_INDEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), DXGI_FORMAT_R32_UINT};',
            'const D3D12_INDEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), DXGI_FORMAT_R16_UINT}; // Defect: reinterpret 32-bit indices.')
        cases = [('wrong_vertex_stride', 'SupermanReturns', 'gfx/d3d12_backend.cpp', stride),
                 ('wrong_index_format', 'SupermanReturns', 'gfx/d3d12_backend.cpp', index)]
    elif args.family == 'samplers':
        native = (ROOT / 'gfx/native_draw.cpp').read_text(encoding='utf-8')
        missing = shared.replace_once(native, 'm_payload->descriptors.push_back(descriptors);',
                                      '/* Defect: omit immutable descriptor owner. */')
        sampler = (ROOT / 'gfx/translated_samplers.cpp').read_text(encoding='utf-8')
        wrong_filter = shared.replace_once(sampler,
            'sampler.Filter = linear ? (anisotropy ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR)\n'
            '                            : D3D12_FILTER_MIN_MAG_MIP_POINT;',
            'sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; // Defect: all production samplers point.')
        cases = [('missing_sampler_owner', 'sr_translated_samplers_test', 'gfx/native_draw.cpp', missing),
                 ('wrong_sampler_filter', 'SupermanReturns', 'gfx/translated_samplers.cpp', wrong_filter)]
    if args.only_production:
        cases = [case for case in cases if case[1] == 'SupermanReturns']
    executables = [build_mutant(*case, bank, ninja) for case in cases]
    baseline = ROOT / 'build/SupermanReturns.exe'
    baseline_pins = {str(path): shared.digest(path) for path in [baseline, *(ROOT / 'build').glob('*.dll')]}
    trace = Path(reference_entry['gates']['SR_GPU_TRACE_REPLAY'])
    input_pins = {'trace': {str(trace): shared.digest(trace)}, 'normal_binary': baseline_pins,
                  'reference_receipt_sha256': shared.digest(reference / 'receipt.json')}
    # A freshly executed normal control establishes the effective configuration
    # for the mutant, then must reproduce the retained reference exactly.
    (bank / 'production_baseline').mkdir()
    cases.insert(0, ('production_baseline',))
    executables.insert(0, baseline)
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]
    slot_owner = 'translated_constant_controls'
    acquired = subprocess.run(slot + ['acquire', '--owner', slot_owner], capture_output=True, text=True, timeout=120)
    if acquired.returncode:
        raise RuntimeError(acquired.stdout + acquired.stderr)
    receipt = {'scope': 'Bounded translated-input/actual production-binding defect controls; no native-zero acceptance',
               'family': args.family,
               'source_pins': pins, 'input_pins': input_pins,
               'slot_acquire_exit_code': acquired.returncode, 'runs': []}
    active = {'proc': None}
    try:
        for case, exe in zip(cases, executables):
            name = case[0]
            folder = bank / name
            env = os.environ.copy()
            cwd = folder
            production_case = name == 'production_baseline' or case[1] == 'SupermanReturns'
            if production_case:
                from PIL import Image
                cwd, env, attempt = production_inputs(folder, reference_entry)
            result = run(exe, cwd, env, folder, active)
            result['name'] = name
            save(folder / 'terminal_receipt.json', result)
            raw = (folder / 'stdout.log').read_text(errors='replace')
            stderr = (folder / 'stderr.log').read_text(errors='replace')
            if name in ('missing_owner', 'wrong_alias'):
                result.update(grade_fixture(raw, stderr, name, result['exit_code'],
                                            result['timed_out'], result['terminal_observed']))
            elif name == 'missing_sampler_owner':
                result.update(grade_sampler_fixture(raw, stderr, name, result['exit_code'],
                                                   result['timed_out'], result['terminal_observed']))
            else:
                grade = grade_replay(raw + '\n' + stderr, result['exit_code'], result['timed_out'],
                                    result['terminal_observed'], reference_entry['summaries'])
                result.update(grade)
                image = Image.open(cwd / 'replay_frame_1.bmp').convert('RGB')
                image.save(folder / 'proof.png')
                expected = reference / (TRACE + '_proof.png') if name == 'production_baseline' else bank / 'production_baseline/proof.png'
                result.update(compare_images(folder / 'proof.png', expected))
                result['raw_attempt'] = attempt
                result['gates'] = {key: value for key, value in env.items() if key.startswith('SR_')}
                result['proof_sha256'] = shared.digest(folder / 'proof.png')
                result['draws_sha256'] = shared.digest(folder / 'draws.txt')
                if name == 'production_baseline':
                    expected_draws = reference / (TRACE + '_draws.txt')
                    result['draw_hashes_equal'] = shared.digest(expected_draws) == result['draws_sha256']
                    ok = result['different_pixels'] == 0 and result['max_difference'] == 0
                else:
                    normal = receipt['runs'][0]
                    result['draw_hashes_equal'] = normal['draws_sha256'] == result['draws_sha256']
                    result['effective_gates_equal'] = {k: v for k, v in normal['gates'].items() if k != 'SR_REPLAY_DRAW_HASH'} == {
                        k: v for k, v in result['gates'].items() if k != 'SR_REPLAY_DRAW_HASH'}
                    ok = result['different_pixels'] > 64 and result['max_difference'] > 8 and result['effective_gates_equal']
                result['matched_expected'] = grade['matched_expected'] and ok and result['draw_hashes_equal']
            receipt['runs'].append(result)
            save(folder / 'run_receipt.json', result)
            print(name + ' matched_expected=' + str(result['matched_expected']), flush=True)
            if not result['matched_expected']:
                raise RuntimeError('Unexpected defect-control outcome: ' + name)
    finally:
        try:
            release = shared.release_slot(slot, slot_owner, bank, active['proc'])
            receipt['slot_release_exit_code'] = release.returncode
        finally:
            receipt['slot_released'] = receipt.get('slot_release_exit_code') == 0
            receipt['source_pins_unchanged'] = all(shared.digest(ROOT / name) == value for name, value in pins.items())
            receipt['normal_binary_pins_unchanged'] = all(shared.digest(Path(name)) == value for name, value in baseline_pins.items())
            receipt['trace_unchanged'] = shared.digest(trace) == input_pins['trace'][str(trace)]
            save(bank / 'receipt.json', receipt)
    return 0 if release.returncode == 0 and all(receipt[key] for key in (
        'source_pins_unchanged', 'normal_binary_pins_unchanged', 'trace_unchanged')) else 2


if __name__ == '__main__':
    raise SystemExit(main())
