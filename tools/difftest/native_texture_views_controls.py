#!/usr/bin/env python3
"""Check native texture pixels and isolated faults; build the fixture first.

    python tools/difftest/native_texture_views_controls.py --bank archive/<fresh-bank>

Compiles isolated sources with the fixture's exact compiler/linker arguments.
Never changes checkout sources or binaries. Build/game slots serialize all work.
Missing descriptor ownership aborts before releasing a delayed GPU queue.
Standalone coverage only: production textures and native-only shipping stay open.
"""
import argparse
from collections import Counter
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

import native_draw_controls as common

ROOT = pathlib.Path(__file__).resolve().parents[2]
TAG = '[NATIVE_TEXTURE_VIEWS_TEST]'
RETAIN = 'texture_versions_retained_after_producer_and_recording_destruction'
ABORT = TAG + ' ABORT dangling queued texture before GPU execution'
PREFIX = (
    'typed_arrays_volumes_cubes_and_depth_planes_admitted',
    'identical_views_admitted', 'changed_duplicate_view_rejected_without_replacing_output',
    'changed_duplicate_buffer_slice_rejected_without_replacing_output',
    'empty_array_rejected_without_replacing_output', 'array_extent_rejected', 'array_overflow_rejected',
    'array_mip_extent_rejected', 'empty_volume_mips_rejected', 'volume_mip_overflow_rejected',
    'cube_mip_extent_rejected', 'cube_with_fewer_than_six_faces_rejected', 'volume_view_of_array_rejected',
    'array_view_of_volume_rejected', 'plain_2d_view_of_array_rejected', 'typeless_shader_view_rejected',
    'incompatible_typeless_family_rejected', 'typed_resource_reinterpretation_rejected',
    'depth_format_on_stencil_plane_rejected', 'stencil_format_on_depth_plane_rejected',
    'wrong_depth_stencil_family_rejected', 'colour_plane_one_rejected', 'nonfinite_view_lod_rejected',
    'unchecked_nonzero_view_lod_rejected', 'unchecked_null_stencil_plane_rejected',
    'unimplemented_multisample_view_rejected', 'd16_depth_resource_rejects_float_reinterpretation',
    'd32_depth_resource_rejects_integer_reinterpretation', 'every_pixel_case_has_a_distinct_target_pixel',
    'depth_producer_leases_destroyed_before_render_submission', 'render_queue_is_actually_delayed', RETAIN,
)
PIXELS = (
    'typeless_unorm_pixels', 'typeless_srgb_pixels', 'selected_mip_pixels', '2d_view_first_mip_pixels',
    'array_first_slice_pixels', 'array_relative_slice_and_mip_pixels', 'array_view_first_mip_pixels',
    'volume_depth_slice_pixels', 'volume_mip_depth_pixels', 'volume_view_first_mip_pixels',
    'cube_positive_x_pixels', 'cube_negative_x_pixels', 'cube_positive_y_pixels', 'cube_negative_y_pixels',
    'cube_positive_z_pixels', 'cube_negative_z_pixels', 'cube_mip_pixels', 'cube_view_first_mip_pixels',
    'd24_depth_plane_pixels', 'd24_stencil_plane_pixels', 'd16_depth_pixels', 'd32_depth_pixels',
    'd32s8_depth_plane_pixels', 'd32s8_stencil_plane_pixels',
    'null_2d_pixels', 'null_array_pixels', 'null_volume_pixels', 'null_cube_pixels',
)
FULL = PREFIX + PIXELS + ('texture_versions_retire_after_checked_completion',)


