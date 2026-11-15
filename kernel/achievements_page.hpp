#pragma once
// lane E L-a (step 5; the owner's E14, mock-up q14_achievements (b), STEP5_PLAN 9): THE ACHIEVEMENTS PAGE's model -
// pure, from the achievements core's read API (kernel/achievements.hpp sr::ach::snapshot(): the title's own XACH
// table, in its own order, and the current save's unlocks). gfx/achievements_ui.cpp draws it in the pause panel's
// look; nothing here reads guest memory or calls the game.
//
// A row is one achievement: its icon (the XDBF image), its name, a line of text, its gamerscore and LOCKED /
// UNLOCKED. Unlocked: the game's own unlocked description. Locked: the game's own how-to text - except a SECRET one
// (the XACH "shown while locked" flag clear), which reads SECRET ACHIEVEMENT / "Hidden until it is unlocked." and
// shows no icon, as the console's Guide showed them. The header line counts this save's unlocks and gamerscore
// and names the save the record belongs to.
//
// Kernel-only header (never include from guest/).
#include "achievements.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sr {
namespace kernel {
namespace achpage {

struct Row {
  uint32_t id = 0;
  std::string title;        // upper case (the panel's type)
  std::string text;         // the description / how-to line
  uint32_t gamerscore = 0;
  bool unlocked = false;
  bool hidden = false;      // a locked secret: no icon, no text of its own
  uint32_t imageId = 0;     // the XDBF image (0 / hidden = the placeholder)
  int64_t unlockedUnixMs = 0;
};

struct Page {
  bool available = false;   // the table could be read
  std::string header;       // "5 OF 18 UNLOCKED  ·  230 / 1000 G  ·  THIS SAVE (SLOT 2)"
  std::string emptyWhy;     // when !available
  int unlockedCount = 0, total = 0;
  uint32_t gamerscoreUnlocked = 0, gamerscoreTotal = 0;
  std::vector<Row> rows;    // the table's order
};

// The page for one snapshot of the core.
Page build(const sr::ach::Snapshot& s);

// The list shows `visible` rows from `top`: the top clamped so the window stays inside the list.
int clampTop(int top, int visible, int total);
// "1-6 OF 18" (an empty list: "0 OF 0").
std::string rangeLabel(int top, int visible, int total);

// The list's view: the focused row and the first row shown (`visible` rows at a time).
struct View {
  int top = 0, focus = 0;
};
// The focus moves by d rows, clamped to the list (d = 0 only clamps, after the list changed); the window follows it.
void moveFocus(View* v, int d, int visible, int total);
// The window scrolls by d rows (the wheel, the arrows); the focus stays inside it.
void scroll(View* v, int d, int visible, int total);

int selfTest();   // difftest --achievements-page-test

}  // namespace achpage
}  // namespace kernel
}  // namespace sr
