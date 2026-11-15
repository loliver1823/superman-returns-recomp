// iter-464 (modmenu lane): the in-game MODS menu - the host half. Contract: mod_menu.hpp,
// docs/MODS.md "The MODS menu", docs/CUSTOM_FRONTEND.md 5.8.
//
// Charter: PORT FEATURE (player-facing host UI). No guest memory, no guest calls. Writes only
// the player's mod list file (or a roll's @ copy) and, on RESTART NOW, relaunches the exe.
#include "mod_menu.hpp"

#include "file_io.hpp"
#include "mod_overlay.hpp"
#include "mod_manager.hpp"
#include "mod_library.hpp"   // iter-465: the mod library overlay replaces these pages
#include "../app/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace sr {
namespace kernel {

namespace {
namespace fs = std::filesystem;

constexpr int kModsPerPage = 3;        // + MORE MODS + the footer row (ScrGenPop has 5 buttons)
constexpr int kFilesPerPage = 8;       // body lines on WHAT IT CHANGES
constexpr int kConflictsPerPage = 3;   // body paragraphs on CONFLICTS
constexpr size_t kWalkCap = 20000;     // files listed per mod folder (a huge pack still shows)
constexpr const char* kDiscFolder = "Superman Returns (USA)";

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

std::string nativePath(const std::string& p) {
#ifdef _WIN32
  std::string s = p;
  std::replace(s.begin(), s.end(), '/', '\\');
  return s;
#else
  return p;
#endif
}

// "sample_mission" -> "SAMPLE MISSION"
std::string displayFromId(const std::string& id) {
  std::string s = id;
  for (char& c : s)
    if (c == '_') c = ' ';
  return hostUpper(s);
}

// ---- a flat JSON object reader for mods/<id>/mod.json (string / bool / number members; nested
// values are skipped). The manifest format is the modcore lane's to define; this reads the
// fields the menu shows.
struct Manifest {
  bool present = false, ok = true;
  std::string err;
  std::map<std::string, std::string> str;
  std::map<std::string, bool> flag;
};

Manifest readManifest(const fs::path& file) {
  Manifest m;
  std::ifstream in(file, std::ios::binary);
  if (!in) return m;
  m.present = true;
  const std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t i = 0;
  auto ws = [&] { while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == ',')) ++i; };
  auto fail = [&](const char* why) {
    m.ok = false;
    m.err = why;
    return m;
  };
  auto readStr = [&](std::string* out) -> bool {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    std::string s;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c == '\\' && i < t.size()) {
        const char e = t[i++];
        c = e == 'n' ? '\n' : e == 't' ? ' ' : e;
        if (e == 'u') {   // \uXXXX: keep ASCII, drop the rest
          unsigned v = 0;
          for (int k = 0; k < 4 && i < t.size(); ++k) v = v * 16 + (std::isdigit(static_cast<unsigned char>(t[i])) ? t[i] - '0' : (std::tolower(t[i]) - 'a' + 10)), ++i;
          c = v < 128 ? static_cast<char>(v) : '?';
        }
      }
      s += c;
    }
    if (i >= t.size()) return false;
    ++i;
    *out = s;
    return true;
  };
  if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF) i = 3;   // UTF-8 BOM
  ws();
  if (i >= t.size() || t[i] != '{') return fail("not a JSON object");
  ++i;
  for (;;) {
    ws();
    if (i >= t.size()) return fail("unterminated object");
    if (t[i] == '}') break;
    std::string key;
    if (!readStr(&key)) return fail("expected a quoted key");
    ws();
    if (i >= t.size() || t[i] != ':') return fail("expected ':'");
    ++i;
    ws();
    if (i >= t.size()) return fail("missing value");
    if (t[i] == '"') {
      std::string v;
      if (!readStr(&v)) return fail("unterminated string");
      m.str[key] = v;
    } else if (t.compare(i, 4, "true") == 0) {
      m.flag[key] = true;
      i += 4;
    } else if (t.compare(i, 5, "false") == 0) {
      m.flag[key] = false;
      i += 5;
    } else if (t[i] == '{' || t[i] == '[') {   // skip a nested value (strings inside may hold brackets)
      int depth = 0;
      do {
        if (t[i] == '"') {
          std::string skip;
          if (!readStr(&skip)) return fail("unterminated string");
          continue;
        }
        if (t[i] == '{' || t[i] == '[') ++depth;
        if (t[i] == '}' || t[i] == ']') --depth;
        ++i;
      } while (depth > 0 && i < t.size());
      if (depth != 0) return fail("unterminated nested value");
    } else {   // number / null
      const size_t s = i;
      while (i < t.size() && t[i] != ',' && t[i] != '}' && !std::isspace(static_cast<unsigned char>(t[i]))) ++i;
      m.str[key] = t.substr(s, i - s);
    }
  }
  return m;
}

}  // namespace

// The disc-tree files a mod folder provides ('/' separators, as spelled on disk), at most
// kWalkCap. A top-level folder counts only when the disc root has it (DATA/...; not '@option'
// folders, which are layers of their own, nor a manifest's entries/ tree); a top-level file
// counts when the disc root has it or it is a new root .AST (a flow bundle).
std::vector<std::string> modMenuWalkGameFiles(const std::string& dirIn, const std::string& discRootIn) {
  std::vector<std::string> out;
  const fs::path dir = fs::u8path(dirIn), disc = fs::u8path(discRootIn);
  std::error_code ec;
  for (auto e = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       !ec && e != fs::recursive_directory_iterator(); e.increment(ec)) {
    const fs::path rel = e->path().lexically_relative(dir);
    const std::string first = rel.begin() != rel.end() ? rel.begin()->string() : std::string();
    std::error_code ec2;
    if (e.depth() == 0) {
      const bool onDisc = fs::exists(disc / first, ec2);
      if (e->is_directory(ec2)) {
        if (!onDisc) e.disable_recursion_pending();
        continue;
      }
      if (!onDisc && lower(e->path().extension().string()) != ".ast") continue;
    }
    if (!e->is_regular_file(ec2)) continue;
    out.push_back(rel.generic_string());
    if (out.size() >= kWalkCap) break;
  }
  return out;
}

