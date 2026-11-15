// iter-463 MOD FRAMEWORK: kernel file-layer asset overlay. Contract: mod_overlay.hpp,
// docs/MODS.md.
#include "mod_overlay.hpp"
#include "mod_manager.hpp"
#include "file_io.hpp"
#include "mod_bridge.hpp"
#include "../app/log.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace sr {
namespace kernel {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDiscMarker = "Superman Returns (USA)/";

struct Mod {
  std::string name;
  fs::path dir;  // <content root>/mods/<name>
};

struct State {
  std::vector<Mod> mods;              // highest priority first; only folders that exist
  std::vector<std::string> names;     // same order
  std::mutex mu;
  std::unordered_map<std::string, std::string> fileCache;  // lower(rel) -> host path or ""
  bool managed = false;
};

State& state() {
  static State s;
  return s;
}

std::once_flag g_once;
bool g_active = false;  // written once under g_once (or by the test seam), read after
// iter-464 modmenu: where the boot list came from, for the MODS menu (kernel/mod_menu.cpp).
std::string g_contentRoot;  // the content root the mods were mounted from
std::string g_listFile;     // the list file the player's switch reads ("" = the list is not a file)
std::string g_source;       // the source line of the [MOD] enabled log

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// A mod name is one plain folder name: letters, digits, '_', '-', '.', not
// starting with '.', at most 64 chars (no separators, no "..", no drive).
bool validModName(const std::string& n) {
  if (n.empty() || n.size() > 64 || n[0] == '.') return false;
  for (char c : n) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

void addUnique(std::vector<std::string>& v, const std::string& n, bool front) {
  for (const auto& x : v)
    if (lower(x) == lower(n)) return;
  if (front) v.insert(v.begin(), n); else v.push_back(n);
}

void removeName(std::vector<std::string>& v, const std::string& n) {
  v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& x) { return lower(x) == lower(n); }),
          v.end());
}

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == ',') { out.push_back(trim(cur)); cur.clear(); }
    else cur += c;
  }
  out.push_back(trim(cur));
  std::vector<std::string> r;
  for (auto& x : out)
    if (!x.empty()) r.push_back(x);
  return r;
}

// Mount the listed mods (priority order) under <root>/mods/. Missing or
// invalid ones get one error line each and are skipped.
void mount(const std::string& contentRoot, const std::vector<std::string>& list, const char* source) {
  State& s = state();
  s.mods.clear();
  s.names.clear();
  s.fileCache.clear();
  s.managed = false;
  g_active = false;
  if (list.empty()) return;
  if (contentRoot.empty()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MOD] error: no content root, %zu enabled mod(s) not mounted", list.size());
    return;
  }
  const fs::path modsDir = fs::u8path(contentRoot) / "mods";
  for (const std::string& n : list) {
    if (!validModName(n)) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[MOD] error: '%s' is not a plain folder name - skipped", n.c_str());
      continue;
    }
    std::error_code ec;
    const fs::path d = modsDir / n;
    if (!fs::is_directory(d, ec) || ec) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[MOD] error: mod '%s' is enabled but \"%s\" is not a folder - skipped",
                  n.c_str(), modPathU8(d).c_str());
      continue;
    }
    // iter-464: OPTION FOLDERS. A mod's subfolder "@<option>" (e.g.
    // mods/custom_frontend/@autosave_without_asking/DATA/baseworl.AST) is part of the mod
    // only while that host option (settings/mod_options.ini, kernel/mod_bridge.cpp) is 1 at
    // boot; it then wins over the mod's own files. Unknown options never mount.
    for (const auto& e : fs::directory_iterator(d, ec)) {
      if (ec) break;
      const std::string sub = e.path().filename().string();
      if (sub.size() < 2 || sub[0] != '@' || !e.is_directory(ec)) continue;
      const std::string key = sub.substr(1);
      const int v = modOptionGet(key.c_str());
      if (v == 1) {
        s.mods.push_back({n + "/" + sub, e.path()});
        modOptionNoteMounted(key.c_str());
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MOD] option folder %s/%s %s (option %s = %d)", n.c_str(), sub.c_str(),
                  v == 1 ? "mounted" : "not mounted", key.c_str(), v);
    }
    s.mods.push_back({n, d});
    s.names.push_back(n);
  }
  g_active = !s.mods.empty();
  std::string joined;
  for (size_t i = 0; i < s.names.size(); ++i) joined += (i ? ", " : "") + s.names[i];
  if (g_active)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOD] enabled (highest priority first): %s  [source: %s; root \"%s\"]",
                joined.c_str(), source, modPathU8(modsDir).c_str());
}

void mountManaged() {
  State& s = state();
  s.mods.clear();
  s.names.clear();
  s.fileCache.clear();
  s.managed = true;
  for (const ModLayer& layer : modActiveLayers()) {
    s.names.push_back(layer.id);
    for (const std::string& root : layer.roots)
      s.mods.push_back({layer.id, fs::u8path(root)});
  }
  g_active = !s.mods.empty();
}

