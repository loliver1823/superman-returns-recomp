// lane E L-a: THE ACHIEVEMENTS PAGE. Contract: achievements_ui.hpp. Dear ImGui is only the drawing layer (the
// shared overlay kit); the look, the layout, focus and input are our own code below.
//
// Charter: a host page. It reads no guest memory, writes none and calls no guest code; the only guest-visible effect
// is the neutral pad while it is open (the mod library's routing).
#ifdef _WIN32

#include "achievements_ui.hpp"

#include "overlay_kit.hpp"
#include "../app/log.hpp"
#include "../kernel/achievements_page.hpp"
#include "../kernel/host_automation.hpp"
#include "../kernel/host_input.hpp"
#include "../kernel/mod_library.hpp"
#include "../kernel/pc_controls.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sr {
namespace gfx {
namespace achui {

namespace {
using namespace sr::gfx::overlay;
namespace ML = sr::kernel::modlib;
namespace HI = sr::kernel::hostinput;
namespace PC = sr::kernel::pccontrols;
namespace AP = sr::kernel::achpage;

// ═════════════════════════════════════════════════════════════════════════════════════════
// Layout (1280x720 design units): the pause panel's interior (SCRPAUSEMENU, frame menukeys after1 #330; mock-up
// q14b). The game's own bevels and its PAUSED line stay above and below the panel.
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kPX0 = 244, kPX1 = 1036, kPY0 = 104, kPY1 = 615;
constexpr float kBandX0 = 284, kBandX1 = 1000;
constexpr float kTitleY0 = 138, kTitleY1 = 170, kHeaderY = 178;
constexpr float kRowsY0 = 207, kPitch = 56, kRowH = 50;
constexpr int kVisible = 6;
constexpr float kIconX = 317, kIconSz = 46, kTextX = 378, kRightX = 962;
constexpr float kFootY = 580;
constexpr int kIconPx = 64;   // the title's icons are 64 x 64
constexpr double kHeldAtOpen = 1e30;   // a repeat deadline: held when the page opened, no repeat until re-pressed

enum Act { aNone = 0, aUp, aDown, aPageUp, aPageDown, aHome, aEnd, aBack, aShot };

// ═════════════════════════════════════════════════════════════════════════════════════════
// State
// ═════════════════════════════════════════════════════════════════════════════════════════
struct Ui {
  bool shown = false, closing = false;
  float openA = 0;
  double lastT = 0, openT = 0;
  std::string ctx;
  AP::Page page;
  uint64_t gen = ~0ull;
  AP::View view;
  float scrollY = 0;           // the drawn first row, eased to view.top
  std::string lastView;        // the last [ACH_PAGE] view line
  bool iconsLogged = false;
  // mouse (stage coords)
  float mx = -1000, my = -1000;
  bool clickL = false, clickR = false, mouseMoved = false;
  int wheel = 0;
  bool hovered = false;
  // pad / harness keyboard edges
  uint16_t padPrev = 0;
  double padRepeatAt[2] = {0, 0};
  uint8_t hkPrev[512] = {0};
  double hkRepeatAt[512] = {0};
  bool shotWanted = false;
} U;

std::atomic<bool> gShotPending{false};
std::vector<Act> gActs;
bool gHaveTestSnap = false;
sr::ach::Snapshot gTestSnap;

// ═════════════════════════════════════════════════════════════════════════════════════════
// The model
// ═════════════════════════════════════════════════════════════════════════════════════════
uint64_t currentGen() { return gHaveTestSnap ? gTestSnap.generation : sr::ach::generation(); }

// The page from the core's snapshot, again only when the core changed (an unlock, a SAVE / LOAD).
void refresh(bool force) {
  if (!force && currentGen() == U.gen) return;
  const sr::ach::Snapshot s = gHaveTestSnap ? gTestSnap : sr::ach::snapshot();
  U.gen = s.generation;
  U.page = AP::build(s);
  AP::moveFocus(&U.view, 0, kVisible, int(U.page.rows.size()));   // keep the focus and the window inside the list
}

std::string iconKey(const AP::Row& r) { return "ach:" + std::to_string(r.imageId); }

// The row's icon (0 until it is uploaded; a hidden row has none). A locked one is decoded grey.
ImTextureID icon(const AP::Row& r) {
  if (r.hidden || r.imageId == 0) return ImTextureID(0);
  const uint32_t id = r.imageId;
  return imageFromMemory(
      iconKey(r), kIconPx, kIconPx, [id](std::vector<uint8_t>* png) { return sr::ach::iconPng(id, png); },
      !r.unlocked);
}

// One line once every icon has an answer (or after 5 s): the live proof that the title's images decode.
void noteIcons() {
  if (U.iconsLogged || !U.page.available) return;
  int want = 0, ready = 0, failed = 0, none = 0;
  for (const AP::Row& r : U.page.rows) {
    if (r.hidden || r.imageId == 0) {
      ++none;
      continue;
    }
    ++want;
    if (icon(r)) ++ready;
    else if (imageFromMemoryFailed(iconKey(r), kIconPx, !r.unlocked)) ++failed;
  }
  if (ready + failed < want && nowSec() - U.openT < 5.0) return;
  U.iconsLogged = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] icons: %d of %d decoded from the title's images, %d failed, %d hidden",
              ready, want, failed, none);
}

void logView() {
  const int n = int(U.page.rows.size());
  std::string v = AP::rangeLabel(U.view.top, kVisible, n);
  if (n > 0) v += ", focus " + std::to_string(U.view.focus + 1) + " " + U.page.rows[size_t(U.view.focus)].title;
  if (v == U.lastView) return;
  U.lastView = v;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] view %s", v.c_str());
}

