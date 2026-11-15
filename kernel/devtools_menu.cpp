// iter-464 (fe lane): the in-game Dev Tools menu - the host half. Contract: devtools_menu.hpp,
// docs/DEVTOOLS_DESIGN.md section 13 "Access", docs/DEVTOOLS.md section 0b.
//
// Charter: DEVTOOL class. This file never reads or writes guest memory and never calls guest
// code; it builds page text, switches closed-set tools (devToolsSetAtRuntime, kernel/devtools.cpp)
// and arms / stops the level-skip executor (kernel/devtools_levelskip.cpp), which acts on the
// game's own tick. Default: nothing is on; the menu itself is only reachable through the
// custom_frontend content mod.
#include "devtools_menu.hpp"

#include "host_pages.hpp"

#include "devtools.hpp"
#include "file_io.hpp"
#include "mod_bridge.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace sr {
namespace kernel {

namespace {
namespace fs = std::filesystem;

constexpr int kListRows = 4;   // a list page shows 4 entries + "MORE" (ScrGenPop has 5 buttons)

using Row = HostPageRow;
using Page = HostPage;
using Out = HostPageOut;

std::mutex g_mu;
std::vector<DevMenuItemDef> g_items;  // devMenuRegister
std::string g_ctx = "FE";             // "FE" (front end) | "IG" (pause menu)

std::string upper(std::string s) { return hostUpper(std::move(s)); }

bool devOn() { return devToolAllowed("devmenu"); }

void allOff(const char* why) {
  devToolsLevelskipStop(why);
  devToolsSetAtRuntime("levelskip", false, "menu");
  std::vector<const char*> tools;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& it : g_items)
      if (it.tool) tools.push_back(it.tool);
  }
  for (const char* t : tools) devToolsSetAtRuntime(t, false, "menu");
  devToolsSetAtRuntime("devmenu", false, "menu");
}

void enableDev() { devToolsSetAtRuntime("devmenu", true, "menu"); }

// ---- level-skip plans (devToolsPlansDir) ----
struct Plan {
  std::string file, label;
  long target = 1L << 30;   // the planner's target objective (story order); unknown = last
};

std::vector<Plan> listPlans() {
  std::vector<Plan> v;
  const std::string dir = devToolsPlansDir();
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return v;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec) || e.path().extension() != ".txt") continue;
    Plan p;
    p.file = e.path().string();
    // "# menu: <label>" names the row; else the plan's target from the planner header.
    std::ifstream f(e.path());
    std::string line, target;
    while (std::getline(f, line)) {
      if (line.rfind("# menu:", 0) == 0) {
        p.label = line.substr(7);
        continue;
      }
      const size_t t = line.find(": target ");
      if (target.empty() && line.rfind("# skipplan", 0) == 0 && t != std::string::npos) {
        std::istringstream ss(line.substr(t + 9));
        std::string idx, flow;
        ss >> idx >> flow;
        target = flow;
        p.target = std::strtol(idx.c_str(), nullptr, 10);
      }
      if (!line.empty() && line[0] != '#') break;
    }
    if (p.label.empty()) p.label = !target.empty() ? target : e.path().stem().string();
    const size_t a = p.label.find_first_not_of(" \t");
    p.label = upper(a == std::string::npos ? std::string() : p.label.substr(a));
    while (!p.label.empty() && (p.label.back() == '\r' || p.label.back() == ' ')) p.label.pop_back();
    v.push_back(p);
  }
  std::sort(v.begin(), v.end(), [](const Plan& a, const Plan& b) {
    return a.target != b.target ? a.target < b.target : a.file < b.file;   // story order
  });
  return v;
}

std::string planLabelFor(const std::string& fileName) {
  for (const auto& p : listPlans())
    if (fs::path(p.file).filename().string() == fileName) return p.label;
  return upper(fileName);
}

