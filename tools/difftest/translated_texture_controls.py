#!/usr/bin/env python3
"""Strict protocol grading for sr_translated_textures_test.

The fixture exercises actual compact ABI bindings and 171 delayed GPU pixel
readbacks. Missing checks, duplicate checks, setup errors and truncated output
cannot pass. GPU execution must be serialized with tools/game_slot.py.
"""
import re
from collections import Counter

CHECKS = (
    'uninitialized_arena_preserves_output', 'zero_page_size_rejected',
    'same_page_versions_have_distinct_gpu_offsets',
    'held_page_budget_refuses_without_replacing_output',
    'invalid_view_preserves_published_output',
    'released_version_retires_independently_of_tail',
    'final_version_release_returns_page_budget',
    'discarding_tail_preserves_outstanding_versions', 'last_owner_releases_page',
    'raw_view_uses_nonzero_owned_buffer_offset', 'unchanged_recipes_reuse_version',
    'srv_change_gets_distinct_same_page_version',
    'invalid_cache_selection_preserves_output', 'all_pixel_cases_recorded',
    'GPU_queue_is_actually_delayed',
    'same_page_versions_and_raw_survive_cache_and_producer_destruction',
    'all_171_ABA_texture_and_raw_pixels_exact_after_owner_destruction',
    'all_versions_retire_after_checked_completion',
    'debug_layer_reports_no_error_or_corruption',
)
TAG = '[TRANSLATED_TEXTURES_TEST]'


def grade(stdout, stderr, exit_code, timed_out, terminal=True):
    raw = stdout + '\n' + stderr
    checks = re.findall(r'^\[TRANSLATED_TEXTURES_TEST\] (PASS|FAIL) (\w+)\s*$', raw, re.M)
    summaries = re.findall(r'^\[TRANSLATED_TEXTURES_TEST\] RESULT checks=(\d+) passed=(\d+) failed=(\d+)\s*$', raw, re.M)
    errors = []
    if tuple(name for _, name in checks) != CHECKS:
        errors.append('Missing, duplicate, foreign or reordered check')
    if summaries != [(str(len(CHECKS)), str(len(CHECKS)), '0')]:
        errors.append('Missing or inconsistent complete result')
    if any(result != 'PASS' for result, _ in checks):
        errors.append('Fixture check failed')
    if re.search(r'SETUP FAILED|D3D12 ERROR|ABORT|PIXEL_FAIL', raw):
        errors.append('Fixture error or abort')
    if exit_code != 0 or timed_out or not terminal:
        errors.append('No successful terminal execution')
    # Reject otherwise unparsed protocol statements rather than silently
    # accepting a valid-looking subset of an unexpected fixture transcript.
    lines = [line.strip() for line in raw.splitlines() if line.startswith(TAG)]
    expected = [TAG + ' PASS ' + name for name in CHECKS] + [
        TAG + f' RESULT checks={len(CHECKS)} passed={len(CHECKS)} failed=0']
    if lines != expected:
        errors.append('Unrecognized protocol line or misplaced final summary')
    if stderr.strip():
        errors.append('Fixture stderr is not empty')
    return {'matched_expected': not errors, 'checks': len(checks),
            'failed': [name for result, name in checks if result != 'PASS'],
            'grading_errors': errors}


def grade_control(stdout, stderr, exit_code, timed_out, terminal, failures, ownership=False):
    """Require exactly the intended defect and a complete safe terminal protocol."""
    tagged = [line for line in stdout.splitlines() if line.startswith(TAG)]
    records = [m.groups() for line in tagged if (m := re.fullmatch(
        re.escape(TAG) + r' (PASS|FAIL) (\w+)', line))]
    sequence = CHECKS[:16] if ownership else CHECKS
    actual = Counter(name for verdict, name in records if verdict == 'FAIL')
    counts = (len(records), sum(v == 'PASS' for v, _ in records), sum(actual.values()))
    expected_lines = [TAG + ' ' + verdict + ' ' + name for verdict, name in records]
    expected_lines += [TAG + ' ABORT dangling queued version'] if ownership else [
        TAG + ' RESULT checks=%d passed=%d failed=%d' % counts]
    return {'matched_expected': terminal and not timed_out and exit_code == 1 and
            tuple(name for _, name in records) == sequence and actual == Counter(failures) and
            tagged == expected_lines and not stderr.strip(),
            'checks': len(records), 'failed': dict(actual)}


