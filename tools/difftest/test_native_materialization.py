"""Bounded release-layout held-target failure tests; no device or rendered-pixel claim.

Each failed prerequisite must terminate in production nbMaterializeHeld before
clearing its ownership. A compiled omission control is run by the VM proof
recipe against this same runner, then the original source is restored.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from set_runner import atomic, digest, no_links, run_child


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    exe, out = args.exe.resolve(), args.out.resolve()
    no_links(out)
    out.mkdir()
    (out / 'temp').mkdir()
    state = dict(exe=str(exe), sha256=digest(exe), passed=False, cases={},
                 scope='actual inactive-holder and failure branches; no successful GPU materialization claim')
    for key in list(os.environ):
        if key.upper().startswith('SR_'):
            del os.environ[key]
    cases = [('inactive', None), ('missing-target', 'materialize-held'),
             ('missing-aux', 'materialize-missing-aux'),
             ('missing-stencil', 'materialize-missing-stencil')]
    for suffix, stage in cases:
        mode = 'materialize-' + suffix
        step = out / mode
        step.mkdir()
        spec = dict(cwd=str(step), env={'SR_LOG': 'dev'}, memory_limit_mb=2048,
                    process_limit=8, quiet_window=str(ROOT / 'archive/QUIET_WINDOW'))
        child = run_child([str(exe), mode, str(step / 'unused_cache')], step, spec, out, 30)
        stdout = (step / 'stdout.txt').read_text(encoding='utf-8')
        stderr = (step / 'stderr.txt').read_text(encoding='utf-8')
        log = step / 'native_draw.log'
        text = log.read_text(encoding='utf-8') if log.is_file() else ''
        passed = child['exit_code'] == (3 if stage else 0) and not child['reason'] and child['tree_empty']
        passed = passed and stdout.count(f'[NATIVE_MATERIALIZE_TEST] ENTER mode={mode} held=1 page=3') == 1
        failures = re.findall(r'\[NATIVE_RECORDING\] FAILED stage=([^ ]+) ', text)
        if stage:
            passed = passed and failures == [stage] and '[NATIVE_MATERIALIZE_TEST] PASS' not in stdout
            if suffix == 'missing-target':
                passed = passed and text.count('materialize FAILED held=1 kind=0 key=0000712300000001 slice=0; ownership retained') == 1
        else:
            passed = passed and not failures and stdout.count('[NATIVE_MATERIALIZE_TEST] PASS inactive-holder-noop') == 1
        passed = passed and not re.search(r'THREAD_FATAL|GPU_HANG|CLEANUP_FAILED', text + stdout + stderr)
        state['cases'][mode] = dict(child, passed=bool(passed), log_sha256=digest(log) if log.is_file() else None,
                                   stdout_sha256=digest(step / 'stdout.txt'), stderr_sha256=digest(step / 'stderr.txt'))
        atomic(out / 'receipt.json', state)
    state['passed'] = digest(exe) == state['sha256'] and all(case['passed'] for case in state['cases'].values())
    atomic(out / 'receipt.json', state)
    print(json.dumps(state, indent=2))
    return 0 if state['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