void beginClose(const char* why) {
  if (U.closing) return;
  U.closing = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] close (%s)", why);
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Hit testing (immediate mode)
// ═════════════════════════════════════════════════════════════════════════════════════════
bool inside(float x0, float y0, float x1, float y1) {
  float cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
  if (!gClip.empty()) {
    const RectD& c = gClip.back();
    cx0 = std::max(cx0, c.x0);
    cy0 = std::max(cy0, c.y0);
    cx1 = std::min(cx1, c.x1);
    cy1 = std::min(cy1, c.y1);
  }
  return U.mx >= cx0 && U.mx < cx1 && U.my >= cy0 && U.my < cy1;
}
struct Hit {
  bool hover = false, click = false;
};
Hit hit(float x0, float y0, float x1, float y1) {
  Hit h;
  h.hover = !U.closing && inside(x0, y0, x1, y1);
  if (h.hover) U.hovered = true;
  h.click = h.hover && U.clickL;
  if (h.click) U.clickL = false;
  return h;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Input
// ═════════════════════════════════════════════════════════════════════════════════════════
// The keys follow the PC controls (kernel/pc_controls.hpp hostKeyAct: the menu bindings, Enter / Esc as failsafes,
// the harness channel's test keys), as the OPTIONS screen's do; then the page keys.
Act keyAct(int sc, bool shift, bool fromHarness) {
  const PC::Keymap km = PC::hostKeymap(fromHarness);
  switch (PC::hostKeyAct(uint16_t(sc), shift, km, PC::config().bindings)) {
    case PC::HostAct::Up: return aUp;
    case PC::HostAct::Down: return aDown;
    case PC::HostAct::LB: return aPageUp;
    case PC::HostAct::RB: return aPageDown;
    case PC::HostAct::Back: return aBack;
    default: break;
  }
  switch (sc) {
    case SDL_SCANCODE_PAGEUP: return aPageUp;
    case SDL_SCANCODE_PAGEDOWN: return aPageDown;
    case SDL_SCANCODE_HOME: return aHome;
    case SDL_SCANCODE_END: return aEnd;
    case SDL_SCANCODE_F12: return aShot;
    default: break;
  }
  return aNone;
}

void onKey(int sc, bool down, bool repeat, uint16_t mods, bool fromHarness) {
  if (!down) return;
  gLastWasPad = false;
  const Act a = keyAct(sc, (mods & SDL_KMOD_SHIFT) != 0, fromHarness);
  if (a == aNone) return;
  if (repeat && a != aUp && a != aDown && a != aPageUp && a != aPageDown) return;
  gActs.push_back(a);
}

void harnessStep(const uint8_t* hk, double t, uint16_t mods) {
  for (int sc = 1; sc < 512; ++sc) {
    const bool d = hk[sc] != 0, p = U.hkPrev[sc] != 0;
    if (d && !p) {
      onKey(sc, true, false, mods, true);
      U.hkRepeatAt[sc] = t + 0.40;
    } else if (d && p && t >= U.hkRepeatAt[sc]) {
      onKey(sc, true, true, mods, true);
      U.hkRepeatAt[sc] = t + 0.09;
    }
    U.hkPrev[sc] = hk[sc];
  }
}

void pollHarnessKeys(double t) {
  const uint8_t* hk = sr::kernel::automation::harnessKeys();
  if (!hk) return;
  harnessStep(hk, t, PC::harnessKeyMods(hk));
}

// The pad's buttons with the left stick's up / down as the D-pad bits (priming and polling read the same bits).
uint16_t padBits(const HI::PadState& ps) {
  uint16_t b = ps.buttons;
  if (ps.ly > 16000) b |= 0x0001;
  if (ps.ly < -16000) b |= 0x0002;
  return b;
}

void padStep(uint16_t b, double t) {
  const uint16_t edge = uint16_t(b & ~U.padPrev);
  if (b) gLastWasPad = true;
  const Act dirs[2] = {aUp, aDown};
  for (int i = 0; i < 2; ++i) {
    const uint16_t bit = uint16_t(1u << i);
    if (edge & bit) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.40;
    } else if ((b & bit) && t >= U.padRepeatAt[i]) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.07;
    }
  }
  if (edge & 0x2000) gActs.push_back(aBack);       // B
  if (edge & 0x0100) gActs.push_back(aPageUp);     // LB
  if (edge & 0x0200) gActs.push_back(aPageDown);   // RB
  U.padPrev = b;
}

