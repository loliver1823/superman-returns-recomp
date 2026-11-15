#pragma once
// iter-463 phase 2: the DEV-TOOL gate and run tags (docs/DEVTOOLS_DESIGN.md 1, 3.1;
// user guide docs/DEVTOOLS.md).
//
// A dev tool is a DEBUGGING AID the user switches on (level skip, in-exe objective-state
// dumps, ...). It is a declared class of its own, NOT forcing:
//   * never on by default: nothing is enabled unless the player's <content root>/dev.txt
//     or SR_DEVTOOLS names it (an automated roll, SR_INSTANCE_ID set, ignores dev.txt);
//   * fully separate from SR_ALLOW_FORCING: devToolAllowed() never consults
//     forcingAllowed() and forcingAllowed() never consults this (a skip test must not be
//     contaminated by diagnostic forcing, and a skip user must not switch it on);
//   * every run that enables a devtool-class tool is tagged at BOOT, before the first
//     [GAUGE]: `[RUN_CLASS] dev-tool-assisted tools=...`, `devtools=<mask>` and
//     `devtoolActs=<n>` on every [GAUGE] line, a window-title suffix, a RUN_CLASS.json
//     sidecar in every present-dump directory, and a `.devtool` marker beside every save
//     package such a run writes (saves loaded from a marked package taint the run too).
//   * audited at the SITE level (scripts/audit_forcing.py DEVTOOL class): every guest
//     write or guest call a tool makes sits behind devToolAllowed("<tool>") or the hook
//     aggregate devToolHookNeeded_sub_XXXXXXXX(); SR_DEVTOOLS / SR_DEVTOOLS_CMDS / dev.txt
//     are read only in kernel/devtools.cpp; the tool set below equals docs/DEVTOOLS.json.
//
// With nothing enabled every predicate is false and every hook is an exact pass-through:
// the game runs the natural path byte for byte (the zero-impact proof, design 10 step 11).
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

// ---- the closed tool set (C++ table == docs/DEVTOOLS.json rows; the audit checks it) ----
struct DevToolInfo {
  const char* name;
  const char* cls;     // "observe" (display / read only) | "devtool" (acts on the game)
};
const DevToolInfo* devToolTable(size_t* count);

// Once at boot (SDL_main, right after the log opens, before the first [GAUGE]). Idempotent;
// every predicate below also initialises lazily on first use.
void devToolsInitEager();

// tool = a string LITERAL from the closed set (the audit enforces it). Default false.
bool devToolAllowed(const char* tool);
// The ONE predicate at each shared tick hook (A2): true when `levelskip`, `objstate`, `cheats` or
// `freecam` is on.
bool devToolHookNeeded_sub_8223EB08();
bool devToolHookNeeded_sub_8223ECC0();
bool devToolHookNeeded_sub_8223F230();

uint32_t devToolMask();          // bit i = devToolTable()[i] enabled (for [GAUGE])
uint64_t devToolActs();          // uncapped count of actions on the game (writes, presses)
void devToolActsAdd(uint64_t n);
bool devToolsAnyDevtoolClass();  // a devtool-class tool is enabled (the run is dev-tool-assisted)
// " [dev-tool-assisted: a,b]" / " [natural+hud: a]" / "" (nothing enabled)
const char* devToolsTitleSuffix();
// The RUN_CLASS.json sidecar text for a present-dump directory: non-empty only for a
// dev-tool-assisted or devtool-derived-save run ("" for natural and natural+hud).
std::string devToolsRunClassJson();

// ---- the in-game Dev Tools menu (iter-464; kernel/devtools_menu.cpp, design section 13) ----
// Switch a DEVTOOL-class tool on (or off) for the rest of THIS process from the menu. Never
// persisted: the next launch starts natural again (only dev.txt / SR_DEVTOOLS persist).
// Switching any devtool-class tool on taints the session for good (switching it off later
// does not un-mark it): `[RUN_CLASS] dev-tool-assisted tools=... source=<source>`, the window
// title suffix, the dump sidecar, and the `.devtool` marker on every save package this
// process writes - including the ones it wrote BEFORE the switch (the notice says "this
// session and the saves made in it"). Observe-class tools are refused (false), as are
// unknown names. tool = a string literal from the closed set.
bool devToolsSetAtRuntime(const char* tool, bool on, const char* source);
// The folder the menu lists level-skip plans from: SR_DEVTOOLS_PLANS, else
// <content root>/data/devtools/plans (the repo ships data/devtools/plans/*.txt).
std::string devToolsPlansDir();
// The level-skip executor, driven from the menu (kernel/devtools_levelskip.cpp).
struct DevToolsSkipStatus {
  std::string state;     // "idle" | "armed" (waits for the first gameplay tick) | "running" | "done" | "stopped"
  std::string plan;      // the plan file name
  size_t k = 0, n = 0;   // current step / step count
  std::string prim;      // the current step's primitive
  std::string last;      // the last event, one line
};
// Arms `planPath` (replaces a loaded plan; the executor starts at the next active gameplay
// tick, so a plan armed in the front end does not burn its first deadline in the menu).
// Requires levelskip to be on (devToolsSetAtRuntime). False + *err on a read/parse error.
bool devToolsLevelskipArm(const std::string& planPath, std::string* err);
void devToolsLevelskipStop(const char* why);
DevToolsSkipStatus devToolsLevelskipStatus();

