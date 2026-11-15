import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from translated_constant_controls import (
    ABORT_LINE, ALIAS_FAILURES, FULL_CHECKS, RETAIN_CHECK, TAG,
    grade_fixture, grade_replay, run,
    SAMPLER_ABORT, SAMPLER_CHECKS, SAMPLER_RETAIN, SAMPLER_TAG, grade_sampler_fixture,
)


def fixture(checks=FULL_CHECKS, failures=(), abort=False):
    lines = [TAG + ' ' + ('FAIL' if name in failures else 'PASS') + ' ' + name for name in checks]
    failed = sum(name in failures for name in checks)
    if not abort:
        lines.append(TAG + ' RESULT checks=%d passed=%d failed=%d' % (len(checks), len(checks) - failed, failed))
    return '\n'.join(lines) + '\n', ABORT_LINE + '\n' if abort else ''


class ConstantControlTests(unittest.TestCase):
    def test_full_normal_and_alias_control(self):
        self.assertEqual(len(FULL_CHECKS), 33)
        self.assertTrue(grade_fixture(*fixture(), 'native', 0, False)['matched_expected'])
        self.assertTrue(grade_fixture(*fixture(failures=ALIAS_FAILURES), 'wrong_alias', 1, False)['matched_expected'])

    def test_missing_owner_requires_exact_guarded_prefix(self):
        prefix = FULL_CHECKS[:FULL_CHECKS.index(RETAIN_CHECK) + 1]
        stdout, stderr = fixture(prefix, (RETAIN_CHECK,), abort=True)
        self.assertTrue(grade_fixture(stdout, stderr, 'missing_owner', 1, False)['matched_expected'])
        for altered in (stderr + stderr, '', TAG + ' ' + stderr):
            self.assertFalse(grade_fixture(stdout, altered, 'missing_owner', 1, False)['matched_expected'])
        self.assertFalse(grade_fixture(*fixture(prefix, (RETAIN_CHECK,)), 'missing_owner', 1, False)['matched_expected'])

    def test_truncated_or_reordered_passes_fail(self):
        for checks in (FULL_CHECKS[:-1], (FULL_CHECKS[1], FULL_CHECKS[0]) + FULL_CHECKS[2:]):
            self.assertFalse(grade_fixture(*fixture(checks), 'native', 0, False)['matched_expected'])

    def test_forged_counts_duplicate_summary_and_unknown_tag_fail(self):
        stdout, stderr = fixture()
        for altered in (stdout.replace('checks=33 passed=33', 'checks=32 passed=32'),
                        stdout + stdout.splitlines()[-1] + '\n', stdout + TAG + ' extra\n'):
            self.assertFalse(grade_fixture(altered, stderr, 'native', 0, False)['matched_expected'])

    def test_duplicate_failure_cannot_replace_another(self):
        stdout, stderr = fixture(failures=ALIAS_FAILURES)
        altered = stdout.replace('FAIL ' + ALIAS_FAILURES[0], 'FAIL ' + ALIAS_FAILURES[1])
        self.assertFalse(grade_fixture(altered, stderr, 'wrong_alias', 1, False)['matched_expected'])

    def test_unproved_terminal_timeout_and_unknown_case_fail(self):
        for name, code, timed_out, terminal in (('native', 0, False, False), ('native', 0, True, True),
                                                ('native', 1, False, True), ('unknown', 0, False, True)):
            self.assertFalse(grade_fixture(*fixture(), name, code, timed_out, terminal)['matched_expected'])

    def test_replay_requires_complete_exact_summaries(self):
        summaries = ['[GPU:REPLAY] SUMMARY frames=1 packets=12 draws=3 parse=CLEAN-EOF',
                     '[VFETCH] SUMMARY vsParsed=2 vsFailed=0 bindings=2',
                     '[VFETCH] SUMMARY   fmt=k_32(33) x2']
        raw = '\n'.join('[prefix] ' + line for line in summaries)
        self.assertTrue(grade_replay(raw, 0, False, True, summaries)['matched_expected'])
        for altered in ('', raw.splitlines()[0], raw + '\n' + raw, raw.replace('CLEAN-EOF', 'TRUNCATED')):
            self.assertFalse(grade_replay(altered, 0, False, True, summaries)['matched_expected'])
        self.assertFalse(grade_replay(raw, 0, True, True, summaries)['matched_expected'])

    def test_timeout_retains_handle_when_termination_unproved(self):
        proc = Mock(pid=42, returncode=None)
        proc.wait.side_effect = subprocess.TimeoutExpired('fixture', 120)
        proc.poll.return_value = None
        active = {'proc': None}
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            with patch('translated_constant_controls.subprocess.Popen', return_value=proc), patch(
                    'translated_constant_controls.shared.terminate_child', return_value={
                        'terminal_observed': False, 'exit_code': None, 'termination_errors': ['wait failed']}):
                result = run(folder / 'fixture.exe', folder, {}, folder, active)
            self.assertIs(active['proc'], proc)
            self.assertFalse(result['terminal_observed'])
            self.assertTrue(result['timed_out'])
            self.assertTrue((folder / 'child_termination.json').is_file())


