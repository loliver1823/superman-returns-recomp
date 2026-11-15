import unittest
from native_pump_driver_run import CHECKS,REQUIRED,grade


def report(failures=(),duplicate=False):
    good=[f'check {i}' for i in range(CHECKS-len(failures))]
    if duplicate:good[-1]=good[0]
    lines=['  ok  '+name for name in good]+['FAIL: '+name for name in failures]
    lines.append(f'[NATIVE_PUMP_DRIVER_TEST] {len(good)} passed, {len(failures)} failed')
    return ('\n'.join(lines)+'\n').encode()


class GradeTest(unittest.TestCase):
    def test_complete_positive(self):self.assertTrue(grade(0,report()))

    def test_all_required_named_controls(self):
        for name,labels in REQUIRED.items():
            with self.subTest(name=name):self.assertTrue(grade(1,report(sorted(labels)),name))

    def test_crash_timeout_or_unrelated_failure_is_rejected(self):
        for code in (124,-1073741819):self.assertFalse(grade(code,report(sorted(REQUIRED['resume_source'])),'resume_source'))
        self.assertFalse(grade(1,report(['unrelated failure']),'resume_source'))

    def test_missing_duplicate_or_forged_accounting_is_rejected(self):
        self.assertFalse(grade(0,f'[NATIVE_PUMP_DRIVER_TEST] {CHECKS} passed, 0 failed\n'.encode()))
        self.assertFalse(grade(0,report(duplicate=True)))
        self.assertFalse(grade(0,report().replace(f'{CHECKS} passed'.encode(),f'{CHECKS-1} passed'.encode())))


if __name__=='__main__':unittest.main()