namespace {

// ---- the built-in fallback catalog -------------------------------------------------------
struct Entry {
  std::string id;
  bool on = false;
};

struct Layer {
  std::string id;         // "<mod>" or "<mod>/@<option>"
  std::string mod;        // the mod it belongs to
  std::string name;       // display name for conflicts
  fs::path dir;
};

class FallbackCatalog final : public ModCatalog {
 public:
  FallbackCatalog(std::string modsDir, std::string listFile, std::vector<std::string> boot)
      : modsDir_(std::move(modsDir)), listFile_(std::move(listFile)), boot_(std::move(boot)) {}

  ModMenuView view() override {
    std::lock_guard<std::mutex> lk(mu_);
    ModMenuView v;
    v.modsDir = nativePath(modsDir_);
    v.canSave = !listFile_.empty();
    v.why = v.canSave ? std::string() : "this run's mods come from SR_MODS";
    v.acceptsZip = false;
    const std::vector<Entry> entries = orderedEntries();
    // Layers in next-launch priority order: every entry (on or off) with its mounted option
    // folders first, as kernel/mod_overlay.cpp mount() orders them.
    std::vector<Layer> layers;
    for (const Entry& e : entries) {
      const fs::path d = fs::u8path(modsDir_) / e.id;
      std::error_code ec;
      if (!fs::is_directory(d, ec)) continue;
      const Manifest man = manifestFor(e.id);
      const std::string name = displayName(e.id, man);
      for (const auto& sub : fs::directory_iterator(d, ec)) {
        if (ec) break;
        const std::string sn = sub.path().filename().string();
        if (sn.size() < 2 || sn[0] != '@' || !sub.is_directory(ec)) continue;
        if (modOptionGet(sn.substr(1).c_str()) != 1) continue;
        layers.push_back({e.id + "/" + sn, e.id, optionDisplay(sn.substr(1), man), sub.path()});
      }
      layers.push_back({e.id, e.id, name + (isPortFeature(man) ? " (PORT FEATURE)" : ""), d});
    }
    std::set<std::string> onSet;
    for (const Entry& e : entries)
      if (e.on) onSet.insert(lower(e.id));
    // target (lower) -> the layers providing it, in priority order
    std::map<std::string, std::vector<size_t>> providers;
    std::map<std::string, std::string> spelled;
    for (size_t li = 0; li < layers.size(); ++li)
      for (const std::string& f : filesOf(layers[li].dir)) {
        providers[lower(f)].push_back(li);
        spelled.emplace(lower(f), f);
      }
    const fs::path disc = fs::u8path(modsDir_).parent_path() / kDiscFolder;
    std::vector<std::string> nextList;
    for (const Entry& e : entries) {
      const fs::path d = fs::u8path(modsDir_) / e.id;
      std::error_code ec;
      const bool isDir = fs::is_directory(d, ec);
      const bool isZip = !isDir && fs::is_regular_file(fs::u8path(modsDir_) / (e.id + ".zip"), ec);
      if (e.on && isDir) nextList.push_back(lower(e.id));
      const Manifest man = isDir ? manifestFor(e.id) : Manifest{};
      if (isPortFeature(man)) continue;   // never listed (USER 2026-10-02: no toggle)
      ModMenuEntry m;
      m.id = e.id;
      m.enabled = e.on;
      m.active = std::any_of(boot_.begin(), boot_.end(), [&](const std::string& b) { return lower(b) == lower(e.id); });
      m.name = displayName(e.id, man);
      m.author = man.str.count("author") ? man.str.at("author") : std::string();
      m.version = man.str.count("version") ? man.str.at("version") : std::string();
      m.description = man.str.count("description") ? man.str.at("description") : std::string();
      if (!isDir) {
        m.state = ModMenuState::Failed;
        m.reason = isZip ? "it is a .zip: unzip it into a folder in the mods folder first"
                         : "its folder is not in the mods folder";
        v.mods.push_back(m);
        continue;
      }
      if (man.present && !man.ok) {
        m.state = ModMenuState::Failed;
        m.reason = "its mod.json could not be read (" + man.err + ")";
      }
      // what it changes (its own folder; its option folders are listed as part of it)
      std::set<std::string> own;
      for (size_t li = 0; li < layers.size(); ++li) {
        if (layers[li].mod != e.id) continue;
        for (const std::string& f : filesOf(layers[li].dir)) {
          if (!own.insert(lower(f)).second) continue;
          std::error_code ec2;
          m.changes.push_back({f, !fs::exists(disc / f, ec2)});
        }
      }
      std::sort(m.changes.begin(), m.changes.end(),
                [](const ModMenuChange& a, const ModMenuChange& b) { return lower(a.target) < lower(b.target); });
      if (m.changes.empty() && m.state == ModMenuState::Ok) {
        m.state = ModMenuState::Failed;
        m.reason = "its folder holds no game files (they go in folders laid out like the game disc, e.g. DATA)";
      }
      // conflicts: another ON layer of another mod provides the same target
      for (const std::string& t : own) {
        const std::vector<size_t>& ps = providers[t];
        size_t mine = ps.size();
        for (size_t k = 0; k < ps.size(); ++k)
          if (layers[ps[k]].mod == e.id) { mine = k; break; }
        for (size_t k = 0; k < ps.size(); ++k) {
          const Layer& o = layers[ps[k]];
          if (o.mod == e.id || !onSet.count(lower(o.mod))) continue;
          m.conflicts.push_back({spelled[t], o.id, o.name, mine < k});
        }
      }
      if (!m.conflicts.empty() && m.state == ModMenuState::Ok) {
        m.state = ModMenuState::Conflict;
        const ModMenuConflict& c = m.conflicts.front();
        m.reason = c.target + " is also changed by " + c.otherName +
                   (c.wins ? " (this mod wins)" : " (that one wins)");
        if (m.conflicts.size() > 1) m.reason += " and " + std::to_string(m.conflicts.size() - 1) + " more";
      }
      v.mods.push_back(m);
    }
    std::vector<std::string> bootLower;
    for (const auto& b : boot_) bootLower.push_back(lower(b));
    v.pendingRestart = nextList != bootLower;
    return v;
  }

  bool setEnabled(const std::string& id, bool on, std::string* err) override {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Entry> es = orderedEntries();
    for (Entry& e : es)
      if (lower(e.id) == lower(id)) {
        if (isPortFeature(manifestFor(e.id))) return failWith(err, "a port feature has no switch");
        e.on = on;
        return save(es, err);
      }
    return failWith(err, "no such mod");
  }

