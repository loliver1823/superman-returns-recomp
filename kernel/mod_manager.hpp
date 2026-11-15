#pragma once
// iter-464 modcore: the mod manager (docs/MODS.md). Decides at boot which LAYERS the file
// overlay (mod_overlay.cpp) serves, highest priority first:
//
//   enabled mods     <content root>/mods/<folder>/ or <file>.zip (extracted once into
//                    mods/.cache/zip/), in the player's load order (mods/mods.json; top wins)
//   port features    built-in parts of the product (the custom front end, ...): SRPATCH1
//                    patches in <exe dir>/port_features/ (or the repo's data/port_features/)
//                    applied to the player's own disc into mods/.cache/gen/ (zero game bytes
//                    shipped)
//
// Content is delivered the engine's own way (the owner's mod-system research, not published,
// approach F): per-asset loose files under DATA/_workinprogress/<mod>/ (the loose storage
// MountLocalizedBigFiles_sub_8218D128 mounts at priority 0x7D0, recursive, keyed by the
// file's stem), new flows as root <Flow>.AST, whole-file replacements as before. Two layers
// that provide the same asset stem, or the same file path, are a CONFLICT: the higher layer
// wins and the overlay hides the loser's file from the game.
//
// Automated runs (SR_INSTANCE_ID) mount neither mods nor port features unless asked
// (SR_MODS=a;b, SR_PORT_FEATURES=1), so docs/NATURAL_BASELINE.md stays honest.
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

namespace mapmk { struct Definition; }  // kernel/map_markers.hpp (iter-466 map lane)

enum class ModStatus : int {
  Ok = 0, Disabled, BadManifest, BadZip, WrongGame, MissingDependency, DeclaredConflict,
  BadPatch, BadContent, Duplicate, Reserved, Off,
};
const char* modStatusName(ModStatus s);

// An asset generated from the player's disc: base = a top-level entry of a disc container,
// patched with SRPATCH1 (port features; mods may use it too).
struct ModGenAsset {
  std::string asset;      // disc-relative output path, e.g. DATA/_workinprogress/custom_frontend/SCRPRESSSTART.ast
  std::string baseFile;   // disc-relative container, e.g. DATA/ui.AST
  std::string baseEntry;  // its top-level entry: a name, or #<16 hex> id
  std::string patchPath;  // host path of the .srpatch
};

// Additions, not replacements of the three retail menu entries (or Costume4).
// Stable key = lower-case "<mod id>/<local costume id>". Numeric DAS slots are
// boot-local and MUST NOT be persisted as a character's identity.
struct ModCostumeInfo {
  std::string id, localId, name, source, entity;
  int slot = 0;  // >= 5; the shared composer appends Costume<slot> attributes
  std::string description, thumbnail, fullsize; // optional scoped Apt movie stems (no extension)
  // optional pack previews (host paths of DXT5 .dds, 128x128 / 512x512): the boot wraps them in
  // generated Fortress COSTUMES movies (kernel/mod_costume_preview.hpp); none = fallback card
  std::string thumbImage, fullImage;
  std::string animationProject; // exact case-sensitive project/resource name, own namespace
  // optional own animation banks ("animation_banks": retail SUPERMAN_* bank name -> pack file):
  // written into baseworl.AST's retail slots when this suit is worn at boot; the suits that ship
  // the same bank bytes form one family "banks_<16 hex>" (kernel/mod_costume_data.hpp)
  std::vector<std::pair<std::string, std::string>> banks;
  std::string bankFamily;
  // iter-465 character switch: worn by spawning the entity in Superman's place (an own
  // animation project, or a built-in retail character such as Bizarro); extra BASEWORLDFLOWPRELOADS
  // entries (preload type, file name) it needs resident
  bool character = false;
  std::string characterName;  // iter-465 viewer: mod.json "character" (the COSTUME VIEWER's CHARACTER line)
  std::vector<std::pair<std::string, std::string>> preloads;
};

struct ModCostumeChoice {
  ModCostumeInfo costume;
  bool included = true; // saved per-suit checkbox, independent of its pack's switch
  bool active = false;  // actually present in this boot's immutable registry
};

