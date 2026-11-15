"""No-game defect-grading regressions for the native submission/park runner."""
import unittest
from native_transport_test_run import (AB_APPEND_ENTRY, AB_APPEND_OFF, AB_APPEND_SDK, AB_CHECKS, AB_END_OFF,
                                       AB_END_SDK, AB_MIXED_ARMS, AB_MIXED_STATE, ALIAS, ARENA, ARENA_TAIL, CHECKS,
                                       D_BOOT, D_DISPATCH, D_MARKER, D_NODE, D_READONLY, D_STATE, D_TAIL, E_FOLLOW,
                                       E_OWN, E_PARK, E_PATH, EXCLUSION_CHECKS, EXCLUSION_ENV_CHECKS, STORE_RELEASE,
                                       T_DISPATCH, T_OWN, T_PATH, T_REL, X_ENV, X_PERMANENT, X_ROUTES, R_HOLDS,
                                       R_NOT_DRAINED, R_DECLINE, R_SKIPPED, R_COMPLETES, R_FREES, RETIRE_CHECKS, grade)


def transcript(failures=(), counts=None, checks=CHECKS):
    good=checks-len(failures)
    passed,failed=counts or (good,len(failures))
    return (b'  ok  fixture check\n'*good + b''.join(b'FAIL: '+line+b'\n' for line in failures) +
            ('[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n' % (passed,failed)).encode())