def grade(raw, failures, exit_code, timed_out):
    tagged = [line for line in raw.splitlines() if line.startswith(TAG)]
    checks = [re.fullmatch(re.escape(TAG) + r' (PASS|FAIL) (\w+)', line) for line in tagged]
    records = [m.groups() for m in checks if m]
    debug = [line for line in tagged if line.startswith(TAG + ' debug_layer=')]
    summaries = [re.fullmatch(re.escape(TAG) + r' RESULT checks=(\d+) passed=(\d+) failed=(\d+)', line)
                 for line in tagged if line.startswith(TAG + ' RESULT')]
    aborts = [line for line in tagged if line.startswith(TAG + ' ABORT')]
    names = tuple(name for _, name in records)
    failed = Counter(name for verdict, name in records if verdict == 'FAIL')
    ownership = failures == {RETAIN}
    enabled = debug == [TAG + ' debug_layer=1']
    expected = PREFIX if ownership else FULL + (('debug_layer_reports_no_error_or_corruption',) if enabled else ())
    counts = (len(records), sum(verdict == 'PASS' for verdict, _ in records), sum(failed.values()))
    if ownership:
        terminal = exit_code == 1 and aborts == [ABORT] and not summaries
    else:
        terminal = exit_code == bool(failures) and not aborts and len(summaries) == 1 and summaries[0] is not None and \
            tuple(map(int, summaries[0].groups())) == counts
    allowed = {TAG + ' debug_layer=' + str(int(enabled)), ABORT}
    unknown = [line for line, match in zip(tagged, checks) if not match and
               line not in allowed and not line.startswith(TAG + ' RESULT')]
    return {'matched_expected': not timed_out and names == expected and failed == Counter(failures) and
            debug in ([TAG + ' debug_layer=0'], [TAG + ' debug_layer=1']) and not unknown and terminal,
            'checks': counts[0], 'passed': counts[1], 'failed': counts[2], 'failures': dict(failed),
            'debug_layer_enabled': enabled, 'exact_scope': names == expected, 'terminal_protocol': terminal}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=pathlib.Path, required=True)
    args = parser.parse_args()
    bank = args.bank.resolve()
    bank.mkdir(parents=True, exist_ok=False)
    # All launches/builds use the project's shared slot; no caller env is trusted
    # to redirect it into a private lock that would allow concurrent GPU work.
    os.environ['SR_MAIN_ROOT'] = str(ROOT.parent / 'dev')
    snapshot = bank / 'snapshot'; snapshot.mkdir()
    source_paths = ('tools/difftest/native_texture_views_test.cpp', 'tools/difftest/native_texture_views_controls.py',
                    'tools/difftest/test_native_texture_views_controls.py',
                    'tools/difftest/native_draw_controls.py', 'tools/game_slot.py', 'tools/build_slot.py',
                    'gfx/native_draw.cpp', 'gfx/native_draw.hpp', 'gfx/native_upload.cpp', 'gfx/native_upload.hpp',
                    'gfx/native_pipeline.cpp', 'gfx/native_pipeline.hpp', 'gfx/native_image_store.cpp',
                    'gfx/native_image_store.hpp', 'CMakeLists.txt')
    pins = {path: common.digest(ROOT / path) for path in source_paths}
    for path in source_paths:
        target = snapshot / path; target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / path, target)
    fixture = ROOT / 'build/sr_native_texture_views_test.exe'
    shutil.copy2(fixture, snapshot / fixture.name)
    cache = (ROOT / 'build/CMakeCache.txt').read_text(encoding='utf-8')
    ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M).group(1)
    dry = subprocess.run([ninja, '-n', 'sr_native_texture_views_test'], cwd=ROOT / 'build',
                         capture_output=True, text=True, check=True, timeout=30)
    if dry.stdout.strip() != 'ninja: no work to do.':
        raise RuntimeError('Fixture build is stale; build the target through build_slot.py first')
    lines = subprocess.run([ninja, '-t', 'commands', 'sr_native_texture_views_test'], cwd=ROOT / 'build',
                           capture_output=True, text=True, check=True, timeout=30).stdout.splitlines()
    wrapped = next(line.strip() for line in lines if ' -o sr_native_texture_views_test.exe ' in line and ' -c ' not in line)
    prefix, link = wrapped.split(' /C "cd . && ', 1)
    if not prefix.lower().endswith('cmd.exe') or not link.endswith(' && cd ."'):
        raise ValueError('Unexpected fixture linker wrapper')
    link = common.command_args(link[:-len(' && cd ."')])
    objects = {part: common.digest(ROOT / 'build' / part) for part in link if part.endswith('.obj')}
    raw_draw = (ROOT / 'gfx/native_draw.cpp').read_text(encoding='utf-8')
    marker = 'd->recipes.assign(recipes.begin(), recipes.end());'
    cases = [
        ('wrong_array_origin', common.replace_once(raw_draw, marker, marker + '\n'
         '    for (auto& r : d->recipes) if (r.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY)'
         ' r.srv.Texture2DArray.FirstArraySlice = 0;'), set(PIXELS[4:7])),
        ('wrong_srgb_view', common.replace_once(raw_draw, marker, marker + '\n'
         '    for (auto& r : d->recipes) if (r.srv.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)'
         ' r.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;'), {'typeless_srgb_pixels'}),
        ('missing_texture_owner', common.replace_once(raw_draw, 'PreparedNativeDraw retained = d;',
         'PreparedNativeDraw retained = d; for (auto& b : retained.bindings) b.table = {};'), {RETAIN}),
    ]
    common.write_json(bank / 'inputs.json', {'source_pins': pins, 'object_pins': objects,
                                           'fixture_sha256': common.digest(fixture)})
    builds = []
    for name, source, _ in cases:
        target = bank / name; target.mkdir()
        mutated = target / 'native_draw.cpp'; mutated.write_text(source, encoding='utf-8')
        argv = common.command_args(next(line for line in lines if 'gfx/native_draw.cpp.obj' in line and ' -c ' in line))
        output = target / 'mutant.obj'
        for flag, value in [('-MT', str(output)), ('-MF', str(output) + '.d'), ('-o', str(output)), ('-c', str(mutated))]:
            argv[argv.index(flag) + 1] = value
        argv.insert(1, '-I' + str(ROOT / 'gfx'))
        link_argv = list(link); original = next(p for p in link_argv if p.endswith('gfx/native_draw.cpp.obj'))
        link_argv[link_argv.index(original)] = str(output)
        exe = target / (name + '.exe'); link_argv[link_argv.index('-o') + 1] = str(exe)
        link_argv = ['/implib:' + str(target / 'control.lib') if p.startswith('/implib:') else
                     '/pdb:' + str(target / 'control.pdb') if p.startswith('/pdb:') else p for p in link_argv]
        receipt = {'name': name, 'source_sha256': common.digest(mutated), 'compile_arguments': argv,
                   'link_arguments': link_argv}
        for stage, command in [('compile', argv), ('link', link_argv)]:
            log = target / (stage + '.log')
            with log.open('wb') as stream:
                result = subprocess.run([sys.executable, str(ROOT / 'tools/build_slot.py'), '--owner',
                    'texture_control_' + name, '--capacity', '1', '--'] + command,
                    cwd=ROOT / 'build', stdout=stream, stderr=subprocess.STDOUT, timeout=180)
            receipt[stage + '_exit_code'] = result.returncode; receipt[stage + '_log_sha256'] = common.digest(log)
            common.write_json(target / 'build.json', receipt)
            if result.returncode: raise RuntimeError(name + ' ' + stage + ' failed')
        receipt['binary_sha256'] = common.digest(exe); common.write_json(target / 'build.json', receipt); builds.append(receipt)
        print('[TEXTURE_CONTROLS] built ' + name, flush=True)
    if any(common.digest(ROOT / path) != pin for path, pin in pins.items()) or any(
            common.digest(ROOT / 'build' / path) != pin for path, pin in objects.items()):
        raise RuntimeError('Inputs changed during control compilation')
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]; owner = 'native_texture_controls'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--max-hours', '0.05', '--poll', '5'],
                             capture_output=True, text=True, timeout=190)
    common.write_json(bank / 'slot_acquire.json', {'exit_code': acquire.returncode, 'stdout': acquire.stdout, 'stderr': acquire.stderr})
    if acquire.returncode: raise RuntimeError('Slot acquisition failed')
    runs = []; active = None
    try:
        tests = [('native', snapshot / fixture.name, set())] + [
            (name, bank / name / (name + '.exe'), failures) for name, _, failures in cases]
        for name, exe, failures in tests:
            log = bank / (name + '.log'); pin = common.digest(exe); started = time.monotonic()
            with log.open('wb') as stream:
                active = subprocess.Popen([str(exe)], cwd=exe.parent, stdout=stream, stderr=subprocess.STDOUT)
                common.write_json(bank / 'active_child.json', {'name': name, 'pid': active.pid, 'terminal_observed': False})
                timed_out = False; termination = {}
                try: active.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    timed_out = True; termination = common.terminate_child(active)
            result = {'name': name, 'arguments': [str(exe)], 'pid': active.pid, 'exit_code': active.returncode,
                      'terminal_observed': active.poll() is not None, 'timed_out': timed_out,
                      'elapsed_seconds': round(time.monotonic() - started, 3), 'binary_sha256': pin,
                      'binary_pin_unchanged': common.digest(exe) == pin, 'log_sha256': common.digest(log),
                      **termination, **grade(log.read_text(encoding='utf-8'), failures, active.returncode, timed_out)}
            runs.append(result); common.write_json(bank / 'runs.json', runs); common.write_json(bank / 'active_child.json', result)
            print('[TEXTURE_CONTROLS] ' + name + ' matched=' + str(result['matched_expected']), flush=True)
            if not result['terminal_observed']: raise RuntimeError('Fixture termination unresolved')
            active = None
    finally:
        release = common.release_slot(slot, owner, bank, active)
    receipt = {'scope': 'Standalone texture views; production texture ownership, live fidelity and native-zero open',
               'source_pins': pins, 'object_pins': objects, 'builds': builds, 'runs': runs,
               'source_pins_unchanged': all(common.digest(ROOT / p) == pin for p, pin in pins.items()),
               'object_pins_unchanged': all(common.digest(ROOT / 'build' / p) == pin for p, pin in objects.items()),
               'slot_release_exit_code': release.returncode}
    common.write_json(bank / 'receipt.json', receipt)
    return 0 if receipt['source_pins_unchanged'] and receipt['object_pins_unchanged'] and release.returncode == 0 and all(
        r['matched_expected'] and r['binary_pin_unchanged'] for r in runs) else 1


if __name__ == '__main__':
    sys.exit(main())