def main():
    import argparse
    import json
    import os
    from pathlib import Path
    import shutil
    import subprocess
    import sys
    import native_draw_controls as shared
    import translated_constant_controls as controls

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True,
                        help='Successfully built candidate with passing fixtures')
    args = parser.parse_args(); root = Path(__file__).resolve().parents[2]; bank = args.bank.resolve()
    build = json.loads((bank / 'build_receipt.json').read_text())
    fixtures = json.loads((bank / 'fixtures/receipt.json').read_text())
    if build['exit_code'] != 0 or not build['terminal_observed'] or fixtures['matched_expected'] is not True:
        raise RuntimeError('Candidate build/fixtures are not successful terminal evidence')
    guards = [(root, build['source_pins']), (bank / 'source_snapshot', build['source_pins']),
              (bank / 'binary', build['binary_pins']), (root, build['owner_wip_pins'])]
    if not all(shared.digest(base / p) == h for base, pins in guards for p, h in pins.items()):
        raise RuntimeError('Candidate provenance changed')
    out = bank / 'descriptor_controls'; out.mkdir(exist_ok=False)
    os.environ['SR_MAIN_ROOT'] = str(root.parent / 'dev')
    os.environ['TEMP'] = os.environ['TMP'] = str(root / 'archive/native_recovery_20261012/temp')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$',
                     (root / 'build/CMakeCache.txt').read_text(), re.M).group(1)
    target = 'sr_translated_textures_test'
    commands = subprocess.run([ninja, '-t', 'commands', target], cwd=root / 'build',
                              capture_output=True, text=True, check=True).stdout.splitlines()
    link = next(line for line in commands if ' -o ' + target + '.exe ' in line and ' -c ' not in line)
    object_paths = re.findall(r'CMakeFiles/[^\s"]+\.obj', link)
    if len(object_paths) != 7 or len(set(object_paths)) != 7:
        raise RuntimeError('Unexpected fixture object layout')
    object_pins = {p: shared.digest(root / 'build' / p) for p in object_paths}
    harness = ['tools/difftest/translated_texture_controls.py',
               'tools/difftest/test_translated_texture_controls.py',
               'tools/difftest/translated_constant_controls.py',
               'tools/difftest/native_draw_controls.py', 'tools/build_slot.py', 'tools/game_slot.py']
    receipt = {'scope': 'Standalone actual-code descriptor offset, mapping and delayed ownership defects; production wiring/full-live/native-zero remain separate',
               'harness_pins': {p: shared.digest(root / p) for p in harness}, 'object_pins': object_pins,
               'candidate_build_sha256': shared.digest(bank / 'build_receipt.json'),
               'candidate_fixtures_sha256': shared.digest(bank / 'fixtures/receipt.json'), 'runs': []}
    shutil.copy2(Path(__file__), out / 'runner_at_execution.py')
    draw = (root / 'gfx/native_draw.cpp').read_text()
    textures = (root / 'gfx/translated_textures.cpp').read_text()
    cases = [
        ('missing_version_owners', 'gfx/native_draw.cpp', shared.replace_once(draw,
            'retained.m_descriptors == descriptors.m_descriptors',
            'retained.heap() == descriptors.heap()'), {CHECKS[15]}, True),
        ('ignored_page_offsets', 'gfx/native_draw.cpp', shared.replace_once(draw,
            '(uint64_t(m_descriptors->firstDescriptor) + first)', 'uint64_t(first)'),
         {CHECKS[2], CHECKS[11], CHECKS[16]}, False),
        ('layered_2d_alias', 'gfx/translated_textures.cpp', shared.replace_once(textures,
            '0x80000000u | (16u + i)', '0x80000000u | i'), {CHECKS[16]}, False),
    ]
    executables = [controls.build_mutant(name, target, source_name, source, out, ninja)
                   for name, source_name, source, _, _ in cases]
    slot = [sys.executable, str(root / 'tools/game_slot.py')]; owner = 'translated_texture_controls'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--min-free-gb', '12',
                                    '--max-hours', '0.05', '--poll', '5'],
                             capture_output=True, text=True, timeout=190)
    shared.write_json(out / 'slot_acquire.json', {'exit_code': acquire.returncode,
        'stdout': acquire.stdout, 'stderr': acquire.stderr})
    if acquire.returncode:
        raise RuntimeError('Game slot unavailable')
    active = {'proc': None}
    try:
        for case, exe in zip(cases, executables):
            name, _, _, failures, ownership = case
            folder = out / name / 'execution'; folder.mkdir()
            result = controls.run(exe, exe.parent, os.environ.copy(), folder, active)
            result.update(grade_control((folder / 'stdout.log').read_text(),
                (folder / 'stderr.log').read_text(), result['exit_code'], result['timed_out'],
                result['terminal_observed'], failures, ownership), name=name,
                expected_failures=sorted(failures), binary_sha256=shared.digest(exe))
            receipt['runs'].append(result); shared.write_json(out / 'receipt.json', receipt)
            print(name + ' defect-sensitive=' + str(result['matched_expected']), flush=True)
            if not result['matched_expected']:
                raise RuntimeError('Unexpected defect-control outcome: ' + name)
    finally:
        release = shared.release_slot(slot, owner, out, active['proc'])
        receipt['slot_release_exit_code'] = release.returncode
        receipt['candidate_pins_unchanged'] = all(shared.digest(base / p) == h for base, pins in guards for p, h in pins.items())
        receipt['object_pins_unchanged'] = all(shared.digest(root / 'build' / p) == h for p, h in object_pins.items())
        receipt['harness_pins_unchanged'] = all(shared.digest(root / p) == h for p, h in receipt['harness_pins'].items())
        receipt['matched_expected'] = len(receipt['runs']) == 3 and all(r['matched_expected'] for r in receipt['runs']) and release.returncode == 0 and all(
            receipt[k] for k in ('candidate_pins_unchanged', 'object_pins_unchanged', 'harness_pins_unchanged'))
        shared.write_json(out / 'receipt.json', receipt)
    return 0 if receipt['matched_expected'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