  bool move(const std::string& id, int delta, std::string* err) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (delta != -1 && delta != 1) return failWith(err, "delta must be -1 or 1");
    std::vector<Entry> es = orderedEntries();
    // visible = the listed mods (port features keep their places)
    std::vector<size_t> vis;
    for (size_t i = 0; i < es.size(); ++i)
      if (!isPortFeature(manifestFor(es[i].id))) vis.push_back(i);
    size_t k = vis.size();
    for (size_t j = 0; j < vis.size(); ++j)
      if (lower(es[vis[j]].id) == lower(id)) k = j;
    if (k == vis.size()) return failWith(err, "no such mod");
    if ((delta < 0 && k == 0) || (delta > 0 && k + 1 == vis.size())) return failWith(err, "already at the end");
    std::swap(es[vis[k]], es[vis[k + delta]]);
    return save(es, err);
  }

  void rescan() override {
    std::lock_guard<std::mutex> lk(mu_);
    walkCache_.clear();
    manCache_.clear();
  }

  std::string sessionListForRestart() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!listFile_.empty()) return {};
    std::string s;
    for (const Entry& e : orderedEntries())
      if (e.on) s += (s.empty() ? "" : ";") + e.id;
    return s.empty() ? "0" : s;
  }

 private:
  static bool failWith(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
  }

  static bool isPortFeature(const Manifest& m) {
    auto it = m.flag.find("port_feature");
    return it != m.flag.end() && it->second;
  }

  static std::string displayName(const std::string& id, const Manifest& m) {
    auto it = m.str.find("name");
    return hostUpper(it != m.str.end() && !trim(it->second).empty() ? trim(it->second) : displayFromId(id));
  }

  static std::string optionDisplay(const std::string& key, const Manifest& m) {
    (void)m;
    return displayFromId(key) + " (PORT FEATURE OPTION)";
  }

  const Manifest& manifestFor(const std::string& id) {
    auto it = manCache_.find(lower(id));
    if (it != manCache_.end()) return it->second;
    return manCache_[lower(id)] = readManifest(fs::u8path(modsDir_) / id / "mod.json");
  }

  // The disc-tree files a layer provides ('/' separators, as spelled on disk). Top-level files
  // count only when the disc has that name at its root (mod.json / README are not game files);
  // top-level '@' folders are separate layers.
  const std::vector<std::string>& filesOf(const fs::path& dir) {
    const std::string key = lower(modPathU8(dir));
    auto it = walkCache_.find(key);
    if (it != walkCache_.end()) return it->second;
    return walkCache_[key] = modMenuWalkGameFiles(modPathU8(dir), modPathU8(fs::u8path(modsDir_).parent_path() / kDiscFolder));
  }

  // The list file's entries ("name" = on, "# name" = off when such a mod is installed) in file
  // order, then every installed mod the file does not name (off, sorted); without a list file
  // the session list (initially what this process mounted).
  std::vector<Entry> orderedEntries() {
    std::vector<Entry> es;
    std::set<std::string> seen;
    auto add = [&](const std::string& id, bool on) {
      if (!modValidName(id)) return;
      const std::string k = lower(id);
      if (seen.count(k)) {
        if (on)
          for (Entry& e : es)
            if (lower(e.id) == k) e.on = true;
        return;
      }
      seen.insert(k);
      es.push_back({id, on});
    };
    const std::set<std::string> installed = installedIds();
    if (!listFile_.empty()) {
      std::ifstream in(fs::u8path(listFile_), std::ios::binary);
      std::string line;
      bool first = true;
      while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
        first = false;
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#') {
          const std::string rest = trim(t.substr(1));
          if (installed.count(lower(rest))) add(rest, false);
          continue;
        }
        const size_t h = t.find('#');
        if (h != std::string::npos) t = trim(t.substr(0, h));
        if (!t.empty()) add(t, true);
      }
    } else {
      if (!sessionInit_) {
        for (const auto& b : boot_) session_.push_back({b, true});
        sessionInit_ = true;
      }
      for (const Entry& e : session_) add(e.id, e.on);
    }
    std::vector<std::string> rest;
    for (const std::string& k : installedSpelled())
      if (!seen.count(lower(k))) rest.push_back(k);
    std::sort(rest.begin(), rest.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    for (const auto& r : rest) add(r, false);
    return es;
  }

  std::vector<std::string> installedSpelled() {
    std::vector<std::string> v;
    std::error_code ec;
    if (modsDir_.empty() || !fs::is_directory(fs::u8path(modsDir_), ec)) return v;
    for (const auto& e : fs::directory_iterator(fs::u8path(modsDir_), ec)) {
      if (ec) break;
      const std::string n = e.path().filename().string();
      std::error_code ec2;
      if (e.is_directory(ec2) && modValidName(n)) v.push_back(n);
      else if (e.is_regular_file(ec2) && lower(e.path().extension().string()) == ".zip" &&
               modValidName(e.path().stem().string()))
        v.push_back(e.path().stem().string());
    }
    return v;
  }

  std::set<std::string> installedIds() {
    std::set<std::string> s;
    for (const auto& n : installedSpelled()) s.insert(lower(n));
    return s;
  }

  bool save(const std::vector<Entry>& es, std::string* err) {
    if (listFile_.empty()) {
      session_ = es;
      sessionInit_ = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] state kept for this session only (%s)",
                  "this run's mods come from SR_MODS");
      return true;
    }
    // Keep the player's own comment lines (those that do not name an installed mod) on top.
    std::vector<std::string> header;
    const std::set<std::string> installed = installedIds();
    {
      std::ifstream in(fs::u8path(listFile_), std::ios::binary);
      std::string line;
      bool first = true;
      while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
        first = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] != '#') continue;
        if (installed.count(lower(trim(t.substr(1))))) continue;
        header.push_back(line);
      }
    }
    if (header.empty())
      header = {"# Mods the game mounts at boot, one folder name (under mods/) per line.",
                "# Earlier lines win when two mods provide the same file. '#' starts a comment.",
                "# \"# <name>\" = installed but switched off. The in-game MODS menu writes this file."};
    std::ostringstream o;
    for (const auto& h : header) o << h << "\n";
    for (const Entry& e : es) o << (e.on ? "" : "# ") << e.id << "\n";
    const fs::path f = fs::u8path(listFile_);
    fs::path tmp = f;
    tmp += ".tmp";
    std::error_code ec;
    if (!f.parent_path().empty()) fs::create_directories(f.parent_path(), ec);
    {
      std::ofstream w(tmp, std::ios::binary | std::ios::trunc);
      w << o.str();
      w.close();
      if (!w) {
        if (err) *err = "could not write " + modPathU8(tmp);
        return false;
      }
    }
    fs::rename(tmp, f, ec);
    if (ec) {
      fs::remove(tmp, ec);
      if (err) *err = "could not replace " + modPathU8(f);
      return false;
    }
    std::string list;
    for (const Entry& e : es) list += (list.empty() ? "" : ", ") + std::string(e.on ? "" : "-") + e.id;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] saved %s: %s", modPathU8(f).c_str(), list.c_str());
    return true;
  }

  std::mutex mu_;
  std::string modsDir_, listFile_;
  std::vector<std::string> boot_;
  bool sessionInit_ = false;
  std::vector<Entry> session_;
  std::map<std::string, std::vector<std::string>> walkCache_;
  std::map<std::string, Manifest> manCache_;
};

