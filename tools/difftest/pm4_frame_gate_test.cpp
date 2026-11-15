// Item M6 (gfx/pm4_frame_gate.hpp): which frames renderPm4Batches may drop quietly, and the gate's arms.
// Built twice (CMakeLists.txt): SR_ORACLE=1 (dev: SR_PM4_FRAME_FAILCLOSED read, =0 the old silent return) and
// SR_ORACLE=0 (release: always fail closed). The wiring (the call in renderPm4Batches reaching nativeRecordingFail)
// is proven by a replay with no shader cache installed (lane C NOTES, M6), not here.
#include "gfx/pm4_frame_gate.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[PM4_FRAME_GATE_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
bool same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
}  // namespace

int main() {
  using sr::gfx::pm4FrameUnavailableStage;
  check(!pm4FrameUnavailableStage(true, true, 0, 0) && !pm4FrameUnavailableStage(true, true, 12, 3),
        "ready_renderer_never_stops");
  check(!pm4FrameUnavailableStage(false, false, 0, 0) && !pm4FrameUnavailableStage(true, false, 0, 0),
        "empty_frame_returns_quietly");
  check(same(pm4FrameUnavailableStage(true, false, 1, 0), "pm4-frame-no-translated-pipeline") &&
            same(pm4FrameUnavailableStage(true, false, 0, 1), "pm4-frame-no-translated-pipeline"),
        "draws_or_pending_without_translation_stop");
  check(same(pm4FrameUnavailableStage(false, true, 5, 0), "pm4-frame-no-pm4-renderer") &&
            same(pm4FrameUnavailableStage(false, false, 0, 2), "pm4-frame-no-pm4-renderer"),
        "draws_or_pending_without_renderer_stop");
  const char* env = std::getenv("SR_PM4_FRAME_FAILCLOSED");
#if SR_ORACLE
  const bool expect = !env || env[0] != '0';
  check(sr::gfx::pm4FrameFailClosedOn() == expect, expect ? "dev_default_fails_closed" : "dev_zero_keeps_old_return");
#else
  check(sr::gfx::pm4FrameFailClosedOn(), env ? "release_ignores_the_gate" : "release_always_fails_closed");
#endif
  std::printf("[PM4_FRAME_GATE_TEST] RESULT checks=%d passed=%d failed=%d\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
