#pragma once
// iter-464 (modmenu lane): the in-game MODS menu - the HOST half (docs/MODS.md "The MODS menu",
// docs/CUSTOM_FRONTEND.md 5.8). USER 2026-10-02: "we're going to need a mod menu".
//
// The pages are real game popups, drawn by the same Apt renderer as the Dev Tools pages
// (host_pages.hpp): MAIN MENU -> MODS (custom front end; lane E L-c: the pause menu has no MODS row).
//
//   mods[:<off>]          the installed mods, in load order (3 per page): "<NAME>: ON|OFF",
//                         a " - CONFLICT" / " - FAILED" badge; A = details, LEFT/RIGHT = on/off;
//                         MORE MODS; the last row is RESTART TO APPLY (pending changes) or
//                         HOW TO INSTALL MODS. No mod installed: the empty-state page instead.
//   mod:<id>              details: version, author, description, status + reason; rows
//                         ENABLED (toggle), LOAD ORDER n OF m (LEFT = earlier, RIGHT = later),
//                         WHAT IT CHANGES, CONFLICTS (when any), OPTIONS (when registered)
//   modfiles:<id>:<off>   what it changes (replaces / adds, per target)
//   modconf:<id>:<off>    its conflicts: the target, the other mod, which one wins
//   modopts:<id>:<off>    per-mod options (EXTENSION POINT: modMenuRegisterOption)
//   modhow                install help, the real mods folder path and OPEN MODS FOLDER
//   modfoldererror        a visible fallback when Explorer could not open that folder
//   modrestart            "restart now?" -> RESTART NOW: the exe relaunches itself
//   costumes[:<off>]      the COSTUME PICKER slot (EXTENSION POINT: modMenuRegisterCostume;
//                         extra skins land later). No Apt entry until a costume exists.
//
// DATA comes from a ModCatalog: what the menu needs from the mod system. The modcore lane
// (branch mods/ship) owns manifests, the state file, the entry-level merge and conflict
// detection; it installs its catalog with modMenuSetCatalog(). Until then the built-in
// FALLBACK catalog serves what the overlay has today (kernel/mod_overlay.cpp): the folders in
// mods/, the boot list file (mods/enabled.txt, or a roll's SR_MODS=@<file>; a line "# <name>"
// keeps a switched-off mod's place in the load order), an optional flat mods/<id>/mod.json
// ({"name","author","version","description","port_feature"}) and FILE-level conflicts (two
// layers shadowing the same disc path: before the entry-level merge, the second loses the file).
// Port features (mod.json "port_feature": true, e.g. the custom front end built into a mod
// folder) are never listed (USER 2026-10-02: no toggle) but are named in conflicts.
//
// Charter: a PORT FEATURE (player-facing host UI). Nothing here reads or writes guest memory or
// calls guest code. It writes the player's list file (mods/enabled.txt) and, on RESTART NOW,
// starts a new process of the same exe and quits through the SR_Quit path. Changes apply at
// the next launch only: the overlay is read once at boot.
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include "devtools_menu.hpp"
#include "host_pages.hpp"
#include "mod_bridge.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

enum class ModMenuState : int { Ok = 0, Conflict = 1, Failed = 2 };

struct ModMenuChange {
  std::string target;     // "DATA/text.AST" (fallback) or "DATA/text.AST:INTERFACETEXT" (entry level)
  bool adds = false;      // the disc has no such target
};

struct ModMenuConflict {
  std::string target;     // what both change
  std::string other;      // the other layer's id ("custom_frontend", "custom_frontend/@autosave...")
  std::string otherName;  // its display name ("AUTOSAVE WITHOUT ASKING (PORT FEATURE)")
  bool wins = false;      // this mod's version is the one the game gets
};

struct ModMenuEntry {
  std::string id;           // the folder name (stable key)
  std::string name, author, version, description;
  std::string category = "Other", preview; // details/card model; local image, not a remote URL
  bool enabled = false;     // in the saved state: applies at the next launch
  bool active = false;      // mounted in this process
  ModMenuState state = ModMenuState::Ok;
  std::string reason;       // one line for Conflict / Failed
  std::vector<ModMenuChange> changes;
  std::vector<ModMenuConflict> conflicts;
};

struct ModMenuView {
  std::vector<ModMenuEntry> mods;     // load order: earlier = higher priority (wins a conflict)
  std::string modsDir;           // shown on the install page (native separators)
  bool canSave = true;           // false: this run's list is fixed (why), changes last for the session
  std::string why;
  bool pendingRestart = false;   // the saved state differs from what this process mounted
  bool acceptsZip = false;       // the loader unpacks a dropped .zip (modcore); the help text says so
  bool autoEnableNew = false;    // newly installed mods default to ON
  bool canRescan = true;         // CHECK AGAIN really discovers additions without remounting
};

