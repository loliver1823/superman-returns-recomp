#pragma once
// iter-465 charswitch: CHEATS - the game's own cheats as a page (PORT FEATURE). Owner decision
// 2026-10-05: suits unlock by the game's own challenges; unlocking them all is a cheat the player
// chooses. Reached from the pause menu (OPTIONS > the CHEATS tab; lane E L-c), where the retail button codes are
// entered too; rendered by the shared host-page renderer (kernel/host_pages.hpp).
//
// Every row is a retail cheat from the disc's CheatDefs. Owner decision 2026-10-06 (charter 2a):
// a row CALLS the game's own functions, no virtual presses. On the next UI tick (the game's own
// thread, after PauseFlowStateTick sub_82151298 - where the game's matcher runs) cheatsMenuOnUiTick
// runs the matcher's own condition (CheatMgr_FeedPadButton_sub_82237360: the world flow's
// objectiveActivated is 0 and objectiveStatus bit 6, WARobj02, is set) and, if it passes, the
// matcher's own effect call sub_82236840(cheatMgr, index, 1). Refused -> the page says so. UNLOCK ALL
// SUITS = EXTRA_UNLOCK: it awards the suits, trophies and theater items to the save (permanent once
// saved). The others are the game's ON/OFF switches. Off unless port features are on.
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include "mod_bridge.hpp"

#include <cstdint>

struct PPCContext;
#include <string>
#include <vector>

namespace sr {
namespace kernel {

// Pages this menu owns ("gamecheats", "gamecheats:1").
bool cheatsMenuOwnsPage(const std::string& page);

// One SR_Menu command for a cheats page (routed here by menuPagesCommand).
void cheatsMenuCommand(const std::string& args, std::vector<ModReply>* out);

// lane E L-b (step 5, STEP5_PLAN 10): the same cheats as the OPTIONS screen's CHEATS tab (kernel/pc_settings.hpp,
// in game only), one action row each; a row's press queues the cheat exactly as this page's row does (the next UI
// tick applies it through the game's own functions, or the game refuses it). Port features only (no rows otherwise).
struct CheatRow {
  std::string id;        // "cheat_<code>" or "cheat_disguise"
  std::string section;   // the tab's group header
  std::string label;     // with the state: "INFINITE STAMINA: ON"
  std::string verb;      // the button's word: TURN ON / TURN OFF / UNLOCK / NEXT
  std::string desc;      // what it does, and the game's refusal of the last one, when there was one
};
std::vector<CheatRow> cheatsMenuRows();
// Queue the row's cheat. *msg = what the screen's toast says. False for an unknown row (or port features off).
bool cheatsMenuRun(const std::string& id, std::string* msg);

// Called by the UI-tick override (kernel/cs_timeout_underflow_fix.cpp sub_82151298) after the
// game's own tick, on its thread: runs a chosen cheat (one per tick) and logs the [CHEATS] state
// line when a cheat's ON flag changes. Returns at once when port features are off.
void cheatsMenuOnUiTick(PPCContext& ctx, uint8_t* base);

}  // namespace kernel
}  // namespace sr
