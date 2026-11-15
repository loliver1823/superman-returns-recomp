// lane E L-b (step 5): difftest --game-options-test - the OPTIONS screen's GAME and CHEATS tabs (kernel/game_options.hpp,
// kernel/pc_settings.hpp, kernel/cheats_menu.hpp, the SR_GameOptionsLive bridge command). No guest: the calls the
// next Apt pump would make are read back through the test seam (takePendingForTest); the store is the in-memory seam.
#include "../../kernel/cheats_menu.hpp"
#include "../../kernel/game_options.hpp"
#include "../../kernel/mod_bridge.hpp"
#include "../../kernel/pc_settings.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void ck(bool c, const char* what) {
  if (c) {
    ++g_pass;
    std::printf("  ok  %s\n", what);
  } else {
    ++g_fail;
    std::printf("  FAIL: %s\n", what);
  }
}

namespace GO = sr::kernel::gameopts;
namespace PS = sr::kernel::pcsettings;

// bits: 0-3 music, 4-7 SFX, 8-9 scheme, 10 X, 11 Y, 12 auto-centre, 13 vibration, 14 apply_to_loaded, 15 volumes known
constexpr int32_t kY = 1 << 11, kAC = 1 << 12, kVib = 1 << 13, kApplyLoaded = 1 << 14, kVolKnown = 1 << 15;

void fresh(int32_t store, int volumes) {
  PS::revertPreviews();   // the screen's previews of an earlier case (it may queue a call: dropped below)
  GO::resetForTest();
  GO::setStoreForTest(store);
  GO::setVolumesForTest(volumes);
}

const PS::Option* row(const std::vector<PS::Option>& cat, const char* id) { return PS::find(cat, id); }

}  // namespace