void pollPad(double t) {
  HI::PadState ps;
  if (!HI::readActivePad(&ps)) {
    U.padPrev = 0;
    return;
  }
  padStep(padBits(ps), t);
}

// What is held when the page opens (the A that chose the row, a held key or stick) is not a press on the page, and
// does not repeat: it acts again only after its release (the review's P2-2).
void primeFrom(const uint8_t* hk, uint16_t padNow) {
  U.padPrev = padNow;
  for (double& r : U.padRepeatAt) r = kHeldAtOpen;
  for (int sc = 0; sc < 512; ++sc) {
    U.hkPrev[sc] = hk ? hk[sc] : 0;
    U.hkRepeatAt[sc] = (hk && hk[sc]) ? kHeldAtOpen : 0;
  }
}

void primeInputs() {
  HI::PadState ps;
  primeFrom(sr::kernel::automation::harnessKeys(), HI::readActivePad(&ps) ? padBits(ps) : 0);
}

void gatherInput(double t) {
  gActs.clear();
  U.clickL = U.clickR = U.mouseMoved = false;
  U.wheel = 0;
  for (const ML::InputEvent& ev : ML::drainInput()) {
    switch (ev.kind) {
      case ML::InputEvent::Key: onKey(ev.scancode, ev.down, ev.repeat, ev.mods, false); break;
      case ML::InputEvent::MouseMove:
        if (std::fabs(ev.x - U.mx) > 0.01f || std::fabs(ev.y - U.my) > 0.01f) U.mouseMoved = true;
        U.mx = ev.x;
        U.my = ev.y;
        break;
      case ML::InputEvent::MouseDown:
        U.mx = ev.x;
        U.my = ev.y;
        if (ev.button == 1) U.clickL = true;
        if (ev.button == 3) U.clickR = true;
        gLastWasPad = false;
        break;
      case ML::InputEvent::Wheel:
        U.mx = ev.x;   // the wheel's own position (a wheel before any move was ignored)
        U.my = ev.y;
        U.wheel += ev.wheel;
        gLastWasPad = false;
        break;
      default: break;
    }
  }
  pollHarnessKeys(t);
  pollPad(t);
}