struct ModInfo {
  std::string id, folder, name, author, version, description;
  std::string category = "other"; // primary browser filter; characters inferred for costume packs
  std::string preview;          // host path of the preview image ("" = none)
  // iter-465 modlib: the library's details view (docs/MODS.md 2.1). Host paths of PNG/JPG
  // screenshots inside the mod (at most 12), and an optional Nexus Mods page of this mod
  // (https://www.nexusmods.com/... only; shown as an explicitly clicked link, never fetched).
  std::vector<std::string> screenshots;
  std::string nexusUrl;
  bool isZip = false, legacy = true;
  std::string hostPath;         // the folder or the zip
  std::string root;             // the content folder (the extracted copy for a zip)
  bool enabled = true;          // the player's saved choice
  bool mounted = false;         // served this boot
  ModStatus status = ModStatus::Ok;
  std::string reason;
  int priority = 0;             // mod.json default-order hint (higher = higher in a new list)
  std::vector<std::string> deps, conflicts;   // mod.json "requires" / "conflicts"
  std::string game;             // mod.json "game" ("" = any)
  std::vector<ModGenAsset> patches;
  std::vector<ModCostumeInfo> costumes;
  // iter-466 map: the pause-map definition (mod.json "map" or map.json; docs/MAP.md 4.2), validated
  std::shared_ptr<const mapmk::Definition> map;
  int conflictCount = 0;
};

struct PortFeatureInfo {
  std::string id, name, description, option, dir;
  bool on = false;              // wanted this boot (player launch / SR_PORT_FEATURES)
  bool applied = false;         // served this boot
  ModStatus status = ModStatus::Off;
  std::string reason;
  std::vector<ModGenAsset> assets;
};

struct ModConflictInfo {
  std::string what;             // "asset SCRPRESSSTART (DATA/_workinprogress/...)" or a file path
  std::string winnerId, loserId, winner, loser;  // ids + display names
};

// One mounted layer, as the overlay serves it (highest priority first).
struct ModLayer {
  std::string id, name;
  bool port = false;
  std::vector<std::string> roots;  // mirror roots (host folders that mirror the disc), first wins
  std::shared_ptr<const mapmk::Definition> map;   // iter-466: its pause-map definition (or none)
};

// The inputs of the boot decision (the environment, or a test's).
struct ModBootEnv {
  std::string contentRoot;        // folder holding "Superman Returns (USA)/" and "mods/"
  std::string exeDir;             // for <exe dir>/port_features
  std::string portDir;            // SR_PORT_FEATURES_DIR (tests / exe snapshots): wins over both
  bool hasSrMods = false; std::string srMods;
  bool hasPort = false; std::string srPortFeatures;
  bool hasCustomFrontend = false; std::string srCustomFrontend;
  bool automated = false;         // SR_INSTANCE_ID set
  bool allowStateWrite = true;    // player launch: migrate enabled.txt, the menu may save
  std::string startupCostume;     // SR_COSTUME stable pack/id: content default, game Lua applies it
  std::string wornCostume;        // <save root>/settings/costume.ini worn= (the Fortress choice)
  std::map<std::string, int> options;  // host options (mod_options.ini) for option-gated features
  // iter-466 density (kernel/mod_density.hpp): game_options.ini traffic_density / population_density,
  // SR_TRAFFIC_DENSITY / SR_POPULATION_DENSITY override; percent, 100 = the retail document.
  int trafficDensity = 100, populationDensity = 100;
};
ModBootEnv modBootEnvFromProcess();

// Boot once (thread-safe; the overlay calls it at the first file open).
void modManagerBoot();
// Re-run the boot decision from explicit inputs (difftest).
void modManagerResetForTest(const ModBootEnv& env);

const std::vector<ModLayer>& modActiveLayers();     // empty = nothing mounted (natural path)
bool modHiddenHostPath(const std::string& hostPath);   // a conflict loser the overlay hides
std::string modBootSummary();                       // "mods: a, b; port: custom_frontend"