int runGameOptionsTest() {
  std::printf("[game-options-test]\n");
  if (!GO::enabled()) {
    std::printf("  FAIL: port features are off in this process (run with SR_PORT_FEATURES=1)\n");
    return 1;
  }
  ck(GO::selfTest() == 0, "G1 the packing: every row's field, its range and the round trip");

  // G2 the values are the game's: the opener's live report wins over the store; volumes only with the mixer
  fresh(kAC | 5 | (6 << 4) | kVolKnown | kApplyLoaded, 1);
  ck(GO::value(GO::kInvertY) == "off" && GO::value(GO::kAutoCenter) == "on" && GO::value(GO::kMusic) == "5",
     "G2a no live report yet: the store stands in");
  GO::noteLive(kY | kVib | 7 | (8 << 4), true);
  ck(GO::value(GO::kInvertY) == "on" && GO::value(GO::kAutoCenter) == "off" && GO::value(GO::kVibration) == "on" &&
         GO::value(GO::kMusic) == "7" && GO::value(GO::kSfx) == "8",
     "G2b SR_GameOptionsLive: the GAME tab shows the game's own getters");
  GO::noteLive(kY | 0, false);
  ck(GO::value(GO::kInvertY) == "on" && GO::value(GO::kMusic) == "5" && GO::value(GO::kSfx) == "6",
     "G2c a report without the mixer: its volumes are not the game's, the store's are shown");
  ck(PS::storedValue(GO::kInvertY) == "on" && PS::storedValue(GO::kMusic) == "5",
     "G2d the screen's stored value of a GAME row is the game's live value");

  // G3 a preview goes to the game's own setters on the next pump, not stored, to the opener of the screen
  fresh(kAC | kApplyLoaded | kVolKnown | 10 | (10 << 4), 1);
  GO::noteOpen("IG");
  GO::noteLive(kAC | 10 | (10 << 4), true);
  ck(PS::preview(GO::kInvertY, "on"), "G3a the screen previews INVERT LOOK");
  GO::Pending p = GO::takePendingForTest();
  ck(p.apply && p.stored == -1 && p.target == "_global.SRPause", "G3b one SR_GameOptions call on the pause menu's SRPause, nothing stored");
  ck(p.packed == (kY | kAC | 10 | (10 << 4) | kApplyLoaded | kVolKnown),
     "G3c the call carries the game's options with the change, the store's apply_to_loaded_saves and volumes known");
  ck(!GO::takePendingForTest().apply, "G3d one call per change: nothing more is pending");
  GO::noteOpen("FE");
  PS::preview(GO::kSfx, "3");
  p = GO::takePendingForTest();
  ck(p.apply && p.target == "_global.SRFrontEnd" && ((p.packed >> 4) & 15) == 3, "G3e opened from the main menu: SRFrontEnd");

  // G4 the latest value wins; an APPLY's store survives a later preview in the same pump
  fresh(kApplyLoaded | kVolKnown | 10 | (10 << 4), 1);
  GO::noteLive(10 | (10 << 4), true);
  PS::preview(GO::kMusic, "4");
  PS::preview(GO::kMusic, "6");
  p = GO::takePendingForTest();
  ck(p.apply && (p.packed & 15) == 6, "G4a two previews before a pump: one call with the last value");
  std::string err;
  ck(PS::commit(GO::kScheme, "2", &err), "G4b APPLY of CONTROL SCHEME");
  PS::preview(GO::kMusic, "9");
  p = GO::takePendingForTest();
  ck(p.apply && p.stored >= 0 && ((p.stored >> 8) & 3) == 2 && (p.packed & 15) == 9,
     "G4c the call carries the stored value (the next _global.SRSettings) and the latest preview");
  ck(GO::value(GO::kScheme) == "2", "G4d after APPLY the row holds the applied value");

  // G5 APPLY stores with the capture's rules; no mixer: no volume bit, the store's volumes kept
  fresh(kApplyLoaded | kVolKnown | 10 | (10 << 4), 0);
  GO::noteLive(kAC, false);
  ck(PS::commit(GO::kVibration, "on", &err), "G5a APPLY of VIBRATION without the mixer");
  p = GO::takePendingForTest();
  ck(p.apply && (p.packed & kVolKnown) == 0 && (p.packed & kVib) && (p.packed & 15) == 10,
     "G5b without the mixer the call leaves the game's volumes alone (bit 15 clear)");
  ck(p.stored >= 0 && (p.stored & kVib), "G5c the stored value holds the change");

  // G6 REVERT sends the stored value back
  fresh(kApplyLoaded | kVolKnown | 10 | (10 << 4), 1);
  GO::noteLive(10 | (10 << 4), true);
  PS::preview(GO::kInvertY, "on");
  GO::takePendingForTest();
  PS::revertPreviews();
  p = GO::takePendingForTest();
  ck(p.apply && (p.packed & kY) == 0 && p.stored == -1, "G6 REVERT / a close without APPLY: the game's options go back");

  // G6b (the review's P2-1) two rows previewed, the first APPLY stored, the second's write refused: the store holds the
  // first only, the baseline too, and REVERT sends the second back to its stored value
  fresh(kApplyLoaded | kVolKnown | 10 | (10 << 4) | kAC, 1);
  GO::noteLive(kAC | 10 | (10 << 4), true);
  PS::preview(GO::kInvertY, "on");
  PS::preview(GO::kAutoCenter, "off");
  GO::takePendingForTest();
  ck(PS::commit(GO::kInvertY, "on", &err), "G6b-1 APPLY of INVERT LOOK stored");
  p = GO::takePendingForTest();
  ck(p.stored >= 0 && (p.stored & kY) && (p.stored & kAC),
     "G6b-2 ... the store holds INVERT LOOK and NOT the other row's preview (auto-centre still on)");
  ck(p.apply && (p.packed & kY) && !(p.packed & kAC), "G6b-3 ... while the game keeps the other row's preview");
  GO::setStoreFailForTest(true);
  ck(!PS::commit(GO::kAutoCenter, "off", &err), "G6b-4 the second row's write refused");
  GO::setStoreFailForTest(false);
  ck(GO::value(GO::kAutoCenter) == "on", "G6b-5 ... its row keeps the stored value");
  PS::revertPreviews();
  p = GO::takePendingForTest();
  ck(p.apply && (p.packed & kY) && (p.packed & kAC), "G6b-6 REVERT: the game's auto-centre back on, INVERT LOOK kept");

  // G7 AUTOSAVE and VIEW PAD LAYOUT
  fresh(kApplyLoaded, 1);
  GO::noteOpen("IG");
  ck(GO::commit(GO::kAutosave, "off", false, &err), "G7a AUTOSAVE WITHOUT ASKING applied (not stored in a test)");
  p = GO::takePendingForTest();
  ck(!p.apply && p.fetchModOptions && p.target == "_global.SRPause", "G7b ... then the opener re-reads its mod options");
  std::string msg;
  ck(GO::takeCloseCallForTest(true) == -1, "G7c the opener is not called back while the screen is open");
  ck(GO::takeCloseCallForTest(false) == 0 && GO::takeCloseCallForTest(false) == -1,
     "G7d a plain close: SR_OptionsClosed(0) once (back to the opener's menu)");
  GO::noteOpen("IG");
  ck(PS::runAction("view_pad_layout", &msg) && GO::takeCloseCallForTest(true) == -1 &&
         GO::takeCloseCallForTest(false) == 1,
     "G7e VIEW PAD LAYOUT: once the screen is gone, SR_OptionsClosed(1) (the game's own CONTROLLER OPTIONS)");
  GO::noteOpen("FE");
  ck(PS::runAction("dev_tools", &msg) && GO::takeCloseCallForTest(false) == 2,
     "G7f DEV TOOLS (a dev build: difftest is SR_ORACLE=1): SR_OptionsClosed(2)");
  ck(GO::takeCloseCallForTest(false) == -1, "G7g no screen opened since: no call");

  // G8 the model: tabs, rows, order
  fresh(kApplyLoaded, 1);
  ck(std::string(PS::tabName(PS::Tab::Game)) == "GAME" && std::string(PS::tabName(PS::Tab::Cheats)) == "CHEATS" &&
         int(PS::Tab::Game) == 0 && int(PS::Tab::Controls) == 1 && int(PS::Tab::Cheats) == int(PS::Tab::Count) - 1,
     "G8a the tab strip: GAME first, CONTROLS second, CHEATS last");
  const std::vector<PS::Option> cat = PS::catalogue();
  const char* ids[] = {GO::kInvertY, GO::kAutoCenter, GO::kVibration, GO::kScheme, GO::kMusic, GO::kSfx, GO::kAutosave};
  bool all = true;
  for (const char* id : ids) {
    const PS::Option* o = row(cat, id);
    all = all && o && o->tab == PS::Tab::Game && o->apply == PS::Apply::Live;
  }
  ck(all, "G8b the seven GAME rows, live");
  const PS::Option* dt = row(cat, "dev_tools");
  ck(dt && dt->tab == PS::Tab::Game && dt->kind == PS::Kind::Action, "G8f the development build's DEV TOOLS row on GAME");
  const PS::Option* vp = row(cat, "view_pad_layout");
  ck(vp && vp->tab == PS::Tab::Controls && vp->kind == PS::Kind::Action, "G8c VIEW PAD LAYOUT on CONTROLS");
  int cheats = 0;
  for (const PS::Option& o : cat)
    if (o.tab == PS::Tab::Cheats) cheats += o.kind == PS::Kind::Action ? 1 : 100;
  ck(cheats == int(sr::kernel::cheatsMenuRows().size()) && cheats >= 8, "G8d the CHEATS tab: the cheats page's rows, as actions");
  GO::setVolumesForTest(0);
  const std::vector<PS::Option> cat2 = PS::catalogue();
  const PS::Option* mv = row(cat2, GO::kMusic);
  ck(mv && !mv->available && !mv->unavailableWhy.empty(), "G8e no mixer: the volume rows are disabled with the reason");

  // G9 the bridge command
  fresh(kApplyLoaded, 1);
  sr::kernel::modBridgeSetAudioMixerForTest(1);
  const auto r = sr::kernel::modBridgeDispatch("SR_GameOptionsLive", "IG 2058");   // Y + music 10
  ck(r == sr::kernel::ModBridgeResult::Handled && GO::value(GO::kInvertY) == "on" && GO::value(GO::kMusic) == "10",
     "G9a SR_GameOptionsLive '<ctx> <packed>' is noted (no reply)");
  sr::kernel::modBridgeDispatch("SR_GameOptionsLive", "IG NaN");
  ck(GO::value(GO::kInvertY) == "on", "G9b a bad report changes nothing");
  sr::kernel::modBridgeSetAudioMixerForTest(-1);

  fresh(INT32_MIN, -1);
  std::printf("[game-options-test] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