// ---- menu state -----------------------------------------------------------------------------
std::mutex g_mu;
std::shared_ptr<ModCatalog> g_catalog;          // modcore's, else the fallback (lazily)
std::string g_ctx = "FE";
std::map<std::string, std::vector<DevMenuItemDef>> g_options;   // lower(mod id) -> rows
std::vector<CostumeDef> g_costumes;
std::atomic<bool> g_restart{false};
std::string g_restartSessionList;
void (*g_restartForTest)() = nullptr;
bool (*g_folderForTest)(const std::string&) = nullptr;

using Row = HostPageRow;
using Page = HostPage;
using Out = HostPageOut;

std::vector<std::string> splitId(const std::string& id) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : id) {
    if (c == ':') {
      v.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  v.push_back(cur);
  return v;
}

int toInt(const std::vector<std::string>& v, size_t i) {
  return i < v.size() ? std::max(0, std::atoi(v[i].c_str())) : 0;
}

const ModMenuEntry* findMod(const ModMenuView& v, const std::string& id, int* index = nullptr) {
  for (size_t i = 0; i < v.mods.size(); ++i)
    if (lower(v.mods[i].id) == lower(id)) {
      if (index) *index = static_cast<int>(i);
      return &v.mods[i];
    }
  return nullptr;
}

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 3) + "..."; }

std::string rowLabel(const ModMenuEntry& m) {
  std::string l = clip(m.name, 24) + ": " + (m.enabled ? "ON" : "OFF");
  if (m.state == ModMenuState::Conflict) l += " - CONFLICT";
  if (m.state == ModMenuState::Failed) l += " - FAILED";
  return l;
}

std::string listPageFor(int index) { return "mods:" + std::to_string((index / kModsPerPage) * kModsPerPage); }

void requestRestartImpl() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_restartSessionList = g_catalog ? g_catalog->sessionListForRestart() : std::string();
  }
  g_restart = true;
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] RESTART NOW -> SDL_EVENT_QUIT; the exit path relaunches the exe");
  if (g_restartForTest) {
    g_restartForTest();
    return;
  }
  SDL_Event e{};
  e.type = SDL_EVENT_QUIT;
  if (!SDL_PushEvent(&e))   // SDL3: bool (false = failed or filtered)
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MODMENU] SDL_PushEvent(SDL_EVENT_QUIT) failed: %s", SDL_GetError());
}

// ---- pages ----------------------------------------------------------------------------------
Page buildPage(const std::string& id);

// Explorer receives a directory, never a shell command or a manifest-supplied URL.
bool openModsFolder(const ModMenuView& v) {
  const fs::path dir = fs::u8path(v.modsDir).lexically_normal();
  if (!dir.is_absolute() || dir.filename() != "mods") return false;
  if (g_folderForTest) return g_folderForTest(v.modsDir);
  const char* instance = std::getenv("SR_INSTANCE_ID");
  if (instance && *instance) return false;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec || !fs::is_directory(dir, ec)) return false;
#ifdef _WIN32
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
  return false;
#endif
}

Row folderRow(const std::string& page, int focus) {
  Row r;
  r.label = "OPEN MODS FOLDER";
  r.press = [page, focus](Out& o) {
    const bool ok = openModsFolder(modMenuCatalog()->view());
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODMENU] open mods folder -> %s", ok ? "opened" : "failed");
    o.page = ok ? page : "modfoldererror";
    o.focus = ok ? focus : 0;
  };
  return r;
}

Row refreshRow(const ModMenuView& v) {
  Row r;
  if (v.canRescan) {
    r.label = "CHECK AGAIN";
    r.press = [](Out& o) {
      modMenuCatalog()->rescan();
      o.page = "mods:0";
      o.focus = 0;
    };
  } else {
    r.label = "RESTART TO APPLY";
    r.press = [](Out& o) {
      o.page = "modrestart";
      o.focus = 0;
    };
  }
  return r;
}

std::string installBody(const ModMenuView& v) {
  std::string body = v.acceptsZip ? "Add mod folders or .zip files."
                                : "Add mod folders. Unzip .zip files first.";
  if (v.autoEnableNew) body += "\nNew mods start enabled.";
  body += v.canRescan ? "\nChoose CHECK AGAIN, then enable your mods.\nRestart the game to apply mods."
                      : "\nRestart the game to apply mods.";
  return body;
}

// Paths belong on the help page, not in the empty-state paragraph. Wrap without
// splitting a UTF-8 code point; the folder button works even for unavailable font glyphs.
std::string wrappedPath(const std::string& path) {
  std::string out;
  size_t at = 0;
  while (path.size() - at > 34) {
    size_t end = at + 34;
    const size_t slash = path.find_last_of("/\\", end - 1);
    if (slash != std::string::npos && slash >= at) end = slash + 1;
    else while (end > at && (static_cast<unsigned char>(path[end]) & 0xC0) == 0x80) --end;
    out += path.substr(at, end - at) + "\n";
    at = end;
  }
  return out + path.substr(at);
}

Page pageEmpty(const ModMenuView& v) {
  Page p;
  p.id = "mods:0";
  p.title = "MODS";
  p.body = "No mods installed.\n" + installBody(v);
  Row how;
  how.label = "HOW TO INSTALL MODS";
  how.press = [](Out& o) {
    o.page = "modhow";
    o.focus = 0;
  };
  p.rows = {folderRow(p.id, 0), refreshRow(v), how};
  return p;   // back unset: B closes the menu
}

