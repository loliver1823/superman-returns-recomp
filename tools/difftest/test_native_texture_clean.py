"""Actual release cache invalidation and a short-span negative control.

This does not execute the texture loader or claim rendered-pixel correctness.
The native fixture uses production texMarkClean/invalidateGuestRange and the
real fast-clean lookup. Its deliberately short registration must fail.
"""
import argparse
import json
import os
from pathlib import Path
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
    state = dict(exe=str(exe), sha256=digest(exe), source_sha256=digest(ROOT / 'gfx/d3d12_backend.cpp'),
                 passed=False, cases={}, scope='actual cache invalidation; caller wiring is source-reviewed only; no loader or pixel claim')
    for key in list(os.environ):
        if key.upper().startswith('SR_'):
            del os.environ[key]
    for mode, expected in [('texture-ranges', 0), ('texture-short-span', 1)]:
        step = out / mode
        step.mkdir()
        spec = dict(cwd=str(step), env={'SR_PM4_RTCACHE': '1', 'SR_RES_SCALE': '1', 'SR_MEM_RESOURCES': '0'},
                    memory_limit_mb=2048, process_limit=8,
                    quiet_window=str(ROOT / 'archive/QUIET_WINDOW'))
        child = run_child([str(exe), mode, str(step / 'unused_cache')], step, spec, out, 30)
        stdout = (step / 'stdout.txt').read_text(encoding='utf-8')
        stderr = (step / 'stderr.txt').read_text(encoding='utf-8')
        passed = child['exit_code'] == expected and not child['reason'] and child['tree_empty']
        if expected == 0:
            passed = passed and stdout.count('[NATIVE_TEXTURE_CLEAN_TEST] PASS tiled-tail mip-tail volume-tail unrelated fast-clean') == 1
        else:
            passed = passed and stderr.count('[NATIVE_DRAW_TEST] FAIL native-tiled-tail-invalidated') == 1
            passed = passed and '[NATIVE_TEXTURE_CLEAN_TEST] PASS' not in stdout
        state['cases'][mode] = dict(child, passed=bool(passed), stdout_sha256=digest(step / 'stdout.txt'),
                                   stderr_sha256=digest(step / 'stderr.txt'))
        atomic(out / 'receipt.json', state)
    state['passed'] = digest(exe) == state['sha256'] and all(case['passed'] for case in state['cases'].values())
    atomic(out / 'receipt.json', state)
    print(json.dumps(state, indent=2))
    return 0 if state['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
