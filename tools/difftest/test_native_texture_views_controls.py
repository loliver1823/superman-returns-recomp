"""Adversarial checks for incomplete or contradictory texture-control evidence."""
import unittest

import native_texture_views_controls as controls


def protocol(names=controls.FULL, failures=frozenset(), debug=True, abort=False):
    sequence = list(names)
    if not abort and debug:
        sequence.append('debug_layer_reports_no_error_or_corruption')
    lines = [controls.TAG + ' debug_layer=' + str(int(debug))]
    lines += [controls.TAG + (' FAIL ' if name in failures else ' PASS ') + name for name in sequence]
    if abort:
        lines.append(controls.ABORT)
    else:
        failed = sum(name in failures for name in sequence)
        lines.append(controls.TAG + f' RESULT checks={len(sequence)} passed={len(sequence)-failed} failed={failed}')
    return '\n'.join(lines)


class TextureEvidenceProtocolTests(unittest.TestCase):
    def test_complete_normal_with_and_without_optional_debug(self):
        for enabled in (True, False):
            self.assertTrue(controls.grade(protocol(debug=enabled), set(), 0, False)['matched_expected'])

    def test_exact_pixel_failure_and_delayed_owner_abort(self):
        fail = {'typeless_srgb_pixels'}
        self.assertTrue(controls.grade(protocol(failures=fail), fail, 1, False)['matched_expected'])
        fail = {controls.RETAIN}
        self.assertTrue(controls.grade(protocol(controls.PREFIX, fail, abort=True), fail, 1, False)['matched_expected'])

    def test_empty_or_truncated_normal_is_not_a_smaller_scope(self):
        for raw in ('', protocol(controls.FULL[:-1])):
            self.assertFalse(controls.grade(raw, set(), 0, False)['matched_expected'])

    def test_duplicate_check_with_consistent_counts_rejected(self):
        self.assertFalse(controls.grade(protocol(controls.FULL + (controls.FULL[-1],)), set(), 0, False)['matched_expected'])

    def test_reordered_checks_rejected(self):
        names = list(controls.FULL); names[0], names[1] = names[1], names[0]
        self.assertFalse(controls.grade(protocol(names), set(), 0, False)['matched_expected'])

    def test_extra_failure_rejected(self):
        expected = {'typeless_srgb_pixels'}
        raw = protocol(failures=expected | {'null_2d_pixels'})
        self.assertFalse(controls.grade(raw, expected, 1, False)['matched_expected'])

    def test_control_must_observe_its_expected_failure(self):
        self.assertFalse(controls.grade(protocol(), {'typeless_srgb_pixels'}, 1, False)['matched_expected'])

    def test_wrong_summary_or_duplicate_summary_rejected(self):
        for debug in (True, False):
            raw = protocol(debug=debug)
            count = len(controls.FULL) + int(debug)
            for bad in (raw.replace(f'checks={count}', f'checks={count - 1}'),
                        raw + '\n' + raw.splitlines()[-1]):
                self.assertFalse(controls.grade(bad, set(), 0, False)['matched_expected'])

    def test_normal_cannot_end_with_ownership_abort(self):
        self.assertFalse(controls.grade(protocol() + '\n' + controls.ABORT, set(), 0, False)['matched_expected'])

    def test_owner_control_requires_complete_prefix_and_no_result(self):
        fail = {controls.RETAIN}
        for bad in (protocol(controls.PREFIX[:-1], fail, abort=True),
                    protocol(controls.PREFIX, fail, abort=True) + '\n' + controls.TAG + ' RESULT checks=29 passed=28 failed=1'):
            self.assertFalse(controls.grade(bad, fail, 1, False)['matched_expected'])

    def test_timeout_and_wrong_exit_rejected(self):
        for code, timeout in ((1, False), (0, True), (None, False)):
            self.assertFalse(controls.grade(protocol(), set(), code, timeout)['matched_expected'])

    def test_unknown_or_duplicate_debug_declaration_rejected(self):
        raw = protocol()
        for bad in (raw.replace('debug_layer=1', 'debug_layer=2'), raw + '\n' + controls.TAG + ' debug_layer=1'):
            self.assertFalse(controls.grade(bad, set(), 0, False)['matched_expected'])

    def test_unknown_tagged_protocol_line_rejected(self):
        self.assertFalse(controls.grade(protocol() + '\n' + controls.TAG + ' ACCEPTED', set(), 0, False)['matched_expected'])


if __name__ == '__main__':
    unittest.main()
