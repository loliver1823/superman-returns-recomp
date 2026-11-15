#pragma once
// iter-464 (modmenu lane): the host-driven, game-style menu pages - the engine shared by the
// Dev Tools menu (kernel/devtools_menu.cpp, docs/DEVTOOLS_DESIGN.md 13) and the MODS menu
// (kernel/mod_menu.cpp, docs/MODS.md "The MODS menu").
//
// The pages are REAL game popups: the custom_frontend Apt renderer
// (tools/apt/mods/custom_frontend/devtools_ui.aptasm, `_global.SRDevTools`) draws each page
// with FlowMgr.ShowGenPop -> ScrGenPop (DisplayWell, BasicButton rows, the game's font and
// colours). The Apt side only renders; every page (title, body, rows) comes from here through
// the FSCommand bridge (kernel/mod_bridge.cpp):
//
//   AS -> host   fscommand("SR_Menu", "<verb> <page> [<row> [<delta>]]")
//                verbs: open (+ " FE" | " IG", the context) | press | adjust (-1|1) | back
//                ("SR_DevMenu" is the older name of the same command and still accepted)
//   host -> AS   FlowMgr.SR_DTBegin(page, title, body)
//                FlowMgr.SR_DTItem(row, label, kind)        one per row (at most 5)
//                FlowMgr.SR_DTEnd(rows, focus, action)      0 close the menu (back to the
//                                                           caller), 1 show the page, 2 the
//                                                           shown rows changed in place
//
// Routing (menuPagesCommand): pages whose id starts with "mod" or "costume" belong to the MODS
// menu, "display" / "displayrestart" to DISPLAY OPTIONS (kernel/display_menu.cpp, iter-465),
// every other page to the Dev Tools menu. Page ids never contain spaces ("name" or
// "name:<arg>[:<arg>]").
//
// Nothing here reads or writes guest memory or calls guest code: a page builder only reads
// host state, and a row's press/adjust only changes host state (files, flags) or queues work.
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include "mod_bridge.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

// The row kinds the renderer knows: LEFT / RIGHT on a Toggle or a Slider row send "adjust".
enum class DevMenuKind : int { Action = 0, Toggle = 1, Slider = 2, Readout = 3 };

// The rows the AS side can show at once (ScrGenPop MAXNUMBUTTONS).
constexpr int kHostPageMaxRows = 5;

// What one command answers: show a page, update it in place, or close the menu.
struct HostPageOut {
  std::string page;   // the page to (re)build
  int focus = 0;
  int action = 1;     // 0 close, 1 show, 2 update in place
};

struct HostPageRow {
  std::string label;
  DevMenuKind kind = DevMenuKind::Action;
  std::function<void(HostPageOut&)> press;        // A (CROSS)
  std::function<void(HostPageOut&, int)> adjust;  // LEFT / RIGHT (Toggle / Slider rows)
};

struct HostPage {
  std::string id, title, body;
  std::vector<HostPageRow> rows;
  std::function<void(HostPageOut&)> back;         // B; unset = close the menu
};

using HostPageBuilder = std::function<HostPage(const std::string& id)>;

// Runs one "<verb> <page> [<row> [<delta>]]" command against `build` and appends the AS calls
// to *out. *ctxOut (optional) receives the context word of an "open" ("FE" | "IG").
void hostPagesCommand(const std::string& args, const HostPageBuilder& build, std::vector<ModReply>* out,
                      std::string* ctxOut, const char* logTag);

// "levelskip:4" -> ("levelskip", 4); a non-numeric tail gives 0.
void hostPageSplit(const std::string& id, std::string* name, int* off);
std::string hostUpper(std::string s);

// The router behind SR_Menu / SR_DevMenu (kernel/mod_bridge.cpp).
void menuPagesCommand(const std::string& args, std::vector<ModReply>* out);

// Test seams (difftest --mod-overlay-test): decode the calls of one command.
struct HostPageForTest {
  std::string page, title, body;
  std::vector<std::string> labels;
  std::vector<int> kinds;
  int focus = 0, action = -1;
};
HostPageForTest hostPageDecodeForTest(const std::vector<ModReply>& calls);

}  // namespace kernel
}  // namespace sr
