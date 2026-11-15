// iter-463 SR_TIMEBASE_50MHZ, phase 1: the guest timebase (mftb) in ONE clock domain.
//
// XenonRecomp lowers every `mftb rN` to `rN = __rdtsc();` (recompiler.cpp PPC_INST_MFTB),
// i.e. the raw host TSC: ~3.7 GHz = 74x the console's 50 MHz timebase, low 32 bits
// wrapping every ~1.16 s instead of ~85.9 s. Xenia's default path gives the guest a
// 50 MHz clock (emulator.cc set_guest_tick_frequency(50000000)).
//
// guest/CMakeLists.txt force-includes this header ONLY into the generated TUs whose
// text contains `__rdtsc` (configure-time detection), so every mftb site - including
// the ones inside spliced bodies - calls sr_guest_mftb() (kernel/sr_timebase.cpp).
// Default ON; with SR_TIMEBASE_50MHZ=0 sr_guest_mftb() returns __rdtsc() unchanged (legacy A/B).
//
// ORDER MATTERS: the intrinsic headers must be seen BEFORE the macro exists, or clang's
// own inline __rdtsc definition would be macro-renamed and break.
// Phase 2 (follow-up): the recompiler emits PPC_MFTB() directly (+ >>32 for mftbu TBR 269)
// and this force-include goes away.
#pragma once
#include <x86intrin.h>
#include <cstdint>

extern "C" uint64_t sr_guest_mftb() noexcept;

#undef __rdtsc
#define __rdtsc() sr_guest_mftb()
