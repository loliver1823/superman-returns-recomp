#pragma once
// lane E L-b (step 5; owner Q5 (b), STEP5_PLAN sections 7 and 10): THE GAME TAB of OPTIONS - the game's own options
// as rows of the OPTIONS screen (gfx/settings_ui.cpp; the model is kernel/pc_settings.hpp): INVERT LOOK (Y AXIS),
// AUTO-CENTRE CAMERA, VIBRATION, CONTROL SCHEME, MUSIC VOLUME, SOUND EFFECTS VOLUME, plus the port's AUTOSAVE
// WITHOUT ASKING.
//
// THE VALUES ARE THE GAME'S. The custom front end that opens OPTIONS sends, right before the open request, what the
// game's own SettingsAdapter getters return (fscommand "SR_GameOptionsLive <packed>", the packing of
// kernel/mod_bridge.hpp: bits 0-3 music, 4-7 SFX, 8-9 scheme, 10 X / 11 Y inversion, 12 auto-centre, 13
// vibration); each options capture (SR_SetSettings) refreshes it. Until the first one the universal store
// (settings/game_options.ini) stands in. The volumes are the game's only while its audio mixer exists (the
// getters return a cached 0 before it): without it the two volume rows are shown unavailable.
//
// A CHANGE GOES BACK THROUGH THE GAME'S OWN SETTERS. On the next Apt pump (kernel/apt_parse_trace.cpp: the guest
// thread, the AS interpreter idle, as the mouse cursor's calls) the host calls the opener's own method,
// _global.SRFrontEnd or _global.SRPause .SR_GameOptions(packed, stored) (kernel/apt_call.hpp callPath;
// tools/apt/mods/custom_frontend/game_options_fn.aptasm). It runs the custom front end's ApplyStore with that
// value - the game's own setters, only where the live value differs. APPLY stores first: the host writes
// settings/game_options.ini with the same function an options screen's capture uses (mod_bridge
// modSettingsSavePacked: the game's volumes only while its mixer exists) and passes the stored value, which
// becomes _global.SRSettings as the capture's reply would make it. A preview (stored = -1: the screen's live
// preview of a changed row, or REVERT / closing without APPLY, which sends the stored value back) changes the
// game's live options only. Never a guest call from the render thread; no guest memory write of ours; one call
// per pump at most (the latest value wins).
//
// AUTOSAVE WITHOUT ASKING is a host-owned mod option (settings/mod_options.ini autosave_without_asking, the same
// setting as the front end's and the pause menu's toggles). It is stored on APPLY; the next pump then asks the
// opener to re-read the mod options (its own FetchModOptions), as its toggle's reply does.
//
// Charter: PORT FEATURE (docs/OVERRIDES.json, the custom front end). Off unless port features are on: the rows
// are not listed, no call is made. Kernel-only header (never include from guest/).
#include <cstdint>
#include <string>

struct PPCContext;

namespace sr {
namespace kernel {
namespace gameopts {

// The row ids (pc_settings' GAME tab), in screen order.
constexpr const char* kInvertY = "game_invert_y";
constexpr const char* kAutoCenter = "game_auto_center";
constexpr const char* kVibration = "game_vibration";
constexpr const char* kScheme = "game_control_scheme";
constexpr const char* kMusic = "game_music_volume";
constexpr const char* kSfx = "game_sfx_volume";
constexpr const char* kAutosave = "autosave_without_asking";

// One of the six game rows (not AUTOSAVE).
bool isGameOption(const std::string& id);
bool isAutosave(const std::string& id);

// Are the rows listed at all (port features on)?
bool enabled();
// The game's volumes are readable (its audio mixer exists).
bool volumesAvailable();

// The value a row holds now (the game's live value, no preview): "on" / "off", the scheme "0".."3", a volume
// "0".."10"; AUTOSAVE "on" / "off" from mod_options.ini.
std::string value(const std::string& id);

// The screen's live preview of a game row (no store): the game's options become the shown values.
bool preview(const std::string& id, const std::string& v);
// APPLY of one row: stored (store = true; false = applied, not stored), then the game's own setters.
bool commit(const std::string& id, const std::string& v, bool store, std::string* err);

// THE SCREEN'S CLOSE CALLS THE OPENER BACK. The open request hands the screen to the overlay with SR_DTEnd action 3
// (kernel/display_menu.cpp: nothing is shown under it, so no popup of the opener stays up), and once the overlay is
// gone (closed, or an open request no frame picked up) the next pump calls <opener>.SR_OptionsClosed(what):
//   0 back to the opener's menu (the MAIN MENU, OPTIONS focused / lane E L-c: the pause menu, its OPTIONS row focused);
//   1 VIEW PAD LAYOUT (the CONTROLS tab): the game's own CONTROLLER OPTIONS screen - its pad diagrams for the current
//     scheme, Y = AIR / GROUND CONTROLS - whose own back returns to that menu;
//   2 DEV TOOLS (the GAME tab, DEV BUILDS ONLY: pc_settings lists the row only with SR_ORACLE, so the release
//     composition compiles it out): the Dev Tools pages, back to that menu.
// *msg = the toast. The screen closes itself after either request.
bool requestPadLayout(std::string* msg);
bool requestDevTools(std::string* msg);

// The OPTIONS screen was opened from "FE" (the main menu) or "IG" (the pause menu): the target of the calls.
void noteOpen(const std::string& ctx);
// The game's live options (SR_GameOptionsLive, or an SR_SetSettings capture). volumesReadable = the audio mixer
// existed when the getters ran (else the volume bits are not the game's and are ignored).
void noteLive(int32_t packed, bool volumesReadable);

// Guest thread, right after the Apt executor pump: at most one SR_GameOptions call, one FetchModOptions call and,
// once the screen is gone, its SR_OptionsClosed call.
void afterAptPump(PPCContext& ctx, uint8_t* base);

// ── pure parts (difftest --game-options-test) ─────────────────────────────────────────────────────────────────
// packed with one row's field set to v; -1 when v is not a valid value of that row (or the row is not a game row).
int32_t packWith(int32_t packed, const std::string& id, const std::string& v);
// One row's value in packed ("" for a non-game row).
std::string unpack(int32_t packed, const std::string& id);
// What the next pump would do (and clears it).
struct Pending {
  bool apply = false;
  int32_t packed = -1;   // bits 0-13 + bit 14 (the store's apply_to_loaded_saves) + bit 15 (volumes known)
  int32_t stored = -1;   // the value APPLY stored (the next _global.SRSettings), -1 = a preview
  bool fetchModOptions = false;
  std::string target;    // "_global.SRFrontEnd" | "_global.SRPause"
};
Pending takePendingForTest();
// The SR_OptionsClosed argument due now (and clears it): -1 while the screen is open or no screen was opened.
int takeCloseCallForTest(bool overlayOpen);
void resetForTest();
void setVolumesForTest(int mode);   // -1 read the guest's mixer, 0 absent, 1 present
void setStoreForTest(int32_t packed);   // instead of settings/game_options.ini (-1 = none); INT32_MIN = the file
void setStoreFailForTest(bool fail);    // the in-memory store refuses every write (a locked file)
int selfTest();

}  // namespace gameopts
}  // namespace kernel
}  // namespace sr
