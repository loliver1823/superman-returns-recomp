import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from native_draw_controls import (
    ABORT_LINE, FULL_CHECKS, OWNERSHIP_CHECKS, PIXEL_FAILURES, RETAIN_CHECK,
    grade_log, release_slot,
)


def fixture_log(checks, failures=(), abort=False):
    lines = ['[NATIVE_DRAW_TEST] ' + ('FAIL' if label in failures else 'PASS') + ' ' + label for label in checks]
    failed = sum(label in failures for label in checks)
    lines.append(ABORT_LINE if abort else '[NATIVE_DRAW_TEST] RESULT checks=%d passed=%d failed=%d' % (
        len(checks), len(checks) - failed, failed))
    return '\n'.join(lines) + '\n'


class ControlRunnerTests(unittest.TestCase):
    def test_full_normal_and_pixel_control_reconcile(self):
        self.assertEqual(len(FULL_CHECKS), 55)
        self.assertTrue(grade_log(fixture_log(FULL_CHECKS), 'native', (), 0, False)['matched_expected'])
        self.assertTrue(grade_log(fixture_log(FULL_CHECKS, PIXEL_FAILURES), 'no_draw_commands',
                                  PIXEL_FAILURES, 1, False)['matched_expected'])

    def test_forged_count_and_repeated_summary_fail(self):
        raw = fixture_log(FULL_CHECKS)
        self.assertFalse(grade_log(raw.replace('checks=55 passed=55', 'checks=54 passed=54'),
                                   'native', (), 0, False)['matched_expected'])
        self.assertFalse(grade_log(raw + raw.splitlines()[-1] + '\n', 'native', (), 0, False)['matched_expected'])

    def test_duplicate_failure_cannot_replace_another_check(self):
        raw = fixture_log(FULL_CHECKS, PIXEL_FAILURES)
        labels = sorted(PIXEL_FAILURES)
        raw = raw.replace('FAIL ' + labels[0], 'FAIL ' + labels[1])
        self.assertFalse(grade_log(raw, 'no_draw_commands', PIXEL_FAILURES, 1, False)['matched_expected'])

    def test_truncated_and_reordered_scope_fail(self):
        self.assertFalse(grade_log(fixture_log(FULL_CHECKS[:-1]), 'native', (), 0, False)['matched_expected'])
        swapped = (FULL_CHECKS[1], FULL_CHECKS[0]) + FULL_CHECKS[2:]
        self.assertFalse(grade_log(fixture_log(swapped), 'native', (), 0, False)['matched_expected'])

    def test_ownership_control_has_exactly_two_checks(self):
        failure = (OWNERSHIP_CHECKS[0],)
        self.assertTrue(grade_log(fixture_log(OWNERSHIP_CHECKS, failure), 'missing_submission_owners',
                                  failure, 1, False)['matched_expected'])
        self.assertFalse(grade_log(fixture_log(FULL_CHECKS, failure), 'missing_submission_owners',
                                   failure, 1, False)['matched_expected'])

    def test_guarded_abort_is_exact_prefix_without_summary(self):
        prefix = FULL_CHECKS[:FULL_CHECKS.index(RETAIN_CHECK) + 1]
        raw = fixture_log(prefix, (RETAIN_CHECK,), abort=True)
        self.assertTrue(grade_log(raw, 'missing_cbv_owner', (RETAIN_CHECK,), 1, False)['matched_expected'])
        self.assertFalse(grade_log(raw + ABORT_LINE + '\n', 'missing_cbv_owner',
                                   (RETAIN_CHECK,), 1, False)['matched_expected'])
        self.assertFalse(grade_log(fixture_log(prefix, (RETAIN_CHECK,)) + ABORT_LINE + '\n',
                                   'missing_cbv_owner', (RETAIN_CHECK,), 1, False)['matched_expected'])
        self.assertFalse(grade_log(fixture_log(prefix[:-1], (), abort=True), 'missing_cbv_owner',
                                   (RETAIN_CHECK,), 1, False)['matched_expected'])

    def test_timeout_or_unknown_protocol_never_passes(self):
        raw = fixture_log(FULL_CHECKS)
        self.assertFalse(grade_log(raw, 'native', (), 0, True)['matched_expected'])
        self.assertFalse(grade_log(raw, 'native', (), None, False)['matched_expected'])
        self.assertFalse(grade_log('[NATIVE_DRAW_TEST] FATAL unexpected\n' + raw,
                                   'native', (), 0, False)['matched_expected'])

    def test_unresolved_child_withholds_slot_release(self):
        child = Mock(pid=1234, args=['control.exe'], returncode=None)
        child.poll.return_value = None
        child.kill.side_effect = OSError('cannot terminate')
        child.wait.side_effect = subprocess.TimeoutExpired(child.args, 10)
        with tempfile.TemporaryDirectory() as directory, patch('native_draw_controls.subprocess.run') as run:
            bank = Path(directory)
            with self.assertRaisesRegex(RuntimeError, 'game slot retained'):
                release_slot(['slot.py'], 'fixture', bank, child)
            run.assert_not_called()
            receipt = json.loads((bank / 'slot_unresolved_child.json').read_text())
            self.assertEqual(receipt['pid'], 1234)
            self.assertFalse(receipt['terminal_observed'])
            self.assertFalse(receipt['slot_released'])

    def test_confirmed_termination_allows_release(self):
        child = Mock(pid=1234, args=['control.exe'], returncode=-9)
        child.poll.side_effect = [None, None, -9]
        result = subprocess.CompletedProcess(['slot.py'], 0, 'released', '')
        with tempfile.TemporaryDirectory() as directory, patch('native_draw_controls.subprocess.run', return_value=result) as run:
            self.assertIs(release_slot(['slot.py'], 'fixture', Path(directory), child), result)
            child.kill.assert_called_once()
            run.assert_called_once_with(['slot.py', 'release', '--owner', 'fixture'],
                                        capture_output=True, text=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
