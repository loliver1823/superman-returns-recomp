"""Bounded actual native-layout backend admission cases; no game/window.

The negative cases must stop in the actual production drain with exact exit 3,
draw identity and reason. A silent drop, fixed fallback or VSF consumed-return
fails. Positive runs also set the removed selectors to hostile values to prove
they cannot disable the release translation path. This proves admission and
batch construction, not rendered pixels or the full game device setup.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from set_runner import atomic, digest, no_links, run_child


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    exe, out = args.exe.resolve(), args.out.resolve()
    no_links(out); out.mkdir()
    (out/'temp').mkdir()
    cases = [('positive', 'positive', 0, None),
             ('selectors-off', 'positive', 0, None),
             ('missing-vs', 'missing-vs', 3, ('missing-vs', 1)),
             ('vsf-capacity', 'vsf-capacity', 3, ('vsf-staging-failed', 12)),
             # step 6 drop fix: an empty-viewport draw after its block writes, then a draw reusing its VS block
             ('drop-then-reuse', 'drop-then-reuse', 0, None),
             # step 6 I10p: today's cbFull at the shared block's boundary, and the view path's compact shared cursor
             # at its own: the same rejection (constant-staging-full, code 8) - and the block that just fits
             ('cb-shared-full', 'cb-shared-full', 3, ('constant-staging-full', 8)),
             ('zc-shared-full', 'zc-shared-full', 3, ('constant-staging-full', 8)),
             ('zc-shared-fits', 'zc-shared-fits', 0, None),
             ('zc-views', 'zc-views', 0, None)]
    state = {'exe': str(exe), 'sha256': digest(exe), 'passed': False, 'cases': {},
             'scope': 'Actual release drain admission and translated batch; no pixel/game acceptance'}
    for key in list(os.environ):
        if key.upper().startswith('SR_'):
            del os.environ[key]
    for name, mode, expected, refusal in cases:
        step = out/name; step.mkdir()
        env = dict(SR_LOG='dev', SR_PM4_RTCACHE='1', SR_PM4_VSF='1', SR_PM4_VSF_SHM='0',
                   SR_SHADER_RUNTIME_XLAT='0', SR_MEM_RESOURCES='0')
        if name == 'selectors-off':
            env.update(SR_XLAT_SHADERS='0', SR_XLAT_MAX='0', SR_XLAT_ONLY='1')
        if name == 'drop-then-reuse':
            env.update(SR_CP_CONST_DEDUP='1')
        spec = {'cwd': str(step), 'env': env, 'memory_limit_mb': 4096, 'process_limit': 16,
                'quiet_window': str(Path(__file__).resolve().parents[2]/'archive/QUIET_WINDOW')}
        result = run_child([str(exe), mode, str(step/'fixture_cache')], step, spec, out, 60)
        log = step/'native_draw.log'
        text = log.read_text(encoding='utf-8', errors='strict') if log.is_file() else ''
        stdout = (step/'stdout.txt').read_text(encoding='utf-8', errors='strict')
        passed = result['exit_code'] == expected and not result['reason'] and result['tree_empty']
        passed = passed and ('[NATIVE_DRAW_TEST] ENTER mode='+mode+' ordinal=71') in stdout
        if refusal:
            reason, code = refusal
            vs = 'DADB00000000000B' if mode == 'missing-vs' else 'DADB000000000001'
            marker = f'stage=untranslated-draw ordinal=71 vs={vs} ps=DADB000000000002 reason={reason} code={code} '
            passed = passed and text.count(marker) == 1 and '[NATIVE_DRAW_TEST] PASS' not in stdout
        elif mode == 'drop-then-reuse':
            passed = passed and '[NATIVE_DRAW_TEST] PASS drop-then-reuse batches=1 reused-block-intact ordinal=72' in stdout
            passed = passed and 'FAILED stage=' not in text
        elif mode == 'zc-views':
            passed = passed and '[NATIVE_DRAW_TEST] PASS view-path batches=2 m1 m2 shared-slot-unwritten ordinal=71' in stdout
            passed = passed and 'FAILED stage=' not in text
        elif mode == 'zc-shared-fits':
            passed = passed and '[NATIVE_DRAW_TEST] PASS view-path shared-block-fits-at-the-boundary ordinal=71' in stdout
            passed = passed and 'FAILED stage=' not in text
        else:
            passed = passed and '[NATIVE_DRAW_TEST] PASS translated-vsf-batch count=1 vertices=3 ordinal=71 fixed=0' in stdout
            passed = passed and 'pipeline census: xlat=1 fixed=0' in text and 'FAILED stage=' not in text
        passed = passed and not re.search(r'THREAD_FATAL|GPU_HANG|CLEANUP_FAILED', text+stdout)
        state['cases'][name] = dict(result, passed=bool(passed), log_sha256=digest(log) if log.is_file() else None)
        atomic(out/'receipt.json', state)
    state['stable_executable'] = digest(exe) == state['sha256']
    state['passed'] = state['stable_executable'] and all(case['passed'] for case in state['cases'].values())
    atomic(out/'receipt.json', state)
    print(json.dumps(state, indent=2))
    return 0 if state['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