class GradingTests(unittest.TestCase):
    label=b'consumer parks before acquired recheck: producer takes direct route'

    def test_complete_positive_and_windows_lines_accept(self):
        self.assertTrue(grade(0,transcript()))
        self.assertTrue(grade(1,transcript((self.label,)).replace(b'\n',b'\r\n'),'chain_recheck'))

    def test_missing_or_unrelated_failure_rejects(self):
        self.assertFalse(grade(1,transcript(),'chain_recheck'))
        self.assertFalse(grade(1,transcript((self.label,b'unrelated failure')),'chain_recheck'))

    def test_incorrect_accounting_rejects(self):
        self.assertFalse(grade(1,transcript((self.label,),counts=(CHECKS-2,2)),'chain_recheck'))
        self.assertFalse(grade(0,transcript().replace(b'  ok  fixture check\n',b'',1)))

    def test_crash_timeout_or_duplicate_summary_rejects(self):
        for code in (124,3221225477):
            self.assertFalse(grade(code,transcript((self.label,)),'chain_recheck'))
        self.assertFalse(grade(0,transcript()+b'[NATIVE_TRANSPORT_TEST] 169 passed, 0 failed\n'))

    def test_range_defects_require_their_range_witness(self):
        self.assertTrue(grade(1,transcript(ALIAS),'contiguity'))
        self.assertFalse(grade(1,transcript(ALIAS[1:]),'contiguity'))  # the node crossing is required
        self.assertFalse(grade(1,transcript((ALIAS[0],ARENA)),'contiguity'))
        self.assertTrue(grade(1,transcript((ARENA,ARENA_TAIL)),'arena_bound'))
        self.assertFalse(grade(1,transcript((ARENA_TAIL,)),'arena_bound'))

    def test_tracker_path_defects_need_the_sdk_differential(self):
        # a bypassing store equals the SDK in every byte: only the tracker's view of the call can catch it
        self.assertTrue(grade(1,transcript((T_PATH,T_REL,T_REL,E_PARK,E_PATH)),'pair_bypass'))
        self.assertFalse(grade(1,transcript((T_REL,E_PARK)),'pair_bypass'))
        self.assertFalse(grade(1,transcript((T_PATH,T_DISPATCH)),'pair_bypass'))
        self.assertTrue(grade(1,transcript((STORE_RELEASE,T_OWN,T_REL,T_PATH,E_OWN,E_PARK,E_FOLLOW)),'handler_release'))
        self.assertFalse(grade(1,transcript((STORE_RELEASE,)),'handler_release'))

    def test_foreign_protection_defects_are_graded_in_their_modes(self):
        raw=transcript((D_NODE,D_TAIL,D_MARKER,D_STATE,D_DISPATCH,D_READONLY,D_BOOT),checks=EXCLUSION_CHECKS)
        self.assertTrue(grade(1,raw,'declared_range',checks=EXCLUSION_CHECKS))
        self.assertFalse(grade(1,raw,'declared_range'))  # the native-mode count
        self.assertFalse(grade(1,transcript((D_READONLY,D_BOOT),checks=EXCLUSION_CHECKS),'declared_range',
                               checks=EXCLUSION_CHECKS))
        raw=transcript((X_ROUTES,X_PERMANENT),checks=EXCLUSION_CHECKS)
        self.assertTrue(grade(1,raw,'exclusion',checks=EXCLUSION_CHECKS))
        self.assertFalse(grade(1,transcript((X_PERMANENT,),checks=EXCLUSION_CHECKS),'exclusion',checks=EXCLUSION_CHECKS))
        raw=transcript((X_ENV,),checks=EXCLUSION_ENV_CHECKS)
        self.assertTrue(grade(1,raw,'exclusion_env',checks=EXCLUSION_ENV_CHECKS))
        self.assertFalse(grade(1,transcript((X_ROUTES,),checks=EXCLUSION_ENV_CHECKS),'exclusion_env',
                               checks=EXCLUSION_ENV_CHECKS))
        self.assertFalse(grade(3221225477,raw,'exclusion_env',checks=EXCLUSION_ENV_CHECKS))

    def test_arena_release_defects_are_graded_in_retire_mode(self):
        # a release that frees under the paused append ends the process by name (the append never resumes)
        raw=transcript((R_NOT_DRAINED,R_SKIPPED,R_COMPLETES,R_FREES),checks=RETIRE_CHECKS)
        self.assertTrue(grade(1,raw,'arena_drain',checks=RETIRE_CHECKS))
        self.assertFalse(grade(1,raw,'arena_drain'))  # the native-mode count
        self.assertFalse(grade(1,transcript((R_SKIPPED,),checks=RETIRE_CHECKS),'arena_drain',checks=RETIRE_CHECKS))
        raw=transcript((R_HOLDS,R_NOT_DRAINED,R_DECLINE,R_SKIPPED,R_COMPLETES,R_FREES),checks=RETIRE_CHECKS)
        self.assertTrue(grade(1,raw,'arena_lease',checks=RETIRE_CHECKS))
        self.assertFalse(grade(1,raw,'release_retire',checks=RETIRE_CHECKS))  # only the release's own checks
        self.assertTrue(grade(1,transcript((R_SKIPPED,R_COMPLETES,R_FREES),checks=RETIRE_CHECKS),'release_retire',
                              checks=RETIRE_CHECKS))
        self.assertFalse(grade(3221225477,transcript((R_SKIPPED,),checks=RETIRE_CHECKS),'release_retire',
                               checks=RETIRE_CHECKS))

    def test_ownership_defect_requires_query_only_complete_accounting(self):
        label=b'mixed tracked and unowned read-only range declines without releasing either page'
        raw=transcript((label,),checks=18)
        self.assertTrue(grade(1,raw,'tracked_ownership',checks=18))
        self.assertFalse(grade(1,raw,'tracked_ownership'))
        self.assertFalse(grade(3221225477,raw,'tracked_ownership',checks=18))

    def test_ab_window_defect_needs_the_counter_witness(self):
        # An entry that ignores its A/B window still equals the SDK: only the arm counters can catch it.
        raw=transcript((AB_APPEND_OFF,)*6+(AB_APPEND_ENTRY,AB_MIXED_ARMS,AB_MIXED_ARMS),checks=AB_CHECKS)
        self.assertTrue(grade(1,raw,'ab_append_window',checks=AB_CHECKS))
        self.assertFalse(grade(1,raw,'ab_append_window'))  # the native-mode count
        self.assertFalse(grade(1,transcript((AB_APPEND_ENTRY,AB_MIXED_ARMS),checks=AB_CHECKS),'ab_append_window',
                               checks=AB_CHECKS))
        self.assertFalse(grade(1,transcript((AB_APPEND_OFF,AB_END_OFF),checks=AB_CHECKS),'ab_append_window',
                               checks=AB_CHECKS))
        self.assertTrue(grade(1,transcript((AB_END_OFF,AB_END_OFF),checks=AB_CHECKS),'ab_endsync_window',
                              checks=AB_CHECKS))

    def test_ab_off_skip_defect_requires_the_sdk_differential(self):
        raw=transcript((AB_APPEND_SDK,AB_END_SDK,AB_MIXED_STATE),checks=AB_CHECKS)
        self.assertTrue(grade(1,raw,'ab_off_skips',checks=AB_CHECKS))
        self.assertFalse(grade(1,transcript((AB_MIXED_STATE,),checks=AB_CHECKS),'ab_off_skips',checks=AB_CHECKS))
        self.assertFalse(grade(1,transcript((AB_APPEND_SDK,AB_APPEND_OFF),checks=AB_CHECKS),'ab_off_skips',
                               checks=AB_CHECKS))


if __name__=='__main__': unittest.main()
