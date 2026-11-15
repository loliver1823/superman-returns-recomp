// iter-463 phase 2: the DEV-TOOL gate and run tags. Contract: devtools.hpp,
// docs/DEVTOOLS_DESIGN.md 1 and 3.1, user guide docs/DEVTOOLS.md.
//
// THE CHOKE POINT (scripts/audit_forcing.py, DEVTOOL rules): this file is the only reader
// of SR_DEVTOOLS, SR_DEVTOOLS_CMDS, SR_DEVTOOLS_OUT, SR_DEVTOOLS_OBJSTATE_EVERY and the
// player's dev.txt. Nothing here consults forcingAllowed(), and the default is OFF: with no
// dev.txt and no SR_DEVTOOLS every predicate is false.
#include "devtools.hpp"

#include "file_io.hpp"
#include "mod_overlay.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>

namespace sr {
namespace kernel {

namespace fs = std::filesystem;

namespace {

// THE closed set (design 1.1). docs/DEVTOOLS.json must list the same rows (audit).
// Order = the [GAUGE] devtools=<mask> bit order: append, never insert (iter-464 added `cheats`).
const DevToolInfo kTools[] = {
    {"hud", "observe"},        {"objstate", "observe"}, {"streaming", "observe"},
    {"levelskip", "devtool"},  {"freecam", "devtool"},  {"menuinput", "devtool"},
    {"devmenu", "devtool"},    // iter-464: the in-game Dev Tools menu's master switch (taints)
    {"cheats", "devtool"},
};
constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

std::once_flag g_once;
std::atomic<uint32_t> g_mask{0};
std::atomic<bool> g_hook{false};
std::atomic<uint64_t> g_acts{0};
std::atomic<bool> g_tainted{false};
std::atomic<bool> g_derivedLogged{false};
std::atomic<bool> g_everAssisted{false};   // a devtool-class tool was on at some point this session
std::string g_suffix;               // guarded by g_suffixMu (the menu changes it at runtime)
std::mutex g_suffixMu;
std::string g_cmds, g_out;
std::string g_warpPresets;  // iter-467 native-a: SR_DEVTOOLS_WARP (the F2 preset warp, kernel/stubs.cpp)
bool g_warpOnMark = false;  // SR_DEVTOOLS_WARP_ON_MARK=1: the preset warp also fires once at the harness mark
uint32_t g_objEvery = 300;
std::mutex g_markMu;
std::set<std::string> g_marked;     // packages this run has marked
std::set<std::string> g_written;    // packages this run wrote while still untainted (marked on a later taint)

int toolIndex(const std::string& name) {
  for (size_t i = 0; i < kToolCount; i++) {
    const char* a = kTools[i].name;
    const std::string& b = name;
    if (std::strlen(a) != b.size()) continue;
    bool eq = true;
    for (size_t k = 0; k < b.size() && eq; k++) {
      const char c = (b[k] >= 'A' && b[k] <= 'Z') ? char(b[k] - 'A' + 'a') : b[k];
      eq = c == a[k];
    }
    if (eq) return static_cast<int>(i);
  }
  return -1;
}

std::vector<std::string> splitEnvList(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    size_t a = 0, b = cur.size();
    while (a < b && std::isspace(static_cast<unsigned char>(cur[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
    if (b > a) out.push_back(cur.substr(a, b - a));
    cur.clear();
  };
  for (char c : s) {
    // '+' too (DT-2): sr_drive's gate list is comma-separated, so a roll names two tools as a+b.
    if (c == ';' || c == ',' || c == '+') flush();
    else cur += c;
  }
  flush();
  return out;
}

bool readText(const fs::path& p, std::string& out) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec) || ec) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::string joinTools(uint32_t mask) {
  std::string s;
  for (size_t i = 0; i < kToolCount; i++)
    if (mask & (1u << i)) s += (s.empty() ? "" : ",") + std::string(kTools[i].name);
  return s;
}

bool anyDevtool(uint32_t mask) {
  for (size_t i = 0; i < kToolCount; i++)
    if ((mask & (1u << i)) && std::string(kTools[i].cls) == "devtool") return true;
  return false;
}

// The tools that run on the game's tick (the sub_8223EB08 / ECC0 / F230 hooks); with none of them
// on, every hook is an exact pass-through. A tool name is written only here and in kTools[]
// (audit_forcing.py: a tool turns on only from SR_DEVTOOLS / dev.txt).
const char* const kTickHookTools[] = {"objstate", "levelskip", "cheats", "freecam"};   // DT-2: + the command channel

void markWritten();

void applyMask(uint32_t mask) {
  g_mask.store(mask);
  bool hook = false;
  for (const char* t : kTickHookTools) {
    const int i = toolIndex(t);
    if (i >= 0 && (mask & (1u << static_cast<uint32_t>(i))) != 0) hook = true;
  }
  g_hook.store(hook);
  {
    std::lock_guard<std::mutex> lk(g_suffixMu);
    // A session tainted by the menu keeps its tag after the tools are switched off again.
    const bool tainted = anyDevtool(mask) || g_tainted.load();
    if (!mask && !tainted) g_suffix.clear();
    else if (!mask) g_suffix = " [dev-tool-assisted: session]";
    else g_suffix = std::string(" [") + (tainted ? "dev-tool-assisted" : "natural+hud") + ": " + joinTools(mask) + "]";
  }
  if (anyDevtool(mask)) g_everAssisted.store(true);
  if (anyDevtool(mask) && !g_tainted.exchange(true)) markWritten();
}

void initOnce() {
  std::string devTxt;
  const std::string root = contentRootDir();
  const bool haveTxt = !root.empty() && readText(fs::path(root) / "dev.txt", devTxt);
  const DevToolsDecision d =
      devToolsCompute(std::getenv("SR_DEVTOOLS"), std::getenv("SR_INSTANCE_ID"), haveTxt ? &devTxt : nullptr);
  for (const auto& u : d.unknown)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] unknown tool %s (ignored; the closed set is kernel/devtools.cpp kTools)",
                u.c_str());
  uint32_t mask = 0;
  for (const auto& t : d.tools) mask |= 1u << static_cast<uint32_t>(toolIndex(t));
  if (const char* e = std::getenv("SR_DEVTOOLS_CMDS")) g_cmds = e;
  if (const char* e = std::getenv("SR_DEVTOOLS_OUT")) g_out = e;
  if (const char* e = std::getenv("SR_DEVTOOLS_WARP")) g_warpPresets = e;
  if (const char* e = std::getenv("SR_DEVTOOLS_WARP_ON_MARK")) g_warpOnMark = e[0] == '1';
  if (const char* e = std::getenv("SR_DEVTOOLS_OBJSTATE_EVERY")) {
    const long v = std::strtol(e, nullptr, 10);
    if (v > 0) g_objEvery = static_cast<uint32_t>(v);
  }
  // The player's defaults (docs/DEVTOOLS.md 3): when the tool list came from dev.txt, the plan
  // is <content root>/levelskip.txt and the output goes to <content root>/devtools_out, unless
  // the env names them. An automated roll (SR_INSTANCE_ID, or an explicit SR_DEVTOOLS list)
  // never picks these files up.
  if (mask && !root.empty()) {
    std::error_code ec;
    const bool planExists = fs::is_regular_file(fs::path(root) / "levelskip.txt", ec) && !ec;
    const DevToolsPaths pth = devToolsPlayerPaths(d.source, root, g_cmds, g_out, planExists);
    g_cmds = pth.cmds;
    g_out = pth.out;
  }
  applyMask(mask);
  if (mask) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] %s tools=%s source=%s",
                anyDevtool(mask) ? "dev-tool-assisted" : "natural+hud", joinTools(mask).c_str(), d.source.c_str());
  }
}

