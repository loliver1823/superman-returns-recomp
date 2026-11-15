#pragma once
// iter-463 MOD FRAMEWORK: the Apt FSCommand -> host bridge (docs/CUSTOM_FRONTEND.md 9,
// docs/MODS.md). Kernel-only header (never include from guest/, never add to
// guest_context.hpp).
//
// AS2 fscommand(cmd, args) runs ActionGetURL / ActionGetURL2 with "FSCommand:<cmd>";
// AptAS2OpGetURL_sub_826BADE0 / AptAS2ActionGetURL2LoadMovie_sub_826BB0F0 ->
// sub_82682DF8 -> the handler pointer *0x829A1A90 = sub_826D39A0(cmd, args), which looks
// the command up in the Apt named registry *0x829A23E0 and calls entry->vt[3](args)
// with NO null check (an unknown command is a fatal host AV). Retail content issues 0
// FSCommands.
//
// kernel/mod_bridge.cpp strong-defines sub_826D39A0. Commands starting with "SR_" are
// handled on the host and never reach the guest registry, whichever mod (or hand-placed
// ui.AST) issued them - the guest registry would miss and null-deref; every other
// command runs the game's own body unchanged. Not tied to a mod name: any Apt mod may
// use the bridge (iter-463 review: a renamed custom_frontend crashed at the title).
//
//   SR_Quit        quit the process the way closing the window does (SDL_EVENT_QUIT ->
//                  PlatformSDL::pumpEvents() false -> SDL_main returns 0)
//   SR_Log <text>  one [SR_FSCMD] log line (mod diagnostics); also prints the save
//                  flow's pending popup id (*(*0x8296EE8C + 16)) read fault-free
//   SR_SetSettings [<tag> ]<packed>   persist the universal game options (below); replies
//                  with the value as stored (a failed file write: the same value, kept for
//                  the session; an invalid value: no reply)
//   SR_GetSettings            reply with the stored options (-1 = no or incomplete store)
//   Replies: the host calls the AS method FlowMgr.SR_OnHostSettings(packed) through the
//   AptExecutor (vt+0x20, target path "FlowMgr", as sub_822B15F8 calls
//   FlowMgr.CppShowPopup); the mod defines that method in the movie that sends the command
//   SR_<other>     logged and ignored
//
// The universal options store (docs/CUSTOM_FRONTEND.md section 0): one text file,
// <content root>/settings/game_options.ini (the xam_content root, next to the saves;
// an automated roll's root is fresh per process, so a roll sees only its SR_CONTENT_DIR
// seed). The AS side packs the game's own SettingsAdapter values into one integer:
//   bits 0-3 music volume 0..10 (GetMusicVol)    bits 4-7 SFX volume 0..10 (GetSFXVol)
//   bits 8-9 control scheme 0..3                 bit 10 X inversion   bit 11 Y inversion
//   bit 12 auto-center                           bit 13 vibration (GetRumble)
//   bit 14 apply_to_loaded_saves (host-owned mod option, default 1: re-apply the store
//          after a save's own options were restored; 0 = console behaviour)
//   bit 15 volumes known (host-owned): the volumes are only taken while the game's audio
//          mixer exists (dword_82976054 / dword_82976070; without it GetMusicVol /
//          GetSFXVol return a cached 0), otherwise the file's volumes are kept (or stay
//          absent = unknown, and ApplyStore leaves the game's volumes alone)
//
// iter-464 (fe lane: "Autosave without asking" + the in-game Dev Tools menu):
//   SR_GetModOptions          reply FlowMgr.SR_OnHostModOptions(packed mod options, below)
//   SR_SetModOption <key> <v> store one mod option (settings/mod_options.ini); same reply
//   SR_SaveEvent <kind>       the current-save-slot tracker (new|load|save|overwrite|query);
//                             reply FlowMgr.SR_OnHostSlot(slot 0..2, or -1 = none yet)
//   SR_Map <verb> ...        iter-466: the useful pause map (kernel/map_markers.hpp, docs/MAP.md):
//                             open / tick <sx> <sy> / filter <type> 0|1 / select <id> / close;
//                             replies FlowMgr.SR_MapBegin / SR_MapType / SR_MapMk / SR_MapInfo /
//                             SR_MapEnd / SR_MapXY (read-only: no guest call, no guest write)
//   SR_Menu <verb> <page> ... the host-driven pages (kernel/host_pages.hpp): the MODS menu
//                             (pages "mod*" / "costume*", kernel/mod_menu.cpp) and the Dev Tools
//                             menu (every other page, kernel/devtools_menu.cpp); replies
//                             FlowMgr.SR_DTBegin / SR_DTItem (per row) / SR_DTEnd. SR_DevMenu is
//                             the older name of the same command.
// A command may now answer with several AS calls, each with up to 3 int or string
// arguments (strings become Apt strings through AptObjectRecycleOrAlloc_sub_8269A210, the
// call sub_822B15F8 makes for popup titles; it copies the text).
//
// The mod options store: <save root>/settings/mod_options.ini (beside game_options.ini).
// Host-owned options the mod UI changes; packed for AS as:
//   bit 0 autosave_without_asking (default 1)   bit 1 devtools_notice_ack (default 0)
//   bit 8 (runtime) dev tools are on in this session (a devtool-class tool is enabled)
//   bit 9 (boot) the option folder mods/<mod>/@autosave_without_asking was mounted
//   bit 10 (runtime) a level skip is armed or running: the autosave answers the save prompt
//          with the plan's own answer, Continue Without Saving (no dev-tool save in a slot)
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct PPCContext;

