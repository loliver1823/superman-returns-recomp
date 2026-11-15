#!/usr/bin/env python3
"""Compile isolated native draw fault controls using the built target's exact argv.

Build sr_native_draw_test first through tools/build_slot.py. Then run:
    python tools/difftest/native_draw_controls.py --bank archive/native_draw_encoding_v5
Sources and executables in the checkout are never mutated. Each compiler/link
invocation uses the shared build slot; all WARP processes use one serial game
slot. Missing submission ownership runs only the empty-list probe. The missing
CBV owner control aborts with its queue delayed, before dangling GPU execution.
This proves the standalone native core, not production wiring or release status.
"""
import argparse
from collections import Counter
import hashlib
import json
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIXEL_FAILURES = {
    'decoded_vertices_exact_pixels_after_owner_destruction',
    'fetch_vertices_exact_pixels_after_owner_destruction',
    'indexed_fetch_exact_pixels_after_owner_destruction',
    'sparse_mrt_preserves_target_two_index',
}
OWNERSHIP_CHECKS = (
    'destroyed_recording_packet_retained_by_queue',
    'recording_packet_retired_after_checked_completion',
)
RETAIN_CHECK = 'each_independent_owner_retained_before_gpu_execution'
ABORT_LINE = '[NATIVE_DRAW_TEST] ABORT dangling queued resource before GPU execution'
PREFIX_CHECKS = OWNERSHIP_CHECKS + (
    'constant_staging_owns_aligned_source_copy', 'zero_padding_metadata_matches_owned_bytes',
    'later_upload_does_not_overwrite_published_bytes', 'invalid_constant_staging_preserves_output',
    'retained_page_blocks_budget_reuse', 'final_slice_returns_page_budget', 'completed_page_budget_can_be_reused',
    'root_buffers_need_declared_extents', 'null_static_sampler_array_rejected',
    'unsupported_table_preserves_root', 'unbounded_table_rejected',
    'invalid_sampler_filter_preserves_version', 'zero_anisotropy_rejected', 'invalid_comparison_rejected',
)
BRANCH_CHECKS = ('constant_and_vertex_allocations_are_independent', 'native_draw_retained',
                 'gpu_is_delayed', 'queue_owns_packet_before_completion', RETAIN_CHECK)
RETIRE_CHECKS = ('completed_packet_retired', 'each_independent_owner_retired_after_completion')
# This is the independently specified fixture protocol. A truncated normal run
# cannot redefine the scope of every control to a smaller passing sequence.
FULL_CHECKS = PREFIX_CHECKS + (
    'null_rtv_owns_descriptor_without_texture', 'null_rtv_clear_rejected', 'invalid_null_rtv_preserves_output',
    BRANCH_CHECKS[0], 'borrowed_upload_keep_rejected', 'texture_mip_extent_rejected',
    'buffer_descriptor_slice_extent_rejected', 'raw_buffer_format_rejected', 'invalid_component_mapping_rejected',
    'write_enabled_null_rtv_rejected_before_recording', 'short_aligned_cbv_extent_rejected_before_recording',
    'unsealed_cbv_padding_rejected_before_recording', 'missing_pixel_scale_rejected_before_recording',
    'vertex_extent_rejected_before_recording', 'native_maximum_stride_rejected_before_recording',
) + BRANCH_CHECKS[1:] + (
    'decoded_vertices_exact_pixels_after_owner_destruction', 'sparse_mrt_preserves_target_two_index',
) + RETIRE_CHECKS + BRANCH_CHECKS + (
    'fetch_vertices_exact_pixels_after_owner_destruction',
) + RETIRE_CHECKS + BRANCH_CHECKS + (
    'indexed_fetch_exact_pixels_after_owner_destruction',
) + RETIRE_CHECKS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf-8')