void ensureInit() { std::call_once(g_once, initOnce); }

}  // namespace

const DevToolInfo* devToolTable(size_t* count) {
  if (count) *count = kToolCount;
  return kTools;
}

DevToolsDecision devToolsCompute(const char* srDevtools, const char* srInstanceId, const std::string* devTxt) {
  DevToolsDecision d;
  std::vector<std::string> names;
  if (srDevtools) {
    const std::string v = srDevtools;
    if (v.empty() || v == "0") {
      d.source = "env";
    } else if (v == "@dev.txt") {
      d.source = "env:@dev.txt";
      if (devTxt) names = modParseEnabledList(*devTxt);
    } else {
      d.source = "env";
      names = splitEnvList(v);
    }
  } else if (srInstanceId && *srInstanceId) {
    d.source = "instance";              // an automated roll never reads the player's dev.txt
  } else if (devTxt) {
    d.source = "dev.txt";
    names = modParseEnabledList(*devTxt);
  } else {
    d.source = "none";
  }
  uint32_t seen = 0;
  for (const auto& n : names) {
    const int i = toolIndex(n);
    if (i < 0) {
      d.unknown.push_back(n);
      continue;
    }
    seen |= 1u << static_cast<uint32_t>(i);
  }
  for (size_t i = 0; i < kToolCount; i++)
    if (seen & (1u << i)) d.tools.push_back(kTools[i].name);
  return d;
}

