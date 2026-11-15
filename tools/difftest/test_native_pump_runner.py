import unittest
from native_pump_test_run import CHECKS, REQUIRED, grade


def report(failures=(), duplicate=False):
    good = [f'check {i}' for i in range(CHECKS-len(failures))]
    if duplicate:
        good[-1] = good[0]
    lines = ['  ok  '+name for name in good]+['FAIL: '+name for name in failures]
    lines.append(f'[NATIVE_PUMP_TEST] {len(good)} passed, {len(failures)} failed')
    return ('\n'.join(lines)+'\n').encode()


class GradeTest(unittest.TestCase):
    def test_complete_positive(self):
        self.assertTrue(grade(0, report()))

    def test_required_failures_for_each_control(self):
        for name, labels in REQUIRED.items():
            with self.subTest(name=name):
                self.assertTrue(grade(1, report(sorted(labels)), name))

    def test_crash_or_timeout_is_not_a_defect_pass(self):
        for code in (124, -1073741819):
            self.assertFalse(grade(code, report(sorted(REQUIRED['record_identity'])), 'record_identity'))

    def test_unrelated_failure_is_rejected(self):
        self.assertFalse(grade(1, report(['unrelated failure']), 'scope_restore'))

    def test_missing_duplicate_or_forged_accounting_is_rejected(self):
        self.assertFalse(grade(0, f'[NATIVE_PUMP_TEST] {CHECKS} passed, 0 failed\n'.encode()))
        self.assertFalse(grade(0, report(duplicate=True)))
        self.assertFalse(grade(0, report().replace(f'{CHECKS} passed'.encode(), f'{CHECKS-1} passed'.encode())))


if __name__ == '__main__':
    unittest.main()
