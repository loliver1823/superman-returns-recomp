#pragma once
// Step-4 lane C, item M6 (Astra P2 on the M2 ZPD merge, PROGRESS 2026-10-17): SR_PM4_FRAME_FAILCLOSED (default ON;
// =0 keeps the old silent return for an A/B, dev build only).
//
// D3D12Backend::renderPm4Batches returned on `!pm4Init() || !xlatReady()` and its callers carried on to the
// scan-out and the present: the frame's draws and its pending shared-memory copies / resolves vanished without a
// word (a black or stale frame, guest memory a resolve never wrote). Neither condition is transient: pm4Init fails
// only on a device / resource creation failure, and xlatReady() is false only when the translated pipeline was
// never created or no shader cache is installed (ShaderCache::installedPresent waits for the load; it never answers
// "not yet"). So a frame with work in it stops through the native failure diagnostic
// (nativeRecordingFail: [NATIVE_RECORDING] FAILED stage=..., DRED, exit 3) - RELEASE_COMPOSITION 3.1 rule 4,
// guest-facing fatality instead of a fallback. A frame with nothing to draw or land returns as before.
#include <cstddef>
#include <cstdlib>

#include "../kernel/sr_oracle.hpp"

namespace sr::gfx {

// The failure stage for a frame renderPm4Batches cannot render, or nullptr when it may return quietly.
inline const char* pm4FrameUnavailableStage(bool pm4Ready, bool xlatReady, size_t batches, size_t pending) {
  if (pm4Ready && xlatReady) return nullptr;  // renders
  if (!batches && !pending) return nullptr;   // nothing would be dropped
  return !pm4Ready ? "pm4-frame-no-pm4-renderer" : "pm4-frame-no-translated-pipeline";
}

inline bool pm4FrameFailClosedOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_FRAME_FAILCLOSED");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;  // the release has no silent-drop arm (the gate's name is absent)
#endif
}

}  // namespace sr::gfx