DevToolsPaths devToolsPlayerPaths(const std::string& source, const std::string& root, const std::string& envCmds,
                                  const std::string& envOut, bool planExists) {
  DevToolsPaths p{envCmds, envOut};
  if (root.empty() || (source != "dev.txt" && source != "env:@dev.txt")) return p;
  if (p.cmds.empty() && planExists) p.cmds = (fs::path(root) / "levelskip.txt").string();
  if (p.out.empty()) p.out = (fs::path(root) / "devtools_out").string();
  return p;
}

void devToolsInitEager() { ensureInit(); }

bool devToolAllowed(const char* tool) {
  ensureInit();
  const int i = tool ? toolIndex(tool) : -1;
  return i >= 0 && (g_mask.load(std::memory_order_relaxed) & (1u << static_cast<uint32_t>(i))) != 0;
}

bool devToolHookNeeded_sub_8223EB08() {
  ensureInit();
  return g_hook.load(std::memory_order_relaxed);
}
bool devToolHookNeeded_sub_8223ECC0() {
  ensureInit();
  return g_hook.load(std::memory_order_relaxed);
}
bool devToolHookNeeded_sub_8223F230() {
  ensureInit();
  return g_hook.load(std::memory_order_relaxed);
}

uint32_t devToolMask() {
  ensureInit();
  return g_mask.load(std::memory_order_relaxed);
}
uint64_t devToolActs() { return g_acts.load(std::memory_order_relaxed); }
void devToolActsAdd(uint64_t n) { g_acts.fetch_add(n, std::memory_order_relaxed); }

bool devToolsAnyDevtoolClass() {
  ensureInit();
  return anyDevtool(g_mask.load(std::memory_order_relaxed));
}

const char* devToolsTitleSuffix() {
  ensureInit();
  // The menu can change the suffix while the main loop formats the title: hand out a
  // per-thread copy.
  thread_local std::string copy;
  std::lock_guard<std::mutex> lk(g_suffixMu);
  copy = g_suffix;
  return copy.c_str();
}

bool devToolsSetAtRuntime(const char* tool, bool on, const char* source) {
  ensureInit();
  const int i = tool ? toolIndex(tool) : -1;
  if (i < 0 || std::string(kTools[i].cls) != "devtool") return false;
  const uint32_t bit = 1u << static_cast<uint32_t>(i);
  const uint32_t old = g_mask.load();
  const uint32_t mask = on ? (old | bit) : (old & ~bit);
  if (mask == old) return true;
  applyMask(mask);
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] dev-tool-assisted tools=%s source=%s (%s %s at runtime)",
              mask ? joinTools(mask).c_str() : "none", source ? source : "menu", tool, on ? "on" : "off");
  return true;
}

std::string devToolsPlansDir() {
  ensureInit();
  // Read per call (the menu lists plans rarely): rolls point it at a worktree's plans.
  if (const char* e = std::getenv("SR_DEVTOOLS_PLANS"); e && *e) return e;
  const std::string root = contentRootDir();
  return root.empty() ? std::string() : (fs::path(root) / "data" / "devtools" / "plans").string();
}

std::string devToolsRunClassJson() {
  ensureInit();
  const uint32_t mask = g_mask.load();
  const bool derived = g_derivedLogged.load();
  // Only a run that is NOT natural evidence gets a sidecar: dev-tool-assisted (a devtool-class
  // tool) or devtool-derived (a marked save was loaded). An observe-only (natural+hud) run is
  // tagged by its [RUN_CLASS] line and window title, not by its frames (design 3.1 A6).
  const bool assisted = anyDevtool(mask) || g_everAssisted.load();
  if (!assisted && !derived) return "";
  std::string tools;
  for (size_t i = 0; i < kToolCount; i++)
    if (mask & (1u << i)) tools += std::string(tools.empty() ? "" : ", ") + "\"" + kTools[i].name + "\"";
  const char* cls = assisted ? "dev-tool-assisted" : (derived ? "devtool-derived-save" : "natural+hud");
  return std::string("{\"schema\": \"run_class/1\", \"run_class\": \"") + cls + "\", \"tools\": [" + tools +
         "], \"devtool_derived_save\": " + (derived ? "true" : "false") + ", \"source\": \"exe\"}\n";
}

std::string devToolsCmdsPath() {
  ensureInit();
  return g_cmds;
}
std::string devToolsOutDir() {
  ensureInit();
  return g_out;
}
std::string devToolsWarpPresets() {
  ensureInit();
  return g_warpPresets;
}
bool devToolsWarpOnMark() {
  ensureInit();
  return g_warpOnMark;
}
uint32_t devToolsObjstateEvery() {
  ensureInit();
  return g_objEvery;
}