// ---- the menu API (docs/MODS.md section 9) ----
std::vector<ModInfo> modMenuList();                 // load order, top = highest priority
std::vector<PortFeatureInfo> modMenuFeatures();
std::vector<ModConflictInfo> modMenuConflicts();
std::vector<ModCostumeInfo> modCostumeList();  // mounted packs only, stable id order
// The Fortress COSTUMES choice: "retail/<1..3>" or an active "<mod>/<costume>"; saved to
// <save root>/settings/costume.ini and worn again at the next boot (SRWornCostume).
bool modCostumeNoteWorn(const std::string& id);
std::string modCostumeWorn();
int modCostumeRetailSlot(const std::string& id);  // retail/<n> -> n (1..3), else 0
bool modCostumePreviewsReady();  // generated Fortress COSTUMES movies mounted this boot
// The animation family this session loaded at boot ("retail" or a suit's "banks_<hash>"): only a
// suit of this family can be put on now (kernel/mod_costume_data.hpp modCostumeFamily).
std::string modCostumeSessionFamily();
bool modNativeCostumesEnabled(); // rollout gate SR_NATIVE_COSTUMES, default off until runtime proof
bool modCharacterSwitchEnabled(); // SR_CHARACTER_SWITCH (unset = modNativeCostumesEnabled)
uint64_t modCharacterAnimBytes(); // own-project character banks preloaded this boot (0 = none)
bool modCostumeFindActive(const std::string& id, ModCostumeInfo* out);
std::vector<ModCostumeChoice> modCostumePackChoices(const std::string& modId);
bool modCostumeSetIncluded(const std::string& id, bool included); // saves stable id; apply at next boot
bool modCostumeRegistryReady();  // combined attribute document generated successfully
std::string modCostumeSelected(); // saved stable id; empty = retail default, not current visual proof
bool modCostumeSelectForRestart(const std::string& id); // saves selection only; game applies content at next boot
std::string modAssetPrefix(const std::string& modId);  // SRM_<FNV64>_, globally scoped stems
std::string modCategoryLabel(const std::string& category); // browser display label (unknown -> Other)
bool modMenuSetEnabled(const std::string& id, bool on);   // saves mods.json
bool modMenuMove(const std::string& id, int delta);       // -1 = up (more priority)
bool modMenuRestartRequired();
bool modMenuAutomated();
std::string modMenuModsDir();  // UTF-8, the exact folder this manager loaded
bool modMenuRestart(std::string* whyNot);          // relaunch the exe, then quit (SR_Quit path)
void modMenuSetRestartHookForTest(bool (*spawn)(std::string* err));

// The FSCommand layer: one SR_Mod* command -> the FlowMgr.<method>(args) calls to deliver.
struct ModMenuArg {
  bool isString = false;
  int32_t i = 0;
  std::string s;
  static ModMenuArg Int(int32_t v) { ModMenuArg a; a.i = v; return a; }
  static ModMenuArg Str(std::string v) { ModMenuArg a; a.isString = true; a.s = std::move(v); return a; }
};
struct ModMenuCall {
  std::string method;
  std::vector<ModMenuArg> args;  // at most 3
};
// False when `cmd` is not an SR_Mod* command (the caller handles it).
bool modMenuCommand(const std::string& cmd, const std::string& args, std::vector<ModMenuCall>* out);

// ---- helpers shared with the overlay and the tests ----
// A path as UTF-8 with '/' separators (std::filesystem::path::generic_u8string is char8_t in C++20).
std::string modPathU8(const std::filesystem::path& p);
// iter-465 viewer: the player's disc folder this manager booted with ("<content root>/Superman Returns (USA)").
std::string modDiscRoot();
std::string modLower(std::string s);
bool modValidId(const std::string& n);
// The data fingerprint (docs/MODS.md 2.1): "SRX360-USA:<16 hex>".
std::string modDataFingerprint(const std::string& discRoot);
// Reads one top-level entry of a disc BGFA container (by name, or #<16 hex> id).
bool modReadTopEntry(const std::string& containerPath, const std::string& entry, std::vector<uint8_t>* out,
                     std::string* err);
// The 29 loose-asset extensions the _workinprogress storage registers (lower case).
bool modLooseExtensionRegistered(const std::string& extLower);

}  // namespace kernel
}  // namespace sr