std::string skipLine(const DevToolsSkipStatus& st) {
  const std::string label = st.plan.empty() ? std::string("-") : planLabelFor(st.plan);
  char b[256];
  if (st.state == "armed")
    std::snprintf(b, sizeof b, "ARMED: %s. It starts with the first objective of a NEW GAME.", label.c_str());
  else if (st.state == "running")
    std::snprintf(b, sizeof b, "RUNNING: %s, step %zu of %zu (%s).", label.c_str(), st.k + 1, st.n, st.prim.c_str());
  else if (st.state == "done")
    std::snprintf(b, sizeof b, "DONE: %s.", label.c_str());
  else if (st.state == "stopped")
    std::snprintf(b, sizeof b, "STOPPED: %s at step %zu of %zu: %s.", label.c_str(), st.k + 1, st.n, st.last.c_str());
  else
    std::snprintf(b, sizeof b, "No level skip is armed.");
  return b;
}

std::string valueText(const DevMenuItemDef& d) {
  if (d.kind == DevMenuKind::Toggle) return (d.get && d.get()) ? "ON" : "OFF";
  if (d.kind == DevMenuKind::Slider) return std::to_string(d.get ? d.get() : d.min) + d.unit;
  if (d.kind == DevMenuKind::Readout) return d.text ? d.text() : std::string();
  return {};
}

// ---- iter-465 playthrough: SKIP CURRENT MISSION / SKIP CHAPTER / SIDE MISSIONS ----
// The active objective comes from the tick thread's host copy (devToolsActiveMission); this file still
// never reads guest memory. Skips go through the level-skip executor's own `pass` (skip_mission) or a
// small generated plan (reach / xp_to_cap / pass), so the game's own end-of-objective path runs.
std::string missionName(const std::string& flow, int64_t idx) {
  auto starts = [&](const char* pre) { return flow.rfind(pre, 0) == 0; };
  if (flow == "FastFlyer") return "RACE #" + std::to_string(idx);
  if (flow == "PlayableBIZ") return "BIZARRO RAMPAGE #" + std::to_string(idx);
  if (starts("MSTobj0") && flow.size() == 8) return std::string("METEOR STORM PART ") + flow[7];
  if (flow == "WARobj01") return "WARWORLD ARENA";
  if (flow == "WARobj02") return "WARWORLD ROUND 2";
  if (flow == "MTOobj09") return "METALLO";
  if (flow == "BIZobj03") return "BIZARRO (BIZOBJ03)";
  if (flow == "BIZobj15") return "BIZARRO (BIZOBJ15)";
  if (flow == "TWSobj04") return "THE TWISTER";
  if (starts("ME") && flow.find("TYPE") != std::string::npos) return "METRO EVENT " + flow.substr(2, flow.find("TYPE") - 2);
  if (starts("Cinematic")) return "CUTSCENE " + upper(flow.substr(9));
  if (flow.find("_Transition") != std::string::npos) return "CHAPTER TRANSITION";
  return upper(flow.empty() ? "#" + std::to_string(idx) : flow);
}

struct SideMission {
  int idx;
  const char* flow;
  const char* label;
};
// data/devtools/objective_table_retail.tsv: the repeatable TYPE_MINIGAME triggers, all open after WARobj02
// (dependency 6); the race chain 99 -> 92 -> 93 -> 101 -> 100 -> 94 needs the race before it won.
const SideMission kSide[] = {
    {95, "FastFlyer", "RACE #95 (CH 2+)"},
    {96, "FastFlyer", "RACE #96 (CH 2+)"},
    {97, "FastFlyer", "RACE #97 (CH 2+)"},
    {99, "FastFlyer", "RACE #99 (AFTER 95-97)"},
    {92, "FastFlyer", "RACE #92 (AFTER 99)"},
    {93, "FastFlyer", "RACE #93 (AFTER 92)"},
    {101, "FastFlyer", "RACE #101 (AFTER 93)"},
    {100, "FastFlyer", "RACE #100 (AFTER 101)"},
    {94, "FastFlyer", "RACE #94 (AFTER 100)"},
    {67, "PlayableBIZ", "BIZARRO RAMPAGE #67 (CH 2+)"},
    {68, "PlayableBIZ", "BIZARRO RAMPAGE #68 (CH 2+)"},
    {69, "PlayableBIZ", "BIZARRO RAMPAGE #69 (CH 2+)"},
    {70, "PlayableBIZ", "BIZARRO RAMPAGE #70 (CH 2+)"},
    {71, "PlayableBIZ", "BIZARRO RAMPAGE #71 (CH 2+)"},
};