// ---------------------------------------------------------------- taint ----
fs::path devToolsMarkerPath(const fs::path& pkg) {
  fs::path p = pkg;
  if (!p.has_filename()) p = p.parent_path();       // a trailing separator
  return p.parent_path() / (p.filename().string() + ".devtool");
}

bool devToolsTainted() {
  ensureInit();
  return g_tainted.load();
}

static void noteDerived(const std::string& where) {
  g_tainted.store(true);
  if (!g_derivedLogged.exchange(true))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] devtool-derived-save marker=%s", where.c_str());
}

void devToolsScanContentRoot(const fs::path& root) {
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec) || ec) return;
  size_t packages = 0;
  std::string marker;
  // <root>/<XUID>/<TITLE>/<TYPE>/<package>/  and  <TYPE>/<package>.devtool
  for (const auto& xu : fs::directory_iterator(root, ec)) {
    if (ec || !xu.is_directory(ec) || xu.path().filename().string().rfind('.', 0) == 0) continue;
    for (const auto& ti : fs::directory_iterator(xu.path(), ec)) {
      if (ec || !ti.is_directory(ec)) continue;
      for (const auto& ty : fs::directory_iterator(ti.path(), ec)) {
        if (ec || !ty.is_directory(ec) || ty.path().filename() == "Headers") continue;
        for (const auto& e : fs::directory_iterator(ty.path(), ec)) {
          if (ec) break;
          if (e.is_directory(ec)) packages++;
          else if (e.path().extension() == ".devtool" && marker.empty()) marker = e.path().generic_string();
        }
      }
    }
  }
  if (packages) {
    std::string label = "unknown", text;
    if (readText(root / "SNAPSHOT.json", text)) {
      const size_t k = text.find("\"label\"");
      const size_t q1 = k == std::string::npos ? k : text.find('"', text.find(':', k) + 1);
      const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
      if (q2 != std::string::npos) label = text.substr(q1 + 1, q2 - q1 - 1);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] save-seeded label=%s packages=%zu root=\"%s\"", label.c_str(),
                packages, root.generic_string().c_str());
  }
  if (!marker.empty()) noteDerived(marker);
}

void devToolsOnSaveLoad(const fs::path& pkg) {
  std::error_code ec;
  const fs::path m = devToolsMarkerPath(pkg);
  if (fs::exists(m, ec) && !ec) noteDerived(m.generic_string());
}

static void writeMarker(const fs::path& pkg);

void devToolsOnSaveWrite(const fs::path& pkg) {
  if (!devToolsTainted()) {
    // Remember it: a later taint in this session (the in-game menu) marks it too.
    std::lock_guard<std::mutex> lk(g_markMu);
    g_written.insert(pkg.generic_string());
    return;
  }
  writeMarker(pkg);
}

namespace {
void markWritten() {
  std::set<std::string> todo;
  {
    std::lock_guard<std::mutex> lk(g_markMu);
    todo.swap(g_written);
  }
  for (const auto& p : todo) writeMarker(fs::path(p));
}
}  // namespace

static void writeMarker(const fs::path& pkg) {
  const fs::path m = devToolsMarkerPath(pkg);
  {
    std::lock_guard<std::mutex> lk(g_markMu);
    if (!g_marked.insert(m.generic_string()).second) return;
  }
  const uint32_t mask = g_mask.load();
  std::ofstream f(m, std::ios::binary | std::ios::trunc);
  const std::time_t now = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  f << "{\"schema\": \"devtool_marker/1\", \"run_class\": \""
    << ((anyDevtool(mask) || g_everAssisted.load()) ? "dev-tool-assisted" : "devtool-derived-save") << "\", \"tools\": \"" << joinTools(mask)
    << "\", \"written\": \"" << ts << "\"}\n";
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] devtool-derived-save written marker=%s%s",
              m.generic_string().c_str(), f ? "" : " (WRITE FAILED)");
}

// ---------------------------------------------------------------- test seams ----
void devToolsResetForTest(uint32_t mask) {
  std::call_once(g_once, [] {});   // the real init never runs after the seam
  g_tainted.store(false);
  applyMask(mask);
  g_tainted.store(anyDevtool(mask));
  g_everAssisted.store(anyDevtool(mask));
  g_derivedLogged.store(false);
  std::lock_guard<std::mutex> lk(g_markMu);
  g_marked.clear();
  g_written.clear();
}

void devToolsSetTaintForTest(bool tainted) { g_tainted.store(tainted); }

}  // namespace kernel
}  // namespace sr