def grade_log(raw, name, expected_failures, exit_code, timed_out):
    tagged = [line for line in raw.splitlines() if line.startswith('[NATIVE_DRAW_TEST]')]
    checks = [match.groups() for line in tagged
              if (match := re.fullmatch(r'\[NATIVE_DRAW_TEST\] (PASS|FAIL) (.+)', line))]
    summaries = [tuple(map(int, match.groups())) for line in tagged if (match := re.fullmatch(
        r'\[NATIVE_DRAW_TEST\] RESULT checks=(\d+) passed=(\d+) failed=(\d+)', line))]
    aborted = name == 'missing_cbv_owner'
    sequence = FULL_CHECKS[:FULL_CHECKS.index(RETAIN_CHECK) + 1] if aborted else (
        OWNERSHIP_CHECKS if name == 'missing_submission_owners' else FULL_CHECKS)
    failures = [label for status, label in checks if status == 'FAIL']
    actual = (len(checks), len(checks) - len(failures), len(failures))
    expected_exit = 0 if name == 'native' else 1
    errors = []
    if timed_out or exit_code != expected_exit:
        errors.append('terminal exit differs from expected or run timed out')
    if tuple(label for _, label in checks) != sequence:
        errors.append('check names, order or scope differ from fixture protocol')
    if Counter(failures) != Counter(expected_failures):
        errors.append('failure names or multiplicities differ from expected')
    if aborted:
        if summaries or tagged[len(checks):] != [ABORT_LINE] or tagged[:len(checks)] != [
                '[NATIVE_DRAW_TEST] ' + status + ' ' + label for status, label in checks]:
            errors.append('guarded abort must end the exact prefix without a summary')
    elif summaries != [actual] or len(tagged) != len(checks) + 1 or tagged[-1:] != [
            '[NATIVE_DRAW_TEST] RESULT checks=%d passed=%d failed=%d' % actual]:
        errors.append('exactly one final summary must reconcile all PASS/FAIL records')
    return {'scope': 'guarded-prefix' if aborted else 'ownership-only' if name == 'missing_submission_owners' else 'full',
            'expected_exit_code': expected_exit, 'expected_checks': len(sequence), 'actual_counts': actual,
            'check_names': [label for _, label in checks], 'failures': failures,
            'summaries': summaries, 'grading_errors': errors, 'matched_expected': not errors}


def terminate_child(proc):
    errors = []
    if proc.poll() is None:
        try:
            proc.kill()
        except Exception as error:
            errors.append('kill: ' + repr(error))
        try:
            proc.wait(timeout=10)
        except Exception as error:
            errors.append('wait: ' + repr(error))
    return {'termination_errors': errors, 'terminal_observed': proc.poll() is not None,
            'exit_code': proc.returncode}


def release_slot(slot, owner, bank, active_proc):
    if active_proc is not None and active_proc.poll() is None:
        termination = terminate_child(active_proc)
        write_json(bank / 'child_termination.json', {'pid': active_proc.pid, 'arguments': active_proc.args, **termination})
        if not termination['terminal_observed']:
            write_json(bank / 'slot_unresolved_child.json', {'owner': owner, 'slot_released': False,
                'pid': active_proc.pid, 'arguments': active_proc.args, **termination})
            raise RuntimeError('Child PID %s has no confirmed terminal state; game slot retained. See %s' % (
                active_proc.pid, bank / 'slot_unresolved_child.json'))
    release = subprocess.run(slot + ['release', '--owner', owner], capture_output=True, text=True, timeout=30)
    write_json(bank / 'slot_release.json', {'exit_code': release.returncode, 'stdout': release.stdout, 'stderr': release.stderr})
    return release


