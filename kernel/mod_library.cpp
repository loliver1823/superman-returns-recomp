// iter-465 modlib lane: the mod library's kernel half (kernel/mod_library.hpp, docs/MODS.md
// "The mod library"): open / close, input routing and the data model over the mod manager.
#include "mod_library.hpp"

#include "../app/log.hpp"
#include "guest_context.hpp"   // hostWindowFocused
#include "host_automation.hpp"
#include "mod_bridge.hpp"      // modOptionGet / modOptionSet
#include "mod_manager.hpp"
#include "mod_menu.hpp"        // modMenuRequestRestart

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#endif

namespace sr {
namespace kernel {
namespace modlib {

namespace {
namespace fs = std::filesystem;

std::string lowerS(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool containsCI(const std::string& hay, const std::string& needleLower) {
  if (needleLower.empty()) return true;
  return lowerS(hay).find(needleLower) != std::string::npos;
}

// ── open / close + input ──────────────────────────────────────────────────────────────────
std::mutex g_inMu;
std::vector<InputEvent> g_queue;
std::atomic<bool> g_shown{false};
std::atomic<bool> g_openReq{false};
std::atomic<bool> g_latch{false};       // closed, waiting for the game input to go neutral
std::atomic<bool> g_hover{false};
std::mutex g_ctxMu;
std::string g_ctx;
std::atomic<int64_t> g_reqAt{0};
std::atomic<uint8_t> g_page{0};          // iter-466 gfx: OverlayPage of the open request
int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

void push(const InputEvent& e) {
  std::lock_guard<std::mutex> lk(g_inMu);
  if (g_queue.size() < 512) g_queue.push_back(e);
}

const char* kCategoryOrder[] = {"all", "characters", "missions", "worlds", "gameplay",
                                "visuals", "audio", "interface", "other", "builtin"};
}  // namespace

bool enabled() {
  // read per call (only on an SR_Menu "open"), so a test can switch it
  const char* e = std::getenv("SR_MOD_LIBRARY");
  return !(e && e[0] == '0');
}

void requestOpen(const std::string& ctx) { requestOpenPage(OverlayPage::Mods, ctx); }

OverlayPage openPage() { return OverlayPage(g_page.load(std::memory_order_acquire)); }

void requestOpenPage(OverlayPage page, const std::string& ctx) {
  g_page.store(uint8_t(page), std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(g_ctxMu);
    g_ctx = ctx;
  }
  {
    std::lock_guard<std::mutex> lk(g_inMu);
    g_queue.clear();
  }
  // Shown from now on: the game reads a neutral pad at once, before the first overlay frame.
  g_reqAt.store(nowMs(), std::memory_order_relaxed);
  g_shown.store(true, std::memory_order_release);
  g_openReq.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] open requested (%s, %s)", ctx.c_str(),
              page == OverlayPage::Settings ? "PC SETTINGS" : page == OverlayPage::Achievements ? "ACHIEVEMENTS" : "MODS");
}

bool takeOpenRequest(std::string* ctx) {
  if (!g_openReq.exchange(false, std::memory_order_acq_rel)) return false;
  if (ctx) {
    std::lock_guard<std::mutex> lk(g_ctxMu);
    *ctx = g_ctx;
  }
  return true;
}

void setShown(bool shown) {
  // lane E L-a (the review's P1-2): an overlay that finishes closing while ANOTHER open request waits leaves the routing
  // shown - the request is the next page's (the game keeps its neutral pad; that page's overlay takes it next frame, or
  // blockGameInput drops it after 3 s)
  if (!shown && g_openReq.load(std::memory_order_acquire)) return;
  const bool was = g_shown.exchange(shown, std::memory_order_acq_rel);
  if (was && !shown) g_latch.store(true, std::memory_order_release);   // release latch
  if (shown && !was) {
    std::lock_guard<std::mutex> lk(g_inMu);
    g_queue.clear();
  }
}

bool isOpen() { return g_shown.load(std::memory_order_acquire); }

bool blockGameInput(bool gameInputHeld) {
  if (g_shown.load(std::memory_order_acquire)) {
    // Never strand the game: an open request no overlay frame picked up in 3 s (a present
    // path without the host overlay) is dropped.
    if (g_openReq.load(std::memory_order_acquire) && nowMs() - g_reqAt.load(std::memory_order_relaxed) > 3000) {
      g_openReq.store(false, std::memory_order_release);
      g_shown.store(false, std::memory_order_release);
      g_latch.store(true, std::memory_order_release);   // lane E L-a (the review's P2-1): as a close - a button held
                                                        // through the drop waits for its release
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODLIB] open request not drawn within 3 s - dropped");
      return gameInputHeld;
    }
    return true;
  }
  if (!g_latch.load(std::memory_order_acquire)) return false;
  if (gameInputHeld) return true;
  g_latch.store(false, std::memory_order_release);
  return false;
}

void onSdlEvent(const SDL_Event& e) {
  if (!g_shown.load(std::memory_order_acquire)) return;
  if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP && e.type != SDL_EVENT_TEXT_INPUT) return;
  // Input isolation (host_automation.hpp): a background roll never reads the desk keyboard;
  // its keys arrive through the harness channel, which the UI polls itself.
  if (!automation::policy().realInput || !hostWindowFocused()) return;
  InputEvent ev;
  if (e.type == SDL_EVENT_TEXT_INPUT) {
    ev.kind = InputEvent::Text;
    std::snprintf(ev.text, sizeof ev.text, "%s", e.text.text);
  } else {
    ev.kind = InputEvent::Key;
    ev.scancode = static_cast<int>(e.key.scancode);
    ev.down = e.type == SDL_EVENT_KEY_DOWN;
    ev.repeat = e.key.repeat;
    ev.mods = static_cast<uint16_t>(e.key.mod);   // SDL3's SDL_KMOD_* bits = SDL2's KMOD_* bits
  }
  push(ev);
}