Page pageList(int off) {
  auto cat = modMenuCatalog();
  const ModMenuView v = cat->view();
  if (v.mods.empty()) return pageEmpty(v);
  const int n = static_cast<int>(v.mods.size());
  if (off >= n) off = ((n - 1) / kModsPerPage) * kModsPerPage;
  Page p;
  p.id = "mods:" + std::to_string(off);
  p.title = "MODS";
  std::ostringstream b;
  b << n << (n == 1 ? " mod installed." : " mods installed.") << " A mod higher in the list wins when two change the same thing.\n";
  if (!v.canSave) b << "This run's mod list is fixed (" << v.why << "): changes last until the game closes.\n";
  b << (v.pendingRestart ? "Your changes apply after a restart." : "A: details.  LEFT / RIGHT: switch on or off.");
  p.body = b.str();
  for (int i = off; i < n && i < off + kModsPerPage; ++i) {
    const ModMenuEntry m = v.mods[i];
    Row r;
    r.label = rowLabel(m);
    r.kind = DevMenuKind::Toggle;
    const std::string pid = p.id;
    const int row = i - off;
    const bool wasPending = v.pendingRestart;
    r.press = [m](Out& o) {
      o.page = "mod:" + m.id;
      o.focus = 0;
    };
    r.adjust = [m, pid, row, wasPending](Out& o, int) {
      std::string err;
      const bool ok = modMenuCatalog()->setEnabled(m.id, !m.enabled, &err);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] %s %s -> %s%s", m.id.c_str(), m.enabled ? "off" : "on",
                  ok ? "saved" : "REFUSED: ", ok ? "" : err.c_str());
      o.page = pid;
      o.focus = row;
      // the footer row and the body change with the pending state: re-show; else relabel in place
      o.action = modMenuCatalog()->view().pendingRestart != wasPending ? 1 : 2;
    };
    p.rows.push_back(r);
  }
  if (off + kModsPerPage < n) {
    Row more;
    more.label = "MORE MODS";
    const int next = off + kModsPerPage;
    more.press = [next](Out& o) {
      o.page = "mods:" + std::to_string(next);
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  Row foot;
  const int footRow = static_cast<int>(p.rows.size());
  if (!cat->costumes().empty()) {
    foot.label = "COSTUMES / MOD OPTIONS";
    foot.press = [](Out& o) { o.page = "modextras"; o.focus = 0; };
  } else if (v.pendingRestart) {
    foot.label = "RESTART TO APPLY";
    foot.press = [](Out& o) {
      o.page = "modrestart";
      o.focus = 0;
    };
  } else {
    foot.label = "HOW TO INSTALL MODS";
    foot.press = [](Out& o) {
      o.page = "modhow";
      o.focus = 0;
    };
  }
  (void)footRow;
  p.rows.push_back(foot);
  if (off > 0)
    p.back = [off](Out& o) {
      o.page = "mods:" + std::to_string(std::max(0, off - kModsPerPage));
      o.focus = 0;
    };
  return p;
}

Page pageDetails(const std::string& id) {
  auto cat = modMenuCatalog();
  const ModMenuView v = cat->view();
  int idx = 0;
  const ModMenuEntry* mp = findMod(v, id, &idx);
  if (!mp) return pageList(0);
  const ModMenuEntry m = *mp;
  const int n = static_cast<int>(v.mods.size());
  Page p;
  p.id = "mod:" + m.id;
  p.title = clip(m.name, 32);
  std::ostringstream b;
  if (!m.version.empty() || !m.author.empty()) {
    if (!m.version.empty()) b << "Version " << m.version;
    if (!m.author.empty()) b << (m.version.empty() ? "By " : " by ") << m.author;
    b << ".\n";
  }
  if (!m.description.empty()) b << clip(m.description, 360) << "\n";
  b << "Status: " << (m.state == ModMenuState::Ok ? "OK" : m.state == ModMenuState::Conflict ? "CONFLICT - " : "FAILED - ")
    << (m.state == ModMenuState::Ok ? "" : m.reason) << "\n";
  if (m.enabled && m.active) b << "It is loaded now.";
  else if (m.enabled) b << "It loads after a restart.";
  else if (m.active) b << "Switched off: it stays loaded until a restart.";
  else b << "It is switched off.";
  p.body = b.str();
  Row en;
  en.label = std::string("ENABLED: ") + (m.enabled ? "ON" : "OFF");
  en.kind = DevMenuKind::Toggle;
  auto toggle = [m](Out& o) {
    std::string err;
    const bool ok = modMenuCatalog()->setEnabled(m.id, !m.enabled, &err);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] %s %s -> %s%s", m.id.c_str(), m.enabled ? "off" : "on",
                ok ? "saved" : "REFUSED: ", ok ? "" : err.c_str());
    o.page = "mod:" + m.id;
    o.focus = 0;   // re-shown (action 1): the body says when it applies
  };
  en.press = toggle;
  en.adjust = [toggle](Out& o, int) { toggle(o); };
  p.rows.push_back(en);
  Row ord;
  ord.label = "LOAD ORDER: " + std::to_string(idx + 1) + " OF " + std::to_string(n);
  ord.kind = DevMenuKind::Slider;
  auto mv = [m](Out& o, int delta) {
    std::string err;
    const bool ok = modMenuCatalog()->move(m.id, delta, &err);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] %s move %s -> %s%s", m.id.c_str(), delta < 0 ? "up" : "down",
                ok ? "saved" : "refused: ", ok ? "" : err.c_str());
    o.page = "mod:" + m.id;
    o.focus = 1;   // re-shown: the status line may change (who wins a conflict)
  };
  ord.press = [mv](Out& o) { mv(o, -1); };   // A moves it one place up
  ord.adjust = mv;                           // LEFT = earlier (up), RIGHT = later (down)
  p.rows.push_back(ord);
  Row ch;
  ch.label = "WHAT IT CHANGES (" + std::to_string(m.changes.size()) + ")";
  ch.press = [m](Out& o) {
    o.page = "modfiles:" + m.id + ":0";
    o.focus = 0;
  };
  p.rows.push_back(ch);
  if (!m.conflicts.empty()) {
    Row cf;
    cf.label = "CONFLICTS (" + std::to_string(m.conflicts.size()) + ")";
    cf.press = [m](Out& o) {
      o.page = "modconf:" + m.id + ":0";
      o.focus = 0;
    };
    p.rows.push_back(cf);
  }
  bool hasOpts = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_options.find(lower(m.id));
    hasOpts = it != g_options.end() && !it->second.empty();
  }
  if (hasOpts) {
    Row op;
    op.label = "OPTIONS";
    op.press = [m](Out& o) {
      o.page = "modopts:" + m.id + ":0";
      o.focus = 0;
    };
    p.rows.push_back(op);
  }
  p.back = [idx](Out& o) {
    o.page = listPageFor(idx);
    o.focus = idx % kModsPerPage;
  };
  return p;
}

