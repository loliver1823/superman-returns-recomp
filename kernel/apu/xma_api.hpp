// Macro-independent XMA import API for the shared recompiled-game library.
// Device and test-oracle types stay private to their respective harness targets.
#pragma once

#include "kernel/sr_oracle.hpp"

namespace sr::kernel {
class ExportRegistry;
}

namespace sr::kernel::apu {
#if SR_ORACLE
bool xmaNewEnabled();   // SR_XMA_NEW; selects the native imported API (=0 keeps the legacy fakes: the A/B).
#else
// C1 (docs/RELEASE_COMPOSITION.md 2.6): the SR_XMA_NEW=0 legacy XMA fakes are a disabled fallback component. The
// release configuration has no selector: the native XMA* exports are the only implementation, and the gate's name
// is absent from the executable (a marker of docs/release_composition.json's xma_api.hpp rule).
inline constexpr bool xmaNewEnabled() { return true; }
#endif
// The SR_XMA_NEW=0 arm: the 16 legacy XMA fakes (kernel/apu/xma_legacy_exports.cpp, an EMULATION_SOURCES unit of the dev
// build and difftest), registered by kernel/stubs.cpp before registerXmaExports, which replaces them when the gate is on.
// The release configuration holds no definition: the call site is #if SR_ORACLE (a release build that calls it fails to
// link; one that links the unit fails the composition audit on the unit and its [XMA_LEGACY] marker).
void registerXmaLegacyExports(ExportRegistry& reg);
}  // namespace sr::kernel::apu
