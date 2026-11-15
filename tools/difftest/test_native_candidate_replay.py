"""Rejected aggregate references cannot seed a fresh candidate replay bank."""
import unittest
from native_candidate_replay import validate_reference


class ReferenceAdmission(unittest.TestCase):
    def passing(self):
        return {'matched_expected': True, 'slot_release_exit_code': 0,
                'source_pins_unchanged': True, 'binary_pins_unchanged': True,
                'harness_pins_unchanged': True, 'owner_wip_pins_unchanged': True}

    def test_accepts_complete_passing_reference(self):
        validate_reference(self.passing())

    def test_rejects_failed_or_missing_aggregate(self):
        for value in (False, None, 1):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_reference({**self.passing(), 'matched_expected': value})

    def test_rejects_failed_or_missing_release(self):
        for value in (1, None, False, '0'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_reference({**self.passing(), 'slot_release_exit_code': value})

    def test_rejects_any_failed_provenance(self):
        for key in (k for k in self.passing() if k.endswith('_unchanged')):
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_reference({**self.passing(), key: False})

    def test_rejects_vacuous_or_nonboolean_provenance(self):
        for guards in ({}, {'source_pins_unchanged': 1}, {'source_pins_unchanged': None}):
            with self.subTest(guards=guards), self.assertRaises(ValueError):
                validate_reference({'matched_expected': True, 'slot_release_exit_code': 0, **guards})


if __name__ == '__main__':
    unittest.main()