namespace sr {
namespace kernel {

enum class ModBridgeResult { NotOurs = 0, Handled = 1, Quit = 2, ReplyInt = 3 };

// One argument of a host -> AS call.
struct ModArg {
  bool isString = false;
  int32_t i = 0;
  std::string s;
  static ModArg Int(int32_t v) { ModArg a; a.i = v; return a; }
  static ModArg Str(std::string v) { ModArg a; a.isString = true; a.s = std::move(v); return a; }
};
// One host -> AS call: FlowMgr.<method>(args...) (at most 3 arguments).
struct ModReply {
  std::string method;
  std::vector<ModArg> args;
};

// The host side of one command. SR_CostumeWear additionally needs the current
// interpreter context: it requests the engine's own SetCostumeNum setter on that
// guest thread. No context = a refused request, never a deferred host tick.
// NotOurs = the caller must run the game's own handler. Every call in
// *replies (in order) must be delivered to the AS side by the caller.
ModBridgeResult modBridgeDispatchEx(const std::string& cmd, const std::string& args,
                                    std::vector<ModReply>* replies, PPCContext* ctx = nullptr);
// The original interface (difftest M9/M10): ReplyInt + *reply = the first reply's first
// int argument when the command answered with a call.
ModBridgeResult modBridgeDispatch(const std::string& cmd, const std::string& args,
                                  int32_t* reply = nullptr);

// Test seam (difftest --mod-overlay-test): replace the SDL_EVENT_QUIT push; nullptr restores it.
void modBridgeSetQuitHandlerForTest(void (*fn)());

// The reply (called by the sub_826D39A0 strong def on the guest thread):
// FlowMgr.<method>(value) via AptExecutor vt+0x20. Logs and does nothing when the executor
// or the dispatch table is not set up (e.g. difftest).
void modBridgeReplyToFlowMgr(PPCContext& ctx, uint8_t* base, const char* method,
                             int32_t value);

// The universal options store.
constexpr int32_t kModSettingsNone = -1;
constexpr uint32_t kModSettingsApplyToLoadedBit = 1u << 14;
constexpr uint32_t kModSettingsVolumesKnownBit = 1u << 15;
std::filesystem::path modSettingsPath();
// -1 when the file is missing or a non-volume value is missing / out of range; bit 15 set
// when both volumes are present.
int32_t modSettingsLoadPacked();
// Validates (music/sfx 0..10, scheme 0..3), keeps the file's apply_to_loaded_saves
// (default 1) and, when !volumesReadable, the file's volumes (or none), then writes the
// file atomically. False on invalid input or an I/O error. *canonical (optional) = the
// packed value the file holds once written (bits 14/15 included), set whenever the input
// is valid - also when the write then fails; -1 for invalid input.
bool modSettingsSavePacked(int32_t packed, bool volumesReadable = true,
                           int32_t* canonical = nullptr);
// Test seam: -1 read the guest's audio mixer globals, 0 absent, 1 present.
void modBridgeSetAudioMixerForTest(int mode);

// iter-465 CHEATS page: the state of one of the game's own cheats (kernel/mod_bridge.cpp, fault-free
// reads): 1 on, 0 off, -1 unknown.
int modCheatState(int code);

// Deliver several calls (the strong def does this after modBridgeDispatchEx).
void modBridgeDeliver(PPCContext& ctx, uint8_t* base, const std::vector<ModReply>& replies);

// ---- the mod options store (settings/mod_options.ini) ----
constexpr uint32_t kModOptAutosaveBit = 1u << 0;
constexpr uint32_t kModOptNoticeAckBit = 1u << 1;
constexpr uint32_t kModOptDevtoolsOnBit = 1u << 8;
constexpr uint32_t kModOptAutosaveMountedBit = 1u << 9;
constexpr uint32_t kModOptLevelskipBit = 1u << 10;   // a level skip is armed or running
std::filesystem::path modOptionsPath();
// The value of a known option: the file's (0/1) or its default; -1 for an unknown key.
int modOptionGet(const char* key);
// Writes the file atomically (all known keys); false for an unknown key, a value other
// than 0/1, or an I/O error.
bool modOptionSet(const char* key, int value);
int32_t modOptionsPacked();
// mod_overlay.cpp: an option folder mods/<mod>/@<key> was mounted at boot.
void modOptionNoteMounted(const char* key);
bool modOptionMounted(const char* key);

// ---- the current-save-slot tracker (SR_SaveEvent; docs/CUSTOM_FRONTEND.md 5.6) ----
// new: a new playthrough (no slot). load / save: the slot the game's save flow just used
// (saveFlow+0x54, SaveTask/LoadTask store it; 3 after an overwrite-confirmed save, so an
// overwrite prompt latches the slot first). Pure state machine; slot -1 = none.
// Returns the tracker's slot after the event. *eventSlot (optional; step5 E14a, the review's P2) = the slot THIS
// event itself names: load = +0x54 if 0..2; save = +0x54 if 0..2, else the latched overwrite slot; else -1 (new,
// overwrite, an out-of-range +0x54). The tracker's remembered slot is never passed off as an event's.
int32_t modSlotEvent(const std::string& kind, int32_t saveFlowSlot, int32_t* eventSlot = nullptr);
int32_t modSlotCurrent();
void modSlotResetForTest();
// Test seam: the saveFlow+0x54 value the next SR_SaveEvent reads (-2 = read the guest).
void modBridgeSetSaveFlowSlotForTest(int32_t v);

}  // namespace kernel
}  // namespace sr