struct CostumeDef {
  std::string id, label, source;
  std::function<bool()> available, current, select;
  bool requiresRestart = false; // content selection; never queue a host-side guest state write
};

class ModCatalog {
 public:
  virtual ~ModCatalog() = default;
  virtual ModMenuView view() = 0;
  // Switch a mod on/off in the saved state (applies at the next launch).
  virtual bool setEnabled(const std::string& id, bool on, std::string* err) = 0;
  // Move a mod in the load order: delta -1 = one place earlier (higher priority), +1 = later.
  virtual bool move(const std::string& id, int delta, std::string* err) = 0;
  // Re-read the mods folder (a mod dropped in while the game runs).
  virtual void rescan() {}
  virtual std::vector<CostumeDef> costumes() { return {}; }
  // The mod list a relaunched process must mount when the state cannot be saved (canSave
  // false): it goes into the child's SR_MODS. Empty = let the child read the list file.
  virtual std::string sessionListForRestart() { return {}; }
};

// modcore's hook: install the real catalog (nullptr = the built-in fallback).
void modMenuSetCatalog(std::shared_ptr<ModCatalog> c);
std::shared_ptr<ModCatalog> modMenuCatalog();
// The built-in fallback over today's overlay. modsDir = <content root>/mods; listFile = the
// boot's list file ("" = not a file: changes stay in memory); bootList = what this process
// mounted (highest priority first). Exposed for tests.
std::shared_ptr<ModCatalog> modMenuMakeFallbackCatalog(const std::string& modsDir, const std::string& listFile,
                                                       const std::vector<std::string>& bootList);

// The adapter over modcore's mod manager (kernel/mod_menu_modcore.cpp): compiled in only when
// kernel/mod_manager.hpp is in the tree (__has_include); nullptr otherwise (the fallback serves).
std::shared_ptr<ModCatalog> modMenuMakeModcoreCatalog();
// The disc-tree files a mod folder provides (shared by both catalogs; see mod_menu.cpp).
std::vector<std::string> modMenuWalkGameFiles(const std::string& dir, const std::string& discRoot);

// The router's question (host_pages.cpp): pages starting "mod" or "costume" are ours.
bool modMenuOwnsPage(const std::string& page);
// One SR_Menu command for a MODS page; appends the AS calls to *out.
void modMenuPagesCommand(const std::string& args, std::vector<ModReply>* out);

// EXTENSION POINT - per-mod option pages: a row on the mod's OPTIONS page (the same row kinds
// as the Dev Tools rows; def.page is ignored, def.tool must be null - a mod option is not a
// dev tool). The details page shows OPTIONS once a mod has a row. modcore can register rows
// from a manifest's option list through this call.
void modMenuRegisterOption(const std::string& modId, const DevMenuItemDef& def);

// EXTENSION POINT - the COSTUME PICKER (pending user request: extra Superman skins + Zod,
// selectable from the front end and/or pause). A costume provider (a skin pack's loader)
// registers each costume. Providers must use content defaults or the game's own
// AS/native API, never host-tick guest writes/calls. Managed packs currently save
// a stable selection for restart; actor Lua consumes SetCostumeNum after load.
void modMenuRegisterCostume(const CostumeDef& def);
int modMenuCostumeCount();

// RESTART NOW: set by the menu; the exit path (app/main.cpp) relaunches the exe after the log
// is flushed. The child gets the same command line, working folder and environment, plus
// SR_RESTART_GENERATION=<n+1> (an automated roll's child logs to superman_returns_<id>_r<n>.log)
// and, when the catalog cannot save, SR_MODS=<the session list>.
bool modMenuRestartRequested();
// The request itself (the RESTART NOW row calls it; modcore's modMenuRestart() can too): flags the
// relaunch and pushes SDL_EVENT_QUIT, the SR_Quit / window-close exit path.
void modMenuRequestRestart();
// Called by both exit paths right before the process ends; no-op unless requested. Logs
// "[MODMENU] restart -> spawned pid N" (or the failure).
void modMenuSpawnRestartIfRequested();

// Test seams (difftest --mod-overlay-test).
void modMenuResetForTest();
void modMenuSetRestartHandlerForTest(void (*fn)());
void modMenuSetFolderHandlerForTest(bool (*fn)(const std::string& path));

}  // namespace kernel
}  // namespace sr