void onMouse(int kind, float sx, float sy, int button, int wheel) {
  if (!g_shown.load(std::memory_order_acquire)) return;
  InputEvent ev;
  switch (kind) {
    case 1: ev.kind = InputEvent::MouseMove; break;
    case 2: ev.kind = InputEvent::MouseDown; break;
    case 3: ev.kind = InputEvent::MouseUp; break;
    case 4: ev.kind = InputEvent::Wheel; break;
    default: return;
  }
  ev.x = sx;
  ev.y = sy;
  ev.button = button;
  ev.wheel = wheel;
  push(ev);
}

bool wantsMouse() { return g_shown.load(std::memory_order_acquire); }
void setMouseHover(bool hover) { g_hover.store(hover, std::memory_order_relaxed); }
bool mouseHover() { return g_hover.load(std::memory_order_relaxed); }

void injectForTest(const InputEvent& e) { push(e); }

std::vector<InputEvent> drainInput() {
  std::lock_guard<std::mutex> lk(g_inMu);
  std::vector<InputEvent> out;
  out.swap(g_queue);
  return out;
}

double autoOpenSeconds() {
  static const double v = [] {
    const char* e = std::getenv("SR_MOD_LIBRARY_AUTOOPEN");
    return e && *e ? std::atof(e) : -1.0;
  }();
  return v;
}

const std::string& shotDir() {
  static const std::string v = [] {
    const char* e = std::getenv("SR_MOD_LIBRARY_SHOTS");
    return std::string(e && *e && !(e[0] == '0' && e[1] == 0) ? e : "");
  }();
  return v;
}