void dispatch(Act a) {
  const int n = int(U.page.rows.size());
  switch (a) {
    case aUp: AP::moveFocus(&U.view, -1, kVisible, n); break;
    case aDown: AP::moveFocus(&U.view, 1, kVisible, n); break;
    case aPageUp: AP::moveFocus(&U.view, -kVisible, kVisible, n); break;
    case aPageDown: AP::moveFocus(&U.view, kVisible, kVisible, n); break;
    case aHome: AP::moveFocus(&U.view, -n, kVisible, n); break;
    case aEnd: AP::moveFocus(&U.view, n, kVisible, n); break;
    case aBack: beginClose("BACK"); break;
    case aShot: U.shotWanted = !ML::shotDir().empty(); break;
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Drawing
// ═════════════════════════════════════════════════════════════════════════════════════════
// The panel: opaque glass over the pause panel's interior (its rows do not show through).
void drawPanel() {
  ImVec2 p[8];
  const int n = chamfer(kPX0, kPY0, kPX1, kPY1, 6, 6, 6, 6, p);
  gradPoly(p, n, col(0.03f, 0.13f, 0.38f, 0.985f), col(0.01f, 0.05f, 0.19f, 0.985f));
  for (int i = 0; i < 6; ++i) {   // the game's faint vertical light streaks
    const float x = kPX0 + 50 + float(i) * 128.0f + 16.0f * std::sin(float(i) * 1.7f);
    const float w = 16 + 8 * std::sin(float(i) * 2.3f);
    DL->AddRectFilledMultiColor(X.p(x, kPY0 + 40), X.p(x + w, kPY1 - 40), col(0.5f, 0.7f, 1.0f, 0.0f),
                                col(0.5f, 0.7f, 1.0f, 0.03f), col(0.5f, 0.7f, 1.0f, 0.0f), col(0.5f, 0.7f, 1.0f, 0.0f));
  }
  DL->AddPolyline(p, n, col(0.48f, 0.70f, 1.0f, 0.45f), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
}

void drawTitle() {
  const ImU32 e0 = col(0.10f, 0.32f, 0.78f, 0.0f), e1 = col(0.10f, 0.32f, 0.78f, 0.80f);
  DL->AddRectFilledMultiColor(X.p(kBandX0, kTitleY0), X.p(640, kTitleY1), e0, e1, e1, e0);
  DL->AddRectFilledMultiColor(X.p(640, kTitleY0), X.p(kBandX1, kTitleY1), e1, e0, e0, e1);
  const ImU32 h0 = col(0.6f, 0.8f, 1.0f, 0.0f), h1 = col(0.6f, 0.8f, 1.0f, 0.5f);
  for (const float y : {kTitleY0, kTitleY1 - 1.2f}) {
    DL->AddRectFilledMultiColor(X.p(kBandX0, y), X.p(640, y + 1.2f), h0, h1, h1, h0);
    DL->AddRectFilledMultiColor(X.p(640, y), X.p(kBandX1, y + 1.2f), h1, h0, h0, h1);
  }
  const std::string t = "ACHIEVEMENTS";
  textTitle(F.title, 22, 640 - textW(F.title, 22, t) * 0.5f, kTitleY0 + 3, t);
  if (U.page.available) {
    const std::string h = ellipsize(F.label, 12.5f, U.page.header, kBandX1 - kBandX0 - 20);
    text(F.label, 12.5f, 640 - textW(F.label, 12.5f, h) * 0.5f, kHeaderY, h, C(kSilver), C(kSilverDim), 1.0f);
  }
}

void drawRow(int i, float y) {
  const AP::Row& r = U.page.rows[size_t(i)];
  const Hit h = hit(kBandX0, y - 3, kBandX1, y + kRowH + 3);
  if (h.hover && (U.mouseMoved || h.click)) U.view.focus = i;   // the mouse points at a row: it is the focus
  if (i == U.view.focus) selBand(kBandX0, y - 3, kBandX1, y + kRowH + 3, 1.0f);
  else if (h.hover) DL->AddRectFilled(X.p(kBandX0, y - 3), X.p(kBandX1, y + kRowH + 3), col(1, 1, 1, 0.04f));
  // the icon: the title's own (grey while locked), "?" for a locked secret
  const float ix = kIconX, iy = y + (kRowH - kIconSz) * 0.5f;
  DL->AddRectFilled(X.p(ix, iy), X.p(ix + kIconSz, iy + kIconSz), col(0.10f, 0.16f, 0.34f, 0.95f));
  if (const ImTextureID tex = icon(r)) {
    DL->AddImage(tex, X.p(ix, iy), X.p(ix + kIconSz, iy + kIconSz), ImVec2(0, 0), ImVec2(1, 1),
                 r.unlocked ? col(1, 1, 1, 1) : col(0.80f, 0.82f, 0.88f, 0.85f));
  } else if (r.hidden) {
    const std::string q = "?";
    text(F.title, 26, ix + kIconSz * 0.5f - textW(F.title, 26, q) * 0.5f, iy + 8, q, col(0.70f, 0.76f, 0.90f, 0.9f),
         col(0.50f, 0.58f, 0.74f, 0.9f), 1.0f);
  }
  DL->AddRect(X.p(ix, iy), X.p(ix + kIconSz, iy + kIconSz), col(0.55f, 0.72f, 1.0f, 0.55f), 0, 0,
              std::max(1.0f, X.d(1.0f)));
  // the gamerscore and LOCKED / UNLOCKED, right-aligned
  const std::string g = std::to_string(r.gamerscore) + " G";
  const float gw = textW(F.card, 15, g);
  text(F.card, 15, kRightX - gw, y + 6, g, r.unlocked ? C(kGold) : col(0.62f, 0.66f, 0.76f),
       r.unlocked ? C(kGoldDeep) : col(0.46f, 0.52f, 0.64f), 1.0f);
  const std::string st = r.unlocked ? "UNLOCKED" : "LOCKED";
  const ImU32 sc = r.unlocked ? C(kOk) : col(0.56f, 0.62f, 0.74f);
  text(F.badge, 10.5f, kRightX - textW(F.badge, 10.5f, st), y + 30, st, sc, sc, 0.0f);
  // the name and the line (the unlocked description, or the game's own how-to text)
  const float room = kRightX - 70 - kTextX;
  text(F.card, 16, kTextX, y + 5, ellipsize(F.card, 16, r.title, room), r.unlocked ? C(kWhite) : col(0.70f, 0.74f, 0.82f),
       r.unlocked ? C(kSilver) : col(0.52f, 0.58f, 0.70f), 1.0f);
  text(F.body, 12, kTextX, y + 28, ellipsize(F.body, 12, r.text, room), r.unlocked ? C(kSilver) : col(0.56f, 0.62f, 0.74f),
       r.unlocked ? C(kSilverDim) : col(0.46f, 0.52f, 0.64f), 1.0f);
}

void drawRows() {
  const int n = int(U.page.rows.size());
  pushClip(kBandX0, kRowsY0 - 4, kBandX1, kRowsY0 + kVisible * kPitch - 2);
  const int first = std::max(0, int(std::floor(U.scrollY)));
  for (int i = first; i < std::min(n, first + kVisible + 1); ++i) drawRow(i, kRowsY0 + (float(i) - U.scrollY) * kPitch);
  popClip();
}

void drawEmpty() {
  const std::string t = "NO ACHIEVEMENTS TO SHOW";
  textTitle(F.card, 16, 640 - textW(F.card, 16, t) * 0.5f, 320, t);
  float y = 348;
  for (const std::string& l : wrap(F.body, 13, U.page.emptyWhy, kBandX1 - kBandX0 - 80)) {
    text(F.body, 13, 640 - textW(F.body, 13, l) * 0.5f, y, l, C(kSilver), C(kSilverDim), 1.0f);
    y += 18;
  }
}

// An arrow (up or down) that scrolls the list one row; dim when there is nothing more that way.
void arrow(float cx, bool up, bool on) {
  const float y0 = kFootY + 4, y1 = kFootY + 13;
  const Hit h = on ? hit(cx - 8, kFootY - 4, cx + 8, kFootY + 22) : Hit{};
  const ImU32 c = !on ? col(0.45f, 0.52f, 0.66f, 0.5f) : h.hover ? C(kGold) : C(kSilver);
  if (up) DL->AddTriangleFilled(X.p(cx, y0), X.p(cx + 5, y1), X.p(cx - 5, y1), c);
  else DL->AddTriangleFilled(X.p(cx - 5, y0), X.p(cx + 5, y0), X.p(cx, y1), c);
  if (h.click) AP::scroll(&U.view, up ? -1 : 1, kVisible, int(U.page.rows.size()));
}

void drawFooter() {
  const int n = int(U.page.rows.size());
  if (U.page.available) {
    const std::string l = AP::rangeLabel(U.view.top, kVisible, n);
    const float lw = textW(F.label, 13, l);
    const float x = 640 - (lw + 44) * 0.5f;
    text(F.label, 13, x, kFootY + 1, l, C(kSilver), C(kSilverDim), 1.0f);
    arrow(x + lw + 18, true, U.view.top > 0);
    arrow(x + lw + 34, false, U.view.top + kVisible < n);
  }
  // BACK: always there (B / Esc / a click)
  const std::string b = "BACK";
  const float bw = textW(F.card, 16, b), gw = std::max(22.0f, glyphW(cB, 22));
  const float xr = kPX1 - 14, x0 = xr - gw - 8 - bw;
  const Hit h = hit(x0 - 8, kFootY - 6, xr + 6, kFootY + 26);
  if (h.hover) selBand(x0 - 14, kFootY - 6, xr + 8, kFootY + 26, 0.8f);
  textTitle(F.card, 16, x0, kFootY + 1, b);
  glyph(cB, xr - gw * 0.5f, kFootY + 10, 22);
  if (h.click) beginClose("click BACK");
}

void choosePrompts() {
  static const int s_pin = [] {
    const char* e = std::getenv("SR_MOD_LIBRARY_PROMPTS");
    if (!e) return -1;
    const std::string v = e;
    return v == "xbox" ? int(psXbox) : v == "ps4" ? int(psPS4) : v == "ps5" ? int(psPS5)
         : v == "switch" ? int(psSwitch) : v == "keyboard" ? int(psKeyboard) : -1;
  }();
  if (s_pin >= 0) {
    gPromptSet = PromptSet(s_pin);
  } else if (HI::glyphsEnabled()) {
    switch (HI::activeGlyphSet()) {
      case HI::GlyphSet::PS4: gPromptSet = psPS4; break;
      case HI::GlyphSet::PS5: gPromptSet = psPS5; break;
      case HI::GlyphSet::Switch: gPromptSet = psSwitch; break;
      case HI::GlyphSet::Keyboard: gPromptSet = psKeyboard; break;
      default: gPromptSet = psXbox; break;
    }
  } else {
    gPromptSet = gLastWasPad ? psXbox : psKeyboard;
  }
  gMoveLeftRight = false;
}

void showPage(const std::string& ctx, int bbW, int bbH) {
  U.shown = true;
  U.closing = false;
  U.ctx = ctx;
  U.openA = 0;
  U.openT = nowSec();
  U.view = AP::View{};
  U.scrollY = 0;
  U.lastView.clear();
  U.iconsLogged = false;
  U.gen = ~0ull;
  refresh(true);
  primeInputs();
  ML::setShown(true);   // shown from the request on; re-said here (one host overlay at a time)
  if (U.page.available)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] shown (%s, %dx%d): %d rows, %s", ctx.c_str(), bbW, bbH,
                int(U.page.rows.size()), U.page.header.c_str());
  else
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] shown (%s, %dx%d): no table (%s)", ctx.c_str(), bbW, bbH,
                U.page.emptyWhy.c_str());
}

}  // namespace

