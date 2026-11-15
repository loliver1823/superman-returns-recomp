#!/usr/bin/env python3
"""Bounded native record framing/adapter/retirement tests, no game or GPU worker.

python tools/difftest/native_record_executor_run.py --out archive/<fresh-bank>
Real-engine comparison uses the preserved development oracle on valid fixtures.
Shader bindings/pixels, real native GPU services and M2 remain unproved.
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
from native_memory_test_run import stop_windows_tree


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def replace_source(path, expected, replacement):
    require(path.read_bytes() == expected, 'concurrent source change; leaving untouched: ' + str(path))
    if expected != replacement:
        path.write_bytes(replacement)
        require(path.read_bytes() == replacement, 'source write verification failed: ' + str(path))


def grade_checks(raw, label, expected, failures):
    raw = raw.replace(b'\r\n', b'\n')
    summaries = re.findall(rb'\[' + label.encode() + rb'\] (\d+) passed, (\d+) failed', raw)
    passed = re.findall(rb'^  ok  (.+)$', raw, re.MULTILINE)
    failed = re.findall(rb'^FAIL: (.+)$', raw, re.MULTILINE)
    return len(summaries) == 1 and tuple(map(int, summaries[0])) == (len(passed), len(failed)) and \
        len(passed) + len(failed) == expected and len(failed) == len(failures) and \
        {s.decode() for s in failed} == failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--fault-controls', action='store_true', help='compile bounded executor and optional draw-builder defects, then restore and rebuild')
    parser.add_argument('--draw-builder', action='store_true', help='include shared draw-state assembly checks and the application target')
    args = parser.parse_args()
    if args.build_only and args.fault_controls:
        parser.error('--build-only and --fault-controls are mutually exclusive')
    root = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    env.update(SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH_CPPM4='0',
               SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_EXEC='0', SR_NATIVE_TRANSPORT='0',
               SR_PM4_CP='0', SR_PM4_RING_EXEC='0', SR_ALLOW_FORCING='0')
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env.get('PATH', '')
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x8003)
    paths = ('CMakeLists.txt', 'kernel/native_record_format.hpp', 'gfx/native_record_executor.hpp',
             'gfx/native_record_executor.cpp', 'gfx/pm4_native_record_adapter.cpp', 'gfx/pm4_engine.hpp',
             'gfx/pm4_engine.cpp', 'gfx/xenos_state.hpp', 'gfx/null_renderer.cpp',
             'kernel/native_flush.hpp', 'kernel/native_flush.cpp', 'kernel/gpu_command_processor.cpp', 'kernel/gpu_submit.cpp',
             'kernel/native_const_dedup.hpp', 'kernel/native_const_dedup.cpp',
             'kernel/native_exec.hpp', 'kernel/native_exec.cpp',
             'tools/difftest/native_record_executor_test.cpp', 'tools/difftest/native_record_adapter_test.cpp',
             'tools/difftest/native_record_failure_test.inc', 'tools/difftest/native_segment_ownership_test.inc',
             'tools/difftest/native_record_executor_run.py', 'tools/difftest/difftest_main.cpp',
             'tools/difftest/native_memory_test_run.py', 'tools/difftest/test_native_record_executor_runner.py',
             'gfx/native_draw_builder.hpp', 'gfx/native_draw_builder.cpp', 'gfx/render_draw_command.hpp')
    if args.draw_builder:
        paths += ('tools/difftest/native_draw_builder_test.cpp', 'tools/difftest/native_draw_builder_oracle.inc')
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    evidence = dict(scope='Record framing/ordering, selected actual-engine outcomes, and owned/segment failure acknowledgement. Existing helper/mapping contracts; no shader-binding, pixels, native GPU or M2 acceptance.',
                    source_sha256={p: sha(root / p) for p in paths}, steps=[], valid=False,
                    python_optimization=sys.flags.optimize, draw_builder=args.draw_builder)

    def save():
        (out / 'executor_matrix.json').write_text(json.dumps(evidence, indent=2) + '\n')

    def run(tag, command, timeout, expected_exit=0):
        command = [str(value) for value in command]
        row = dict(tag=tag, command=command, terminal_observed=False, timeout_seconds=timeout, expected_exit=expected_exit)
        evidence['steps'].append(row)
        with (out / (tag + '_stdout.txt')).open('xb') as stdout, (out / (tag + '_stderr.txt')).open('xb') as stderr:
            process = subprocess.Popen(command, cwd=root, env=env, stdout=stdout, stderr=stderr)
            row['pid'] = process.pid
            save()
            try:
                try:
                    code = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    stop_windows_tree(process)
                    code = 124
                row.update(exit=code, terminal_observed=True)
            except BaseException as error:
                row['error'] = str(error)
                save()
                raise
        raw = (out / (tag + '_stdout.txt')).read_bytes()
        errors = (out / (tag + '_stderr.txt')).read_bytes()
        row.update(stdout_sha256=hashlib.sha256(raw).hexdigest(), stderr_sha256=hashlib.sha256(errors).hexdigest())
        save()
        print(json.dumps({k: row[k] for k in ('tag', 'pid', 'exit', 'terminal_observed')}), flush=True)
        if code != expected_exit:
            lines = (raw + errors).decode(errors='replace').splitlines()
            print('\n'.join(line for line in lines if 'error:' in line or 'FAIL:' in line or 'failed' in line)[-5000:], flush=True)
            raise RuntimeError(tag + ' failed; retained full output in ' + str(out))
        return row, raw

    guard = "$ErrorActionPreference='Stop'; if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Less than 8 GiB free RAM' }; if (Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(SupermanReturns|xenia.*|difftest)\\.exe$' }) { throw 'Game or test active' }"
    run('resource_guard', ['powershell', '-NoProfile', '-Command', guard], 30)
    run('grader', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/difftest',
                   '-p', 'test_native_record_executor_runner.py'], 30)
    build_command = [sys.executable, 'tools/build_slot.py', '--owner', 'native-record-executor', '--capacity', '1', '--',
                     'cmake', '--build', 'build', '--target', 'difftest', 'sr_native_record_executor_test', '-j4']
    if args.draw_builder:
        build_command[-1:-1] = ['sr_native_draw_builder_test', 'SupermanReturns']
    run('build', build_command, 300)
    if args.build_only:
        evidence['sources_unchanged'] = all(sha(root / p) == value for p, value in evidence['source_sha256'].items())
        evidence['quiescent'] = all(row['terminal_observed'] for row in evidence['steps'])
        evidence['build_only'] = True
        save()
        return 0 if evidence['sources_unchanged'] and evidence['quiescent'] else 1
    tests = [('standalone', root / 'build/sr_native_record_executor_test.exe', [], 'NATIVE_RECORD_EXECUTOR_TEST', 77, '0'),
             ('adapter', root / 'build/difftest.exe', ['--native-record-adapter-test'], 'NATIVE_RECORD_ADAPTER_TEST', 18, '0'),
             ('failure', root / 'build/difftest.exe', ['--native-record-failure-test'], 'NATIVE_RECORD_FAILURE_TEST', 9, '0'),
             ('failure_cppm4', root / 'build/difftest.exe', ['--native-record-failure-test'], 'NATIVE_RECORD_FAILURE_TEST', 9, '1'),
             ('ownership', root / 'build/difftest.exe', ['--native-segment-ownership-test'], 'NATIVE_SEGMENT_OWNERSHIP_TEST', 43, '0')]
    if args.draw_builder:
        tests.append(('draw_builder', root / 'build/sr_native_draw_builder_test.exe', [], 'NATIVE_DRAW_BUILDER_TEST', 138, '0'))
    def check_tests(prefix='', expected_failures=None):
        expected_failures = expected_failures or {}
        for name, exe, options, label, expected, cppm4 in tests:
            tag = prefix + name
            failures = expected_failures.get(name, set())
            env['SR_NATIVE_FLUSH_CPPM4'] = cppm4
            row, raw = run(tag, [exe, *options], 30, expected_exit=1 if failures else 0)
            raw = raw.replace(b'\r\n', b'\n')
            summaries = re.findall(rb'\[' + label.encode() + rb'\] (\d+) passed, (\d+) failed', raw)
            row['summary'] = [[int(a), int(b)] for a, b in summaries]
            checks = re.findall(rb'^(?:  ok  |FAIL: ).+$', raw, re.MULTILINE)
            actual_failures = re.findall(rb'^FAIL: (.+)$', raw, re.MULTILINE)
            row.update(expected_checks=expected, observed_checks=len(checks),
                       expected_failures=sorted(failures), failures=[s.decode() for s in actual_failures])
            save()
            require(grade_checks(raw, label, expected, failures), tag + ' incomplete or unexpected accounting')
            _, digest = run(tag + '_exe_hash', [sys.executable, '-c', 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())', exe], 30)
            row['exe_sha256'] = digest.decode().strip()
            require(re.fullmatch('[0-9a-f]{64}', row['exe_sha256']), tag + ' invalid executable hash')
            save()
        if args.draw_builder:
            _, digest = run(prefix + 'application_exe_hash', [sys.executable, '-c',
                            'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())',
                            root / 'build/SupermanReturns.exe'], 30)
            value = digest.decode().strip()
            require(re.fullmatch('[0-9a-f]{64}', value), 'invalid application executable hash')
            evidence.setdefault('application_executables', {})[prefix or 'normal'] = value
            save()

    check_tests()
    if args.fault_controls:
        latch_path = root / 'gfx/pm4_native_record_adapter.cpp'
        completion_path = root / 'kernel/native_flush.cpp'
        originals = {path: path.read_bytes() for path in (latch_path, completion_path)}
        if args.draw_builder:
            builder_path = root / 'gfx/native_draw_builder.cpp'
            originals[builder_path] = builder_path.read_bytes()
        expected_bytes = dict(originals)
        for path, original in originals.items():
            (out / (path.name + '.original')).write_bytes(original)
        latch = originals[latch_path]
        replacements = [(b'    if (engine_.nativeRecordFailed) return false;', b''),
                        (b'return !engine_.nativeRecordFailed;', b'return true;'),
                        (b'return complete && !engine_.nativeRecordFailed;', b'return complete;'),
                        (b'  if (nativeRecordFailed) return false;', b''),
                        (b' && !nativeRecordFailed;', b';')]
        for before, after in replacements:
            require(before in latch, 'missing nested-latch fault site')
            latch = latch.replace(before, after)
        completion = originals[completion_path]
        marker = b'  if (!complete || st.bad || eng.nativeRecordFailed) {'
        require(completion.count(marker) == 1, 'ambiguous completion fault site')
        completion = completion.replace(marker, b'  if (owned && (!complete || st.bad || eng.nativeRecordFailed)) owned->completed.store(true, std::memory_order_release);\n' + marker)
        nested_suffix = 'nested consumer failure stops outer owned suffix despite helper success'
        completion_labels = {'rejected owned record is not completed or retired',
                             'nested consumer failure prevents outer owned completion and retirement'}
        faults = [('ignored_latch', latch_path, latch,
                   {'adapter': {'failure latch blocks both later native records and packet replay',
                                'nested failure overrides helper success and prevents outer suffix execution'},
                    'failure': {nested_suffix}, 'failure_cppm4': {nested_suffix}}),
                  ('completion_on_failure', completion_path, completion,
                   {'failure': completion_labels, 'failure_cppm4': completion_labels})]
        if args.draw_builder:
            builder = originals[builder_path]
            marker = b'  output.alphaRef = state[0x210Eu] ? state[0x210Eu] : state[0x10Eu];'
            require(builder.count(marker) == 1, 'ambiguous draw alias fault site')
            builder = builder.replace(marker, b'  output.alphaRef = state[0x10Eu];')
            labels = {'original assembly layout/source/index-size/endian ' +
                      f'{layout}/{source}/{size32}/{endian}'
                      for layout in range(4) for source in range(4) for size32 in range(2) for endian in range(4)}
            faults.append(('draw_alias_precedence', builder_path, builder, {'draw_builder': labels}))
            builder = originals[builder_path]
            marker = b'  return true;\n}'
            require(builder.count(marker) == 1, 'ambiguous early shader capture fault site')
            early_capture = (b'  std::memcpy(output.boolConsts, state.data() + 0x4900u, sizeof(output.boolConsts));\n'
                             b'  std::memcpy(output.fetchRaw, state.data() + 0x4800u, sizeof(output.fetchRaw));\n')
            builder = builder.replace(marker, early_capture + marker)
            faults.append(('draw_early_shader_capture', builder_path, builder,
                           {'draw_builder': {'fixed state preserves caller-owned shader arrays until late capture'}}))
        evidence['faults'] = []
        try:
            for name, path, mutant, failures in faults:
                replace_source(path, expected_bytes[path], mutant)
                expected_bytes[path] = mutant
                evidence['faults'].append(dict(name=name, path=str(path.relative_to(root)),
                                               original_sha256=sha(out / (path.name + '.original')),
                                               mutant_sha256=sha(path)))
                save()
                run(name + '_build', build_command, 300)
                check_tests(name + '_', failures)
                replace_source(path, expected_bytes[path], originals[path])
                expected_bytes[path] = originals[path]
        finally:
            # Failed process-tree cleanup leaves the original backups and mutant
            # in place; never restore sources under a possibly live compiler.
            if all(row['terminal_observed'] for row in evidence['steps']):
                for path, expected in expected_bytes.items():
                    require(path.read_bytes() == expected, 'concurrent source change; cannot restore: ' + str(path))
                for path, original in originals.items():
                    replace_source(path, expected_bytes[path], original)
                evidence['all_sources_restored'] = all(path.read_bytes() == data for path, data in originals.items())
                save()
                run('restored_build', build_command, 300)
                check_tests('restored_')
    evidence['sources_unchanged'] = all(sha(root / p) == value for p, value in evidence['source_sha256'].items())
    evidence['quiescent'] = all(row['terminal_observed'] for row in evidence['steps'])
    evidence['valid'] = evidence['sources_unchanged'] and all(row['terminal_observed'] and row['exit'] == row['expected_exit'] for row in evidence['steps'])
    save()
    return 0 if evidence['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
