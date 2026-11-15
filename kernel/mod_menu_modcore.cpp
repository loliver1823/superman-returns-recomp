// iter-464 (modmenu lane): the MODS menu's adapter over the modcore lane's mod manager
// (kernel/mod_manager.hpp, branch mods/ship, docs/MODS.md section 9 "The menu API").
//
// It compiles to a stub returning nullptr until kernel/mod_manager.hpp is in the tree: the menu
// then runs on its built-in fallback catalog (kernel/mod_menu.cpp). When modcore's manager
// lands, this file wires it in with no other change: modMenuCatalog() asks here first.
// Written against modcore's header as it stood on 2026-10-02 ~05:15 (uncommitted in
// sr-wt-modship); whichever lane merges second fixes any drift here.
//
// Charter: PORT FEATURE host UI; reads modcore's state, writes only through its API.
#include "mod_menu.hpp"
#include "mod_overlay.hpp"

#include "file_io.hpp"
#include "../app/log.hpp"

#if defined(__has_include)
#if __has_include("mod_manager.hpp")
#define SR_MODMENU_HAVE_MODCORE 1
#endif
#endif

#ifdef SR_MODMENU_HAVE_MODCORE
#include "mod_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>

namespace sr {
namespace kernel {

namespace {
namespace fs = std::filesystem;

std::string upperName(const std::string& s) { return hostUpper(s); }

class ModcoreCatalog final : public ModCatalog {
 public:
  ModMenuView view() override {
    ModMenuView v;
    v.modsDir = modMenuModsDir();
#ifdef _WIN32
    std::replace(v.modsDir.begin(), v.modsDir.end(), '/', '\\');
#endif
    v.canSave = !modMenuAutomated();
    v.why = v.canSave ? std::string() : "an automated run never writes the player's mods.json";
    v.pendingRestart = modMenuRestartRequired();
    v.acceptsZip = true;
    v.autoEnableNew = true;
    // Mounts are immutable for this process. Until discovery-only rescan is
    // implemented, offer an honest restart instead of a non-working CHECK AGAIN.
    v.canRescan = false;
    const fs::path disc = fs::u8path(v.modsDir).parent_path() / "Superman Returns (USA)";
    const std::vector<ModConflictInfo> conflicts = modMenuConflicts();
    std::lock_guard<std::mutex> lk(mu_);
    for (const ModInfo& mi : modMenuList()) {
      ModMenuEntry e;
      e.id = mi.id;
      e.name = upperName(mi.name.empty() ? mi.id : mi.name);
      e.author = mi.author;
      e.version = mi.version;
      e.description = mi.description;
      e.category = modCategoryLabel(mi.category);
      e.preview = mi.preview;
      e.enabled = mi.enabled;
      e.active = mi.mounted;
      const bool okish = mi.status == ModStatus::Ok || mi.status == ModStatus::Disabled || mi.status == ModStatus::Off;
      if (!okish) {
        e.state = ModMenuState::Failed;
        e.reason = mi.reason.empty() ? std::string(modStatusName(mi.status)) : mi.reason;
      }
      for (const ModConflictInfo& c : conflicts) {
        const bool won = c.winnerId == mi.id, lost = c.loserId == mi.id;
        if (!won && !lost) continue;
        e.conflicts.push_back({c.what, won ? c.loserId : c.winnerId, upperName(won ? c.loser : c.winner), won});
      }
      if (!e.conflicts.empty() && e.state == ModMenuState::Ok) {
        e.state = ModMenuState::Conflict;
        const ModMenuConflict& c = e.conflicts.front();
        e.reason = c.target + " is also changed by " + c.otherName + (c.wins ? " (this mod wins)" : " (that one wins)");
        if (e.conflicts.size() > 1) e.reason += " and " + std::to_string(e.conflicts.size() - 1) + " more";
      }
      // what it changes: the game files under the mod's content folder (a zip's extracted copy)
      auto it = changes_.find(mi.id);
      if (it == changes_.end()) {
        std::vector<ModMenuChange> ch;
        if (!mi.root.empty())
          for (const std::string& f : modMenuWalkGameFiles(mi.root, modPathU8(disc))) {
            std::error_code ec;
            ch.push_back({f, !fs::exists(disc / f, ec)});
          }
        it = changes_.emplace(mi.id, std::move(ch)).first;
      }
      e.changes = it->second;
      v.mods.push_back(std::move(e));
    }
    return v;
  }

  bool setEnabled(const std::string& id, bool on, std::string* err) override {
    if (modMenuSetEnabled(id, on)) return true;
    if (err) *err = "the mod manager refused it";
    return false;
  }

  bool move(const std::string& id, int delta, std::string* err) override {
    if (modMenuMove(id, delta)) return true;
    if (err) *err = "the mod manager refused it";
    return false;
  }

  void rescan() override {
    std::lock_guard<std::mutex> lk(mu_);
    changes_.clear();
  }

  std::vector<CostumeDef> costumes() override {
    const auto entries = modCostumeList();
    if (entries.empty()) return {};
    std::vector<CostumeDef> result;
    auto add = [&](const std::string& id, const std::string& label, const std::string& source) {
      CostumeDef c;
      c.id = id; c.label = upperName(label); c.source = source; c.requiresRestart = true;
      c.available = [id] { return id.empty() || modCostumeRegistryReady(); };
      c.current = [id] { return modCostumeSelected() == id; };
      c.select = [id] { return modCostumeSelectForRestart(id); };
      result.push_back(std::move(c));
    };
    add("", "RETAIL DEFAULT", "Superman Returns");
    for (const auto& c : entries) add(c.id, c.name, c.source);
    return result;
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::vector<ModMenuChange>> changes_;
};

}  // namespace

std::shared_ptr<ModCatalog> modMenuMakeModcoreCatalog() {
  // The UI must operate on the loader that actually mounted this process.
  if (!modManagerOverlayEnabled()) return nullptr;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODMENU] catalog: the mod manager (kernel/mod_manager.hpp)");
  return std::make_shared<ModcoreCatalog>();
}

}  // namespace kernel
}  // namespace sr

#else  // no mod manager in the tree yet: the menu's fallback catalog serves

namespace sr {
namespace kernel {
std::shared_ptr<ModCatalog> modMenuMakeModcoreCatalog() { return nullptr; }
}  // namespace kernel
}  // namespace sr

#endif
