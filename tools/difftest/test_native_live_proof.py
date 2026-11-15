"""Adversarial freeze identity and coverage tests; no game or GPU process."""
import copy
import unittest
from native_live_proof import plan_pairs


def fixture():
    return ({'world': {'gameplay': [1, 2], 'menu_world': []}, 'world_drawn': 2, 'counted': 2,
             'freezes': [{'freeze': 1, 'ingame': True, 'line': '1 | 3 | 2 | 1 | p100->p101 | 7 | 2 | 0,0-1,1'},
                         {'freeze': 2, 'ingame': True, 'line': '2 | 2 | 1 | 0 | - | 0 | 0 | -'}]},
            {1: [(0, 100), (1, 101), (2, 102)], 2: [(0, 200), (1, 201)]},
            {100, 101, 102, 200, 201})


class FreezeProofAdmission(unittest.TestCase):
    def test_explicit_pair_and_zero_row_cover_every_world_freeze(self):
        grade, markers, files = fixture()
        plans = plan_pairs(grade, markers, files)
        self.assertEqual([(x[0]['freeze'], x[1]) for x in plans], [(1, [100, 101]), (2, [200, 201])])

    def test_frame_from_another_freeze_is_rejected(self):
        grade, markers, files = fixture()
        grade['freezes'][0]['line'] = grade['freezes'][0]['line'].replace('p101', 'p201')
        with self.assertRaisesRegex(ValueError, 'belong'):
            plan_pairs(grade, markers, files)

    def test_nonconsecutive_same_freeze_pair_is_rejected(self):
        grade, markers, files = fixture()
        grade['freezes'][0]['line'] = grade['freezes'][0]['line'].replace('p101', 'p102')
        with self.assertRaisesRegex(ValueError, 'consecutive'):
            plan_pairs(grade, markers, files)

    def test_missing_qualifying_world_row_is_rejected(self):
        grade, markers, files = fixture()
        grade['freezes'].pop()
        with self.assertRaisesRegex(ValueError, 'coverage'):
            plan_pairs(grade, markers, files)

    def test_duplicate_qualifying_world_row_is_rejected(self):
        grade, markers, files = fixture()
        grade['freezes'].append(copy.deepcopy(grade['freezes'][0]))
        with self.assertRaisesRegex(ValueError, 'repeated'):
            plan_pairs(grade, markers, files)

    def test_duplicate_world_identity_is_rejected(self):
        grade, markers, files = fixture()
        grade['world']['menu_world'].append(1)
        with self.assertRaisesRegex(ValueError, 'reconcile'):
            plan_pairs(grade, markers, files)

    def test_false_world_total_is_rejected(self):
        grade, markers, files = fixture()
        grade['world_drawn'] = 3
        with self.assertRaisesRegex(ValueError, 'reconcile'):
            plan_pairs(grade, markers, files)

    def test_counted_scope_below_world_is_rejected(self):
        grade, markers, files = fixture()
        grade['counted'] = 1
        with self.assertRaisesRegex(ValueError, 'counted'):
            plan_pairs(grade, markers, files)

    def test_ungraded_world_row_is_rejected(self):
        grade, markers, files = fixture()
        grade['freezes'][1]['ingame'] = False
        with self.assertRaisesRegex(ValueError, 'ungraded'):
            plan_pairs(grade, markers, files)

    def test_missing_source_frame_is_rejected(self):
        grade, markers, files = fixture()
        files.remove(101)
        with self.assertRaisesRegex(ValueError, 'frame scope'):
            plan_pairs(grade, markers, files)

    def test_repeated_marker_present_is_rejected(self):
        grade, markers, files = fixture()
        markers[1].append((3, 101))
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            plan_pairs(grade, markers, files)

    def test_table_identity_cannot_name_another_freeze(self):
        grade, markers, files = fixture()
        grade['freezes'][0]['line'] = grade['freezes'][0]['line'].replace('1 | 3', '2 | 3')
        with self.assertRaisesRegex(ValueError, 'identity'):
            plan_pairs(grade, markers, files)

    def test_difference_row_cannot_omit_its_pair(self):
        grade, markers, files = fixture()
        grade['freezes'][1]['line'] = '2 | 2 | 1 | 1 | - | 7 | 2 | -'
        with self.assertRaisesRegex(ValueError, 'zero-difference'):
            plan_pairs(grade, markers, files)

    def test_empty_world_is_not_proof(self):
        grade, markers, files = fixture()
        grade['world']['gameplay'] = []
        grade['world_drawn'] = 0
        with self.assertRaisesRegex(ValueError, 'reconcile'):
            plan_pairs(grade, markers, files)


if __name__ == '__main__':
    unittest.main()