// ---- the scripted command source and outputs (read ONLY here; design 2.1 choke point) ----
std::string devToolsCmdsPath();       // SR_DEVTOOLS_CMDS ("" = none)
std::string devToolsOutDir();         // SR_DEVTOOLS_OUT  ("" = log only)
std::string devToolsWarpPresets();    // SR_DEVTOOLS_WARP ("X Y Z" or "X Y Z;X Y Z"; the F2 preset warp; "" = none)
bool devToolsWarpOnMark();            // SR_DEVTOOLS_WARP_ON_MARK=1: the preset warp fires once at the harness mark
uint32_t devToolsObjstateEvery();     // SR_DEVTOOLS_OBJSTATE_EVERY active ticks (default 300)
// The level-skip host check while no tick runs (kernel/devtools_levelskip.cpp; called by the
// [GAUGE] census thread every 10 s): the step deadline, then the idle-pad path (a pad step with
// no tick since the previous poll; fault-free guest READS only). A no-op unless levelskip is on.
void devToolsPoll();
// The level-skip virtual pad: the buttons a `pad` step is pressing right now (0 = none; always 0
// unless levelskip queued a press). kernel/stubs.cpp stub_XamInputGetState ORs it into wButtons.
uint16_t devToolsVirtualPadButtons();

// ---- DT-2: the immediate command channel (docs/DEVTOOLS.md 6; kernel/devtools_levelskip.cpp) ----
// The in-game Dev Tools menu (FSCommand bridge), the F6 / F7 / F8 hotkeys and tests submit ONE command
// line:
//   pause_timers on|off   minievents on|off   metro_lock on|off      (tool `cheats`)
//   freecam on|off                                                   (tool `freecam`)
//   warp X Y Z | warp camera     (tool `cheats`; `camera` also needs `freecam`; F7; not a plan line)
//   infinite_stamina on|off      (tool `cheats`; F8; not a plan line)
// (the plan-language ones optionally guarded: `when <cond> [and ...] do <cmd>`). The line is parsed here, on the
// caller's thread; it runs on the game's own tick thread at the drain point (entry of
// sub_8223EB08) on the next active tick, exactly like a plan step. Returns a ticket id > 0, or 0
// with *err set (parse error, not a command, its tool not enabled, queue full). Never touches
// guest memory on the caller's thread.
uint64_t devToolsSubmit(const std::string& line, std::string* err);
// The readout for the menu, JSON (schema devtools_readout/1): enabled tools, the run class, the
// command results by ticket ({id, line, status: queued|running|done|refused|stopped|timeout,
// event}), and the live state the commands act on ("cheats": timers / minievents / metro, read on
// the tick thread - a host copy, never read here - plus "freecam": on|off). Any thread; cheap.
std::string devToolsReadoutJson();
// iter-465 playthrough: the objective the game has ACTIVE, as the tick thread last read it (every 30
// active ticks while levelskip / devmenu / cheats / freecam is on; a host copy, never read here).
// `skip_mission` (tool levelskip; the Dev Tools menu's "SKIP: <name>", the F10 hotkey) passes exactly
// this objective with the plan's `pass` stores. known=false until a tick has read it.
struct DevToolsActiveMission {
  bool known = false, active = false;
  int64_t idx = -1;
  std::string flow;          // objectiveWorldFlowName, e.g. "FastFlyer", "MTOobj09", "ME12TYPE01"
  int64_t tickActive = 0;
};
DevToolsActiveMission devToolsActiveMission();
// freecam (the retail debug camera) is on: kernel/stubs.cpp routes the host pad and keyboard to
// guest controller 2 (the DebugContext controller the dev DebugCam switch used) and reports
// controller 1 neutral. One relaxed atomic load; always false unless the `freecam` tool turned it on.
bool devToolsFreecamRouting();

// ---- taint (design 3.1 A4/A5/B7) ----
// Boot scan of the save content root: `[RUN_CLASS] save-seeded label=<SNAPSHOT.json label>`
// when a package exists, `[RUN_CLASS] devtool-derived-save` when a `.devtool` marker does.
void devToolsScanContentRoot(const std::filesystem::path& root);
bool devToolsTainted();
// kernel/xam_content.cpp: a package was opened (LOAD) / written. The marker is
// <TYPE dir>/<package>.devtool, BESIDE the package folder (it never travels inside one).
void devToolsOnSaveLoad(const std::filesystem::path& pkg);
void devToolsOnSaveWrite(const std::filesystem::path& pkg);
std::filesystem::path devToolsMarkerPath(const std::filesystem::path& pkg);

// ---- the pure decision (tested by build/difftest --devtools-test) ----
struct DevToolsDecision {
  std::vector<std::string> tools;     // canonical names, table order, no duplicates
  std::vector<std::string> unknown;   // names not in the closed set (logged, ignored)
  std::string source;                 // "env" | "env:@dev.txt" | "dev.txt" | "none" | "instance"
};
// srDevtools / srInstanceId may be null; devTxt = the dev.txt text or null when absent.
DevToolsDecision devToolsCompute(const char* srDevtools, const char* srInstanceId, const std::string* devTxt);

// The player's default plan / output paths (docs/DEVTOOLS.md 3): only when the tool list came
// from dev.txt (source "dev.txt" or "env:@dev.txt"), an unset SR_DEVTOOLS_CMDS becomes
// <root>/levelskip.txt when that file exists, and an unset SR_DEVTOOLS_OUT becomes
// <root>/devtools_out. Any other source (env list, SR_INSTANCE_ID roll) keeps the env values.
struct DevToolsPaths {
  std::string cmds;
  std::string out;
};
DevToolsPaths devToolsPlayerPaths(const std::string& source, const std::string& root, const std::string& envCmds,
                                  const std::string& envOut, bool planExists);

// ---- test seams (difftest only) ----
void devToolsResetForTest(uint32_t mask);
void devToolsSetTaintForTest(bool tainted);

}  // namespace kernel
}  // namespace sr
