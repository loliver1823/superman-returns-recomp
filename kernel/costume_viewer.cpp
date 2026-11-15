// iter-465 viewer lane: THE COSTUME VIEWER - state, commands and model sources (see the header).
#include "costume_viewer.hpp"

#include "costume_viewer_model.hpp"
#include "host_input.hpp"
#include "mod_costume_data.hpp"
#include "mod_manager.hpp"
#include "../app/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

namespace sr::kernel {
namespace {
namespace fs = std::filesystem;
using cview::Model;

struct State {
  std::mutex mu;
  bool open = false, popup = false;
  std::string id;
  bool locked = false;
  float yaw = 3.49f, yawTarget = 3.49f, zoom = 1.0f, zoomTarget = 1.0f;
  double seconds = 0;
  std::chrono::steady_clock::time_point last{};
  std::map<std::string, std::shared_ptr<const Model>> cache;
  std::map<std::string, std::string> failed;   // id -> why
  std::string loading;                          // id being built
};
State& st() { static State s; return s; }
std::atomic<int16_t> g_padRX{0}, g_padRY{0};

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }
bool readFile(const fs::path& p, std::vector<uint8_t>* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out->empty();
}

// Loose files of every mounted layer (the engine's own _workinprogress names): lower-case file
// name -> host path, first layer wins.
const std::map<std::string, fs::path>& looseFiles() {
  static std::map<std::string, fs::path> files;
  static std::once_flag once;
  std::call_once(once, [] {
    for (const auto& layer : modActiveLayers())
      for (const auto& r : layer.roots) {
        std::error_code ec;
        const fs::path base = fs::u8path(r) / "DATA/_workinprogress";
        if (!fs::is_directory(base, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
          if (it->is_regular_file(ec)) files.emplace(lower(modPathU8(it->path().filename())), it->path());
      }
  });
  return files;
}
bool loose(const std::string& name, std::vector<uint8_t>* out, fs::path* where = nullptr) {
  const auto& f = looseFiles();
  auto it = f.find(lower(name));
  if (it == f.end() || !readFile(it->second, out)) return false;
  if (where) *where = it->second;
  return true;
}
std::string attr(const std::string& xml, const std::string& elem, const std::string& name) {
  const std::regex re("<" + elem + "\\b[^>]*\\b" + name + "=\"([^\"]*)\"", std::regex::icase);
  std::smatch m;
  return std::regex_search(xml, m, re) ? m[1].str() : std::string();
}

// The retail bundles a suit's parts may come from (the player's own disc).
struct Disc {
  std::string root;
  std::map<std::string, std::vector<uint8_t>> top, part;
  bool topEntry(const std::string& file, const std::string& name, std::vector<uint8_t>** out, std::string* err) {
    const std::string key = file + "/" + name;
    auto it = top.find(key);
    if (it == top.end()) {
      std::vector<uint8_t> b;
      if (!cview::astReadFileEntry(root + "/DATA/" + file, name, &b, err)) return false;
      it = top.emplace(key, std::move(b)).first;
    }
    *out = &it->second;
    return true;
  }
  bool nested(const std::string& file, const std::string& topName, const std::string& partName, const std::string& entry,
              std::vector<uint8_t>* out, std::string* err) {
    std::vector<uint8_t>* t = nullptr;
    if (!topEntry(file, topName, &t, err)) return false;
    if (partName.empty()) return cview::astReadNested(*t, entry, out, err);
    const std::string key = file + "/" + topName + "/" + partName;
    auto it = part.find(key);
    if (it == part.end()) {
      std::vector<uint8_t> b;
      if (!cview::astReadNested(*t, partName, &b, err)) return false;
      it = part.emplace(key, std::move(b)).first;
    }
    return cview::astReadNested(it->second, entry, out, err);
  }
};

// An entity / cloth definition as text: a loose file, else the retail compiled document.
bool defText(Disc& disc, const std::string& name, std::string* text) {
  std::vector<uint8_t> b, t;
  std::string err;
  if (!loose(name + ".xml", &b)) {
    if (!disc.nested("objtiv.AST", "SUPERMENACTORWORLDFLOW", "SUPERMENACTORWORLDFLOW_OTHER", name, &b, &err) &&
        !disc.nested("objtiv.AST", "PLAYABLEBIZ", "PLAYABLEBIZ_OTHER", name, &b, &err)) return false;
  }
  if (b.size() > 4 && b[0] == '<') { text->assign(b.begin(), b.end()); return true; }
  if (!modCompiledXmlToText(b, &t, &err)) return false;
  text->assign(t.begin(), t.end());
  return true;
}

std::shared_ptr<const Model> build(const std::string& id, std::string* why) {
  auto m = std::make_shared<Model>();
  Disc disc;
  disc.root = modDiscRoot();
  std::string entity;
  std::vector<std::pair<std::string, std::string>> banks;
  if (id.rfind("retail/", 0) == 0) {
    const int n = std::atoi(id.c_str() + 7);
    static const char* kEnt[] = {"SupermanEnt", "SupermanGrayEnt", "SupermanClassicEnt"};
    if (n < 1 || n > 3) { *why = "unknown retail suit"; return nullptr; }
    entity = kEnt[n - 1];
  } else {
    ModCostumeInfo c;
    if (!modCostumeFindActive(id, &c)) { *why = "suit not active"; return nullptr; }
    entity = c.entity;
    banks = c.banks;
  }
  std::string ent, err;
  if (!defText(disc, entity, &ent)) { *why = "entity " + entity + " not found"; return nullptr; }
  const std::string asset = attr(ent, "visrep", "asset"), clothDef = attr(ent, "visrep", "clothdef");
  const std::string project = attr(ent, "animsys", "project");
  // body: a loose arena (+ the loose arenas beside it for its rasters), else the retail bundle
  std::vector<std::vector<uint8_t>> arenas(1);
  fs::path bodyPath;
  if (loose(asset + ".rx2", &arenas[0], &bodyPath)) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(bodyPath.parent_path(), ec)) {
      std::vector<uint8_t> b;
      if (e.path() != bodyPath && lower(modPathU8(e.path().extension())) == ".rx2" && readFile(e.path(), &b) && b.size() > 8 &&
          b[0] == 0x89 && b[1] == 'R')
        arenas.push_back(std::move(b));
    }
  } else {
    const bool biz = lower(asset).find("bizarro") != std::string::npos;
    const std::string topName = biz ? "PLAYABLEBIZ" : "SUPERMENACTORWORLDFLOW";
    if (!disc.nested("objtiv.AST", topName, topName + "_RENDER", asset, &arenas[0], &err)) { *why = "body " + asset + ": " + err; return nullptr; }
    std::vector<uint8_t> render;
    std::vector<uint8_t>* t = nullptr;
    if (disc.topEntry("objtiv.AST", topName, &t, &err) && cview::astReadNested(*t, topName + "_RENDER", &render, &err))
      cview::astForEachNested(render, [&](const std::vector<uint8_t>& p) {
        if (p.size() > 8 && p[0] == 0x89 && p[1] == 'R' && p != arenas[0]) arenas.push_back(p);
        return true;
      }, &err);
  }
  if (!cview::loadBody(arenas, m.get(), &err)) { *why = "body: " + err; return nullptr; }
  arenas.clear();
  // cape: the cloth definition's RSS surface and colour map
  if (!clothDef.empty()) {
    std::string cloth;
    if (defText(disc, clothDef, &cloth)) {
      const std::string rssName = attr(cloth, "model", "rssModelName");
      std::string ddsName;
      {
        const std::regex re("ddsTextureName=\"([^\"]*)\"", std::regex::icase);
        std::smatch mm;
        if (std::regex_search(cloth, mm, re)) ddsName = mm[1].str();
      }
      std::vector<uint8_t> rss, dds;
      bool haveRss = loose(rssName + ".rss", &rss) ||
                     disc.nested("preload.AST", "PRELOAD_OTHER", "", rssName, &rss, &err);
      if (!ddsName.empty() && !loose(ddsName + ".dds", &dds))
        if (!disc.nested("objtiv.AST", "SUPERMENACTORWORLDFLOW", "SUPERMENACTORWORLDFLOW_OTHER", ddsName, &dds, &err))
          disc.nested("objtiv.AST", "PLAYABLEBIZ", "PLAYABLEBIZ_OTHER", ddsName, &dds, &err);
      if (haveRss && !cview::loadCape(rss, dds, m.get(), &err))
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[COSTUME_VIEWER] %s cape: %s", id.c_str(), err.c_str());
    }
  }
  // animation: the CCT of its project and the flight bank (own banks, own project, else retail)
  std::vector<uint8_t> cct, bank;
  std::string projText;
  bool ownProject = false;
  if (!project.empty() && lower(project) != "supermananims" && loose(project + ".xml", &cct)) {
    projText.assign(cct.begin(), cct.end());
    const std::string rcb = attr(projText, "project", "rcbfile");
    const std::regex re("<bank\\b[^>]*name=\"([^\"]*)\"", std::regex::icase);
    std::smatch mm;
    std::vector<uint8_t> b;
    if (std::regex_search(projText, mm, re) && loose(rcb + ".rcb", &cct) && loose(mm[1].str() + ".bnk", &b)) {
      bank.swap(b);
      ownProject = true;
    }
  }
  if (!ownProject) {
    if (!disc.nested("baseworl.AST", "BASEWORLDFLOW_ANIM", "", "SUPERMANCCTPROJECT", &cct, &err)) { *why = "CCT: " + err; return nullptr; }
    bool own = false;
    for (const auto& [name, path] : banks)
      if (lower(name) == "superman_flight" && readFile(fs::u8path(path), &bank)) own = true;
    if (!own && !disc.nested("baseworl.AST", "BASEWORLDFLOW_ANIM", "", "SUPERMAN_FLIGHT", &bank, &err)) { *why = "bank: " + err; return nullptr; }
  }
  if (!cview::loadClip(cct, bank, "Hover", m.get(), &err))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[COSTUME_VIEWER] %s clip: %s (bind pose)", id.c_str(), err.c_str());
  std::vector<float> sk;
  cview::evalSkin(*m, 0.0, 0.0f, &sk);
  cview::posedBounds(*m, sk, m->boundsMin, m->boundsMax);
  size_t tris = 0;
  for (const auto& me : m->meshes) tris += me.idx.size() / 3;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_VIEWER] %s: %s (%zu meshes, %zu tris, %zu textures, %d joints, clip %d keys%s)",
              id.c_str(), asset.c_str(), m->meshes.size(), tris, m->textures.size(), m->skel.n, m->clip.frames,
              ownProject ? ", own project" : banks.empty() ? "" : ", own banks");
  return m;
}