void initOnce() {
  if (modManagerOverlayEnabled()) {
    modManagerBoot();
    mountManaged();
    return;
  }
  const std::string root = contentRootDir();
  std::string source, listFile;
  const std::vector<std::string> list =
      modComputeEnabledList(root, std::getenv("SR_MODS"), std::getenv("SR_CUSTOM_FRONTEND"),
                            std::getenv("SR_INSTANCE_ID"), &source, &listFile);
  g_contentRoot = root;
  g_listFile = listFile;
  g_source = source;
  mount(root, list, source.c_str());
}

void ensureInit() { std::call_once(g_once, initOnce); }

// "<root>Superman Returns (USA)/<rel>" -> the '/'-joined safe components of <rel>
// (empty components dropped). False when the path is outside the disc tree or
// a component is unsafe ("." / ".." / drive or stream ':').
bool discRel(const std::string& hostDiscPath, std::string& rel) {
  const size_t p = hostDiscPath.find(kDiscMarker);
  if (p == std::string::npos) return false;
  const std::string tail = hostDiscPath.substr(p + std::char_traits<char>::length(kDiscMarker));
  rel.clear();
  std::string comp;
  auto flush = [&]() -> bool {
    if (comp.empty()) return true;
    if (comp == "." || comp == ".." || comp.find(':') != std::string::npos) return false;
    rel += (rel.empty() ? "" : "/") + comp;
    comp.clear();
    return true;
  };
  for (char c : tail) {
    if (c == '/' || c == '\\') { if (!flush()) return false; }
    else comp += c;
  }
  if (!flush()) return false;
  return true;
}

}  // namespace

bool modManagerOverlayEnabled() {
  const char* gate = std::getenv("SR_MOD_MANAGER");
  return !gate || gate[0] != '0'; // baked product framework; =0 is a developer A/B opt-out
}

std::vector<std::string> modParseEnabledList(const std::string& text) {
  std::vector<std::string> list;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (list.empty() && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
      line = line.substr(3);  // UTF-8 BOM (Notepad)
    const size_t h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    line = trim(line);
    if (!line.empty()) addUnique(list, line, false);
  }
  return list;
}

std::vector<std::string> modComputeEnabledList(const std::string& contentRoot, const char* srMods,
                                               const char* srCustomFrontend, const char* srInstanceId,
                                               std::string* sourceOut, std::string* listFileOut) {
  std::vector<std::string> list;
  std::string source, listFile;
  const bool automated = srInstanceId && *srInstanceId;
  const fs::path enabledFile = fs::u8path(contentRoot.empty() ? "." : contentRoot) / "mods" / "enabled.txt";
  // SR_MODS=@enabled.txt: read the PLAYER's file even in an automated roll, so a
  // roll can exercise the user-facing switch end to end (explicit opt-in; a plain
  // roll still ignores enabled.txt). iter-464 modmenu: SR_MODS=@<path> reads another
  // list file with the same grammar (a roll's own copy, so the MODS menu's writes and a
  // restart are tested without touching the player's file).
  const bool atFile = srMods && srMods[0] == '@' && srMods[1] != 0;
  const bool fromFile = atFile && std::string(srMods) == "@enabled.txt";
  if (srMods && !atFile) {
    source = "SR_MODS";
    if (std::string(srMods) != "0") list = splitList(srMods);
  } else if (atFile && !fromFile) {
    const fs::path f = fs::u8path(srMods + 1);
    listFile = modPathU8(f);
    source = listFile + " (SR_MODS=@<file>)";
    std::ifstream in(f, std::ios::binary);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      list = modParseEnabledList(text);
    }
  } else if (!contentRoot.empty() && (!automated || fromFile)) {
    listFile = modPathU8(enabledFile);
    std::ifstream in(enabledFile, std::ios::binary);
    if (in) {
      source = fromFile ? "mods/enabled.txt (SR_MODS=@enabled.txt)" : "mods/enabled.txt";
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      list = modParseEnabledList(text);
    }
  } else if (automated) {
    std::error_code ec;
    if (!contentRoot.empty() && fs::exists(enabledFile, ec)) {
      source = "none (enabled.txt ignored: automated roll)";
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MODS] automated roll (SR_INSTANCE_ID): mods/enabled.txt ignored; use SR_MODS / SR_CUSTOM_FRONTEND");
    }
  }
  if (srCustomFrontend) {
    if (srCustomFrontend[0] == '1') {
      addUnique(list, "custom_frontend", true);
      source += source.empty() ? "SR_CUSTOM_FRONTEND" : "+SR_CUSTOM_FRONTEND";
    } else if (srCustomFrontend[0] == '0') {
      removeName(list, "custom_frontend");
    }
  }
  if (source.empty()) source = "none";
  if (sourceOut) *sourceOut = source;
  if (listFileOut) *listFileOut = listFile;
  return list;
}

bool modValidName(const std::string& n) { return validModName(n); }

std::string modOverlayModsDir() {
  ensureInit();
  const std::string root = g_contentRoot.empty() ? contentRootDir() : g_contentRoot;
  return root.empty() ? std::string() : modPathU8(fs::u8path(root) / "mods");
}

std::string modOverlayListFile() {
  ensureInit();
  return g_listFile;
}