bool shotRequested() { return gShotPending.exchange(false); }

void setSnapshotForTest(const sr::ach::Snapshot& s) {
  gTestSnap = s;
  gHaveTestSnap = true;
}

int inputSelfTest() {
  int fails = 0;
  auto ck = [&](bool c, const char* what) {
    if (!c) {
      std::fprintf(stderr, "FAIL: achui input %s\n", what);
      ++fails;
    }
  };
  // a harness DOWN held when the page opens: no act on the first frames (no repeat), one DOWN after release + press
  uint8_t hk[512] = {0};
  hk[SDL_SCANCODE_DOWN] = 1;
  primeFrom(hk, 0);
  gActs.clear();
  harnessStep(hk, 100.0, 0);
  harnessStep(hk, 100.6, 0);
  ck(gActs.empty(), "a key held at open neither presses nor repeats");
  hk[SDL_SCANCODE_DOWN] = 0;
  harnessStep(hk, 100.7, 0);
  hk[SDL_SCANCODE_DOWN] = 1;
  harnessStep(hk, 100.8, 0);
  ck(gActs.size() == 1 && gActs[0] == aDown, "... released and pressed again: one DOWN");
  harnessStep(hk, 101.3, 0);
  ck(gActs.size() == 2 && gActs[1] == aDown, "... and then held: it repeats");
  // the left stick held up when the page opens: not an edge, no repeat; B then: BACK
  HI::PadState up;
  up.ly = 30000;
  primeFrom(nullptr, padBits(up));
  gActs.clear();
  padStep(padBits(up), 200.0);
  padStep(padBits(up), 200.6);
  ck(gActs.empty(), "a stick held at open is not a press and does not repeat");
  padStep(uint16_t(padBits(up) | 0x2000), 200.7);
  ck(gActs.size() == 1 && gActs[0] == aBack, "B pressed after the open: BACK");
  padStep(0, 200.8);
  padStep(0x0002, 200.9);
  ck(gActs.size() == 2 && gActs[1] == aDown, "a fresh D-pad DOWN after the release: DOWN");
  gActs.clear();
  primeFrom(nullptr, 0);
  return fails;
}

