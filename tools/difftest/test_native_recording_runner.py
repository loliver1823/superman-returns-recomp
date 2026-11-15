import unittest
from pathlib import Path
import tempfile
from native_recording_run import ALLOWED, EQUALITY, REQUIRED, grade, restore_source


def output(count, failed=()):
    failed = list(failed)
    lines = ['  ok  check ' + str(i) for i in range(count - len(failed))]
    lines += ['FAIL: ' + name for name in failed]
    lines += [f'[NATIVE_RECORDING_TEST] {count - len(failed)} passed, {len(failed)} failed']
    return ('\n'.join(lines) + '\n').encode()


class GraderTest(unittest.TestCase):
    def test_positive_and_gate_off(self):
        self.assertTrue(grade(0, output(106)))
        self.assertTrue(grade(0, output(123), gate_off=True))
        self.assertFalse(grade(0, output(106), gate_off=True))

    def test_all_named_controls(self):
        for name, required in REQUIRED.items():
            self.assertTrue(grade(1, output(106, required), defect=name), name)
            self.assertFalse(grade(0, output(106, required), defect=name), name)
            self.assertFalse(grade(1, output(106, ['unrelated']), defect=name), name)
            unrelated = EQUALITY - ALLOWED[name]
            if unrelated:
                self.assertFalse(grade(1, output(106, required | {min(unrelated)}), defect=name), name)

    def test_crashes_timeouts_and_forged_counts(self):
        for code in (124, 3221226505):
            self.assertFalse(grade(code, output(106, REQUIRED['frame']), defect='frame'))
        self.assertFalse(grade(0, output(105)))
        self.assertFalse(grade(0, output(106).replace(b'check 1\n', b'check 0\n')))
        self.assertFalse(grade(0, output(106) + b'[NATIVE_RECORDING_TEST] 106 passed, 0 failed\n'))

    def test_callback_flag_failure_set(self):
        required = {'cpu-mask: dispatch equals actual SDK', 'cpu-mask: direct equals actual SDK'}
        self.assertTrue(grade(1, output(106, required), defect='callback_flags'))
        for route in required:
            self.assertFalse(grade(1, output(106, [route]), defect='callback_flags'))
        self.assertFalse(grade(1, output(106, required | {'plain: dispatch equals actual SDK'}),
                               defect='callback_flags'))

    def test_guarded_restoration(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source.cpp'
            source.write_bytes(b'mutant')
            with self.assertRaisesRegex(RuntimeError, 'cleanup unproved'):
                restore_source(source, b'original', b'mutant', False)
            self.assertEqual(source.read_bytes(), b'mutant')
            source.write_bytes(b'concurrent edit')
            with self.assertRaisesRegex(RuntimeError, 'Concurrent source edit'):
                restore_source(source, b'original', b'mutant', True)
            self.assertEqual(source.read_bytes(), b'concurrent edit')
            source.write_bytes(b'mutant')
            restore_source(source, b'original', b'mutant', True)
            self.assertEqual(source.read_bytes(), b'original')


if __name__ == '__main__':
    unittest.main()
