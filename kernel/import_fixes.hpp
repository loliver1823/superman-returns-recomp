#pragma once
// step4/audit (logic audit 9a): import answers made equal to Xenia 0bd090dbe's, each behind its own gate
// (batch 1, baked default ON 2026-10-18: SR_XAM_XUID_ABI, SR_XCONFIG_XENIA, SR_XAM_SYSVER_XENIA, SR_XMP_APP_XENIA;
// =0 restores the old answer). kernel/import_fixes.cpp.
#include <cstdint>

namespace sr {
namespace kernel {

class ExportRegistry;

// Called at the end of registerStubs(): replaces an export only when its gate is on at startup.
void registerImportFixes(ExportRegistry& reg);

// Test seam: the big-endian XConfigData image ExGetXConfigSetting answers from (Xenia XConfig::SetDefaults).
const uint8_t* importFixesXConfigImageForTest(uint32_t* size);

// Xenia AudioMediaPlayer::IsTitleInPlaybackControl over SR_XMP_APP_XENIA's state: !override && client == Game &&
// controller == Game (the boot state: true). The notification queue primes XN_XMP_PLAYBACKCONTROLLERCHANGED with
// it (kernel_state.cc:1060-1066). Takes the XMP state's own lock: never call it while holding that lock.
bool xmpTitleInPlaybackControl();
// Test seam (difftest N12): spin up to maxUs microseconds between an XMP state change and its publication (0 = off).
void xmpPublishSpinForTest(uint32_t maxUs);

}  // namespace kernel
}  // namespace sr
