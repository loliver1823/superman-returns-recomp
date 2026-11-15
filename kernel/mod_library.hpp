#pragma once
// iter-465 modlib lane: THE MOD LIBRARY - the in-game mod organiser (docs/MODS.md "The mod
// library"). USER 2026-10-03: "i asked him to create a proper frontend for mod selection taking
// inspiration of the look from other in game mod organisers like bethesda, and all he did was
// make a list."
//
// A full-screen HOST OVERLAY drawn by our D3D12 backend after the game's frame (gfx/
// mod_library_ui.cpp, Dear ImGui as the drawing layer): LIBRARY (category rail, search, a grid of
// cards with thumbnails and badges, a details view with screenshots, conflicts and per-suit
// checkboxes), LOAD ORDER (drag / move, enable checkboxes, conflict markers) and GET MORE MODS
// (an explicitly clicked Nexus Mods link and the mods folder). It replaces the Apt list pages
// (kernel/mod_menu.cpp) behind the same entries: MAIN MENU > MODS (lane E L-c: no longer from the pause menu).
//
// This file is the kernel half: open / close, input routing and the data model. Everything the
// library shows and changes goes through the mod manager (kernel/mod_manager.hpp): enable /
// disable, load order and per-suit inclusion persist in mods/mods.json, and a change applies at
// the next start (assets mount at boot), which the RESTART banner says.
//
// INPUT while it is open: the overlay owns the keyboard, the mouse and the pad. The game reads a
// neutral pad (kernel/stubs.cpp XamInputGetState) - what a 360 title saw while the system Guide
// was up - and, after the close, until every button and key is released, so the B / Esc that
// closed the library never reaches the menu underneath.
//
// Charter: a PORT FEATURE (player-facing host UI). It reads no guest memory, writes none and calls
// no guest code; the only guest-visible effect is the neutral pad while it is open. Automated runs
// reach it only through a port-feature roll (SR_PORT_FEATURES=1, the custom front end's MODS row).
// SR_MOD_LIBRARY=0 restores the retired Apt list pages (developer A/B only).
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>
#include <string>
#include <vector>

union SDL_Event;

