// iter-464: the in-game mouse cursor (kernel/mouse_ui.{hpp,cpp}; docs/MOUSE_CURSOR.md).
//
// Run: build/difftest.exe --mouse-ui-test     (exit = failed checks)
//
// What it proves (all in kernel/mouse_ui.cpp mouseUiSelfTest):
//   M1  the Apt property-table key (FNV-1a-16, case-insensitive, 0 -> 17767) and the
//       window -> stage mapping (1280x720 identity, 1080p scales; a 16:10 / 21:9 output
//       maps through the centred 16:9 fit = render/resscale's present fit; 4K fit = x3);
//   M2  the slider drag formula against the retail Slider (10 sections of 17 px from
//       x 489.8; a click inside section i = value i, clamped to the track);
//   M3  navigation: the focus-manager list wraps (shorter way), the IconMenu row does not,
//       the ThumbViewer grid moves the column first; overlapping popup rows resolve to the
//       nearer centre; disabled rows are never hit;
//   M4  the READER on a synthetic Apt heap laid out as the runtime lays it out (deleted
//       and hash-not-yet-cached table slots, prototypes on property-table +12, arrays,
//       wrapper -> instance -> state, a 2x-scaled parent): the screen walk picks the popup
//       (last screen, not pass-through), the title when there is no popup, nothing in
//       gameplay (BG / FG / HUD all pass through), and it reports an input lock;
//   M5  host-built pages on the game's popup (the Dev Tools pages): mParams.context.kinds
//       (dense or sparse) marks toggle / slider rows as value rows; a context without
//       `kinds` (the front end's panels) stays a plain popup.
#include "kernel/mouse_ui.hpp"

#include <cstdio>

int runMouseUiTest() {
  std::printf("[mouse-ui-test] kernel/mouse_ui.cpp self test\n");
  const int fails = sr::kernel::mouseUiSelfTest();
  std::printf("[mouse-ui-test] %s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails;
}