bool wantsFrame() {
  if (U.shown) return true;
  return ML::isOpen() && ML::openPage() == ML::OverlayPage::Achievements;
}

bool render(const settingsui::FrameCtx& f, bool otherOverlay) {
  const double t = nowSec();
  // one owner (the review's P1-2): another page asked, or another overlay drawing, while this page is up -> hand over at
  // once (the routing is the other page's now: no setShown); while another overlay draws, a request of ours waits
  if (U.shown && (otherOverlay || ML::openPage() != ML::OverlayPage::Achievements)) {
    U.shown = false;
    U.closing = false;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] hidden (handed over to another page)");
    return false;
  }
  if (otherOverlay) return false;
  std::string ctx;
  if (ML::openPage() == ML::OverlayPage::Achievements && ML::takeOpenRequest(&ctx)) showPage(ctx, f.bbW, f.bbH);
  if (!U.shown) return false;
  if (!ensureContext(f.device, f.queue)) {   // never strand the player behind a page that cannot draw
    U.shown = false;
    ML::setShown(false);
    return false;
  }
  ++G.frame;
  collectGrave();
  const float dt = U.lastT > 0 ? float(std::clamp(t - U.lastT, 0.001, 0.1)) : 1.0f / 60.0f;
  U.lastT = t;
  {
    const float W = float(f.bbW), H = float(f.bbH);
    float fw, fh;
    if (W * 720.0f <= H * 1280.0f) {
      fw = W;
      fh = W * 720.0f / 1280.0f;
    } else {
      fh = H;
      fw = H * 1280.0f / 720.0f;
    }
    X.ox = (W - fw) * 0.5f;
    X.oy = (H - fh) * 0.5f;
    X.s = fh / 720.0f;
    X.bbW = f.bbW;
    X.bbH = f.bbH;
  }
  if (std::fabs(F.scale - X.s) > 0.001f) {
    shutdownBackend();
    buildFonts(X.s);
  }
  if (!ensureBackend(f.rtvFormat)) {
    U.shown = false;
    ML::setShown(false);
    return false;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(f.bbW), float(f.bbH));
  io.DeltaTime = dt;
  ImGui_ImplDX12_NewFrame();
  ImGui::NewFrame();
  DL = ImGui::GetBackgroundDrawList();
  gClip.clear();
  gAlpha = 1.0f;

  U.hovered = false;
  gatherInput(t);
  choosePrompts();
  refresh(false);
  const int n = int(U.page.rows.size());
  if (!U.closing) {
    if (U.wheel && inside(kPX0, kPY0, kPX1, kPY1)) AP::scroll(&U.view, -U.wheel, kVisible, n);
    for (Act a : gActs) dispatch(a);
    if (U.clickR) beginClose("right click");
  }
  gActs.clear();
  U.wheel = 0;
  U.openA = approach(U.openA, U.closing ? 0.0f : 1.0f, 12.0f, dt);
  if (!U.closing && U.openA > 0.995f) U.openA = 1.0f;
  U.scrollY = approach(U.scrollY, float(U.view.top), 18.0f, dt);
  if (std::fabs(U.scrollY - float(U.view.top)) < 0.002f) U.scrollY = float(U.view.top);
  gAlpha = smooth01(U.openA);
  const float zoom = 0.98f + 0.02f * smooth01(U.openA);
  {
    const float fw = 1280.0f * X.s, fh = 720.0f * X.s;
    const float cx = X.ox + fw * 0.5f, cy = X.oy + fh * 0.5f;
    X.ox = cx - fw * 0.5f * zoom;
    X.oy = cy - fh * 0.5f * zoom;
    X.s *= zoom;
  }
  drawPanel();
  drawTitle();
  if (U.page.available) drawRows();
  else drawEmpty();
  drawFooter();
  logView();
  noteIcons();
  ML::setMouseHover(U.hovered);
  gAlpha = 1.0f;

  ImGui::Render();
  ID3D12DescriptorHeap* heaps[] = {G.heap.Get()};
  pumpUploads(f.cl);
  f.cl->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), f.cl);
  if (U.shotWanted) {
    U.shotWanted = false;
    gShotPending.store(true);
  }
  if (U.closing && U.openA < 0.01f) {
    U.shown = false;
    U.closing = false;
    ML::setShown(false);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH_PAGE] hidden");
  }
  return true;
}

}  // namespace achui
}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
