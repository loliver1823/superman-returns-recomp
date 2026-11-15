#pragma once
// iter-463 MOD FRAMEWORK: kernel file-layer asset overlay (docs/MODS.md,
// docs/CUSTOM_FRONTEND.md 7.1).
//
// A mod is a folder <content root>/mods/<name>/ that mirrors the disc tree
// ("Superman Returns (USA)/"). While a mod is ENABLED, a file at
// mods/<name>/<rel> shadows (or adds) the disc file <rel> for NtCreateFile /
// NtOpenFile (opened read-only), and therefore for NtReadFile and
// NtQueryInformationFile, which read the opened FILE*; NtQueryDirectoryFile
// lists the union (the shadowing file's size, disc order kept, mod-only names
// appended). The game folder is never written.
//
// Enabling (read ONCE, at the first file open):
//   <content root>/mods/enabled.txt  one mod folder name per line, '#' starts a
//                                    comment. EARLIER lines win a name clash.
//   SR_MODS=a;b                      replaces the file's list (empty or 0 = none)
//   SR_CUSTOM_FRONTEND=1 / =0        puts custom_frontend first / removes it
// An automated roll (SR_INSTANCE_ID set) ignores enabled.txt; every other
// automated launcher (and difftest's main) sets SR_MODS=0 unless the caller set
// it (docs/MODS.md "Automated runs"). Mods reach test runs only through SR_MODS /
// SR_CUSTOM_FRONTEND.
// With nothing enabled every call below returns at once: the file layer is
// byte-for-byte the natural path (no extra stat or probe per open).
//
// Charter: a CONTENT MOD mechanism, opt-in. It serves files the user placed;
// it calls no guest function and writes no guest state. Runs with a mod enabled
// are tagged mod:<name> and never count toward docs/NATURAL_BASELINE.md.
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <string>
#include <vector>

namespace sr {
namespace kernel {

struct ModBootEnv;

// Built-in product framework. SR_MOD_MANAGER=0 is the legacy-path diagnostic opt-out.
// Mod content and port features still follow their separate boot policy.
bool modManagerOverlayEnabled();

// True when at least one enabled mod folder exists (initialises on first call).
bool modOverlayActive();

// Enabled mods, highest priority first (empty when none).
const std::vector<std::string>& modOverlayEnabled();

// True when `name` is enabled AND its folder exists (for mod-owned host
// features, e.g. the custom_frontend FSCommand bridge).
bool modEnabled(const char* name);

// hostDiscPath = resolveGuestPath() output ("<root>Superman Returns (USA)/<rel>").
// Returns the host path of the highest-priority mod's regular file for <rel>,
// or "" (no mod provides it, the path is outside the disc tree, or no mod is
// active). Logs "[MOD] overlay ..." once per distinct path.
std::string modOverlayResolveFile(const std::string& hostDiscPath);

// True when some enabled mod has a directory at the disc path's <rel>.
bool modOverlayHasDir(const std::string& hostDiscPath);

// The host path of the highest-priority enabled mod's directory at the disc path's <rel>, or "" (none). The attribute
// query (kernel/xam_content.cpp NtQueryFullAttributesFile) stats it for a folder only a mod provides.
std::string modOverlayResolveDir(const std::string& hostDiscPath);

// One directory entry for NtQueryDirectoryFile.
struct ModDirEntry {
  std::string name;       // as listed to the guest
  std::string hostPath;   // the file that answers for it (mod or disc)
  unsigned long long size = 0;
  bool isDir = false;
};

// Union listing: `disc` is the disc directory's listing (kept in order); an
// entry some mod shadows takes that mod's size/path (the disc name is kept);
// names only mods provide are appended (highest-priority mod first, each mod's
// names sorted case-insensitively). No-op when no mod is active.
void modOverlayMergeDir(const std::string& hostDiscDir, std::vector<ModDirEntry>& disc);

// mods/enabled.txt grammar: one name per line, '#' to end of line is a comment,
// blanks and surrounding spaces ignored, a UTF-8 BOM tolerated, duplicates
// (case-insensitive) dropped, order kept (earlier = higher priority).
std::vector<std::string> modParseEnabledList(const std::string& text);

// The boot-time decision, as a pure function of its inputs (the env values may
// be null): SR_MODS replaces the list; else <root>/mods/enabled.txt unless
// srInstanceId is non-empty (automated roll) - SR_MODS=@enabled.txt reads the
// file anyway (a roll testing the player's switch); then SR_CUSTOM_FRONTEND=1 puts
// custom_frontend first / =0 removes it. *sourceOut names where it came from.
// iter-464 modmenu: SR_MODS=@<path> reads that list file instead (same grammar; a roll's own
// copy). *listFileOut (optional) = the list file the player's switch uses for this boot
// (mods/enabled.txt for a player launch, or the @ file), "" when the list is not a file
// (SR_MODS=a;b, or an automated roll without @).
std::vector<std::string> modComputeEnabledList(const std::string& contentRoot, const char* srMods,
                                               const char* srCustomFrontend, const char* srInstanceId,
                                               std::string* sourceOut, std::string* listFileOut = nullptr);

// iter-464 modmenu (kernel/mod_menu.cpp reads these; they never change after boot):
bool modValidName(const std::string& n);       // the plain-folder-name rule
std::string modOverlayModsDir();               // <content root>/mods ('/' separators; "" = no root)
std::string modOverlayListFile();              // the boot's list file ("" = the list was not a file)
std::string modOverlaySource();                // where the boot list came from (the [MOD] log source)

// Test seam (difftest --mod-overlay-test): re-initialise from an explicit
// content root and list, bypassing enabled.txt and the environment.
void modOverlayResetForTest(const std::string& contentRoot, const std::vector<std::string>& mods);
void modOverlayResetForTestEnv(const ModBootEnv& env);
void modOverlaySetListFileForTest(const std::string& listFile);

}  // namespace kernel
}  // namespace sr