def command_args(line):
    return [p[1:-1] if p.startswith('"') and p.endswith('"') else p
            for p in shlex.split(line, posix=False)]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Fault-control anchor must match exactly once: ' + old)
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=pathlib.Path, required=True)
    args = parser.parse_args()
    bank = args.bank.resolve()
    if bank.exists() and any((bank / name).exists() for name in ('controls_receipt.json', 'control_inputs.json')):
        raise ValueError('Use a fresh evidence bank; do not overwrite prior control receipts')
    bank.mkdir(parents=True, exist_ok=True)
    files = ['CMakeLists.txt', 'gfx/native_draw.hpp', 'gfx/native_draw.cpp',
             'gfx/native_upload.hpp', 'gfx/native_upload.cpp', 'gfx/native_pipeline.hpp',
             'gfx/native_pipeline.cpp', 'gfx/native_image_store.hpp', 'gfx/native_image_store.cpp',
             'tools/difftest/native_draw_test.cpp', 'tools/difftest/native_draw_controls.py',
             'tools/difftest/test_native_draw_controls.py']
    pins = {name: digest(ROOT / name) for name in files}
    cache = (ROOT / 'build/CMakeCache.txt').read_text(encoding='utf-8')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M).group(1)
    lines = subprocess.run([ninja, '-t', 'commands', 'sr_native_draw_test'], cwd=ROOT / 'build',
                           capture_output=True, text=True, check=True).stdout.splitlines()
    lines = [line.strip() for line in lines if line.strip()]
    wrapped = next(line for line in lines if ' -o sr_native_draw_test.exe ' in line and ' -c ' not in line)
    prefix, link = wrapped.split(' /C "cd . && ', 1)
    if not prefix.lower().endswith('cmd.exe') or not link.endswith(' && cd ."'):
        raise ValueError('Unexpected native linker wrapper')
    link = command_args(link[:-len(' && cd ."')])
    object_pins = {part: digest(ROOT / 'build' / part) for part in link if part.endswith('.obj')}
    write_json(bank / 'control_inputs.json', {'source_pins': pins, 'object_pins': object_pins})
    raw_draw = (ROOT / 'gfx/native_draw.cpp').read_text(encoding='utf-8')
    raw_image = (ROOT / 'gfx/native_image_store.cpp').read_text(encoding='utf-8')
    raw_upload = (ROOT / 'gfx/native_upload.cpp').read_text(encoding='utf-8')
    no_draw, count = re.subn(r'm_commands->Draw(?:Indexed)?Instanced\([^;\n]+\)', '(void)0', raw_draw)
    if count != 3:
        raise ValueError('Expected all three actual native draw calls')
    cases = [
        ('no_draw_commands', 'native_draw.cpp', no_draw, [], PIXEL_FAILURES),
        ('wrong_cbv_route', 'native_draw.cpp', replace_once(raw_draw,
            'SetGraphicsRootConstantBufferView(b.parameter, b.buffer.address())',
            'SetGraphicsRootConstantBufferView(b.parameter, b.parameter == 1 ? d.bindings[3].buffer.address() : b.buffer.address())'),
            [], PIXEL_FAILURES),
        ('missing_submission_owners', 'native_image_store.cpp', replace_once(raw_image,
            'pending.owners.assign(owners.begin(), owners.end());', '// Fault: omit submission packet owners.'),
            ['--ownership-only'], {'destroyed_recording_packet_retained_by_queue'}),
        ('missing_cbv_owner', 'native_draw.cpp', replace_once(raw_draw,
            'PreparedNativeDraw retained = d;', 'PreparedNativeDraw retained = d; retained.bindings[0].buffer = {};'),
            [], {'each_independent_owner_retained_before_gpu_execution'}),
        ('smaller_declared_extent', 'native_draw.cpp', replace_once(raw_draw,
            'rule.requiredBufferBytes = extent;', 'rule.requiredBufferBytes = extent == 512 ? 256 : extent;'),
            [], {'short_aligned_cbv_extent_rejected_before_recording'}),
        ('bad_constant_padding', 'native_upload.cpp', replace_once(raw_upload,
            'std::memset(page->mapped + offset + bytes.size(), 0, paddedBytes - bytes.size());',
            'std::memset(page->mapped + offset + bytes.size(), 0xa5, paddedBytes - bytes.size());'),
            [], {'zero_padding_metadata_matches_owned_bytes'}),
    ]
    builds = []
    for name, filename, source, _, _ in cases:
        target = bank / 'controls' / name
        target.mkdir(parents=True, exist_ok=True)
        mutated = target / filename
        mutated.write_text(source, encoding='utf-8')
        compile_line = next(line for line in lines if 'gfx/' + filename + '.obj' in line and ' -c ' in line)
        compile_argv = command_args(compile_line)
        output = target / 'mutant.obj'
        for flag, value in [('-MT', str(output)), ('-MF', str(output) + '.d'), ('-o', str(output)), ('-c', str(mutated))]:
            compile_argv[compile_argv.index(flag) + 1] = value
        compile_argv.insert(1, '-I' + str(ROOT / 'gfx'))
        link_argv = list(link)
        original = next(p for p in link_argv if p.endswith('gfx/' + filename + '.obj'))
        link_argv[link_argv.index(original)] = str(output)
        exe = target / (name + '.exe')
        link_argv[link_argv.index('-o') + 1] = str(exe)
        link_argv = ['/implib:' + str(target / 'control.lib') if p.startswith('/implib:') else
                     '/pdb:' + str(target / 'control.pdb') if p.startswith('/pdb:') else p for p in link_argv]
        receipt = {'name': name, 'source_sha256': digest(mutated), 'compile_arguments': compile_argv,
                   'link_arguments': link_argv}
        for stage, command in [('compile', compile_argv), ('link', link_argv)]:
            log = target / (stage + '_raw.log')
            with log.open('wb') as stream:
                result = subprocess.run([sys.executable, str(ROOT / 'tools/build_slot.py'), '--owner',
                    'native_draw_control_' + name, '--capacity', '1', '--'] + command,
                    cwd=ROOT / 'build', stdout=stream, stderr=subprocess.STDOUT, timeout=180)
            receipt[stage + '_exit_code'] = result.returncode
            receipt[stage + '_raw_sha256'] = digest(log)
            write_json(target / 'build_receipt.json', receipt)
            if result.returncode:
                raise RuntimeError(name + ' ' + stage + ' failed; inspect ' + str(log))
        receipt['binary_sha256'] = digest(exe)
        write_json(target / 'build_receipt.json', receipt)
        builds.append(receipt)
        print('[NATIVE_DRAW_CONTROLS] built ' + name, flush=True)

    snapshot = bank / 'snapshot'
    snapshot.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'build/sr_native_draw_test.exe', snapshot / 'sr_native_draw_test.exe')
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]
    owner = 'native_draw_controls'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--max-hours', '0.05', '--poll', '5'],
                             capture_output=True, text=True, timeout=190)
    write_json(bank / 'slot_acquire.json', {'exit_code': acquire.returncode, 'stdout': acquire.stdout, 'stderr': acquire.stderr})
    if acquire.returncode:
        raise RuntimeError('Game slot acquisition failed')
    runs = []
    active_proc = None
    try:
        tests = [('native', snapshot / 'sr_native_draw_test.exe', [], set())]
        tests += [(name, bank / 'controls' / name / (name + '.exe'), argv, expected)
                  for name, _, _, argv, expected in cases]
        for name, exe, argv, expected in tests:
            log = bank / (name + '_raw.log')
            binary_pin = digest(exe)
            started = time.monotonic()
            with log.open('wb') as stream:
                proc = active_proc = subprocess.Popen([str(exe)] + argv, cwd=exe.parent, stdout=stream, stderr=subprocess.STDOUT)
                write_json(bank / 'active_child.json', {'name': name, 'pid': proc.pid, 'arguments': proc.args,
                    'binary_sha256': binary_pin, 'terminal_observed': False})
                timed_out = False
                termination = {}
                try:
                    proc.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    termination = terminate_child(proc)
            terminal_observed = proc.poll() is not None
            raw = log.read_text(encoding='utf-8')
            grade = grade_log(raw, name, expected, proc.returncode, timed_out)
            receipt = {'name': name, 'arguments': [str(exe)] + argv, 'pid': proc.pid, 'exit_code': proc.returncode,
                       'timed_out': timed_out, 'elapsed_seconds': round(time.monotonic() - started, 3),
                       'binary_sha256': binary_pin, 'binary_pin_unchanged': digest(exe) == binary_pin,
                       'raw_log_sha256': digest(log), 'terminal_observed': terminal_observed, **termination, **grade}
            runs.append(receipt)
            write_json(bank / 'runs_partial.json', runs)
            write_json(bank / 'active_child.json', receipt)
            if not terminal_observed:
                raise RuntimeError('Child termination remains unresolved; serial runs stopped')
            active_proc = None
            print('[NATIVE_DRAW_CONTROLS] ' + name + ' expected=' + str(grade['matched_expected']), flush=True)
    finally:
        release = release_slot(slot, owner, bank, active_proc)
    receipt = {'scope': 'Standalone native core fault sensitivity; production/full-live/release acceptance open',
               'source_pins': pins, 'source_pins_unchanged': all(digest(ROOT / p) == h for p, h in pins.items()),
               'object_pins': object_pins,
               'object_pins_unchanged': all(digest(ROOT / 'build' / p) == h for p, h in object_pins.items()),
               'builds': builds, 'runs': runs, 'slot_release_exit_code': release.returncode}
    write_json(bank / 'controls_receipt.json', receipt)
    return 0 if receipt['source_pins_unchanged'] and receipt['object_pins_unchanged'] and release.returncode == 0 and all(
        run['matched_expected'] and run['binary_pin_unchanged'] for run in runs) else 1


if __name__ == '__main__':
    sys.exit(main())