std::string guardRunning(int64_t idx, const std::string& flow) {
  return "das objectiveActivated == 1 and das objectiveBitIndex == " + std::to_string(idx) + " and flow_loaded " +
         flow + " and base_state 107 and das waitingForLoad == 0 and stable 30";
}
std::string guardIdle(int idx) {
  return "trigger " + std::to_string(idx) +
         " available and das objectiveActivated == 0 and das triggersAvailable == 1 and das waitingForLoad == 0 and "
         "base_state 1807139120";
}

// Writes a generated plan next to the level skip's output (never into the plans folder) and arms it.
bool armGenerated(const std::string& tag, const std::string& text, std::string* err) {
  std::string root = contentRootDir();
  if (root.empty()) root = ".";
  const fs::path dir = fs::path(root) / "devtools_out";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path f = dir / ("menu_" + tag + ".txt");
  {
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    if (!o) {
      if (err) *err = "cannot write " + f.string();
      return false;
    }
    o << "# menu-generated (iter-465 playthrough): " << tag << "\n" << text;
  }
  devToolsSetAtRuntime("levelskip", true, "menu");
  return devToolsLevelskipArm(f.string(), err);
}

std::string sidePlan(int idx, const std::string& flow, bool xpToCap, bool complete = true) {
  std::string t;
  t += "timeout 1800\ndeadline 300\nwhen " + guardIdle(idx) + " do reach " + std::to_string(idx) + " " + flow + "\n";
  if (xpToCap) t += "timeout 1800\ndeadline 300\nwhen " + guardRunning(idx, flow) + " do xp_to_cap\n";
  if (complete)
    t += "timeout 1800\ndeadline 300\nwhen " + guardRunning(idx, flow) + " do pass " + std::to_string(idx) + " " + flow + "\n";
  return t;
}

std::string g_lastSkipNote;   // what the last skip row did (shown on the LEVEL SKIP page)

// ---- the pages ----
Page buildPage(const std::string& id);

Page pageMain() {
  Page p;
  p.id = "main";
  p.title = "DEV TOOLS";
  if (!devOn()) {
    p.body =
        "Debugging aids for testing. They change the game: when you switch them on, this session and every "
        "save made in it are marked dev-tool assisted.";
  } else {
    p.body = "Dev tools are ON. This session and every save made in it are marked dev-tool assisted.\n" +
             skipLine(devToolsLevelskipStatus());
  }
  Row t;
  t.label = devOn() ? "DEV TOOLS: ON" : "DEV TOOLS: OFF";
  t.kind = DevMenuKind::Toggle;
  t.press = [](Out& o) {
    if (devOn()) {
      allOff("dev tools switched off in the menu");
      o.page = "main";
    } else if (modOptionGet("devtools_notice_ack") != 1) {
      o.page = "notice";
    } else {
      enableDev();
      o.page = "main";
    }
    o.focus = 0;
  };
  t.adjust = [](Out& o, int) {
    if (!devOn() && modOptionGet("devtools_notice_ack") != 1) {   // the notice first: a page change
      o.page = "notice";
      o.focus = 0;
      return;
    }
    if (devOn()) allOff("dev tools switched off in the menu");
    else enableDev();
    o.page = "main";   // re-shown (action 1): the body text says what changed
    o.focus = 0;
  };
  p.rows.push_back(t);
  auto nav = [](const char* label, const char* target, int focusBack) {
    Row r;
    r.label = label;
    r.press = [target](Out& o) {
      o.page = devOn() ? target : "notice";
      o.focus = 0;
    };
    (void)focusBack;
    return r;
  };
  p.rows.push_back(nav("LEVEL SKIP", "levelskip", 1));
  p.rows.push_back(nav("CHEATS", "cheats", 2));
  p.rows.push_back(nav("STATUS", "status", 3));
  p.back = [](Out& o) { o.action = 0; };
  return p;
}

Page pageNotice() {
  Page p;
  p.id = "notice";
  p.title = "DEV TOOLS";
  p.body =
      "Dev tools change the game. This session and every save made in it will be marked dev-tool assisted. "
      "This notice is shown once.";
  Row en;
  en.label = "ENABLE DEV TOOLS";
  en.press = [](Out& o) {
    modOptionSet("devtools_notice_ack", 1);
    enableDev();
    o.page = "main";
    o.focus = 0;
  };
  Row cancel;
  cancel.label = "CANCEL";
  cancel.press = [](Out& o) {
    o.page = "main";
    o.focus = 0;
  };
  p.rows = {en, cancel};
  p.back = [](Out& o) {
    o.page = "main";
    o.focus = 0;
  };
  return p;
}

