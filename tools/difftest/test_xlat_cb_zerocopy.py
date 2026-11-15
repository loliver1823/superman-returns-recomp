"""step 6 I10p (Astra I10p r2): the view path's production binder and lifetime on WARP - no game or window.

`difftest --xlat-cb-zerocopy-test` (tools/difftest/xlat_cb_zerocopy_test.cpp, the development build) must pass every
case with checked_xframe > 0, and each control must FAIL exactly its named check (the fixture can see the defect the
control injects): rewrite-slot (the binder's write-once guard cleared before the re-render: the outstanding first
consumer reads the changed bytes) and skip-keep (the reused fallback slice not kept by the recording that binds it);
step 6 R1a adds skip-page-keep / release-at-slice-death (the recording's own upload-keep mutants: a page stamped
retained but not pushed / no upload retained), each failing the same named check, in either SR_RENDER_KEEP_LEAN arm.
    python tools/difftest/test_xlat_cb_zerocopy.py --exe <build>/difftest.exe --out <fresh dir> [--keep-lean 0|1]
--keep-lean passes SR_RENDER_KEEP_LEAN to the child (step 6 R1a; omitted = the default, on); every other SR_* variable
is dropped, as before.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from set_runner import atomic, digest, no_links, run_child

# Every check of the healthy run (a missing one fails the case: a renamed or skipped check cannot pass silently).
CHECKS = [
    'first-rotation-no-assert', 'first-render-writes-each-shared-slot-once', 'first-render-consume-checks',
    'first-consumer-outstanding', 're-render-stages-fallbacks-never-rewrites-a-slot', 're-render-counts-checked-xframe',
    're-render-leaves-the-first-consumers-slots-intact', 'both-consumers-outstanding',
    'unchanged-reuse-across-recording-reset-binds-the-staged-slice', 'later-change-stages-a-new-fallback',
    'gate-1-completes-the-first-two-recordings-only', 'fallback-slice-held-by-the-recording-that-reuses-it',
    'fallback-slice-released-after-its-last-consumer', 'phase-a-no-BAD', 'finished-set-records-its-own-poison-range',
    'retire-after-completion-rechecks-every-range-no-BAD', 'retired-range-poisoned',
    'p3-empty-tail-generation-records-no-poison-range', 'phase-b-no-BAD', 'stale-view-negative-detected-by-consume',
    'stale-bind-ranges-retire-unchanged', 'stale-bytes-negative-detected-on-retire',
    'outstanding-consumer-negative-detected-by-the-retire-assert',
    'open-recording-negative-detected-by-the-retire-assert', 'rotation-without-a-fence-wait-detected',
    'rotation-waiting-for-another-third-detected', 'first-consumer-reads-the-cp-views',
    'first-consumer-reads-the-original-shared-bytes', 're-render-reads-its-changed-shared-bytes',
    'unchanged-reuse-reads-the-staged-bytes', 'later-fallback-reads-its-own-bytes', 'stale-view-reads-the-poison',
    'stale-bytes-consumer-read-the-bytes-it-bound', 'outstanding-consumer-reads-the-poison-the-assert-names',
    'p2-binder-slot-writes-racing-the-retire-poison-leave-twin-equal-to-shadow',
]
CASES = [('healthy', None, 0, None),
         ('rewrite-slot', 'rewrite-slot', 1, 'first-consumer-reads-the-original-shared-bytes'),
         ('skip-keep', 'skip-keep', 1, 'fallback-slice-held-by-the-recording-that-reuses-it'),
         # step 6 R1a: the recording's own upload-keep mutants (both SR_RENDER_KEEP_LEAN arms)
         ('skip-page-keep', 'skip-page-keep', 1, 'fallback-slice-held-by-the-recording-that-reuses-it'),
         ('release-at-slice-death', 'release-at-slice-death', 1, 'fallback-slice-held-by-the-recording-that-reuses-it')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--keep-lean', choices=('0', '1'), default=None)
    args = parser.parse_args()
    exe, out = args.exe.resolve(), args.out.resolve()
    no_links(out); out.mkdir()
    state = {'exe': str(exe), 'sha256': digest(exe), 'passed': False, 'cases': {}, 'keep_lean': args.keep_lean,
             'scope': 'The view path binder, CP rotation and check mode on WARP with real submissions; no game'}
    for key in list(os.environ):
        if key.upper().startswith('SR_'):
            del os.environ[key]
    for name, control, expected, must_fail in CASES:
        step = out/name; step.mkdir()
        env = {'SR_LOG': 'dev'}
        if args.keep_lean is not None:
            env['SR_RENDER_KEEP_LEAN'] = args.keep_lean
        spec = {'cwd': str(step), 'env': env, 'memory_limit_mb': 4096, 'process_limit': 16,
                'quiet_window': str(Path(__file__).resolve().parents[2]/'archive/QUIET_WINDOW')}
        argv = [str(exe), '--xlat-cb-zerocopy-test'] + (['--control=' + control] if control else [])
        result = run_child(argv, step, spec, out, 300)
        stdout = (step/'stdout.txt').read_text(encoding='utf-8', errors='strict')
        log = step/'xlat_cb_zerocopy.log'
        text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
        passes = set(re.findall(r'^\[XLAT_CB_ZC_TEST\] PASS (\S+)', stdout, re.M))
        fails = set(re.findall(r'^\[XLAT_CB_ZC_TEST\] FAIL (\S+)', stdout, re.M))
        complete = re.search(r'^\[XLAT_CB_ZC_TEST\] COMPLETE passed=(\d+) failed=(\d+) control=(\S+)', stdout, re.M)
        passed = result['exit_code'] == expected and not result['reason'] and result['tree_empty'] and bool(complete)
        passed = passed and complete.group(3) == (control or 'none')
        lean = re.search(r'^\[XLAT_CB_ZC_TEST\] COMPLETE .* keep_lean=(on|off)', stdout, re.M)
        passed = passed and bool(lean) and lean.group(1) == ('off' if args.keep_lean == '0' else 'on')
        xframe = None
        if control is None:
            m = re.search(r'^\[XLAT_CB_ZC_TEST\] COUNTS checked=(\d+) checked_xframe=(\d+)', stdout, re.M)
            xframe = int(m.group(2)) if m else None
            passed = passed and not fails and passes == set(CHECKS) and int(complete.group(2)) == 0
            passed = passed and int(complete.group(1)) == len(CHECKS) and bool(xframe)
        else:
            passed = passed and must_fail in fails and int(complete.group(2)) >= 1
        passed = passed and not re.search(r'THREAD_FATAL|GPU_HANG|CLEANUP_FAILED|SETUP_OR_EXECUTION_FAILED',
                                          text + stdout)
        state['cases'][name] = dict(result, passed=bool(passed), passes=len(passes), fails=sorted(fails),
                                    missing=sorted(set(CHECKS) - passes) if control is None else None,
                                    checked_xframe=xframe, log_sha256=digest(log) if log.is_file() else None)
        atomic(out/'receipt.json', state)
    state['stable_executable'] = digest(exe) == state['sha256']
    state['passed'] = state['stable_executable'] and all(case['passed'] for case in state['cases'].values())
    atomic(out/'receipt.json', state)
    print(json.dumps(state, indent=2))
    return 0 if state['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
