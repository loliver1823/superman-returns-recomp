import unittest
import numpy as np
from native_freeze_sequence import sequence_metrics


def metadata(k, present, stamp='1.0000', held=None):
    return {'k': k, 'present': present, 'shown_stamp': stamp,
            'held_ms': 400 + k * 100 if held is None else held}


class SequenceTests(unittest.TestCase):
    def test_large_initial_transition_is_kept(self):
        dark = np.zeros((2, 2, 3), dtype=np.uint8)
        bright = np.full((2, 2, 3), 255, dtype=np.uint8)
        result = sequence_metrics([(metadata(0, 20), dark), (metadata(1, 21), bright),
                                   (metadata(2, 22), bright)])
        self.assertEqual(result['large_pair_indices'], [0])
        self.assertEqual(result['pairs'][0]['max_difference'], 255)
        self.assertEqual(result['pairs'][0]['signed_channel_sum'], 3060)
        self.assertEqual(result['pairs'][1]['greater_than_one_pixels'], 0)

    def test_late_large_transition_is_distinguished(self):
        a = np.zeros((1, 1, 3), dtype=np.uint8)
        b = np.full_like(a, 20)
        result = sequence_metrics([(metadata(0, 1), a), (metadata(1, 2), a), (metadata(2, 3), b)])
        self.assertEqual(result['large_pair_indices'], [1])

    def test_clock_change_rejected(self):
        a = np.zeros((1, 1, 3), dtype=np.uint8)
        with self.assertRaises(ValueError):
            sequence_metrics([(metadata(0, 1), a), (metadata(1, 2, stamp='2.0000'), a)])

    def test_incomplete_or_repeated_membership_rejected(self):
        a = np.zeros((1, 1, 3), dtype=np.uint8)
        for second in (metadata(2, 2), metadata(1, 1), metadata(1, 2, held=400)):
            with self.subTest(second=second), self.assertRaises(ValueError):
                sequence_metrics([(metadata(0, 1), a), (second, a)])

    def test_changed_dimensions_rejected(self):
        with self.assertRaises(ValueError):
            sequence_metrics([(metadata(0, 1), np.zeros((1, 1, 3))),
                              (metadata(1, 2), np.zeros((2, 1, 3)))])

    def test_single_frame_is_not_evidence(self):
        with self.assertRaises(ValueError):
            sequence_metrics([(metadata(0, 1), np.zeros((1, 1, 3)))])


if __name__ == '__main__':
    unittest.main()