std::string modOverlaySource() {
  ensureInit();
  return g_source;
}

bool modOverlayActive() {
  ensureInit();
  return g_active;
}

const std::vector<std::string>& modOverlayEnabled() {
  ensureInit();
  return state().names;
}

bool modEnabled(const char* name) {
  if (!name || !modOverlayActive()) return false;
  for (const auto& n : state().names)
    if (lower(n) == lower(name)) return true;
  return false;
}

std::string modOverlayResolveFile(const std::string& hostDiscPath) {
  if (!modOverlayActive()) return {};
  std::string rel;
  if (!discRel(hostDiscPath, rel) || rel.empty()) return {};
  State& s = state();
  const std::string key = lower(rel);
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.fileCache.find(key);
  if (it != s.fileCache.end()) return it->second;
  std::string hit;
  const Mod* owner = nullptr;
  for (const Mod& m : s.mods) {
    std::error_code ec;
    const fs::path cand = m.dir / fs::u8path(rel);  // case-insensitive on the Windows host FS
    if (fs::is_regular_file(cand, ec) && !ec &&
        (!s.managed || !modHiddenHostPath(modPathU8(cand)))) {
      hit = modPathU8(cand);
      owner = &m;
      break;
    }
  }
  s.fileCache.emplace(key, hit);
  if (owner) {
    std::error_code ec;
    const bool onDisc = fs::exists(fs::u8path(hostDiscPath), ec) && !ec;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MOD] overlay %s '%s' -> \"%s\" (mod %s, %llu bytes)",
                onDisc ? "shadows" : "adds", rel.c_str(), hit.c_str(), owner->name.c_str(),
                static_cast<unsigned long long>(fs::file_size(fs::u8path(hit), ec)));
  }
  return hit;
}

bool modOverlayHasDir(const std::string& hostDiscPath) {
  return !modOverlayResolveDir(hostDiscPath).empty();
}

std::string modOverlayResolveDir(const std::string& hostDiscPath) {
  if (!modOverlayActive()) return {};
  std::string rel;
  if (!discRel(hostDiscPath, rel)) return {};
  for (const Mod& m : state().mods) {
    std::error_code ec;
    const fs::path d = rel.empty() ? m.dir : m.dir / fs::u8path(rel);
    if (fs::is_directory(d, ec) && !ec) return modPathU8(d);
  }
  return {};
}

void modOverlayMergeDir(const std::string& hostDiscDir, std::vector<ModDirEntry>& disc) {
  if (!modOverlayActive()) return;
  std::string rel;
  if (!discRel(hostDiscDir, rel)) return;
  std::unordered_map<std::string, size_t> index;  // lower(name) -> position in disc
  for (size_t i = 0; i < disc.size(); ++i) index.emplace(lower(disc[i].name), i);
  std::unordered_set<std::string> taken;  // names a higher-priority mod already answered
  for (const Mod& m : state().mods) {
    std::error_code ec;
    const fs::path d = rel.empty() ? m.dir : m.dir / fs::u8path(rel);
    if (!fs::is_directory(d, ec) || ec) continue;
    std::vector<ModDirEntry> mine;
    for (const auto& e : fs::directory_iterator(d, ec)) {
      if (ec) break;
      if (state().managed && modHiddenHostPath(modPathU8(e.path()))) continue;
      ModDirEntry me;
      me.name = e.path().filename().string();
      if (rel.empty() && !me.name.empty() && me.name[0] == '@') continue;   // an option folder
      me.hostPath = modPathU8(e.path());
      std::error_code ec2;
      me.isDir = e.is_directory(ec2);
      me.size = (!me.isDir && e.is_regular_file(ec2)) ? e.file_size(ec2) : 0;
      mine.push_back(std::move(me));
    }
    std::sort(mine.begin(), mine.end(),
              [](const ModDirEntry& a, const ModDirEntry& b) { return lower(a.name) < lower(b.name); });
    for (auto& me : mine) {
      const std::string k = lower(me.name);
      if (!taken.insert(k).second) continue;  // a higher-priority mod has it
      auto it = index.find(k);
      if (it != index.end()) {
        ModDirEntry& de = disc[it->second];
        if (de.isDir || me.isDir) continue;  // folders are unioned when opened, not replaced
        de.hostPath = me.hostPath;
        de.size = me.size;
      } else {
        index.emplace(k, disc.size());
        disc.push_back(std::move(me));
      }
    }
  }
}

void modOverlayResetForTest(const std::string& contentRoot, const std::vector<std::string>& mods) {
  std::call_once(g_once, [] {});  // the real init never runs after the seam
  std::lock_guard<std::mutex> lk(state().mu);
  g_contentRoot = contentRoot;
  g_listFile.clear();
  g_source = "test seam";
  mount(contentRoot, mods, "test seam");
}

void modOverlayResetForTestEnv(const ModBootEnv& env) {
  std::call_once(g_once, [] {});
  modManagerResetForTest(env);
  std::lock_guard<std::mutex> lk(state().mu);
  mountManaged();
}
void modOverlaySetListFileForTest(const std::string& listFile) { g_listFile = listFile; }

}  // namespace kernel
}  // namespace sr