void startLoad(const std::string& id) {
  State& s = st();
  if (id.empty() || s.cache.count(id) || s.failed.count(id) || !s.loading.empty()) return;
  s.loading = id;
  std::thread([id] {
    std::string why;
    std::shared_ptr<const Model> m;
    try { m = build(id, &why); } catch (const std::exception& e) { why = e.what(); }
    State& s2 = st();
    std::lock_guard<std::mutex> lk(s2.mu);
    if (m) s2.cache[id] = m;
    else {
      s2.failed[id] = why;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COSTUME_VIEWER] %s: no model (%s)", id.c_str(), why.c_str());
    }
    s2.loading.clear();
    if (!s2.id.empty() && s2.id != id) startLoad(s2.id);
  }).detach();
}

}  // namespace

std::vector<CostumeViewerInfo> costumeViewerInfo() {
  std::vector<CostumeViewerInfo> out;
  for (int i = 1; i <= 3; ++i) out.push_back({"retail/" + std::to_string(i), "SUPERMAN", ""});
  for (const auto& c : modCostumeList()) {
    std::string ch = c.characterName;
    if (ch.empty()) {
      const std::string up = [&] { std::string u = c.name; for (char& x : u) x = char(std::toupper(uint8_t(x))); return u; }();
      if (c.id == "builtin/bizarro") ch = "BIZARRO";
      else if (up.find("ZOD") != std::string::npos) ch = "GENERAL ZOD";        // test packs without the field
      else if (up.find("CYBORG") != std::string::npos) ch = "CYBORG SUPERMAN";
      else ch = "SUPERMAN";
    }
    out.push_back({c.id, ch, c.description});
  }
  return out;
}

