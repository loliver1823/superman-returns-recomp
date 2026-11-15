"""Reject vacuous, crashed and unrelated lifetime-defect receipts without a game."""
import unittest
from native_segment_ownership_run import CHECKS, REQUIRED, grade


def transcript(failures=()):
    good = CHECKS - len(failures)
    return (b'  ok  fixture\n' * good + b''.join(b'FAIL: '+x.encode()+b'\n' for x in failures)
            + ('[NATIVE_SEGMENT_OWNERSHIP_TEST] %d passed, %d failed\n' % (good,len(failures))).encode())


class OwnershipGradingTests(unittest.TestCase):
    def test_positive_requires_complete_success(self):
        self.assertTrue(grade(0,transcript()))
        self.assertFalse(grade(1,transcript()))
        self.assertFalse(grade(0,transcript().replace(b'  ok  fixture\n',b'',1)))

    def test_each_defect_requires_its_primary_failure(self):
        for name, labels in REQUIRED.items():
            self.assertTrue(grade(1,transcript(sorted(labels)),name),name)
            self.assertFalse(grade(1,transcript(('unrelated failure',)),name),name)
        self.assertFalse(grade(1,transcript(('owned production consumers report no bad or torn records',)),
                               'arena_source'))

    def test_defects_reject_crashes_and_timeouts(self):
        raw=transcript(sorted(REQUIRED['premature_completion']))
        for code in (124,3221225477,0):
            self.assertFalse(grade(code,raw,'premature_completion'))

    def test_writer_control_requires_capture_and_execution_failure(self):
        for label in REQUIRED['writer_capture']:
            self.assertFalse(grade(1,transcript((label,)),'writer_capture'))

    def test_duplicate_summary_rejects(self):
        raw=transcript()
        self.assertFalse(grade(0,raw+raw.splitlines(keepends=True)[-1]))


if __name__=='__main__': unittest.main()
