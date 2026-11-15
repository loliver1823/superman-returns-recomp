#pragma once
// lane E L-a (step 5; the owner's E14, mock-up q14_achievements (b), STEP5_PLAN 9): THE ACHIEVEMENTS PAGE - this
// save's achievements over the paused game, drawn in the pause panel's look with the shared overlay kit
// (gfx/overlay_kit.hpp). The model is kernel/achievements_page.hpp (built from the achievements core's read API,
// kernel/achievements.hpp); the icons are the title's own (its XDBF images, sr::ach::iconPng, decoded on the kit's
// image worker; a locked one drawn grey, a locked secret as "?"). The routing is the mod library's
// (kernel/mod_library.hpp OverlayPage::Achievements: one host overlay at a time, the neutral pad while it is open, the
// release latch when it closes).
//
// Opened by the SR_Achievements fscommand (kernel/mod_bridge.cpp; the pause menu's ACHIEVEMENTS row sends it - L-c).
// Drawn through the OPTIONS screen's slot on the present path (gfx/settings_ui.cpp render hands the frame here while
// this page is up), so the present path itself is unchanged.
// Input: UP / DOWN (pad, keys, the harness keyboard) move the focus and the list follows it; LB / RB and
// PgUp / PgDn move a page, Home / End to the ends; the wheel and the two arrows scroll; B / Esc / a right click / a
// click on BACK close it. The pause menu under it is as it was (ACHIEVEMENTS still focused). No toast; nothing is
// written anywhere.
//
// Charter: a host page. It reads no guest memory, writes none and calls no guest code (the core's snapshot is a host
// copy; the icons come from the loader's copy of the XDBF).
//
// Log lines: [ACH_PAGE] shown / view / icons / close / hidden.
// (L-c: the pause menu's own ACHIEVEMENTS row opens it; L-a's developer aid SR_ACH_PAGE_ON_PAUSE is gone.)
#ifdef _WIN32
#include "settings_ui.hpp"

#include "../kernel/achievements.hpp"

namespace sr {
namespace gfx {
namespace achui {

// Every present (through settingsui::wantsFrame): is the page up or asked for?
bool wantsFrame();
// The page's frame (through settingsui::render while wantsFrame()). `otherOverlay` = another host overlay (the mod
// library) draws this frame: an open request waits for it to close; a page already up hands over (one owner).
bool render(const settingsui::FrameCtx& ctx, bool otherOverlay);
// Test seam (difftest --achievements-page-test): the input edges (held keys at open, repeats, the stick); 0 = pass.
int inputSelfTest();
// F12 (with SR_MOD_LIBRARY_SHOTS) on the page: capture this composited frame.
bool shotRequested();

// Test seam (difftest --achievements-render): draw this snapshot instead of the core's.
void setSnapshotForTest(const sr::ach::Snapshot& s);

}  // namespace achui
}  // namespace gfx
}  // namespace sr
#endif