bool costumeViewerEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("SR_COSTUME_VIEWER");
    if (e && *e) return e[0] != '0';
    return modNativeCostumesEnabled();
  }();
  return on;
}

void costumeViewerCommand(const std::string& args) {
  if (!costumeViewerEnabled()) return;
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  const size_t sp = args.find(' ');
  const std::string cmd = args.substr(0, sp), rest = sp == std::string::npos ? std::string() : args.substr(sp + 1);
  if (cmd == "show") {
    const size_t sp2 = rest.find(' ');
    const std::string id = lower(rest.substr(0, sp2));
    const bool locked = sp2 != std::string::npos && std::atoi(rest.c_str() + sp2 + 1) != 0;
    if (!s.open || id != s.id) {
      SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_VIEWER] show %s%s", id.c_str(), locked ? " (locked)" : "");
      s.seconds = 0;
    }
    s.open = true;
    s.popup = false;
    s.id = id;
    s.locked = locked;
    startLoad(id);
  } else if (cmd == "key") {   // an input the page passed on to the retail screen (diagnostic line)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_VIEWER] key %s passed to the retail page", rest.c_str());
  } else if (cmd == "hide") {
    s.popup = true;
  } else if (cmd == "close") {
    if (s.open) SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_VIEWER] close");
    s.open = false;
    s.popup = false;
  }
}

