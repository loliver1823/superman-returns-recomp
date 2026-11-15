#pragma once
// iter-464 (fe lane): the in-game Dev Tools menu - the HOST half (docs/DEVTOOLS_DESIGN.md
// section 13 "Access", docs/CUSTOM_FRONTEND.md 5.7).
//
// The menu is built from REAL game-style pages: the custom_frontend Apt mod draws every page
// with the game's own popup chrome (FlowMgr.ShowGenPop -> ScrGenPop / DisplayWell /
// BasicButton; tools/apt/mods/custom_frontend/devtools_ui.aptasm). It is reached from the
// front end's SETTINGS panel and from the pause menu's OPTIONS (the GAME tab's DEV TOOLS row, lane E L-c). The Apt side is a
// thin renderer: every page (title, body text, rows) comes from here, through the FSCommand
// bridge (kernel/mod_bridge.cpp):
//
//   AS -> host   fscommand("SR_DevMenu", "<verb> <page> [<row> [<delta>]]")
//                verbs: open (+ " FE" | " IG" for the context) | press | adjust | back
//   host -> AS   FlowMgr.SR_DTBegin(page, title, body)
//                FlowMgr.SR_DTItem(row, label, kind)        one per row (at most 5 rows)
//                FlowMgr.SR_DTEnd(rows, focus, action)      action 0 close the menu (back to
//                                                           the caller), 1 show the page,
//                                                           2 update the shown rows in place
//
// What the menu does is DEVTOOL-class (docs/DEVTOOLS.md): never on by default; switching the
// dev tools on shows a one-time notice (settings/mod_options.ini devtools_notice_ack) and
// tags the session for good (devToolsSetAtRuntime: [RUN_CLASS] dev-tool-assisted, the title
// suffix, the save markers). Nothing here writes guest memory or calls guest code: the menu
// only switches tools and arms the level-skip executor, which then acts on the game's own
// tick (kernel/devtools_levelskip.cpp). Items run on the Apt thread inside the fscommand, so
// an item's set() must only queue work for the tick.
//
// EXTENSION POINT (the DT-2 cheats lane): devMenuRegister() adds a row to a page ("cheats" is
// listed under DEV TOOLS) - a toggle, a slider (LEFT / RIGHT change it in place), an action or
// a readout - with no Apt rebuild. A row names the closed-set devtool it acts through; using
// it switches that tool on (and so tags the session).
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include "host_pages.hpp"
#include "mod_bridge.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

struct DevMenuItemDef {
  std::string page = "cheats";      // the page the row is listed on
  std::string id;                   // stable id (log lines)
  std::string label;                // upper case; a toggle / slider shows ": <value>" after it
  DevMenuKind kind = DevMenuKind::Action;
  const char* tool = nullptr;       // closed-set devtool (string literal) switched on at first use
  std::function<int()> get;         // Toggle (0/1) / Slider: the current value
  std::function<bool(int)> set;     // Toggle / Slider: apply the new value; Action: run (arg 0).
                                    // Apt thread: queue work for the tick, never touch the guest.
  std::function<std::string()> text;  // Readout: the value shown after the label
  int min = 0, max = 1, step = 1;
  std::string unit;                 // after a slider value, e.g. "%"
};

// DT-2 extension point. Thread-safe; later registrations with the same id replace earlier ones.
void devMenuRegister(const DevMenuItemDef& def);

// One SR_DevMenu command (mod_bridge.cpp); appends the AS calls to *out.
void devMenuCommand(const std::string& args, std::vector<ModReply>* out);

// The rows the AS side can show at once (ScrGenPop MAXNUMBUTTONS; host_pages.hpp).
constexpr int kDevMenuMaxRows = kHostPageMaxRows;

// Test seams (difftest --devmenu-test).
void devMenuResetForTest();
using DevMenuPageForTest = HostPageForTest;
inline DevMenuPageForTest devMenuDecodeForTest(const std::vector<ModReply>& calls) {
  return hostPageDecodeForTest(calls);
}

}  // namespace kernel
}  // namespace sr