Page pageFiles(const std::string& id, int off) {
  const ModMenuView v = modMenuCatalog()->view();
  const ModMenuEntry* mp = findMod(v, id);
  if (!mp) return pageList(0);
  const ModMenuEntry m = *mp;
  const int n = static_cast<int>(m.changes.size());
  Page p;
  p.id = "modfiles:" + m.id + ":" + std::to_string(off);
  p.title = "WHAT IT CHANGES";
  std::ostringstream b;
  b << clip(m.name, 32) << (n == 1 ? " changes 1 game file" : " changes " + std::to_string(n) + " game files")
    << (n > kFilesPerPage ? " (" + std::to_string(off + 1) + "-" + std::to_string(std::min(n, off + kFilesPerPage)) + ")" : "")
    << ":";
  for (int i = off; i < n && i < off + kFilesPerPage; ++i)
    b << "\n" << (m.changes[i].adds ? "Adds " : "Replaces ") << m.changes[i].target;
  p.body = b.str();
  if (off + kFilesPerPage < n) {
    Row more;
    more.label = "MORE";
    const std::string next = "modfiles:" + m.id + ":" + std::to_string(off + kFilesPerPage);
    more.press = [next](Out& o) {
      o.page = next;
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  Row back;
  back.label = "BACK";
  back.press = [m](Out& o) {
    o.page = "mod:" + m.id;
    o.focus = 2;
  };
  p.rows.push_back(back);
  p.back = [m, off](Out& o) {
    o.page = off > 0 ? "modfiles:" + m.id + ":" + std::to_string(std::max(0, off - kFilesPerPage)) : "mod:" + m.id;
    o.focus = off > 0 ? 0 : 2;
  };
  return p;
}

Page pageConflicts(const std::string& id, int off) {
  const ModMenuView v = modMenuCatalog()->view();
  const ModMenuEntry* mp = findMod(v, id);
  if (!mp) return pageList(0);
  const ModMenuEntry m = *mp;
  const int n = static_cast<int>(m.conflicts.size());
  Page p;
  p.id = "modconf:" + m.id + ":" + std::to_string(off);
  p.title = "CONFLICTS";
  std::ostringstream b;
  for (int i = off; i < n && i < off + kConflictsPerPage; ++i) {
    const ModMenuConflict& c = m.conflicts[i];
    if (i > off) b << "\n";
    b << c.target << " is also changed by " << c.otherName << ". "
      << (c.wins ? "This mod's version is used." : "That version is used; this mod's is not.");
  }
  b << "\nTwo mods that change the same file cannot both apply: the one higher in the load order wins.";
  p.body = b.str();
  if (off + kConflictsPerPage < n) {
    Row more;
    more.label = "MORE";
    const std::string next = "modconf:" + m.id + ":" + std::to_string(off + kConflictsPerPage);
    more.press = [next](Out& o) {
      o.page = next;
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  Row back;
  back.label = "BACK";
  back.press = [m](Out& o) {
    o.page = "mod:" + m.id;
    o.focus = 3;
  };
  p.rows.push_back(back);
  p.back = [m, off](Out& o) {
    o.page = off > 0 ? "modconf:" + m.id + ":" + std::to_string(std::max(0, off - kConflictsPerPage)) : "mod:" + m.id;
    o.focus = off > 0 ? 0 : 3;
  };
  return p;
}

std::string valueText(const DevMenuItemDef& d) {
  if (d.kind == DevMenuKind::Toggle) return (d.get && d.get()) ? "ON" : "OFF";
  if (d.kind == DevMenuKind::Slider) return std::to_string(d.get ? d.get() : d.min) + d.unit;
  if (d.kind == DevMenuKind::Readout) return d.text ? d.text() : std::string();
  return {};
}

Page pageOptions(const std::string& id, int off) {
  std::vector<DevMenuItemDef> rows;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_options.find(lower(id));
    if (it != g_options.end()) rows = it->second;
  }
  const ModMenuView v = modMenuCatalog()->view();
  const ModMenuEntry* mp = findMod(v, id);
  Page p;
  p.id = "modopts:" + id + ":" + std::to_string(off);
  p.title = (mp ? clip(mp->name, 24) : hostUpper(id)) + " OPTIONS";
  p.body = rows.empty() ? "This mod has no options." : "A changes a row. LEFT and RIGHT change toggles and sliders in place.";
  const int n = static_cast<int>(rows.size());
  const int per = kHostPageMaxRows - 1;
  for (int i = off; i < n && i < off + per; ++i) {
    const DevMenuItemDef d = rows[i];
    Row r;
    r.kind = d.kind;
    const std::string val = valueText(d);
    r.label = hostUpper(d.label) + (val.empty() ? "" : ": " + hostUpper(val));
    const std::string pid = p.id;
    const int row = i - off;
    auto apply = [d, pid, row](Out& o, int delta) {
      bool ok = true;
      if (d.kind == DevMenuKind::Toggle) ok = d.set && d.set((d.get && d.get()) ? 0 : 1);
      else if (d.kind == DevMenuKind::Slider)
        ok = d.set && d.set(std::max(d.min, std::min(d.max, (d.get ? d.get() : d.min) + delta * d.step)));
      else if (d.kind == DevMenuKind::Action) ok = d.set && d.set(0);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] option %s delta=%d -> %s", d.id.c_str(), delta, ok ? "ok" : "refused");
      o.page = pid;
      o.focus = row;
    };
    r.press = [apply, d](Out& o) { apply(o, d.kind == DevMenuKind::Slider ? 1 : 0); };
    if (d.kind == DevMenuKind::Toggle || d.kind == DevMenuKind::Slider)
      r.adjust = [apply](Out& o, int delta) {
        apply(o, delta);
        o.action = 2;
      };
    p.rows.push_back(r);
  }
  if (off + per < n) {
    Row more;
    more.label = "MORE";
    const std::string next = "modopts:" + id + ":" + std::to_string(off + per);
    more.press = [next](Out& o) {
      o.page = next;
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  if (p.rows.empty()) {
    Row back;
    back.label = "BACK";
    back.press = [id](Out& o) {
      o.page = "mod:" + id;
      o.focus = 0;
    };
    p.rows.push_back(back);
  }
  p.back = [id, off, per](Out& o) {
    o.page = off > 0 ? "modopts:" + id + ":" + std::to_string(std::max(0, off - per)) : "mod:" + id;
    o.focus = 0;
  };
  return p;
}

Page pageHow() {
  const ModMenuView v = modMenuCatalog()->view();
  Page p;
  p.id = "modhow";
  p.title = "HOW TO INSTALL MODS";
  p.body = "Your mods folder:\n" + wrappedPath(v.modsDir) +
           (v.acceptsZip ? "\nAdd mod folders or .zip files here." : "\nUnzip mod folders here.");
  if (v.canRescan) p.body += "\nChoose CHECK AGAIN, then enable your mods.";
  p.body += "\nRestart the game to apply mods.";
  p.rows = {folderRow(p.id, 0), refreshRow(v)};
  p.back = [](Out& o) {
    o.page = "mods:0";
    o.focus = 0;
  };
  return p;
}

Page pageFolderError() {
  Page p;
  p.id = "modfoldererror";
  p.title = "MODS FOLDER";
  p.body = "The mods folder could not be opened.\nOpen the mods folder in your Superman Returns installation, or choose HOW TO INSTALL MODS to see its location.";
  Row how;
  how.label = "HOW TO INSTALL MODS";
  how.press = [](Out& o) { o.page = "modhow"; o.focus = 0; };
  p.rows = {how};
  p.back = [](Out& o) { o.page = "mods:0"; o.focus = 0; };
  return p;
}

Page pageRestart() {
  const ModMenuView v = modMenuCatalog()->view();
  Page p;
  p.id = "modrestart";
  p.title = "RESTART";
  p.body = g_ctx == "IG" ? "The game restarts now to apply your mod changes. Progress since your last save is lost."
                         : "The game restarts now to apply your mod changes.";
  if (!v.canSave) p.body += "\nThis run's list is not saved to a file: the restarted game gets it directly.";
  Row now;
  now.label = "RESTART NOW";
  now.press = [](Out& o) {
    requestRestartImpl();
    o.action = 0;
  };
  Row later;
  later.label = "NOT NOW";
  later.press = [](Out& o) {
    o.page = "mods:0";
    o.focus = 0;
  };
  p.rows = {now, later};
  p.back = [](Out& o) {
    o.page = "mods:0";
    o.focus = 0;
  };
  return p;
}

Page pageCostumes(int off) {
  std::vector<CostumeDef> cs;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    cs = g_costumes;
  }
  const auto managed = modMenuCatalog()->costumes();
  cs.insert(cs.end(), managed.begin(), managed.end());
  const bool restart = std::any_of(cs.begin(), cs.end(), [](const auto& c) { return c.requiresRestart; });
  Page p;
  p.title = "COSTUMES";
  const int n = static_cast<int>(cs.size());
  const int per = kHostPageMaxRows - 1;
  off = n ? std::min(std::max(0, off), ((n - 1) / per) * per) : 0;
  p.id = "costumes:" + std::to_string(off);
  p.body = n == 0 ? "No costume packs are installed. Costume mods add playable suits here."
                 : restart ? "Choose a suit, then restart.\nThe game applies it when gameplay loads."
                           : "A puts the costume on. It changes the next time the game can switch the suit.";
  for (int i = off; i < n && i < off + per; ++i) {
    const CostumeDef c = cs[i];
    Row r;
    const bool avail = !c.available || c.available();
    r.label = clip(c.label, 27) + ((c.current && c.current()) ? (c.requiresRestart ? " (SELECTED)" : " (WEARING)") : "") +
              (avail ? "" : " (UNAVAILABLE)");
    const std::string pid = p.id;
    const int row = i - off;
    r.press = [c, avail, pid, row](Out& o) {
      const bool ok = avail && c.select && c.select();
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] costume %s -> %s", c.id.c_str(), ok ? "queued" : "refused");
      o.page = c.requiresRestart ? (ok ? "modrestart" : "modcostumeerror") : pid;
      o.focus = c.requiresRestart ? 0 : row;
    };
    p.rows.push_back(r);
  }
  if (off + per < n) {
    Row more;
    more.label = "MORE COSTUMES";
    const int next = off + per;
    more.press = [next](Out& o) {
      o.page = "costumes:" + std::to_string(next);
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  if (p.rows.empty()) {
    Row back;
    back.label = "BACK";
    back.press = [](Out& o) { o.action = 0; };
    p.rows.push_back(back);
  }
  if (off > 0)
    p.back = [off, per](Out& o) {
      o.page = "costumes:" + std::to_string(std::max(0, off - per));
      o.focus = 0;
    };
  else if (!managed.empty())
    p.back = [](Out& o) { o.page = "modextras"; o.focus = 0; };
  return p;
}

Page pageModExtras() {
  Page p;
  p.id = "modextras"; p.title = "MOD OPTIONS";
  p.body = "Costume packs add suits.\nThe retail costume menu stays available.";
  for (const auto& [label, page] : std::vector<std::pair<std::string, std::string>>{
           {"COSTUME PICKER", "costumes:0"}, {"HOW TO INSTALL MODS", "modhow"}, {"RESTART TO APPLY", "modrestart"}}) {
    Row row; row.label = label;
    row.press = [page](Out& o) { o.page = page; o.focus = 0; };
    p.rows.push_back(row);
  }
  p.back = [](Out& o) { o.page = "mods:0"; o.focus = 0; };
  return p;
}

Page pageCostumeError() {
  Page p;
  p.id = "modcostumeerror"; p.title = "COSTUME NOT SAVED";
  p.body = "The selection could not be saved.\nThis run may be read-only, or the pack\nis unavailable. No suit was changed.";
  Row back; back.label = "BACK";
  back.press = [](Out& o) { o.page = "costumes:0"; o.focus = 0; };
  p.rows.push_back(back);
  p.back = back.press;
  return p;
}

Page buildPage(const std::string& id) {
  const std::vector<std::string> parts = splitId(id.empty() ? "mods" : id);
  const std::string& name = parts[0];
  if (name == "mod" && parts.size() >= 2) return pageDetails(parts[1]);
  if (name == "modfiles" && parts.size() >= 2) return pageFiles(parts[1], toInt(parts, 2));
  if (name == "modconf" && parts.size() >= 2) return pageConflicts(parts[1], toInt(parts, 2));
  if (name == "modopts" && parts.size() >= 2) return pageOptions(parts[1], toInt(parts, 2));
  if (name == "modhow") return pageHow();
  if (name == "modextras") return pageModExtras();
  if (name == "modcostumeerror") return pageCostumeError();
  if (name == "modfoldererror") return pageFolderError();
  if (name == "modrestart") return pageRestart();
  if (name == "costumes") return pageCostumes(toInt(parts, 1));
  return pageList(toInt(parts, 1));
}

}  // namespace

std::shared_ptr<ModCatalog> modMenuMakeFallbackCatalog(const std::string& modsDir, const std::string& listFile,
                                                       const std::vector<std::string>& bootList) {
  return std::make_shared<FallbackCatalog>(modsDir, listFile, bootList);
}

void modMenuSetCatalog(std::shared_ptr<ModCatalog> c) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_catalog = std::move(c);
}

std::shared_ptr<ModCatalog> modMenuCatalog() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_catalog) g_catalog = modMenuMakeModcoreCatalog();   // modcore's manager, once it is in the tree
  if (!g_catalog)
    g_catalog = modMenuMakeFallbackCatalog(modOverlayModsDir(), modOverlayListFile(), modOverlayEnabled());
  return g_catalog;
}

