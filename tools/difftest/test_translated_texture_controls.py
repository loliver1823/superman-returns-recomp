import unittest
from translated_texture_controls import CHECKS, TAG, grade, grade_control


def complete():
    return '\n'.join([TAG + ' PASS ' + name for name in CHECKS] +
                     [TAG + f' RESULT checks={len(CHECKS)} passed={len(CHECKS)} failed=0'])


class ProtocolTests(unittest.TestCase):
    def test_complete_terminal_protocol(self):
        self.assertTrue(grade(complete(), '', 0, False)['matched_expected'])

    def test_missing_gpu_lifetime_check(self):
        raw = complete().replace(TAG + ' PASS ' + CHECKS[15] + '\n', '')
        self.assertFalse(grade(raw, '', 0, False)['matched_expected'])

    def test_duplicate_check(self):
        self.assertFalse(grade(complete() + '\n' + TAG + ' PASS ' + CHECKS[0], '', 0, False)['matched_expected'])

    def test_terminal_and_timeout(self):
        for code, timeout, terminal in [(1, False, True), (0, True, True), (0, False, False)]:
            self.assertFalse(grade(complete(), '', code, timeout, terminal)['matched_expected'])

    def test_error_and_foreign_protocol(self):
        for line in ['SETUP FAILED device', 'D3D12 ERROR wrong descriptor',
                     'PIXEL_FAIL index=0', TAG + ' ABORT dangling queued version',
                     TAG + ' EXTRA unexpected']:
            self.assertFalse(grade(complete(), line, 0, False)['matched_expected'])

    def test_false_summary_and_failed_check(self):
        self.assertFalse(grade(complete().replace('failed=0', 'failed=1'), '', 0, False)['matched_expected'])
        self.assertFalse(grade(complete().replace('PASS ' + CHECKS[-1], 'FAIL ' + CHECKS[-1]), '', 0, False)['matched_expected'])

    def test_summary_must_follow_checks(self):
        lines = complete().splitlines()
        self.assertFalse(grade('\n'.join(lines[-1:] + lines[:-1]), '', 0, False)['matched_expected'])

    def test_ownership_control_requires_exact_safe_abort(self):
        lines = [TAG + (' FAIL ' if name == CHECKS[15] else ' PASS ') + name for name in CHECKS[:16]]
        raw = '\n'.join(lines + [TAG + ' ABORT dangling queued version'])
        self.assertTrue(grade_control(raw, '', 1, False, True, {CHECKS[15]}, True)['matched_expected'])
        self.assertFalse(grade_control(raw, '', 1, True, True, {CHECKS[15]}, True)['matched_expected'])

    def test_control_rejects_healthy_or_incomplete_transcript(self):
        self.assertFalse(grade_control(complete(), '', 1, False, True, {CHECKS[16]})['matched_expected'])
        self.assertFalse(grade_control('', '', 1, False, True, {CHECKS[16]})['matched_expected'])

    def test_pixel_control_requires_exact_failure_set(self):
        raw = complete().replace('PASS ' + CHECKS[16], 'FAIL ' + CHECKS[16]).replace(
            f'passed={len(CHECKS)} failed=0', f'passed={len(CHECKS)-1} failed=1')
        self.assertTrue(grade_control(raw, '', 1, False, True, {CHECKS[16]})['matched_expected'])
        self.assertFalse(grade_control(raw, '', 1, False, True, {CHECKS[2]})['matched_expected'])


if __name__ == '__main__':
    unittest.main()