// ── the data model ────────────────────────────────────────────────────────────────────────
LibSnapshot snapshot() {
  LibSnapshot s;
  s.automated = modMenuAutomated();
  s.canSave = !s.automated || [] {
    const char* w = std::getenv("SR_MODS_ALLOW_WRITE");
    return w && w[0] == '1';
  }();
  // iter-465 (player merge): the relaunch is safe in automated runs too (the child gets no stdio,
  // sr_drive follows it: followRestart), as on the MODS menu's own RESTART NOW path.
  s.canRestart = true;
  s.restartRequired = modMenuRestartRequired();
  s.modsDir = modMenuModsDir();
#ifdef _WIN32
  std::replace(s.modsDir.begin(), s.modsDir.end(), '/', '\\');
#endif
  const std::vector<ModConflictInfo> conflicts = modMenuConflicts();
  int pos = 0;
  for (const ModInfo& mi : modMenuList()) {
    LibEntry e;
    e.id = mi.id;
    e.name = mi.name.empty() ? mi.id : mi.name;
    // a plain folder (no mod.json) is named by its folder: show it as a title
    if (mi.legacy && e.name == mi.id) e.name = folderTitle(mi.id);
    e.author = mi.author;
    e.version = mi.version;
    e.description = mi.description;
    e.category = mi.category.empty() ? "other" : lowerS(mi.category);
    e.preview = mi.preview;
    e.screenshots = mi.screenshots;
    e.nexusUrl = mi.nexusUrl;
    e.root = mi.root;
    e.enabled = mi.enabled;
    e.mounted = mi.mounted;
    e.isZip = mi.isZip;
    e.legacy = mi.legacy;
    e.status = modStatusName(mi.status);
    e.reason = mi.reason;
    e.failed = !(mi.status == ModStatus::Ok || mi.status == ModStatus::Disabled ||
                 mi.status == ModStatus::Off);
    // Saved on but not mounted (or the reverse): the change applies at the next start. A mod
    // that failed its checks is not "pending": the reason says why it is off.
    e.needsRestart = !e.failed && (e.enabled != e.mounted);
    e.order = pos++;
    for (const ModConflictInfo& c : conflicts) {
      const bool won = c.winnerId == mi.id, lost = c.loserId == mi.id;
      if (!won && !lost) continue;
      e.conflicts.push_back({c.what, won ? c.loserId : c.winnerId, won ? c.loser : c.winner, won});
    }
    for (const ModCostumeChoice& ch : modCostumePackChoices(mi.id)) {
      LibSuit su;
      su.id = ch.costume.id;
      su.name = ch.costume.name;
      su.description = ch.costume.description;
      su.included = ch.included;
      su.active = ch.active;
      e.suits.push_back(std::move(su));
    }
    if (!e.suits.empty() && e.category == "other") e.category = "characters";
    s.installed++;
    if (e.enabled) s.enabledCount++;
    s.mods.push_back(std::move(e));
  }
  for (const PortFeatureInfo& f : modMenuFeatures()) {
    LibEntry e;
    e.id = f.id;
    e.name = f.name.empty() ? f.id : f.name;
    e.author = "Superman Returns PC port";
    e.version = "built in";
    e.description = f.description;
    e.category = "builtin";
    e.builtin = true;
    e.optionKey = f.option;
    e.hasToggle = !f.option.empty();
    e.enabled = e.hasToggle ? modOptionGet(f.option.c_str()) == 1 : true;
    e.mounted = f.applied;
    e.status = modStatusName(f.status);
    e.reason = f.reason;
    // A toggle's change applies at the next start too (the feature is generated at boot).
    e.needsRestart = e.hasToggle && f.on && e.enabled != f.applied;
    s.restartRequired = s.restartRequired || e.needsRestart;
    s.mods.push_back(std::move(e));
  }
  // a conflict names the other mod as the library shows it (a plain folder by its title)
  for (LibEntry& e : s.mods)
    for (LibConflict& c : e.conflicts)
      for (const LibEntry& o : s.mods)
        if (o.id == c.otherId) c.otherName = o.name;
  return s;
}

bool setEnabled(const std::string& id, bool on, std::string* msg) {
  const bool ok = modMenuSetEnabled(id, on);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] %s %s -> %s", on ? "enable" : "disable", id.c_str(),
              ok ? "saved" : "refused");
  if (!ok && msg) *msg = modMenuAutomated() ? "This run does not save mod changes." : "The change could not be saved.";
  return ok;
}

bool move(const std::string& id, int delta, std::string* msg) {
  const bool ok = modMenuMove(id, delta);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] move %s %s -> %s", id.c_str(), delta < 0 ? "up" : "down",
              ok ? "saved" : "refused");
  if (!ok && msg) *msg = "The load order could not be saved.";
  return ok;
}

bool setSuitIncluded(const std::string& suitId, bool on, std::string* msg) {
  const bool ok = modCostumeSetIncluded(suitId, on);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] suit %s %s -> %s", suitId.c_str(), on ? "included" : "excluded",
              ok ? "saved" : "refused");
  if (!ok && msg) *msg = "The suit choice could not be saved.";
  return ok;
}

bool setBuiltinOption(const std::string& key, bool on, std::string* msg) {
  const bool ok = !key.empty() && modOptionSet(key.c_str(), on ? 1 : 0);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] option %s = %d -> %s", key.c_str(), on ? 1 : 0,
              ok ? "saved" : "refused");
  if (!ok && msg) *msg = "The setting could not be saved.";
  return ok;
}

bool restartNow(std::string* msg) {
  // The MODS menu's own relaunch path (kernel/mod_menu.cpp requestRestartImpl: SDL_EVENT_QUIT, then the
  // exit path starts the exe again with no stdio). Automated runs relaunch too, like that path:
  // a roll that presses it follows the child (sr_drive followRestart).
  (void)msg;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] RESTART NOW -> relaunch");
  modMenuRequestRestart();
  return true;
}