Page pageLevelskip(int off) {
  Page p;
  p.id = "levelskip:" + std::to_string(off);
  p.title = "LEVEL SKIP";
  const DevToolsSkipStatus st = devToolsLevelskipStatus();
  const bool busy = st.state == "armed" || st.state == "running";
  if (busy) {
    p.body = skipLine(st);
    Row stop;
    stop.label = "STOP LEVEL SKIP";
    stop.press = [](Out& o) {
      devToolsLevelskipStop("stopped from the menu");
      o.page = "levelskip";
      o.focus = 0;
    };
    Row status;
    status.label = "STATUS";
    status.press = [](Out& o) {
      o.page = "status";
      o.focus = 0;
    };
    p.rows = {stop, status};
  } else {
    // iter-465 playthrough: the skip hub - SKIP: <active mission>, SKIP CHAPTER, SIDE MISSIONS, STORY TARGETS
    const DevToolsActiveMission am = devToolsActiveMission();
    std::string body;
    if (st.state != "idle") body = skipLine(st) + "\n";
    if (!g_lastSkipNote.empty()) body += g_lastSkipNote + "\n";
    if (!am.known)
      body += "Active mission: not read yet (start or load a game; it updates while the game runs).";
    else if (am.active)
      body += "Active mission: " + missionName(am.flow, am.idx) + " (" + am.flow + ", objective " +
              std::to_string(am.idx) + "). SKIP completes it the way winning it would.";
    else
      body += "No mission is active. SKIP CHAPTER plays on to the next chapter through its story objective.";
    p.body = body;
    Row skip;
    skip.label = am.active ? "SKIP: " + missionName(am.flow, am.idx) : std::string("SKIP: NOTHING ACTIVE");
    skip.press = [am](Out& o) {
      if (!am.active) {
        g_lastSkipNote = "Nothing to skip: no mission is active.";
      } else {
        devToolsSetAtRuntime("levelskip", true, "menu");
        std::string err;
        const uint64_t t = devToolsSubmit("skip_mission", &err);
        g_lastSkipNote = t ? "Skipping " + missionName(am.flow, am.idx) + " (it completes when the game runs)."
                           : "Skip refused: " + err;
      }
      o.page = "levelskip";
      o.focus = 0;
    };
    Row chapter;
    chapter.label = "SKIP CHAPTER";
    chapter.press = [](Out& o) {
      // iter-465: the tick-side controller (skip_chapter): the chapter's own story objective, never a race
      devToolsSetAtRuntime("levelskip", true, "menu");
      std::string err;
      const uint64_t t = devToolsSubmit("skip_chapter", &err);
      g_lastSkipNote = t ? std::string("SKIP CHAPTER: the chapter's story objective completes and the game advances "
                                       "the chapter itself (resume the game).")
                         : "SKIP CHAPTER refused: " + err;
      o.page = "levelskip";
      o.focus = 1;
    };
    Row side;
    side.label = "SIDE MISSIONS";
    side.press = [](Out& o) {
      o.page = "sidemissions";
      o.focus = 0;
    };
    Row targets;
    targets.label = "STORY TARGETS";
    targets.press = [](Out& o) {
      o.page = "lstargets";
      o.focus = 0;
    };
    p.rows = {skip, chapter, side, targets};
  }
  p.back = [](Out& o) {
    g_lastSkipNote.clear();
    o.page = "main";
    o.focus = 1;
  };
  return p;
}

