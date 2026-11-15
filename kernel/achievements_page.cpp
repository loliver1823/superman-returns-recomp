// lane E L-a: the ACHIEVEMENTS page's model. Contract: achievements_page.hpp.
#include "achievements_page.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace sr {
namespace kernel {
namespace achpage {

namespace {
std::string upper(std::string s) {
  for (char& c : s) c = char((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
  return s;
}
const char* const kDot = "  \xC2\xB7  ";   // the mock-up's separator (U+00B7)
}  // namespace

Page build(const sr::ach::Snapshot& s) {
  Page p;
  if (s.table != sr::ach::TableStatus::Ok || s.list.empty()) {
    p.emptyWhy = "The game's achievement table could not be read from your copy of the game.";
    return p;
  }
  p.available = true;
  std::set<uint32_t> got;
  std::vector<std::pair<uint32_t, int64_t>> when;
  for (const sr::ach::Unlock& u : s.unlocked) {
    got.insert(u.id);
    when.emplace_back(u.id, u.unixMs);
  }
  for (const sr::ach::Achievement& a : s.list) {
    Row r;
    r.id = a.id;
    r.gamerscore = a.gamerscore;
    r.unlocked = got.count(a.id) != 0;
    r.imageId = a.imageId;
    for (const auto& w : when)
      if (w.first == a.id) r.unlockedUnixMs = w.second;
    if (r.unlocked) {
      r.title = upper(a.name);
      r.text = a.unlockedText;
    } else if (a.secret) {
      r.hidden = true;
      r.imageId = 0;
      r.title = "SECRET ACHIEVEMENT";
      r.text = "Hidden until it is unlocked.";
    } else {
      r.title = upper(a.name);
      r.text = a.lockedText;
    }
    p.total++;
    p.gamerscoreTotal += a.gamerscore;
    if (r.unlocked) {
      p.unlockedCount++;
      p.gamerscoreUnlocked += a.gamerscore;
    }
    p.rows.push_back(std::move(r));
  }
  std::string save;
  switch (s.save) {
    case sr::ach::SaveStatus::Slot: save = "THIS SAVE (SLOT " + std::to_string(s.slot + 1) + ")"; break;
    case sr::ach::SaveStatus::SlotUnreadable: save = "SLOT " + std::to_string(s.slot + 1) + ": ITS RECORD COULD NOT BE READ"; break;
    default: save = "NOT SAVED YET"; break;
  }
  p.header = std::to_string(p.unlockedCount) + " OF " + std::to_string(p.total) + " UNLOCKED" + kDot +
             std::to_string(p.gamerscoreUnlocked) + " / " + std::to_string(p.gamerscoreTotal) + " G" + kDot + save;
  return p;
}

int clampTop(int top, int visible, int total) {
  const int maxTop = std::max(0, total - std::max(1, visible));
  return std::clamp(top, 0, maxTop);
}

std::string rangeLabel(int top, int visible, int total) {
  if (total <= 0) return "0 OF 0";
  const int t = clampTop(top, visible, total);
  const int last = std::min(total, t + std::max(1, visible));
  return std::to_string(t + 1) + "-" + std::to_string(last) + " OF " + std::to_string(total);
}

void moveFocus(View* v, int d, int visible, int total) {
  if (total <= 0) {
    *v = View{};
    return;
  }
  const int vis = std::max(1, visible);
  v->focus = std::clamp(v->focus + d, 0, total - 1);
  if (v->focus < v->top) v->top = v->focus;
  if (v->focus >= v->top + vis) v->top = v->focus - vis + 1;
  v->top = clampTop(v->top, vis, total);
}

void scroll(View* v, int d, int visible, int total) {
  if (total <= 0) {
    *v = View{};
    return;
  }
  const int vis = std::max(1, visible);
  v->top = clampTop(v->top + d, vis, total);
  v->focus = std::clamp(v->focus, v->top, std::min(total, v->top + vis) - 1);
}

int selfTest() {
  int fails = 0;
  auto ck = [&](bool c, const char* what) {
    if (!c) {
      std::fprintf(stderr, "FAIL: achpage %s\n", what);
      ++fails;
    }
  };
  sr::ach::Snapshot s;
  ck(!build(s).available, "no table: nothing listed");
  s.table = sr::ach::TableStatus::Ok;
  sr::ach::Achievement a;
  a.id = 11; a.name = "Armageddon Averted"; a.unlockedText = "Survived the meteor shower."; a.lockedText = "Stop the meteors.";
  a.gamerscore = 30; a.imageId = 5;
  sr::ach::Achievement b;
  b.id = 12; b.name = "Hidden One"; b.unlockedText = "x"; b.lockedText = "y"; b.gamerscore = 100; b.secret = true; b.imageId = 6;
  sr::ach::Achievement c;
  c.id = 13; c.name = "Plain"; c.unlockedText = "done"; c.lockedText = "do it"; c.gamerscore = 50; c.imageId = 7;
  s.list = {a, b, c};
  s.unlocked = {{11, 1729670000000}};
  s.save = sr::ach::SaveStatus::Slot;
  s.slot = 1;
  Page p = build(s);
  ck(p.available && p.rows.size() == 3, "three rows, the table's order");
  ck(p.rows[0].unlocked && p.rows[0].title == "ARMAGEDDON AVERTED" && p.rows[0].text == "Survived the meteor shower." &&
         p.rows[0].unlockedUnixMs == 1729670000000, "unlocked: the name, the unlocked text, the time");
  ck(!p.rows[1].unlocked && p.rows[1].hidden && p.rows[1].title == "SECRET ACHIEVEMENT" && p.rows[1].imageId == 0,
     "a locked secret is hidden (no name, no icon)");
  ck(!p.rows[2].unlocked && !p.rows[2].hidden && p.rows[2].text == "do it", "locked: the how-to text");
  ck(p.unlockedCount == 1 && p.total == 3 && p.gamerscoreUnlocked == 30 && p.gamerscoreTotal == 180, "the counts");
  ck(p.header.find("1 OF 3 UNLOCKED") == 0 && p.header.find("30 / 180 G") != std::string::npos &&
         p.header.find("THIS SAVE (SLOT 2)") != std::string::npos, "the header names this save");
  s.unlocked.push_back({12, 1729670001000});
  p = build(s);
  ck(p.rows[1].unlocked && !p.rows[1].hidden && p.rows[1].title == "HIDDEN ONE" && p.rows[1].imageId == 6,
     "an unlocked secret shows itself");
  s.save = sr::ach::SaveStatus::NoSave;
  ck(build(s).header.find("NOT SAVED YET") != std::string::npos, "no save bound yet");
  s.save = sr::ach::SaveStatus::SlotUnreadable;
  s.slot = 0;
  ck(build(s).header.find("SLOT 1: ITS RECORD COULD NOT BE READ") != std::string::npos, "an unreadable record is said so");
  ck(rangeLabel(0, 6, 18) == "1-6 OF 18" && rangeLabel(15, 6, 18) == "13-18 OF 18" && rangeLabel(-3, 6, 4) == "1-4 OF 4" &&
         rangeLabel(0, 6, 0) == "0 OF 0", "the range label");
  ck(clampTop(20, 6, 18) == 12 && clampTop(-1, 6, 18) == 0 && clampTop(3, 6, 4) == 0, "the scroll clamp");
  // the view: the focus walks the list and the window follows; the wheel moves the window and keeps the focus in it
  View v;
  for (int i = 0; i < 6; ++i) moveFocus(&v, 1, 6, 18);
  ck(v.focus == 6 && v.top == 1, "DOWN x6 from the top: focus row 7, the window 2-7");
  moveFocus(&v, -3, 6, 18);
  ck(v.focus == 3 && v.top == 1, "UP inside the window: only the focus moves");
  moveFocus(&v, -10, 6, 18);
  ck(v.focus == 0 && v.top == 0, "past the top: clamped to row 1");
  moveFocus(&v, 6, 6, 18);
  moveFocus(&v, 6, 6, 18);
  moveFocus(&v, 6, 6, 18);
  ck(v.focus == 17 && v.top == 12, "a page at a time to the end: the last row, the window 13-18");
  scroll(&v, -5, 6, 18);
  ck(v.top == 7 && v.focus == 12, "the wheel up 5: the window 8-13, the focus pulled into it");
  scroll(&v, 40, 6, 18);
  ck(v.top == 12 && v.focus == 12, "the wheel past the end: the window clamped, the focus at its top");
  View w{3, 5};
  moveFocus(&w, 0, 6, 4);
  ck(w.top == 0 && w.focus == 3, "the list shrank to 4: the focus and the window clamped");
  moveFocus(&w, 1, 6, 0);
  scroll(&w, 1, 6, 0);
  ck(w.top == 0 && w.focus == 0, "an empty list: nothing to focus");
  return fails;
}

}  // namespace achpage
}  // namespace kernel
}  // namespace sr