namespace sr {
namespace kernel {
namespace modlib {

// ── the data the library shows: a snapshot of the mod manager ─────────────────────────────
struct LibConflict {
  std::string what;        // the asset / file both change
  std::string otherId, otherName;
  bool wins = false;       // this mod's version is the one the game gets
};

struct LibSuit {
  std::string id;          // stable "<mod id>/<suit id>" (mods.json disabled_costumes key)
  std::string name, description;
  bool included = true;    // the saved checkbox
  bool active = false;     // registered in this boot
};

struct LibEntry {
  std::string id, name, author, version, description;
  std::string category;    // characters missions worlds gameplay visuals audio interface other builtin
  std::string preview;     // host path of the preview image ("" = generated card art)
  std::vector<std::string> screenshots;
  std::string nexusUrl;    // this mod's Nexus page ("" = none)
  std::string root;        // the mod's content folder (size)
  bool builtin = false;    // a port feature (always loaded under the mods)
  bool hasToggle = true;   // false: a built-in without an option (always on)
  std::string optionKey;   // built-in: the mod_options.ini key its toggle writes
  bool enabled = false;    // the saved choice (built-in: its option, or true)
  bool mounted = false;    // active in this boot
  bool failed = false;     // the manager refused it (status / reason say why)
  std::string status, reason;
  bool needsRestart = false;  // the saved choice differs from this boot
  bool isZip = false, legacy = false;
  int order = -1;          // load-order position (0 = top, wins); -1 for built-ins
  std::vector<LibConflict> conflicts;
  std::vector<LibSuit> suits;
};

struct LibSnapshot {
  std::vector<LibEntry> mods;   // load order (top wins), then the built-in features
  int installed = 0;            // mods (not built-ins)
  int enabledCount = 0;         // mods switched on (saved)
  bool restartRequired = false; // saved state != what this boot mounted
  bool canSave = true;          // false: this run never writes mods.json (automated roll)
  bool canRestart = true;
  bool automated = false;
  std::string modsDir;
};

// Builds the snapshot from the mod manager (thread-safe; touches no guest state).
LibSnapshot snapshot();

// ── actions (each saves through the mod manager; false + *msg when refused) ───────────────
bool setEnabled(const std::string& id, bool on, std::string* msg);
bool move(const std::string& id, int delta, std::string* msg);          // -1 = up (wins more)
bool setSuitIncluded(const std::string& suitId, bool on, std::string* msg);
bool setBuiltinOption(const std::string& key, bool on, std::string* msg);
bool restartNow(std::string* msg);             // relaunch (the MODS menu's RESTART NOW path)
bool openUrl(const std::string& url, std::string* msg);   // an explicitly clicked Nexus link
bool openModsFolder(std::string* msg);
// The base Nexus Mods site the GET MORE MODS tab links to (no invented game page).
const char* nexusHomeUrl();

// ── pure helpers (unit-tested: difftest --mod-library-test) ───────────────────────────────
struct Category { const char* key; const char* label; };
// All, Characters, Missions, Worlds, Gameplay, Visuals, Audio, Interface, Other, Built-in.
const std::vector<Category>& categories();
// Does `e` belong in category `key` ("all" holds everything) and match `search` (case-
// insensitive substring of name, author, id or description; "" matches all)?
bool matches(const LibEntry& e, const std::string& key, const std::string& search);
// The indices into s.mods the library grid shows for (category, search), in display order:
// the load order, then the built-ins (stable: a toggle never moves a card).
std::vector<int> filter(const LibSnapshot& s, const std::string& key, const std::string& search);
int countIn(const LibSnapshot& s, const std::string& key);
// Grid navigation: from `index` in a grid of `count` items, `cols` wide, move by (dx, dy).
// Clamps at the edges (no wrap); moving down from the last row's gap lands on the last item.
// Returns the new index, or -1 when the move leaves the grid on that side (dx<0 at column 0 =
// "into the category rail", dy<0 at row 0 = "into the header").
int gridMove(int index, int count, int cols, int dx, int dy);
// "12.4 MB" style size label.
std::string sizeLabel(uint64_t bytes);
// Upper-cases ASCII (the library's display fonts are caps-styled).
std::string upper(std::string s);
// The display name of a plain-folder mod (no mod.json: the manager names it by its folder):
// "sample_mission" -> "Sample Mission" (separators to spaces, each word capitalised).
std::string folderTitle(const std::string& folder);
// Wall-clock-free self test of the helpers; returns failures (0 = pass).
int selfTest();

// ── open / close and input routing ────────────────────────────────────────────────────────
bool enabled();                        // SR_MOD_LIBRARY (default ON; =0: the Apt list pages)
// The SR_Menu "open mods FE|IG" request (guest thread, inside the Apt interpreter).
void requestOpen(const std::string& ctx);
// iter-466 gfx: the same routing (one host overlay at a time, the neutral pad, keyboard / mouse
// queue, release latch) serves the PC SETTINGS screen (gfx/settings_ui.cpp). An open request names
// its page; each overlay takes only its own page's requests.
// lane E L-a: Achievements = the pause menu's ACHIEVEMENTS page (gfx/achievements_ui.hpp; drawn through the OPTIONS
// screen's slot on the present path).
enum class OverlayPage : uint8_t { Mods = 0, Settings = 1, Achievements = 2 };
void requestOpenPage(OverlayPage page, const std::string& ctx);
OverlayPage openPage();
// Render thread: take a pending open request (true once per request).
bool takeOpenRequest(std::string* ctx);
// Render thread: the overlay is shown / its close animation finished.
void setShown(bool shown);
bool isOpen();                         // shown (incl. the close animation)
// XamInputGetState (poll thread): true = the game must read a neutral pad now. `gameInputHeld`
// = any button / stick / trigger / key the merged report holds this poll (the release latch).
bool blockGameInput(bool gameInputHeld);

// Main thread (app/platform_sdl.cpp pumpEvents): every SDL event; keyboard + text only, and
// only while open and where real input is allowed (players; never a background roll).
void onSdlEvent(const SDL_Event& e);
// Mouse (kernel/mouse_ui.cpp, real mouse or the harness feed), in STAGE coords (1280x720 over
// the game's 16:9 image). kind: 1 move, 2 down, 3 up, 4 wheel (n in `wheel`).
// button: 1 left, 2 middle, 3 right.
void onMouse(int kind, float sx, float sy, int button, int wheel);
bool wantsMouse();                     // the overlay takes the mouse (open)
// The cursor overlay's hover ring (set by the UI each frame).
void setMouseHover(bool hover);
bool mouseHover();

struct InputEvent {
  enum Kind : uint8_t { Key = 0, Text, MouseMove, MouseDown, MouseUp, Wheel };
  Kind kind = Key;
  int scancode = 0;                    // Key: SDL scancode
  bool down = false, repeat = false;
  uint16_t mods = 0;                   // Key: SDL KMOD_* bits
  char text[16] = {0};                 // Text: UTF-8
  float x = 0, y = 0;                  // mouse: stage coords
  int button = 0, wheel = 0;
};
// Render thread: everything queued since the last call.
std::vector<InputEvent> drainInput();
// Test seam (difftest --mod-library-render): queue an event as if a device produced it.
void injectForTest(const InputEvent& e);

// Developer aids (never on for players):
//   SR_MOD_LIBRARY_AUTOOPEN=<sec>  open the library <sec> s after the first present (look checks)
//   SR_MOD_LIBRARY_SHOTS=<dir>     F12 (real or harness keyboard) writes the composited frame,
//                                  overlay included, to <dir>/modlib_<n>_<w>x<h>.bmp
double autoOpenSeconds();
const std::string& shotDir();

}  // namespace modlib
}  // namespace kernel
}  // namespace sr