Page pageSideMissions(int off) {
  Page p;
  p.id = "sidemissions:" + std::to_string(off);
  p.title = "SIDE MISSIONS";
  p.body = "Pick one: the game starts it (dev-tool assisted). Play it, or SKIP (LEVEL SKIP or F10) completes it "
           "the way winning it would. Races and Bizarro rampages open after Warworld.";
  const int n = static_cast<int>(sizeof(kSide) / sizeof(kSide[0]));
  for (int i = off; i < n && static_cast<int>(p.rows.size()) < kListRows; ++i) {
    Row r;
    r.label = kSide[i].label;
    const int idx = kSide[i].idx;
    const std::string flow = kSide[i].flow;
    r.press = [idx, flow](Out& o) {
      std::string err;
      const bool ok = armGenerated("side_" + std::to_string(idx), sidePlan(idx, flow, false, /*complete=*/false), &err);
      g_lastSkipNote = ok ? "Side mission armed: " + missionName(flow, idx) + " starts when the game runs; play it, or SKIP it."
                          : "Side mission failed: " + err;
      o.page = "levelskip";
      o.focus = 0;
    };
    p.rows.push_back(r);
  }
  if (off + kListRows < n) {
    Row more;
    more.label = "MORE SIDE MISSIONS";
    const int next = off + kListRows;
    more.press = [next](Out& o) {
      o.page = "sidemissions:" + std::to_string(next);
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  p.back = [off](Out& o) {
    o.page = off > 0 ? "sidemissions:" + std::to_string(std::max(0, off - kListRows)) : "levelskip";
    o.focus = off > 0 ? 0 : 2;
  };
  return p;
}

Page pageTargets(int off) {
  Page p;
  p.id = "lstargets:" + std::to_string(off);
  p.title = "STORY TARGETS";
  const DevToolsSkipStatus st = devToolsLevelskipStatus();
  {
    const std::vector<Plan> plans = listPlans();
    std::string intro = g_ctx == "IG"
                            ? "Pick a target. Plans start from a NEW GAME (the first objective running): quit to the "
                              "main menu and start one, then let go of the controller."
                            : "Pick a target, then start a NEW GAME and let go of the controller: the game plays its own "
                              "objectives to the target.";
    if (st.state != "idle") intro = skipLine(st) + "\n" + intro;
    if (plans.empty()) intro += "\nNo plans found in " + devToolsPlansDir() + ".";
    p.body = intro;
    const int n = static_cast<int>(plans.size());
    for (int i = off; i < n && static_cast<int>(p.rows.size()) < kListRows; ++i) {
      Row r;
      r.label = plans[i].label;
      const std::string file = plans[i].file;
      r.press = [file](Out& o) {
        devToolsSetAtRuntime("levelskip", true, "menu");
        std::string err;
        if (!devToolsLevelskipArm(file, &err))
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] menu: arming %s failed: %s", file.c_str(), err.c_str());
        o.page = "levelskip";
        o.focus = 0;
      };
      p.rows.push_back(r);
    }
    if (off + kListRows < n) {
      Row more;
      more.label = "MORE TARGETS";
      const int next = off + kListRows;
      more.press = [next](Out& o) {
        o.page = "lstargets:" + std::to_string(next);
        o.focus = 0;
      };
      p.rows.push_back(more);
    }
  }
  p.back = [off](Out& o) {
    o.page = off > 0 ? "lstargets:" + std::to_string(std::max(0, off - kListRows)) : "levelskip";
    o.focus = off > 0 ? 0 : 3;
  };
  return p;
}

Page pageStatus() {
  Page p;
  p.id = "status";
  p.title = "DEV TOOLS STATUS";
  const DevToolsSkipStatus st = devToolsLevelskipStatus();
  std::string tools;
  size_t n = 0;
  const DevToolInfo* t = devToolTable(&n);
  const uint32_t mask = devToolMask();
  for (size_t i = 0; i < n; ++i)
    if (mask & (1u << i)) tools += (tools.empty() ? "" : ", ") + std::string(t[i].name);
  std::ostringstream b;
  b << "Session: " << (devToolsTainted() ? "dev-tool assisted" : "natural") << " (tools on: "
    << (tools.empty() ? "none" : tools) << ")\n";
  b << "Actions on the game: " << devToolActs() << "\n";
  b << "Level skip: " << skipLine(st) << "\n";
  if (!st.last.empty()) b << "Last event: " << st.last << "\n";
  const int32_t slot = modSlotCurrent();
  b << "Save slot in use: " << (slot >= 0 ? std::to_string(slot + 1) : std::string("none yet")) << "\n";
  b << "Autosave without asking: " << (modOptionGet("autosave_without_asking") == 1 ? "ON" : "OFF");
  p.body = b.str();
  Row refresh;
  refresh.label = "REFRESH";
  refresh.press = [](Out& o) {
    o.page = "status";
    o.focus = 0;
  };
  p.rows = {refresh};
  p.back = [](Out& o) {
    o.page = "main";
    o.focus = 3;
  };
  return p;
}

// A page of registered rows (the DT-2 extension point), e.g. "cheats".
Page pageRegistered(const std::string& name, int off) {
  Page p;
  p.id = name + ":" + std::to_string(off);
  p.title = upper(name);
  std::vector<DevMenuItemDef> mine;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& d : g_items)
      if (d.page == name) mine.push_back(d);
  }
  if (mine.empty()) {
    Row back;
    back.label = "BACK";
    back.press = [](Out& o) {
      o.page = "main";
      o.focus = 2;
    };
    p.rows.push_back(back);
    p.body = name == "cheats"
                 ? "No cheats are built yet. They arrive with the DT-2 dev tools: pause timers, mini-event control, "
                   "free camera, then sliders such as flight speed."
                 : "Nothing here yet.";
  } else {
    p.body = "A changes a row. LEFT and RIGHT change toggles and sliders in place.";
  }
  const int n = static_cast<int>(mine.size());
  for (int i = off; i < n && static_cast<int>(p.rows.size()) < kListRows; ++i) {
    const DevMenuItemDef d = mine[i];
    Row r;
    r.kind = d.kind;
    const std::string v = valueText(d);
    r.label = upper(d.label) + (v.empty() ? "" : ": " + upper(v));
    const std::string pid = p.id;
    const int row = static_cast<int>(p.rows.size());
    auto apply = [d, pid, row](Out& o, int delta) {
      if (d.tool) devToolsSetAtRuntime(d.tool, true, "menu");
      bool ok = true;
      if (d.kind == DevMenuKind::Toggle) {
        ok = d.set && d.set((d.get && d.get()) ? 0 : 1);
      } else if (d.kind == DevMenuKind::Slider) {
        const int cur = d.get ? d.get() : d.min;
        const int nv = std::max(d.min, std::min(d.max, cur + delta * d.step));
        ok = d.set && d.set(nv);
      } else if (d.kind == DevMenuKind::Action) {
        ok = d.set && d.set(0);
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] menu %s %s delta=%d -> %s", d.id.c_str(),
                  d.kind == DevMenuKind::Action ? "run" : "set", delta, ok ? "ok" : "refused");
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
  if (off + kListRows < n) {
    Row more;
    more.label = "MORE";
    const std::string next = name + ":" + std::to_string(off + kListRows);
    more.press = [next](Out& o) {
      o.page = next;
      o.focus = 0;
    };
    p.rows.push_back(more);
  }
  p.back = [name, off](Out& o) {
    o.page = off > 0 ? name + ":" + std::to_string(std::max(0, off - kListRows)) : "main";
    o.focus = off > 0 ? 0 : 2;
  };
  return p;
}

Page buildPage(const std::string& id) {
  std::string name;
  int off = 0;
  hostPageSplit(id, &name, &off);
  if (name == "notice") return pageNotice();
  if (name == "levelskip") return pageLevelskip(off);
  if (name == "lstargets") return pageTargets(off);
  if (name == "sidemissions") return pageSideMissions(off);
  if (name == "status") return pageStatus();
  if (name == "cheats") return pageRegistered("cheats", off);
  if (name != "main") {
    bool known = false;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      for (const auto& d : g_items) known = known || d.page == name;
    }
    if (known) return pageRegistered(name, off);
  }
  return pageMain();
}

}  // namespace

void devMenuRegister(const DevMenuItemDef& def) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& d : g_items)
    if (d.id == def.id) {
      d = def;
      return;
    }
  g_items.push_back(def);
}

void devMenuCommand(const std::string& args, std::vector<ModReply>* out) {
  hostPagesCommand(args, [](const std::string& id) { return buildPage(id.empty() ? "main" : id); }, out, &g_ctx,
                   "DEVTOOL");
}

void devMenuResetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_items.clear();
  g_ctx = "FE";
}

}  // namespace kernel
}  // namespace sr