bool modMenuOwnsPage(const std::string& page) {
  return page.rfind("mod", 0) == 0 || page.rfind("costume", 0) == 0;
}

void modMenuPagesCommand(const std::string& args, std::vector<ModReply>* out) {
  // iter-465 modlib: the MODS entries (MAIN MENU > MODS; lane E L-c: not from the pause menu) open the mod
  // library overlay (kernel/mod_library.hpp) instead of these Apt list pages, which are RETIRED
  // (SR_MOD_LIBRARY=0 restores them for a developer A/B). The Apt menu closes back to its caller
  // at once (SR_DTEnd action 0: the caller re-shows its own row), and the overlay draws over it.
  // The costume page and the other sub-pages are reachable only from the list, so they retire
  // with it (suit inclusion is the library's details view; wearing is the Fortress costume screen).
  if (modlib::enabled()) {
    std::istringstream ss(args);
    std::string verb, page, ctx;
    ss >> verb >> page >> ctx;
    if (verb == "open" && (page == "mods" || page.rfind("mods:", 0) == 0)) {
      modlib::requestOpen(ctx.empty() ? std::string("FE") : ctx);
      out->push_back({"SR_DTEnd", {ModArg::Int(0), ModArg::Int(0), ModArg::Int(0)}});
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MODMENU] %s -> the mod library overlay", args.c_str());
      return;
    }
  }
  std::string ctx = g_ctx;
  hostPagesCommand(args, buildPage, out, &ctx, "MODMENU");
  g_ctx = ctx;
}

