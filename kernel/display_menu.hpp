#pragma once
// iter-465 polish: DISPLAY OPTIONS - the PC display settings as a real game page (PORT FEATURE).
//
// Reached from the custom front end (MAIN MENU > SETTINGS > DISPLAY OPTIONS) and from the pause
// menu (OPTIONS, lane E L-c); rendered by the shared host-page renderer
// (kernel/host_pages.hpp, tools/apt/mods/custom_frontend/devtools_ui.aptasm) in the game's own
// popup (ScrGenPop). Rows (LEFT / RIGHT change a row in place, A too):
//   RESOLUTION     AUTO | 1X | 2X | 3X | 4X   the internal draw scale (gfx/res_scale.hpp):
//                                             frozen at start (as in Xenia), so a change applies
//                                             when the game restarts - A on a pending change
//                                             offers RESTART NOW (the MODS menu's relaunch)
//   DISPLAY        WINDOWED | BORDERLESS FULLSCREEN | EXCLUSIVE FULLSCREEN   live
//   FRAME RATE     30 | 60 | 120 | DISPLAY (<refresh> HZ) | UNLIMITED     live (kernel/highfps)
//   VSYNC          ON | OFF                                                live
//   WINDOW SIZE    windowed: the window's size; exclusive: the OUTPUT resolution  live
//
// The store is <save root>/settings/display.ini (the folder of game_options.ini and
// mod_options.ini, read by both the front end and the pause menu, so the two always agree);
// the same keys a power user can edit by hand (docs/RESOLUTION.md, docs/HIGH_FRAMERATE.md 8).
//
// Charter: host UI plumbing + the two port features' own setters. No guest memory, no guest
// calls here (the live render-loop rate goes through highfps::applyLive).
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include "mod_bridge.hpp"

#include <string>
#include <vector>

namespace sr {
namespace kernel {

// Pages this menu owns ("display", "displayrestart").
bool displayMenuOwnsPage(const std::string& page);

// One SR_Menu command for a display page (routed here by menuPagesCommand).
void displayMenuCommand(const std::string& args, std::vector<ModReply>* out);

}  // namespace kernel
}  // namespace sr
