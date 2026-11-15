"""Runner regression checks, including a real bounded parent/child cleanup.

python -m unittest discover -s tools/difftest -p test_native_memory_runner.py
No builds, game launches or production-source mutations.
"""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from native_memory_test_run import grade_defect, stop_windows_tree


def transcript(label, unrelated=None, counts=(35, 1)):
    failures = [label] + ([unrelated] if unrelated else [])
    return (b'  ok  fixture check\n' * (36 - len(failures)) +
            b''.join(b'  FAIL: ' + entry + b'\n' for entry in failures) +
            ('[NATIVE_MEMORY_TEST] %d passed, %d failed\n' % counts).encode())


class GradingTests(unittest.TestCase):
    label = b'valid query reporting a bad writable page rejects'

    def test_expected_failure_accepts(self):
        self.assertTrue(grade_defect('ignored_bad_page', 1, transcript(self.label)))

    def test_actual_windows_newlines_accept(self):
        self.assertTrue(grade_defect('ignored_bad_page', 1, transcript(self.label).replace(b'\n', b'\r\n')))

    def test_unrelated_failure_rejects_even_with_matching_summary(self):
        self.assertFalse(grade_defect('ignored_bad_page', 1,
                                     transcript(self.label, b'fixture allocation failed', (34, 2))))

    def test_wrong_summary_rejects(self):
        self.assertFalse(grade_defect('ignored_bad_page', 1, transcript(self.label, counts=(34, 2))))

    def test_wrong_exit_or_missing_marker_rejects(self):
        self.assertFalse(grade_defect('ignored_bad_page', 0, transcript(self.label)))
        self.assertFalse(grade_defect('ignored_bad_page', 1, transcript(b'fixture allocation failed')))


@unittest.skipUnless(os.name == 'nt', 'Windows process-tree cleanup')
class CleanupTests(unittest.TestCase):
    @staticmethod
    def alive(pid):
        kernel = ctypes.windll.kernel32
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT
        finally:
            kernel.CloseHandle(handle)

    def test_real_parent_and_child_stop_before_return(self):
        with tempfile.TemporaryDirectory(prefix='sr-native-memory-runner-') as folder:
            pid_path = Path(folder) / 'child.pid'
            code = ('import pathlib,subprocess,sys,time; '
                    'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(30)"]); '
                    'pathlib.Path(sys.argv[1]).write_text(str(child.pid)); time.sleep(30)')
            parent = subprocess.Popen([sys.executable, '-c', code, str(pid_path)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            child_pid = None
            try:
                deadline = time.monotonic() + 5
                while not pid_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(pid_path.exists(), 'bounded child fixture did not start')
                child_pid = int(pid_path.read_text())
                self.assertTrue(self.alive(parent.pid) and self.alive(child_pid))
                stop_windows_tree(parent)
                self.assertFalse(self.alive(parent.pid) or self.alive(child_pid))
            finally:
                # Only these fixture processes are eligible for emergency cleanup.
                if parent.poll() is None:
                    stop_windows_tree(parent)
                if child_pid and self.alive(child_pid):
                    subprocess.run(['taskkill', '/PID', str(child_pid), '/T', '/F'],
                                   check=True, capture_output=True, timeout=10)
                parent.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