void modMenuRegisterOption(const std::string& modId, const DevMenuItemDef& def) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto& rows = g_options[lower(modId)];
  for (auto& r : rows)
    if (r.id == def.id) {
      r = def;
      r.tool = nullptr;
      return;
    }
  rows.push_back(def);
  rows.back().tool = nullptr;
}

void modMenuRegisterCostume(const CostumeDef& def) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& c : g_costumes)
    if (c.id == def.id) {
      c = def;
      return;
    }
  g_costumes.push_back(def);
}

int modMenuCostumeCount() {
  size_t registered;
  { std::lock_guard<std::mutex> lk(g_mu); registered = g_costumes.size(); }
  return static_cast<int>(registered + modMenuCatalog()->costumes().size());
}

bool modMenuRestartRequested() { return g_restart.load(); }

void modMenuRequestRestart() { requestRestartImpl(); }

void modMenuSpawnRestartIfRequested() {
  if (!g_restart.exchange(false)) return;
#ifdef _WIN32
  int gen = 0;
  if (const char* g = std::getenv("SR_RESTART_GENERATION")) gen = std::atoi(g);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d", gen + 1);
  SetEnvironmentVariableA("SR_RESTART_GENERATION", buf);
  std::snprintf(buf, sizeof buf, "%lu", static_cast<unsigned long>(GetCurrentProcessId()));
  SetEnvironmentVariableA("SR_RESTART_PARENT_PID", buf);
  std::string sessionList;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    sessionList = g_restartSessionList;
  }
  if (!sessionList.empty()) SetEnvironmentVariableA("SR_MODS", sessionList.c_str());
  // A roll's frame dump (SR_PRESENT_DUMP_DIR, test infrastructure; players never set it): the
  // child writes next to the parent's frames, never over them.
  if (const char* dd = std::getenv("SR_PRESENT_DUMP_DIR")) {
    if (*dd) {
      const std::string child = std::string(dd) + "_r" + std::to_string(gen + 1);
      SetEnvironmentVariableA("SR_PRESENT_DUMP_DIR", child.c_str());
      std::error_code ec;
      fs::create_directories(child, ec);
    }
  }
  std::wstring exe(32768, L'\0');
  const DWORD len = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  exe.resize(len);
  std::wstring cmd = GetCommandLineW();
  // iter-465 player: the child gets NO stdio and no console. It logs to its own file
  // (app/main.cpp: superman_returns[_<id>_r<gen>].log). Before this it silently received this
  // process's stdout/stderr - in the owner's 2026-10-02 test a launcher's pipe whose reader was
  // tied to THIS process's lifetime: the launcher stopped draining it when this process exited,
  // and the child's first log line into the full pipe froze it at the EA logo
  // (app/stdio_guard.hpp has the receipts). DETACHED_PROCESS also keeps a console-subsystem
  // child from opening a console window of its own.
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = nullptr;
  si.hStdError = nullptr;
  PROCESS_INFORMATION pi{};
  if (len > 0 && CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                                nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  // The log is closed by now (the caller flushed it first, so the child's log never mixes with
  // this one); the child logs "[MODMENU] restarted by the MODS menu" at its boot.
#endif
}

void modMenuResetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_catalog.reset();
  g_ctx = "FE";
  g_options.clear();
  g_costumes.clear();
  g_restart = false;
  g_restartSessionList.clear();
  g_folderForTest = nullptr;
}

void modMenuSetRestartHandlerForTest(void (*fn)()) { g_restartForTest = fn; }
void modMenuSetFolderHandlerForTest(bool (*fn)(const std::string&)) { g_folderForTest = fn; }

}  // namespace kernel
}  // namespace sr