class SamplerControlTests(unittest.TestCase):
    def fixture(self, abort=False):
        names = SAMPLER_CHECKS[:16] if abort else SAMPLER_CHECKS
        failures = ('queued_version_survives_more_than_seven_option_changes',) if abort else ()
        lines = [SAMPLER_TAG + ' ' + ('FAIL' if n in failures else 'PASS') + ' ' + n for n in names]
        lines += [SAMPLER_TAG + ' ' + SAMPLER_ABORT] if abort else [
            SAMPLER_TAG + ' RESULT checks=23 passed=23 failed=0']
        return '\n'.join(lines) + '\n'

    def test_sampler_full_and_exact_guarded_abort(self):
        self.assertEqual(len(SAMPLER_CHECKS), 23)
        self.assertTrue(grade_sampler_fixture(self.fixture(), '', 'native', 0, False)['matched_expected'])
        self.assertTrue(grade_sampler_fixture(self.fixture(True), '', 'missing_sampler_owner', 1, False)['matched_expected'])

    def test_sampler_rejects_missing_binding_or_lod_proof_and_forged_counts(self):
        raw = self.fixture()
        for altered in (raw.replace(SAMPLER_TAG + ' PASS ' + SAMPLER_CHECKS[-2] + '\n', ''),
                        raw.replace('checks=23 passed=23', 'checks=22 passed=22'), raw + raw.splitlines()[-1] + '\n'):
            self.assertFalse(grade_sampler_fixture(altered, '', 'native', 0, False)['matched_expected'])

    def test_sampler_abort_requires_exact_failures_and_guard(self):
        raw = self.fixture(True)
        for altered in (raw.replace(SAMPLER_ABORT, ''), raw.replace('FAIL ' + SAMPLER_CHECKS[15], 'PASS ' + SAMPLER_CHECKS[15]),
                        raw.replace('FAIL ' + SAMPLER_CHECKS[15], 'FAIL ' + SAMPLER_RETAIN)):
            self.assertFalse(grade_sampler_fixture(altered, '', 'missing_sampler_owner', 1, False)['matched_expected'])

    def test_sampler_rejects_unproved_terminal_timeout_and_setup_error(self):
        raw = self.fixture()
        for name, code, timeout, terminal, err in (('native', 0, False, False, ''), ('native', 0, True, True, ''),
                ('native', 0, False, True, 'SETUP FAILED'), ('unknown', 0, False, True, '')):
            self.assertFalse(grade_sampler_fixture(raw, err, name, code, timeout, terminal)['matched_expected'])


if __name__ == '__main__':
    unittest.main()
