import tempfile
import unittest
from pathlib import Path

from native_record_executor_run import grade_checks, replace_source, require


class RunnerTests(unittest.TestCase):
    def test_ownership_can_repeat_success_labels(self):
        raw = b'  ok  delayed\n  ok  delayed\n[T] 2 passed, 0 failed\n'
        self.assertTrue(grade_checks(raw, 'T', 2, set()))

    def test_wrong_failure_cannot_replace_required_defect(self):
        raw = b'  ok  control\nFAIL: different\n[T] 1 passed, 1 failed\n'
        self.assertFalse(grade_checks(raw, 'T', 2, {'nested'}))

    def test_duplicate_failures_cannot_forge_summary(self):
        raw = b'FAIL: nested\nFAIL: nested\n[T] 1 passed, 1 failed\n'
        self.assertFalse(grade_checks(raw, 'T', 2, {'nested'}))

    def test_missing_control_cannot_forge_summary(self):
        self.assertFalse(grade_checks(b'[T] 2 passed, 0 failed\n', 'T', 2, set()))

    def test_concurrent_edit_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'source.cpp'
            path.write_bytes(b'owner edit')
            with self.assertRaises(RuntimeError):
                replace_source(path, b'mutant', b'original')
            self.assertEqual(path.read_bytes(), b'owner edit')

    def test_validation_survives_python_optimization(self):
        with self.assertRaises(RuntimeError):
            require(False, 'rejected')


if __name__ == '__main__':
    unittest.main()