const char* nexusHomeUrl() { return "https://www.nexusmods.com/"; }

// ShellExecute on its own short-lived thread (with its own COM apartment): starting a browser or
// Explorer can take a while, and the caller is the render thread.
static void shellOpenAsync(std::wstring target, std::string what) {
#ifdef _WIN32
  std::thread([target = std::move(target), what = std::move(what)] {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                                                            SW_SHOWNORMAL)) > 32;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] open %s -> %s", what.c_str(), ok ? "opened" : "failed");
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
#else
  (void)target;
  (void)what;
#endif
}

bool openUrl(const std::string& url, std::string* msg) {
  const std::string lo = lowerS(url);
  if (lo.rfind("https://www.nexusmods.com/", 0) != 0 && lo.rfind("https://nexusmods.com/", 0) != 0) {
    if (msg) *msg = "Only Nexus Mods links open from here.";
    return false;
  }
  if (automation::policy().automated) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] open link %s -> not opened (automated run)", url.c_str());
    if (msg) *msg = "Links do not open in automated runs.";
    return false;
  }
  shellOpenAsync(std::wstring(url.begin(), url.end()), "link " + url);   // validated ASCII
  return true;
}

bool openModsFolder(std::string* msg) {
  const std::string dir = modMenuModsDir();
  if (automation::policy().automated) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] open mods folder -> not opened (automated run)");
    if (msg) *msg = "Explorer does not open in automated runs.";
    return false;
  }
  std::error_code ec;
  const fs::path p = fs::u8path(dir);
  fs::create_directories(p, ec);
  if (!fs::is_directory(p, ec)) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] open mods folder -> failed (no folder)");
    if (msg) *msg = "The mods folder could not be created.";
    return false;
  }
  shellOpenAsync(p.wstring(), "mods folder");
  return true;
}

// ── pure helpers ──────────────────────────────────────────────────────────────────────────
const std::vector<Category>& categories() {
  static const std::vector<Category> v = {
      {"all", "All"},           {"characters", "Characters"}, {"missions", "Missions"},
      {"worlds", "Worlds"},     {"gameplay", "Gameplay"},     {"visuals", "Visuals"},
      {"audio", "Audio"},       {"interface", "Interface"},   {"other", "Other"},
      {"builtin", "Built-in features"}};
  return v;
}

bool matches(const LibEntry& e, const std::string& key, const std::string& search) {
  if (!key.empty() && key != "all" && e.category != key) return false;
  const std::string q = lowerS(search);
  if (q.empty()) return true;
  return containsCI(e.name, q) || containsCI(e.author, q) || containsCI(e.id, q) ||
         containsCI(e.description, q);
}

std::vector<int> filter(const LibSnapshot& s, const std::string& key, const std::string& search) {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(s.mods.size()); ++i)
    if (matches(s.mods[size_t(i)], key, search)) out.push_back(i);
  return out;
}

int countIn(const LibSnapshot& s, const std::string& key) {
  int n = 0;
  for (const auto& e : s.mods) n += matches(e, key, "") ? 1 : 0;
  return n;
}

int gridMove(int index, int count, int cols, int dx, int dy) {
  if (count <= 0 || cols <= 0) return -1;
  index = std::clamp(index, 0, count - 1);
  const int row = index / cols, col = index % cols;
  const int rows = (count + cols - 1) / cols;
  if (dx < 0) return col == 0 ? -1 : index - 1;
  if (dx > 0) return (col == cols - 1 || index + 1 >= count) ? index : index + 1;
  if (dy < 0) return row == 0 ? -1 : index - cols;
  if (dy > 0) {
    if (row + 1 >= rows) return index;
    return std::min(index + cols, count - 1);
  }
  return index;
}

std::string sizeLabel(uint64_t bytes) {
  char b[32];
  if (bytes < 1024ull) std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(bytes));
  else if (bytes < 1024ull * 1024ull) std::snprintf(b, sizeof b, "%.1f KB", double(bytes) / 1024.0);
  else if (bytes < 1024ull * 1024ull * 1024ull) std::snprintf(b, sizeof b, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
  else std::snprintf(b, sizeof b, "%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return b;
}

std::string upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return s;
}