void costumeViewerNotePad(int16_t rx, int16_t ry) {
  g_padRX.store(rx, std::memory_order_relaxed);
  g_padRY.store(ry, std::memory_order_relaxed);
}

void costumeViewerOnSdlEvent(const void* ev) {
  if (!ev || !costumeViewerEnabled() || !hostinput::realInputAllowed()) return;
  const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  static bool drag = false;
  if (!s.open || s.popup) { drag = false; return; }
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) drag = true;
  else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) drag = false;
  else if (e.type == SDL_EVENT_MOUSE_MOTION && drag) {   // drag: left / right turns, up / down zooms
    s.yawTarget += float(e.motion.xrel) * 0.012f;
    s.zoomTarget = std::clamp(s.zoomTarget * std::exp(-float(e.motion.yrel) * 0.006f), 1.0f, 2.6f);
  }
  else if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.integer_y != 0)   // SDL3: whole ticks (SDL2's int y)
    s.zoomTarget = std::clamp(s.zoomTarget * (e.wheel.integer_y > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 2.6f);
}

CostumeViewerFrame costumeViewerFrame() {
  CostumeViewerFrame f;
  State& s = st();
  static const char* dev = std::getenv("SR_COSTUME_VIEWER_DEV");   // developer: show <id> at once
  if (dev && *dev) {
    static bool once = false;
    if (!once) { once = true; costumeViewerCommand(std::string("show ") + dev + " 0"); }
  }
  std::lock_guard<std::mutex> lk(s.mu);
  const auto now = std::chrono::steady_clock::now();
  double dt = s.last.time_since_epoch().count() ? std::chrono::duration<double>(now - s.last).count() : 0.0;
  s.last = now;
  dt = std::clamp(dt, 0.0, 0.1);
  if (!s.open || s.popup) return f;
  s.seconds += dt;
  // turning: a slow turntable, the right stick (players), and the Apt left/right presses
  // the right stick the game reads this frame (players' pads, the harness on rolls): X turns, Y zooms
  float stick = 0;
  {
    const float rx = g_padRX.load(std::memory_order_relaxed) / 32767.0f;
    const float ry = g_padRY.load(std::memory_order_relaxed) / 32767.0f;
    if (std::fabs(rx) > 0.25f) stick = rx;
    if (std::fabs(ry) > 0.25f) s.zoomTarget = std::clamp(s.zoomTarget * (1.0f + float(dt) * ry * 1.6f), 1.0f, 2.6f);
  }
  s.yawTarget += float(dt) * (stick != 0 ? stick * 2.6f : 0.42f);
  const float k = 1.0f - std::exp(-float(dt) * 8.0f);
  s.yaw += (s.yawTarget - s.yaw) * k;
  s.zoom += (s.zoomTarget - s.zoom) * k;
  f.visible = true;
  f.id = s.id;
  f.silhouette = s.locked;
  f.seconds = s.seconds;
  f.yaw = s.yaw;
  f.zoom = s.zoom;
  auto it = s.cache.find(s.id);
  if (it != s.cache.end()) f.model = it->second;
  return f;
}

}  // namespace sr::kernel
