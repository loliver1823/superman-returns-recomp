#pragma once
// iter-467 native-a: the shader time 0x29 held with the sim under SR_DIAG_SIM_FREEZE (kernel/timebase_obs.cpp).
#include <cstdint>

namespace sr::shtime {

// Test-only (difftest --shtime-hold-test-on): arm / disarm THIS thread's QueryPerformanceCounter substitution
// with a fixed value, exactly as a hold arms it around one ShaderParam_WriteTimeSeconds call. It takes effect only
// where the gate (SR_DIAG_SIM_FREEZE) is on.
void armFixedForTest(uint64_t qpc) noexcept;
void disarmForTest() noexcept;
// QueryPerformanceCounter reads the hold substituted so far (0 in every run without SR_DIAG_SIM_FREEZE).
uint64_t substitutions() noexcept;

}  // namespace sr::shtime