std::string folderTitle(const std::string& folder) {
  std::string o;
  bool start = true;
  for (char c : folder) {
    if (c == '_' || c == '-' || c == '.' || c == ' ') {
      if (!o.empty() && o.back() != ' ') o += ' ';
      start = true;
      continue;
    }
    if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    o += c;
    start = false;
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  return o.empty() ? folder : o;
}

int selfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      ++fails;
      std::printf("  FAIL %s\n", what);
    } else {
      std::printf("  ok   %s\n", what);
    }
  };
  // categories: the ten the owner listed, All first, Built-in last
  const auto& cats = categories();
  check(cats.size() == 10 && std::string(cats.front().key) == "all" &&
            std::string(cats.back().key) == "builtin",
        "L1 ten categories, All first, Built-in features last");
  bool order = cats.size() == 10;
  for (size_t i = 0; order && i < cats.size(); ++i) order = std::string(cats[i].key) == kCategoryOrder[i];
  check(order, "L1b the owner's category order");
  LibSnapshot s;
  auto add = [&](const char* id, const char* name, const char* cat, const char* author, bool builtin) {
    LibEntry e;
    e.id = id; e.name = name; e.category = cat; e.author = author; e.builtin = builtin;
    e.description = std::string("about ") + name;
    s.mods.push_back(e);
  };
  add("sample_mission", "Sample Mission", "missions", "Lachlan", false);
  add("suits", "Extra Suits", "characters", "Someone", false);
  add("hud", "Clean HUD", "interface", "Lachlan", false);
  add("custom_frontend", "Custom front end", "builtin", "port", true);
  check(filter(s, "all", "").size() == 4, "L2 All shows every entry, built-ins included");
  check(filter(s, "characters", "").size() == 1 && filter(s, "characters", "")[0] == 1,
        "L3 a category shows only its mods");
  check(filter(s, "builtin", "").size() == 1 && filter(s, "builtin", "")[0] == 3,
        "L4 Built-in features shows the port features");
  check(filter(s, "all", "LACH").size() == 2, "L5 search is case-insensitive over the author");
  check(filter(s, "all", "about clean").size() == 1, "L6 search covers the description");
  check(filter(s, "missions", "hud").empty(), "L7 category and search combine");
  check(countIn(s, "interface") == 1 && countIn(s, "worlds") == 0, "L8 category counts");
  // grid navigation: 7 items, 4 columns  [0 1 2 3] [4 5 6]
  check(gridMove(0, 7, 4, -1, 0) == -1, "G1 left from column 0 leaves the grid (rail)");
  check(gridMove(1, 7, 4, -1, 0) == 0, "G2 left");
  check(gridMove(3, 7, 4, 1, 0) == 3, "G3 right at the last column stays");
  check(gridMove(6, 7, 4, 1, 0) == 6, "G4 right at the last item stays");
  check(gridMove(2, 7, 4, 0, -1) == -1, "G5 up from row 0 leaves the grid (header)");
  check(gridMove(3, 7, 4, 0, 1) == 6, "G6 down into a short last row lands on the last item");
  check(gridMove(5, 7, 4, 0, 1) == 5, "G7 down on the last row stays");
  check(gridMove(5, 7, 4, 0, -1) == 1, "G8 up");
  check(gridMove(9, 7, 4, 0, 0) == 6, "G9 an out-of-range index clamps");
  check(gridMove(0, 0, 4, 1, 0) == -1, "G10 an empty grid");
  check(sizeLabel(512) == "512 B" && sizeLabel(1536) == "1.5 KB" && sizeLabel(3u * 1024u * 1024u) == "3.0 MB",
        "S1 size labels");
  check(upper("Sample mission v0.2") == "SAMPLE MISSION V0.2", "S2 upper");
  check(folderTitle("sample_mission") == "Sample Mission" && folderTitle("hd-city__pack") == "Hd City Pack" &&
            folderTitle("_") == "_" && folderTitle("MyMod") == "MyMod",
        "S3 a plain folder's display name");
  // blockGameInput: open blocks; closing latches until the pad is released
  g_openReq.store(false);
  setShown(true);
  check(blockGameInput(false), "B1 open: the game reads a neutral pad");
  setShown(false);
  check(blockGameInput(true), "B2 closed while B is still held: still neutral");
  check(!blockGameInput(false), "B3 released: the game reads input again");
  check(!blockGameInput(true), "B4 a later press reaches the game");
  return fails;
}

}  // namespace modlib
}  // namespace kernel
}  // namespace sr
