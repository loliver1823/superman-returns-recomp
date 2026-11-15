#pragma once
// C1 (docs/RELEASE_PLAN.md section 1; ledger release.composition): the development-oracle switch.
//
// SR_ORACLE = 1 (the dev build, difftest): the development oracles are compiled in - CHECK paths, in-roll A/B
// toggles, PM4 traces / replays / coverage, censuses, observe brackets, self-tests.
// SR_ORACLE = 0 (CMake SR_RELEASE_COMPOSITION=ON, the release configuration - no preset enables it yet): they are
// compiled OUT of SupermanReturns (CMakeLists.txt ORACLE_SOURCES; the D3D12 backend split, step 2, took out the HLE
// renderer A/B and the observers wired into the backend).
// An oracle header keeps its API as inline no-ops under SR_ORACLE=0, so shipping code calls it unchanged, and the
// oracle's .cpp leaves the target (CMakeLists.txt ORACLE_SOURCES). Overrides that lived in an oracle .cpp were all
// observe-only (docs/OVERRIDES.json), so the recompiled bodies run in their place.
// tools/player/composition_audit.py reads -DSR_ORACLE=0 per object and audits what the release build links.
//
// Test instruments that grade the shipped binary itself (the harness input channel, present dumps, the freeze holds)
// are not oracles: RELEASE_PLAN step 10 re-runs the gates on the final package, so they stay (docs/
// release_composition.json, class diag).
#ifndef SR_ORACLE
#define SR_ORACLE 1
#endif
