// iter-465 modlib lane: THE MOD LIBRARY overlay (gfx/mod_library_ui.hpp, docs/MODS.md "The mod
// library"). Dear ImGui is only the drawing layer (font atlas, draw lists, DX12 renderer); the
// look, the layout, focus, navigation and input are our own code below.
#ifdef _WIN32

#include "mod_library_ui.hpp"
#include "overlay_kit.hpp"

#include "glyph_subst.hpp"
#include "../app/log.hpp"
#include "../kernel/host_automation.hpp"
#include "../kernel/host_input.hpp"
#include "../kernel/mod_library.hpp"
#include "../kernel/pc_controls.hpp"   // lane E E3: hostKeyAct (the keys follow the PC controls)

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sr {
namespace gfx {
namespace modlibui {

namespace {
using Microsoft::WRL::ComPtr;
namespace ML = sr::kernel::modlib;
namespace HI = sr::kernel::hostinput;
namespace PC = sr::kernel::pccontrols;
namespace fs = std::filesystem;
using namespace sr::gfx::overlay;   // iter-466: the shared overlay kit

// ── icons (procedural, so they are crisp at any size) ────────────────────────────────────
void catIcon(const std::string& key, float cx, float cy, float r, ImU32 c) {
  const float t = std::max(1.0f, X.d(r * 0.16f));
  auto P = [&](float x, float y) { return X.p(cx + x * r, cy + y * r); };
  if (key == "all") {
    for (int i = 0; i < 4; ++i) {
      const float x = (i % 2) ? 0.12f : -0.82f, y = (i / 2) ? 0.12f : -0.82f;
      DL->AddRectFilled(P(x, y), P(x + 0.7f, y + 0.7f), c, X.d(r * 0.08f));
    }
  } else if (key == "characters") {
    DL->AddCircleFilled(P(0, -0.42f), X.d(r * 0.36f), c, 20);
    DL->PathArcTo(P(0, 0.95f), X.d(r * 0.78f), 3.14159f * 1.05f, 3.14159f * 1.95f, 20);
    DL->PathFillConvex(c);
  } else if (key == "missions") {
    DL->AddLine(P(-0.6f, -0.9f), P(-0.6f, 0.95f), c, t);
    DL->AddTriangleFilled(P(-0.6f, -0.85f), P(0.85f, -0.45f), P(-0.6f, -0.05f), c);
  } else if (key == "worlds") {
    DL->AddCircle(P(0, 0), X.d(r * 0.85f), c, 28, t);
    DL->AddEllipse(P(0, 0), ImVec2(X.d(r * 0.38f), X.d(r * 0.85f)), c, 0, 24, t);
    DL->AddLine(P(-0.85f, 0), P(0.85f, 0), c, t);
  } else if (key == "gameplay") {
    DL->AddRect(P(-0.95f, -0.5f), P(0.95f, 0.5f), c, X.d(r * 0.42f), 0, t);
    DL->AddLine(P(-0.62f, 0), P(-0.22f, 0), c, t);
    DL->AddLine(P(-0.42f, -0.2f), P(-0.42f, 0.2f), c, t);
    DL->AddCircleFilled(P(0.38f, -0.1f), X.d(r * 0.11f), c, 10);
    DL->AddCircleFilled(P(0.62f, 0.12f), X.d(r * 0.11f), c, 10);
  } else if (key == "visuals") {
    // a sparkle: two crossed diamonds and a small one
    ImVec2 v[4] = {P(0, -0.98f), P(0.22f, 0), P(0, 0.98f), P(-0.22f, 0)};
    ImVec2 h[4] = {P(-0.98f, 0), P(0, -0.22f), P(0.98f, 0), P(0, 0.22f)};
    DL->AddConvexPolyFilled(v, 4, c);
    DL->AddConvexPolyFilled(h, 4, c);
    ImVec2 s2[4] = {P(0.62f, -0.92f), P(0.72f, -0.62f), P(0.62f, -0.32f), P(0.52f, -0.62f)};
    DL->AddConvexPolyFilled(s2, 4, c);
  } else if (key == "audio") {
    DL->AddRectFilled(P(-0.9f, -0.3f), P(-0.42f, 0.3f), c);
    ImVec2 cone[4] = {P(-0.45f, -0.3f), P(0.05f, -0.8f), P(0.05f, 0.8f), P(-0.45f, 0.3f)};
    DL->AddConvexPolyFilled(cone, 4, c);
    DL->PathArcTo(P(0.1f, 0), X.d(r * 0.42f), -0.9f, 0.9f, 12);
    DL->PathStroke(c, 0, t);
    DL->PathArcTo(P(0.1f, 0), X.d(r * 0.78f), -0.9f, 0.9f, 16);
    DL->PathStroke(c, 0, t);
  } else if (key == "interface") {
    DL->AddRect(P(-0.95f, -0.75f), P(0.95f, 0.75f), c, X.d(r * 0.1f), 0, t);
    DL->AddRectFilled(P(-0.95f, -0.75f), P(0.95f, -0.42f), c, X.d(r * 0.1f));
    DL->AddRectFilled(P(-0.7f, -0.2f), P(-0.05f, 0.5f), c);
    DL->AddRectFilled(P(0.12f, -0.2f), P(0.7f, 0.05f), c);
  } else if (key == "builtin") {
    ImVec2 st[10];
    for (int i = 0; i < 10; ++i) {
      const float a = float(i) * 3.14159f / 5.0f;
      const float rr = (i % 2) ? 0.42f : 0.98f;
      st[i] = P(std::sin(a) * rr, -std::cos(a) * rr);
    }
    for (int i = 0; i < 10; i += 2)
      DL->AddTriangleFilled(P(0, 0.02f), st[i], st[(i + 1) % 10], c), DL->AddTriangleFilled(P(0, 0.02f), st[i], st[(i + 9) % 10], c);
  } else if (key == "search") {
    DL->AddCircle(P(-0.18f, -0.18f), X.d(r * 0.58f), c, 20, t * 1.2f);
    DL->AddLine(P(0.25f, 0.25f), P(0.85f, 0.85f), c, t * 1.6f);
  } else {   // other: a cube
    ImVec2 h[6] = {P(0, -0.95f), P(0.85f, -0.48f), P(0.85f, 0.48f), P(0, 0.95f), P(-0.85f, 0.48f), P(-0.85f, -0.48f)};
    DL->AddPolyline(h, 6, c, ImDrawFlags_Closed, t);
    DL->AddLine(P(-0.85f, -0.48f), P(0, 0), c, t);
    DL->AddLine(P(0.85f, -0.48f), P(0, 0), c, t);
    DL->AddLine(P(0, 0), P(0, 0.95f), c, t);
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// The UI state
// ═════════════════════════════════════════════════════════════════════════════════════════
enum Tab { kLibrary = 0, kLoadOrder = 1, kGetMore = 2, kTabCount = 3 };
const char* kTabNames[kTabCount] = {"LIBRARY", "LOAD ORDER", "GET MORE MODS"};
enum Act { aNone = 0, aUp, aDown, aLeft, aRight, aAccept, aBack, aX, aY, aLB, aRB, aLT, aRT, aSearch, aShot };

struct Ui {
  bool shown = false, closing = false;
  float openA = 0;              // 0..1 open animation
  double lastT = 0;
  std::string ctx;
  ML::LibSnapshot snap;
  bool snapDirty = true;
  int tab = kLibrary;
  float tabSlide = 0;           // content slide after a tab change (design units, decays to 0)
  // LIBRARY
  int cat = 0;
  std::string search;
  bool typing = false;
  int zone = 1;                 // 0 rail, 1 grid
  int railFocus = 1;            // 0 = search, 1.. = categories
  int gridFocus = 0;
  float gridScroll = 0, gridScrollT = 0;
  float focusX0 = 0, focusY0 = 0, focusX1 = 0, focusY1 = 0;   // the animated focus frame
  bool focusInit = false;
  // details
  bool details = false;
  float detailsA = 0;
  std::string detId;
  int detFocus = 0;
  int imgIdx = 0;
  float imgFade = 1;
  float detScroll = 0, detScrollT = 0;
  // LOAD ORDER
  int loFocus = 0;
  float loScroll = 0, loScrollT = 0;
  bool grabbed = false;
  // mouse drag (load order)
  bool lDown = false, dragArmed = false, dragging = false;
  std::string dragId;
  float dragFromY = 0, dragY = 0, dragGrabDy = 0;
  // GET MORE MODS
  int gmFocus = 0;
  // restart modal
  bool confirm = false;
  int confirmFocus = 0;
  // toast
  std::string toast;
  double toastT = -100;
  // mouse (stage coords)
  float mx = -1000, my = -1000;
  bool mouseMoved = false, clickL = false, clickR = false, releaseL = false;
  int wheel = 0;
  bool mouseSeen = false;
  // pad / harness keyboard edges
  uint16_t padPrev = 0;
  bool ltPrev = false, rtPrev = false;
  double padRepeatAt[4] = {0, 0, 0, 0};
  uint8_t hkPrev[512] = {0};
  double hkRepeatAt[512] = {0};
  float rsY = 0;                // right stick (scroll)
  bool hovered = false;
  uint64_t frames = 0;
  double firstFrameT = -1;
  bool autoOpened = false;
  bool shotWanted = false;
} U;

void toast(const std::string& s) {
  U.toast = s;
  U.toastT = nowSec();
}

void refresh() {
  U.snap = ML::snapshot();
  U.snapDirty = false;
}

const ML::LibEntry* findMod(const std::string& id) {
  for (const auto& e : U.snap.mods)
    if (e.id == id) return &e;
  return nullptr;
}

std::vector<int> gridItems() {
  return ML::filter(U.snap, ML::categories()[size_t(U.cat)].key, U.search);
}
// The load-order list: the mods (movable), then the built-ins.
std::vector<int> loItems() {
  std::vector<int> v;
  for (int i = 0; i < int(U.snap.mods.size()); ++i)
    if (!U.snap.mods[size_t(i)].builtin) v.push_back(i);
  for (int i = 0; i < int(U.snap.mods.size()); ++i)
    if (U.snap.mods[size_t(i)].builtin) v.push_back(i);
  return v;
}

// ── actions on the model ─────────────────────────────────────────────────────────────────
void toggleMod(const ML::LibEntry& e) {
  std::string msg;
  if (e.builtin) {
    if (!e.hasToggle) {
      toast(ML::upper(e.name) + " IS PART OF THE GAME AND ALWAYS ON");
      return;
    }
    if (ML::setBuiltinOption(e.optionKey, !e.enabled, &msg)) toast(ML::upper(e.name) + (e.enabled ? " OFF" : " ON") + " \xC2\xB7 APPLIES AFTER A RESTART");
    else toast(msg);
  } else if (e.failed && !e.enabled) {
    if (ML::setEnabled(e.id, true, &msg)) toast(ML::upper(e.name) + " ON \xC2\xB7 IT HAS A PROBLEM: " + ML::upper(e.reason));
    else toast(msg);
  } else {
    if (ML::setEnabled(e.id, !e.enabled, &msg))
      toast(ML::upper(e.name) + (e.enabled ? " DISABLED" : " ENABLED") + " \xC2\xB7 APPLIES AFTER A RESTART");
    else toast(msg);
  }
  refresh();
}

void moveMod(const std::string& id, int delta) {
  std::string msg;
  if (!ML::move(id, delta, &msg)) toast(msg);
  refresh();
}

void openDetails(const std::string& id) {
  U.details = true;
  U.detId = id;
  U.detFocus = 0;
  U.imgIdx = 0;
  U.imgFade = 0;
  U.detScroll = U.detScrollT = 0;
}

void beginClose() {
  if (U.closing) return;
  U.closing = true;
  U.typing = false;
  U.confirm = false;
  U.grabbed = false;
  U.dragging = U.dragArmed = false;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] close");
}

// RESTART NOW is always offered: with no change saved it still loads what is in the mods folder
// now (a mod copied in while the game runs is only found at the next start).
void askRestart() {
  if (!U.confirm)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] restart asked (%s)", U.snap.restartRequired ? "changes pending" : "no changes");
  U.confirm = true;
  U.confirmFocus = U.snap.canRestart ? 0 : 1;   // a disabled RESTART NOW never holds the focus (Astra E3 r2)
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Hit testing (immediate mode): mouse in stage coords, honouring the clip stack
// ═════════════════════════════════════════════════════════════════════════════════════════
int gLayer = 0;        // 0 the page, 1 the modal (when a modal is up only layer 1 takes the mouse)
bool inside(float x0, float y0, float x1, float y1) {
  if (U.confirm && gLayer == 0) return false;
  float cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
  if (!gClip.empty()) {
    const RectD& c = gClip.back();
    cx0 = std::max(cx0, c.x0); cy0 = std::max(cy0, c.y0);
    cx1 = std::min(cx1, c.x1); cy1 = std::min(cy1, c.y1);
  }
  return U.mx >= cx0 && U.mx < cx1 && U.my >= cy0 && U.my < cy1;
}
struct Hit { bool hover = false, click = false; };
Hit hit(float x0, float y0, float x1, float y1) {
  Hit h;
  h.hover = inside(x0, y0, x1, y1);
  if (h.hover) U.hovered = true;
  h.click = h.hover && U.clickL;
  if (h.click) U.clickL = false;   // one consumer per click
  return h;
}

// A scroll bar a mouse can use (iter-465 review: the grid's lower rows must be obviously
// reachable with a mouse): the thumb drags, a click on the track pages toward the click, and it
// lights when hovered. `id` names the bar that owns a drag. Draw it after the content it scrolls
// (it sits outside the content's hit rects, so the order of clicks does not matter).
int gSbDrag = 0;
float gSbGrab = 0;
void scrollBar(int id, float x, float y0, float y1, float viewH, float maxS, float scroll, float& scrollT, float a = 1.0f) {
  if (maxS <= 1) {
    if (gSbDrag == id) gSbDrag = 0;
    return;
  }
  const float w = 6, trackH = y1 - y0;
  const float thumbH = std::max(32.0f, trackH * viewH / (viewH + maxS));
  const float span = trackH - thumbH;
  const float thumbY = y0 + span * clampf(scroll / maxS, 0, 1);
  const bool overBar = inside(x - 5, y0, x + w + 5, y1);
  const bool overThumb = inside(x - 5, thumbY, x + w + 5, thumbY + thumbH);
  if (overBar) U.hovered = true;
  if (overBar && U.clickL) {
    U.clickL = false;
    if (overThumb) {
      gSbDrag = id;
      gSbGrab = U.my - thumbY;
    } else {
      scrollT += (U.my < thumbY ? -1.0f : 1.0f) * viewH * 0.85f;
    }
  }
  if (gSbDrag == id) {
    if (!U.lDown) gSbDrag = 0;
    else if (span > 0) scrollT = clampf((U.my - gSbGrab - y0) / span, 0, 1) * maxS;
  }
  const bool hot = overThumb || gSbDrag == id;
  DL->AddRectFilled(X.p(x, y0), X.p(x + w, y1), col(0.01f, 0.04f, 0.14f, 0.70f * a), X.d(3));
  DL->AddRect(X.p(x, y0), X.p(x + w, y1), col(0.45f, 0.66f, 1.0f, (overBar ? 0.55f : 0.30f) * a), X.d(3), 0,
              std::max(1.0f, X.d(0.8f)));
  DL->AddRectFilled(X.p(x + 0.8f, thumbY + 0.8f), X.p(x + w - 0.8f, thumbY + thumbH - 0.8f),
                    hot ? C(kGold, a) : col(0.78f, 0.86f, 0.98f, 0.80f * a), X.d(2.5f));
  // grip lines
  const float gy = thumbY + thumbH * 0.5f;
  for (int k = -1; k <= 1; ++k)
    DL->AddLine(X.p(x + 1.8f, gy + float(k) * 3.0f), X.p(x + w - 1.8f, gy + float(k) * 3.0f),
                col(0.05f, 0.10f, 0.25f, 0.55f * a), std::max(1.0f, X.d(0.8f)));
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Page frame: backdrop, chrome, header (title, tabs, counts), status line, prompt bar
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kFX0 = 22, kFY0 = 14, kFX1 = 1258, kFY1 = 706;
constexpr float kBodyY0 = 126, kBodyY1 = 642;

// key: a fixed keyboard cap for a mode that takes fixed keys, not the bindings (setKeyCapOverride; Astra E3 r3)
struct Prompt { Ctl c; std::string label; int act; const char* key = nullptr; };
std::vector<Prompt> gPrompts, gPromptsLeft;
int gPromptClick = aNone;

void drawBackdrop() {
  // the game behind: dimmed and tinted toward the library's blue
  const ImVec2 a(0, 0), b(float(X.bbW), float(X.bbH));
  DL->AddRectFilledMultiColor(a, b, col(0.0f, 0.03f, 0.12f, 0.70f), col(0.0f, 0.03f, 0.12f, 0.70f),
                              col(0.0f, 0.01f, 0.05f, 0.86f), col(0.0f, 0.01f, 0.05f, 0.86f));
}

void drawFrame(double t) {
  // the main glass panel
  ImVec2 pts[8];
  const int n = chamfer(kFX0, kFY0, kFX1, kFY1, 26, 10, 26, 10, pts);
  gradPoly(pts, n, col(0.05f, 0.16f, 0.44f, 0.975f), col(0.01f, 0.05f, 0.18f, 0.985f));
  // faint vertical light streaks, like the game's popup glass
  for (int i = 0; i < 9; ++i) {
    const float x = kFX0 + 60 + i * 140.0f + 20.0f * std::sin(float(i) * 1.7f);
    const float w = 18 + 10 * std::sin(float(i) * 2.3f);
    DL->AddRectFilledMultiColor(X.p(x, kFY0 + 70), X.p(x + w, kFY1 - 60), col(0.5f, 0.7f, 1.0f, 0.0f),
                                col(0.5f, 0.7f, 1.0f, 0.035f), col(0.5f, 0.7f, 1.0f, 0.0f), col(0.5f, 0.7f, 1.0f, 0.0f));
  }
  DL->AddPolyline(pts, n, col(0.48f, 0.70f, 1.0f, 0.55f), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  // the crystal chrome at the top and the bottom (the game's popup frame)
  chrome(kFX0 + 160, kFY0 - 6, kFX1 - 160, kFY0 + 8, true);
  chrome(kFX0 + 160, kFY1 - 8, kFX1 - 160, kFY1 + 6, false);
  // header divider: a bright line that fades at both ends
  const float y = 92;
  const ImU32 e0 = col(0.6f, 0.8f, 1.0f, 0.0f), e1 = col(0.6f, 0.8f, 1.0f, 0.55f);
  DL->AddRectFilledMultiColor(X.p(kFX0 + 20, y), X.p(640, y + 1.2f), e0, e1, e1, e0);
  DL->AddRectFilledMultiColor(X.p(640, y), X.p(kFX1 - 20, y + 1.2f), e1, e0, e0, e1);
  (void)t;
}

void drawHeader(double t) {
  shield(62, 52, 24);
  textTitle(F.display, kSzDisplay, 96, 26, "MODS");
  // tabs
  float x = 312;
  const float ty = 44;
  glyph(cLB, x - 12 - glyphW(cLB, 18) * 0.5f, ty + 11, 18);
  for (int i = 0; i < kTabCount; ++i) {
    const std::string name = kTabNames[i];
    const float w = textW(F.tab, kSzTab, name);
    const bool act = U.tab == i;
    Hit h = hit(x - 10, ty - 8, x + w + 10, ty + 30);
    if (h.click && !U.confirm) {
      if (U.tab != i) U.tabSlide = (i > U.tab ? 40.0f : -40.0f);
      U.tab = i;
      U.details = false;
      U.typing = false;
    }
    if (act) {
      // red bar with a gold edge, and a soft glow
      DL->AddRectFilledMultiColor(X.p(x - 6, ty + 25), X.p(x + w + 6, ty + 29), C(kRed), C(kRed), C(kRedDeep), C(kRedDeep));
      DL->AddRectFilled(X.p(x - 6, ty + 24), X.p(x + w + 6, ty + 25.2f), C(kGold));
      DL->AddRectFilledMultiColor(X.p(x - 10, ty - 6), X.p(x + w + 10, ty + 24), col(0.6f, 0.8f, 1.0f, 0.0f),
                                  col(0.6f, 0.8f, 1.0f, 0.0f), col(0.6f, 0.8f, 1.0f, 0.16f), col(0.6f, 0.8f, 1.0f, 0.16f));
      textTitle(F.tab, kSzTab, x, ty, name);
    } else {
      const float a = h.hover ? 0.95f : 0.62f;
      text(F.tab, kSzTab, x, ty, name, col(0.80f, 0.86f, 0.96f, a), col(0.60f, 0.68f, 0.82f, a), 1.0f);
    }
    x += w + 38;
  }
  glyphAt(cRB, x - 26, ty + 11, 18);
  // counts, right-aligned
  char buf[96];
  std::snprintf(buf, sizeof buf, "%d INSTALLED", U.snap.installed);
  const std::string a = buf;
  std::snprintf(buf, sizeof buf, "%d ENABLED", U.snap.enabledCount);
  const std::string b = buf;
  const float wb = textW(F.label, kSzLabel, b), wa = textW(F.label, kSzLabel, a);
  const float rx = kFX1 - 34;
  text(F.label, kSzLabel, rx - wb, 46, b, C(kGold), C(kGoldDeep), 1.0f);
  DL->AddCircleFilled(X.p(rx - wb - 12, 55), X.d(2.2f), C(kSilverDim), 8);
  text(F.label, kSzLabel, rx - wb - 24 - wa, 46, a, C(kWhite), C(kSilver), 1.0f);
  (void)t;
}

// The line under the header: what is shown on the left, the restart banner on the right.
void drawStatusLine(double t, const std::string& left) {
  text(F.label, kSzLabel, 44, 100, left, C(kSilver), C(kSilverDim), 1.0f);
  if (!U.snap.restartRequired) return;
  // the banner: red glass with a gold rim and a breathing glow
  const std::string msg = "CHANGES APPLY AFTER A RESTART";
  const std::string act = "RESTART NOW";
  const float wm = textW(F.badge, 11.5f, msg), wa = textW(F.card, 13, act), gw = glyphW(cY, 16);
  const float x1 = kFX1 - 34, x0 = x1 - (wm + wa + 58 + gw), y0 = 96, y1 = 120;
  Hit h = hit(x0, y0, x1, y1);
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 8, 0, 8, 0, p);
  const float pulse = 0.75f + 0.25f * float(std::sin(t * 2.6));
  gradPoly(p, n, col(0.70f, 0.08f, 0.10f, 0.92f), col(0.38f, 0.02f, 0.05f, 0.95f));
  DL->AddPolyline(p, n, col(kGold.r, kGold.g, kGold.b, h.hover ? 1.0f : pulse), ImDrawFlags_Closed,
                  std::max(1.0f, X.d(1.4f)));
  textPlain(F.badge, 11.5f, x0 + 12, y0 + 6.5f, msg, col(1.0f, 0.90f, 0.70f));
  glyphAt(cY, x0 + 18 + wm, y0 + 12, 16);
  text(F.card, 13, x0 + 24 + wm + gw, y0 + 4.5f, act, C(kWhite), col(0.9f, 0.85f, 0.75f), 1.0f);
  if (h.click) askRestart();
}

void drawPromptBar() {
  // right-aligned like the game ("SELECT (A)  BACK (B)"), each one clickable
  float x = kFX1 - 40;
  const float y = 668;
  for (int i = int(gPrompts.size()) - 1; i >= 0; --i) {
    const Prompt& p = gPrompts[size_t(i)];
    const std::string l = ML::upper(p.label);
    if (p.key) setKeyCapOverride(p.c, p.key);
    const float w = textW(F.card, 14, l);
    const float gw = std::max(22.0f, glyphW(p.c, 22));
    const float x0 = x - w - gw - 10;
    Hit h = hit(x0 - 6, y - 6, x + 6, y + 24);
    if (h.hover) selBand(x0 - 14, y - 6, x + 14, y + 24, 0.8f);
    textTitle(F.card, 14, x0, y + 1, l);
    glyph(p.c, x - gw * 0.5f, y + 9, 22);
    if (p.key) setKeyCapOverride(p.c, nullptr);
    if (h.click) gPromptClick = p.act;
    x = x0 - 26;
  }
  float lx = kFX0 + 40;
  for (const Prompt& p : gPromptsLeft) {
    const std::string l = ML::upper(p.label);
    float gx = lx;
    if (p.c == cLB || p.c == cLT) {   // a pair: LB RB / LT RT
      // lane E E3: each glyph of the pair is clickable (its own act: previous / next), one action per click
      for (const Ctl g : {p.c, Ctl(p.c + 1)}) {
        const float w = glyphW(g, 20);
        const Hit h = hit(gx - 4, y - 6, gx + w + 4, y + 24);
        if (h.hover) selBand(gx - 8, y - 6, gx + w + 8, y + 24, 0.8f);
        glyphAt(g, gx, y + 9, 20);
        if (h.click) gPromptClick = g == cLB ? aLB : g == cRB ? aRB : g == cLT ? aLT : aRT;
        gx += w + (g == p.c ? 6 : 10);
      }
    } else {
      gx += glyphAt(p.c, gx, y + 9, 20) + 10;
    }
    text(F.smallF, 12.5f, gx, y + 1.5f, l, C(kSilver), C(kSilverDim), 1.0f);
    lx = gx + textW(F.smallF, 12.5f, l) + 26;
  }
}

void drawToast(double t) {
  const double age = t - U.toastT;
  if (age > 3.2 || U.toast.empty()) return;
  const float a = float(std::min(1.0, std::min(age / 0.15, (3.2 - age) / 0.4)));
  const float w = std::min(980.0f, textW(F.label, 13, U.toast) + 48);
  const float x0 = 640 - w * 0.5f, y0 = 606 - 6 * (1 - a), y1 = y0 + 30;
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x0 + w, y1, 9, 0, 9, 0, p);
  gradPoly(p, n, col(0.10f, 0.28f, 0.66f, 0.96f * a), col(0.03f, 0.10f, 0.32f, 0.96f * a));
  DL->AddPolyline(p, n, col(kGold.r, kGold.g, kGold.b, 0.9f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  const std::string s = ellipsize(F.label, 13, U.toast, w - 40);
  text(F.label, 13, 640 - textW(F.label, 13, s) * 0.5f, y0 + 6.5f, s, C(kWhite, a), C(kSilver, a), 1.0f);
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Card art: the mod's preview, or generated art (category tint + icon + name) when it has none
// ═════════════════════════════════════════════════════════════════════════════════════════
Rgb catTint(const std::string& c) {
  if (c == "characters") return {0.62f, 0.10f, 0.16f};
  if (c == "missions") return {0.62f, 0.42f, 0.08f};
  if (c == "worlds") return {0.06f, 0.42f, 0.48f};
  if (c == "gameplay") return {0.18f, 0.44f, 0.20f};
  if (c == "visuals") return {0.40f, 0.18f, 0.62f};
  if (c == "audio") return {0.12f, 0.34f, 0.66f};
  if (c == "interface") return {0.28f, 0.36f, 0.50f};
  if (c == "builtin") return {0.66f, 0.50f, 0.12f};
  return {0.20f, 0.30f, 0.52f};
}

void generatedArt(const ML::LibEntry& e, float x0, float y0, float x1, float y1, float a) {
  const Rgb t = catTint(e.category);
  DL->AddRectFilledMultiColor(X.p(x0, y0), X.p(x1, y1), col(t.r * 0.9f, t.g * 0.9f, t.b * 0.9f, a),
                              col(t.r * 0.45f, t.g * 0.45f, t.b * 0.55f, a), col(0.02f, 0.05f, 0.14f, a),
                              col(t.r * 0.35f, t.g * 0.35f, t.b * 0.45f, a));
  // diagonal speed lines, spaced per mod (an FNV-1a of the id) so two cards of one category
  // still differ
  uint32_t hs = 2166136261u;
  for (const char ch : e.id) hs = (hs ^ uint8_t(ch)) * 16777619u;
  pushClip(x0, y0, x1, y1);
  const float w = x1 - x0, h = y1 - y0;
  const float pitch = 0.12f + 0.06f * float(hs & 0xFF) / 255.0f, phase = float((hs >> 8) & 0xFF) / 255.0f;
  const float thick = 0.025f + 0.025f * float((hs >> 16) & 0xFF) / 255.0f;
  for (int i = 0; i < 12; ++i) {
    const float sx = x0 + w * (pitch * (float(i) + phase) - 0.25f);
    ImVec2 q[4] = {X.p(sx, y1), X.p(sx + h * 0.9f, y0), X.p(sx + h * 0.9f + w * thick, y0), X.p(sx + w * thick, y1)};
    DL->AddConvexPolyFilled(q, 4, col(1, 1, 1, 0.035f * a));
  }
  // the category's emblem, centred. No name on the art: the library prints it under every card
  // and beside the hero (iter-465 review: a title on the art and under it read twice).
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, r = h * 0.30f;
  DL->AddCircleFilled(X.p(cx, cy), X.d(r * 1.35f), col(0.0f, 0.02f, 0.08f, 0.22f * a), 48);
  DL->AddCircle(X.p(cx, cy), X.d(r * 1.35f), col(1, 1, 1, 0.10f * a), 48, std::max(1.0f, X.d(h * 0.012f)));
  catIcon(e.category, cx, cy, r * 0.8f, col(1, 1, 1, 0.30f * a));
  popClip();
}

// Draws the image `path` cover-fitted into the rect (crop to fill), or generated art.
void coverImage(const ML::LibEntry& e, const std::string& path, int maxW, float x0, float y0, float x1, float y1,
                float a = 1.0f) {
  int w = 0, h = 0;
  const ImTextureID tex = path.empty() ? ImTextureID(0) : image(path, maxW, maxW, &w, &h);
  if (!tex || w <= 0 || h <= 0) {
    generatedArt(e, x0, y0, x1, y1, a);
    if (!path.empty() && !imageFailed(path, maxW)) {
      // loading: a soft shimmer over the generated art
      sheen(x0, y0, x1, y1, nowSec() * 2.0, 0.6f * a);
    }
    return;
  }
  const float rw = x1 - x0, rh = y1 - y0;
  const float ia = float(w) / float(h), ra = rw / rh;
  ImVec2 uv0(0, 0), uv1(1, 1);
  if (ia > ra) {   // wider: crop the sides
    const float k = ra / ia;
    uv0.x = (1 - k) * 0.5f;
    uv1.x = 1 - uv0.x;
  } else {
    const float k = ia / ra;
    uv0.y = (1 - k) * 0.5f;
    uv1.y = 1 - uv0.y;
  }
  DL->AddImage(tex, X.p(x0, y0), X.p(x1, y1), uv0, uv1, col(1, 1, 1, a));
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// LIBRARY: category rail + search + card grid
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kRailX0 = 40, kRailX1 = 254;
constexpr float kGridX0 = 272, kGridX1 = 1224;   // the header's right margin; the scroll bar sits beyond it
constexpr int kCols = 4;
constexpr float kGap = 16;
constexpr float kCardW = (kGridX1 - kGridX0 - kGap * (kCols - 1)) / kCols;
constexpr float kThumbH = kCardW * 9.0f / 16.0f;
constexpr float kCardH = kThumbH + 60;
constexpr float kRowPitch = kCardH + 18;
constexpr float kSearchY0 = 130, kSearchY1 = 164;
constexpr float kCatY0 = 178, kCatH = 34;

void drawSearch(double t, bool focus) {
  const float x0 = kRailX0, x1 = kRailX1, y0 = kSearchY0, y1 = kSearchY1;
  Hit h = hit(x0, y0, x1, y1);
  if (h.hover && U.mouseMoved) { U.zone = 0; U.railFocus = 0; }
  if (h.click) {
    U.zone = 0;
    U.railFocus = 0;
    U.typing = true;
  }
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 9, 0, 9, 0, p);
  gradPoly(p, n, col(0.01f, 0.05f, 0.16f, 0.92f), col(0.03f, 0.10f, 0.28f, 0.92f));
  const bool hot = focus || U.typing;
  DL->AddPolyline(p, n, hot ? C(kGold, U.typing ? 1.0f : 0.85f) : col(0.48f, 0.68f, 1.0f, 0.55f), ImDrawFlags_Closed,
                  std::max(1.0f, X.d(hot ? 1.6f : 1.1f)));
  catIcon("search", x0 + 18, (y0 + y1) * 0.5f, 7.5f, hot ? C(kWhite) : C(kSilver));
  const float tx = x0 + 34;
  pushClip(tx, y0, x1 - 8, y1);
  if (U.search.empty() && !U.typing) {
    textPlain(F.label, 13, tx, y0 + 9, "SEARCH MODS", col(0.62f, 0.70f, 0.84f, 0.85f));
  } else {
    // keep the caret end visible
    const float w = textW(F.label, 13.5f, U.search);
    const float room = (x1 - 8) - tx - 6;
    const float off = w > room ? w - room : 0;
    textPlain(F.label, 13.5f, tx - off, y0 + 8.5f, U.search, C(kWhite));
    if (U.typing && std::fmod(t, 1.0) < 0.55) {
      const float cx = tx - off + w + 1.5f;
      DL->AddRectFilled(X.p(cx, y0 + 8), X.p(cx + 1.6f, y1 - 8), C(kGold));
    }
  }
  popClip();
}

void drawRail(double t) {
  drawSearch(t, U.zone == 0 && U.railFocus == 0);
  const auto& cats = ML::categories();
  for (int i = 0; i < int(cats.size()); ++i) {
    const float y0 = kCatY0 + float(i) * kCatH, y1 = y0 + kCatH - 4;
    const bool sel = U.cat == i;
    const bool focus = U.zone == 0 && U.railFocus == i + 1;
    Hit h = hit(kRailX0, y0, kRailX1, y1);
    if (h.hover && U.mouseMoved && !U.typing) { U.zone = 0; U.railFocus = i + 1; }
    if (h.click) {
      if (U.cat != i) { U.cat = i; U.gridFocus = 0; U.gridScrollT = 0; }
      U.zone = 0;
      U.railFocus = i + 1;
      U.typing = false;
    }
    if (sel) {
      // the active filter: a lit plate with the red-and-gold accent
      DL->AddRectFilledMultiColor(X.p(kRailX0, y0), X.p(kRailX1, y1), col(0.25f, 0.45f, 0.85f, 0.42f),
                                  col(0.25f, 0.45f, 0.85f, 0.06f), col(0.25f, 0.45f, 0.85f, 0.06f), col(0.25f, 0.45f, 0.85f, 0.42f));
      DL->AddRectFilled(X.p(kRailX0, y0), X.p(kRailX0 + 4, y1), C(kRed));
      DL->AddRectFilled(X.p(kRailX0 + 4, y0), X.p(kRailX0 + 5.2f, y1), C(kGold));
    }
    if (focus) selBand(kRailX0 - 4, y0 - 1, kRailX1 + 4, y1 + 1, 1.0f);
    else if (h.hover) selBand(kRailX0, y0, kRailX1, y1, 0.45f);
    const ImU32 ic = sel || focus ? C(kWhite) : C(kSilver, 0.85f);
    catIcon(cats[size_t(i)].key, kRailX0 + 22, (y0 + y1) * 0.5f, 7.5f, ic);
    const std::string lab = ML::upper(cats[size_t(i)].label);
    if (sel || focus) textTitle(F.label, 13, kRailX0 + 38, y0 + 7.5f, ellipsize(F.label, 13, lab, 150));
    else text(F.label, 13, kRailX0 + 38, y0 + 7.5f, ellipsize(F.label, 13, lab, 150), C(kSilver, 0.9f), C(kSilverDim, 0.9f), 1.0f);
    const int cnt = ML::countIn(U.snap, cats[size_t(i)].key);
    const std::string cs = std::to_string(cnt);
    const float cw = textW(F.badge, 11, cs);
    textPlain(F.badge, 11, kRailX1 - 10 - cw, y0 + 9, cs, cnt ? C(kSilver, sel ? 1.0f : 0.8f) : C(kSilverDim, 0.5f));
  }
}

void cardBadges(const ML::LibEntry& e, float x, float y) {
  float bx = x;
  if (e.builtin) bx += badge(bx, y, "BUILT-IN", col(0.55f, 0.38f, 0.06f, 0.92f), col(1.0f, 0.93f, 0.70f), C(kGold, 0.9f)) + 5;
  if (e.failed) bx += badge(bx, y, "PROBLEM", col(0.45f, 0.03f, 0.06f, 0.92f), col(1.0f, 0.80f, 0.78f), col(1.0f, 0.4f, 0.4f, 0.9f)) + 5;
  else if (!e.conflicts.empty()) {
    bool loses = false;
    for (const auto& c : e.conflicts) loses = loses || !c.wins;
    bx += badge(bx, y, loses ? "CONFLICT" : "WINS CONFLICT", col(0.62f, 0.06f, 0.08f, 0.92f), col(1.0f, 0.88f, 0.80f),
                C(kGold, 0.8f)) + 5;
  }
  if (e.needsRestart) badge(bx, y, "NEEDS RESTART", col(0.70f, 0.48f, 0.04f, 0.94f), col(0.12f, 0.06f, 0.0f), C(kGold));
}

void drawCard(const ML::LibEntry& e, float x0, float y0, bool focus, double t, float focusA) {
  const float x1 = x0 + kCardW, y1 = y0 + kCardH;
  glass(x0, y0, x1, y1, 12, 1.0f, focus);
  // thumbnail (16:9), inset
  const float tx0 = x0 + 3, ty0 = y0 + 3, tx1 = x1 - 3, ty1 = y0 + 3 + kThumbH - 6;
  pushClip(tx0, ty0, tx1, ty1);
  coverImage(e, e.preview, 720, tx0, ty0, tx1, ty1, (e.enabled || e.builtin) ? 1.0f : 0.55f);
  if (!(e.enabled || e.builtin)) DL->AddRectFilled(X.p(tx0, ty0), X.p(tx1, ty1), col(0.02f, 0.05f, 0.14f, 0.35f));
  // a dark fade at the bottom of the art so the badges read
  DL->AddRectFilledMultiColor(X.p(tx0, ty0), X.p(tx1, ty0 + 30), col(0.0f, 0.02f, 0.08f, 0.45f),
                              col(0.0f, 0.02f, 0.08f, 0.45f), col(0, 0, 0, 0), col(0, 0, 0, 0));
  DL->AddRectFilledMultiColor(X.p(tx0, ty1 - 22), X.p(tx1, ty1), col(0, 0, 0, 0), col(0, 0, 0, 0),
                              col(0.0f, 0.02f, 0.08f, 0.45f), col(0.0f, 0.02f, 0.08f, 0.45f));
  if (focus) sheen(tx0, ty0, tx1, ty1, t, focusA);
  popClip();
  DL->AddLine(X.p(tx0, ty1), X.p(tx1, ty1), col(0.6f, 0.8f, 1.0f, 0.45f), std::max(1.0f, X.d(1.0f)));
  cardBadges(e, tx0 + 6, ty0 + 7);
  // the enabled checkbox, top right (clickable)
  const float cs = 20, cx0 = tx1 - cs - 6, cy0 = ty0 + 6;
  if (!e.builtin || e.hasToggle) {
    DL->AddRectFilled(X.p(cx0 - 3, cy0 - 3), X.p(cx0 + cs + 3, cy0 + cs + 3), col(0, 0.02f, 0.08f, 0.55f), X.d(3));
    checkbox(cx0, cy0, cs, e.enabled, false);
    Hit hc = hit(cx0 - 4, cy0 - 4, cx0 + cs + 4, cy0 + cs + 4);
    if (hc.click) toggleMod(e);
  } else {
    // always on: a small lock
    DL->AddRectFilled(X.p(cx0 + 3, cy0 + 9), X.p(cx0 + 17, cy0 + 20), C(kGold, 0.9f), X.d(2));
    DL->AddCircle(X.p(cx0 + 10, cy0 + 9), X.d(4.6f), C(kGold, 0.9f), 16, X.d(2.0f));
  }
  // title + meta
  const std::string title = ellipsize(F.card, kSzCard, ML::upper(e.name), kCardW - 18);
  if (focus) textTitle(F.card, kSzCard, x0 + 9, ty1 + 8, title);
  else text(F.card, kSzCard, x0 + 9, ty1 + 8, title, col(0.88f, 0.92f, 0.98f), col(0.66f, 0.72f, 0.84f), 1.2f);
  std::string meta = e.author.empty() ? std::string() : "by " + e.author;
  if (!e.version.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + (e.builtin ? e.version : "v" + e.version);
  meta = ellipsize(F.smallF, kSzSmall, meta, kCardW - 44);
  textPlain(F.smallF, kSzSmall, x0 + 9, ty1 + 30, meta, C(kSilver, 0.92f));
  catIcon(e.category, x1 - 17, ty1 + 37, 6.5f, C(kSilverDim, 0.9f));
}

void drawEmptyGrid() {
  const float cx = (kGridX0 + kGridX1) * 0.5f;
  catIcon(ML::categories()[size_t(U.cat)].key, cx, 300, 34, col(0.6f, 0.75f, 1.0f, 0.25f));
  const std::string a = U.search.empty() ? (U.snap.installed == 0 ? "NO MODS INSTALLED YET" : "NOTHING IN THIS CATEGORY")
                                         : "NO MODS MATCH \"" + ML::upper(U.search) + "\"";
  textTitle(F.tab, kSzTab, cx - textW(F.tab, kSzTab, a) * 0.5f, 356, a);
  const std::string b = U.snap.installed == 0 ? "Put a mod's folder or .zip into your mods folder, then restart the game."
                                              : "Try another category, or clear the search.";
  textPlain(F.body, kSzBody, cx - textW(F.body, kSzBody, b) * 0.5f, 390, b, C(kSilver));
  if (U.snap.installed == 0) {
    const std::string d = U.snap.modsDir;
    const std::string ds = ellipsize(F.smallF, kSzSmall, d, 760);
    textPlain(F.smallF, kSzSmall, cx - textW(F.smallF, kSzSmall, ds) * 0.5f, 416, ds, C(kGold, 0.9f));
  }
}

void drawLibrary(double t, float dt) {
  drawRail(t);
  const std::vector<int> items = gridItems();
  const int n = int(items.size());
  if (U.gridFocus >= n) U.gridFocus = std::max(0, n - 1);
  // scrolling: keep the focused row in view
  const int rows = (n + kCols - 1) / kCols;
  const float viewH = kBodyY1 - kBodyY0 - 4;
  const float contentH = rows > 0 ? float(rows) * kRowPitch - 18 : 0;
  const float maxScroll = std::max(0.0f, contentH - viewH + 12);
  static int s_lastGridFocus = -1;
  if (U.zone == 1 && n > 0 && U.gridFocus != s_lastGridFocus) {
    s_lastGridFocus = U.gridFocus;
    const int fr = U.gridFocus / kCols;
    const float fy0 = float(fr) * kRowPitch, fy1 = fy0 + kCardH;
    if (fy0 - U.gridScrollT < 8) U.gridScrollT = fy0 - 8;
    if (fy1 - U.gridScrollT > viewH - 8) U.gridScrollT = fy1 - viewH + 8;
  }
  if (inside(kGridX0, kBodyY0, kGridX1 + 24, kBodyY1) && U.wheel != 0) {
    U.gridScrollT -= float(U.wheel) * kRowPitch * 0.5f;
    U.wheel = 0;
  }
  U.gridScrollT = clampf(U.gridScrollT, 0, maxScroll);
  U.gridScroll = approach(U.gridScroll, U.gridScrollT, 14.0f, dt);
  if (n == 0) {
    drawEmptyGrid();
    return;
  }
  pushClip(kGridX0 - 10, kBodyY0 - 6, kGridX1 + 10, kBodyY1 + 2);
  int focusIdx = -1;
  float fx0 = 0, fy0 = 0;
  for (int k = 0; k < n; ++k) {
    const int r = k / kCols, c = k % kCols;
    const float x0 = kGridX0 + float(c) * (kCardW + kGap);
    const float y0 = kBodyY0 + 4 + float(r) * kRowPitch - U.gridScroll + U.tabSlide * 0.0f;
    if (y0 > kBodyY1 + 4 || y0 + kCardH < kBodyY0 - 8) continue;
    const ML::LibEntry& e = U.snap.mods[size_t(items[size_t(k)])];
    const bool focus = U.zone == 1 && U.gridFocus == k && !U.typing;
    const bool hov = inside(x0, y0, x0 + kCardW, y0 + kCardH);
    if (hov) U.hovered = true;
    if (hov && U.mouseMoved) {
      U.zone = 1;
      U.gridFocus = k;
      U.typing = false;
    }
    // the focused card lifts a little (a raised plate with a shadow; no scaling, so the type
    // stays pixel-exact)
    static std::unordered_map<std::string, float> s_lift;
    float& lift = s_lift[e.id];
    lift = approach(lift, focus ? 1.0f : 0.0f, 16.0f, dt);
    const float ly0 = y0 - 5.0f * lift;
    if (lift > 0.01f) {
      for (int k2 = 3; k2 >= 1; --k2) {
        const float g = float(k2) * 3.0f;
        DL->AddRectFilled(X.p(x0 + 4 - g * 0.3f, ly0 + 10 - g * 0.2f), X.p(x0 + kCardW + 4 + g * 0.3f, ly0 + kCardH + 8 + g),
                          col(0, 0, 0.03f, 0.16f * lift), X.d(10 + g));
      }
    }
    drawCard(e, x0, ly0, focus, t, 1.0f);   // its checkbox takes its own click first
    if (hov && U.clickL) {
      U.clickL = false;
      U.zone = 1;
      U.gridFocus = k;
      openDetails(e.id);
    }
    if (focus) {
      focusIdx = k;
      fx0 = x0;
      fy0 = ly0;
    }
  }
  // no mods installed yet (only the built-ins show): say how to get some, under the cards
  if (U.snap.installed == 0 && U.search.empty()) {
    const float hy0 = kBodyY0 + 4 + float((n + kCols - 1) / kCols) * kRowPitch + 6, hy1 = hy0 + 120;
    glass(kGridX0, hy0, kGridX1, hy1, 14, 0.85f);
    catIcon("other", kGridX0 + 56, (hy0 + hy1) * 0.5f, 26, col(0.6f, 0.78f, 1.0f, 0.55f));
    textTitle(F.tab, kSzTab, kGridX0 + 104, hy0 + 22, "NO MODS INSTALLED YET");
    textPlain(F.body, kSzBody, kGridX0 + 104, hy0 + 50,
              "Put a mod's folder or .zip into your mods folder, then restart the game.", C(kSilver));
    textPlain(F.smallF, kSzSmall, kGridX0 + 104, hy0 + 76, ellipsize(F.smallF, kSzSmall, U.snap.modsDir, 560), C(kGold, 0.9f));
    const float bx = kGridX1 - 300;
    const float gw = glyphAt(cRB, bx - 10, hy0 + 64, 20);
    textTitle(F.card, 14, bx - 2 + gw, hy0 + 56, "GET MORE MODS");
  }
  // the focus frame glides between cards
  if (focusIdx >= 0) {
    if (!U.focusInit) {
      U.focusX0 = fx0; U.focusY0 = fy0; U.focusInit = true;
    }
    U.focusX0 = approach(U.focusX0, fx0, 22.0f, dt);
    U.focusY0 = approach(U.focusY0, fy0, 22.0f, dt);
    focusFrame(U.focusX0, U.focusY0, U.focusX0 + kCardW, U.focusY0 + kCardH, 12, 1.0f, t);
  }
  popClip();
  // fades at the clip edges when there is more to scroll
  if (U.gridScroll > 2)
    DL->AddRectFilledMultiColor(X.p(kGridX0 - 10, kBodyY0 - 6), X.p(kGridX1 + 10, kBodyY0 + 14), col(0.02f, 0.08f, 0.24f, 0.9f),
                                col(0.02f, 0.08f, 0.24f, 0.9f), col(0.02f, 0.08f, 0.24f, 0), col(0.02f, 0.08f, 0.24f, 0));
  if (U.gridScroll < maxScroll - 2) {
    DL->AddRectFilledMultiColor(X.p(kGridX0 - 10, kBodyY1 - 46), X.p(kGridX1 + 10, kBodyY1 + 2), col(0.01f, 0.05f, 0.19f, 0),
                                col(0.01f, 0.05f, 0.19f, 0), col(0.01f, 0.05f, 0.19f, 0.97f), col(0.01f, 0.05f, 0.19f, 0.97f));
    // a "more below" chevron
    const float cx = (kGridX0 + kGridX1) * 0.5f, cy = kBodyY1 - 10;
    DL->AddTriangleFilled(X.p(cx - 9, cy - 5), X.p(cx + 9, cy - 5), X.p(cx, cy + 3), C(kGold, 0.85f));
  }
  scrollBar(1, kGridX1 + 9, kBodyY0, kBodyY1, viewH, maxScroll, U.gridScroll, U.gridScrollT);
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// DETAILS: hero image + screenshot strip | title, meta, badges, actions, description,
// conflicts, the pack's suits
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kHeroX0 = 40, kHeroX1 = 676, kHeroY0 = 130;
constexpr float kHeroY1 = kHeroY0 + (kHeroX1 - kHeroX0) * 9.0f / 16.0f;
constexpr float kDetX0 = 700, kDetX1 = 1224;  // the header's right margin (counts, restart banner)

std::vector<std::string> detImages(const ML::LibEntry& e) {
  std::vector<std::string> v;
  if (!e.preview.empty()) v.push_back(e.preview);
  for (const auto& s : e.screenshots) v.push_back(s);
  if (v.empty()) v.push_back(std::string());   // generated art
  return v;
}

// focus items of the details page: 0 primary toggle, 1 nexus (if any), then the suits
struct DetItem { int kind; int index; };   // kind 0 toggle, 1 nexus, 2 suit
std::vector<DetItem> detItems(const ML::LibEntry& e) {
  std::vector<DetItem> v;
  v.push_back({0, 0});
  if (!e.nexusUrl.empty()) v.push_back({1, 0});
  for (int i = 0; i < int(e.suits.size()); ++i) v.push_back({2, i});
  return v;
}

void activateDetItem(const ML::LibEntry& e, const DetItem& it) {
  std::string msg;
  if (it.kind == 0) {
    toggleMod(e);
  } else if (it.kind == 1) {
    if (ML::openUrl(e.nexusUrl, &msg)) toast("OPENED IN YOUR BROWSER: " + e.nexusUrl);
    else toast(msg);
  } else if (it.kind == 2 && it.index < int(e.suits.size())) {
    const ML::LibSuit& s = e.suits[size_t(it.index)];
    if (ML::setSuitIncluded(s.id, !s.included, &msg))
      toast(ML::upper(s.name) + (s.included ? " REMOVED FROM" : " ADDED TO") + " THE COSTUME LIST \xC2\xB7 APPLIES AFTER A RESTART");
    else toast(msg);
    refresh();
  }
}

float button(float x0, float y0, float w, Ctl g, const std::string& label, bool focus, bool primary, bool enabled,
             double t, bool* clicked, Ctl g2 = cCount) {
  const float h = 40, x1 = x0 + w, y1 = y0 + h;
  Hit hh = hit(x0, y0, x1, y1);
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 11, 0, 11, 0, p);
  if (primary && enabled)
    gradPoly(p, n, col(0.80f, 0.12f, 0.14f, 0.96f), col(0.42f, 0.03f, 0.07f, 0.96f));
  else
    gradPoly(p, n, col(0.12f, 0.30f, 0.66f, enabled ? 0.92f : 0.45f), col(0.03f, 0.11f, 0.34f, enabled ? 0.94f : 0.45f));
  // inner highlight
  DL->AddLine(X.p(x0 + 12, y0 + 1.5f), X.p(x1 - 3, y0 + 1.5f), col(1, 1, 1, 0.30f), std::max(1.0f, X.d(1.0f)));
  DL->AddPolyline(p, n, primary ? C(kGold, 0.85f) : col(0.55f, 0.75f, 1.0f, 0.7f), ImDrawFlags_Closed,
                  std::max(1.0f, X.d(1.2f)));
  if (focus || hh.hover) focusFrame(x0, y0, x1, y1, 11, focus ? 1.0f : 0.55f, t);
  // lane E E3 (Astra r4): the glyphs and the label stay inside the button, as on PC SETTINGS: the glyphs shrink
  // (to 11 px) when they would leave under 40 px for the label, then the second glyph and the first are dropped if
  // they still do not fit; the label is drawn only when at least "..." fits (ellipsized to the rest)
  const float room = std::max(0.0f, w - 30);
  auto glyphsW = [&](float gh) {
    float gwid = 0;
    if (g != cCount) gwid = glyphW(g, gh);
    if (g2 != cCount) gwid += 4 + glyphW(g2, gh);
    return gwid;
  };
  float gh = 22;
  float gw = glyphsW(gh);
  if (gw > room - 40 && gw > 0) {
    gh = std::max(11.0f, gh * std::max(0.0f, room - 40) / gw);
    gw = glyphsW(gh);
  }
  if (gw > room && g2 != cCount) {
    g2 = cCount;
    gw = glyphsW(gh);
  }
  if (gw > room) {
    g = cCount;
    gw = 0;
  }
  if (g != cCount) {
    const float w1 = glyphAt(g, x0 + 11, y0 + h * 0.5f, gh);
    if (g2 != cCount) glyphAt(g2, x0 + 11 + w1 + 4, y0 + h * 0.5f, gh);
  }
  const std::string l = ML::upper(label);
  const float textRoom = room - gw;
  if (textRoom >= textW(F.card, 14.5f, "..."))
    textTitle(F.card, 14.5f, x0 + 20 + gw, y0 + 11, ellipsize(F.card, 14.5f, l, textRoom), enabled ? 1.0f : 0.5f);
  if (clicked) *clicked = hh.click && enabled;
  return h;
}

void drawDetails(double t, float dt) {
  const ML::LibEntry* ep = findMod(U.detId);
  if (!ep) {
    U.details = false;
    return;
  }
  const ML::LibEntry& e = *ep;
  const float a = smooth01(U.detailsA);
  const float slide = (1 - a) * 30;
  // ── left: the hero image and the strip ──
  const std::vector<std::string> imgs = detImages(e);
  if (U.imgIdx >= int(imgs.size())) U.imgIdx = 0;
  U.imgFade = approach(U.imgFade, 1.0f, 10.0f, dt);
  const float hx0 = kHeroX0 - slide, hx1 = kHeroX1 - slide;
  ImVec2 hp[8];
  const int hn = chamfer(hx0 - 4, kHeroY0 - 4, hx1 + 4, kHeroY1 + 4, 16, 6, 16, 6, hp);
  gradPoly(hp, hn, col(0.30f, 0.55f, 1.0f, 0.55f * a), col(0.05f, 0.18f, 0.55f, 0.55f * a));
  pushClip(hx0, kHeroY0, hx1, kHeroY1);
  coverImage(e, imgs[size_t(U.imgIdx)], 1920, hx0, kHeroY0, hx1, kHeroY1, a * U.imgFade);
  popClip();
  DL->AddPolyline(hp, hn, col(0.75f, 0.88f, 1.0f, 0.8f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  // image arrows + counter
  if (imgs.size() > 1) {
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d / %d", U.imgIdx + 1, int(imgs.size()));
    const float cw = textW(F.badge, 11, cnt);
    DL->AddRectFilled(X.p(hx1 - cw - 22, kHeroY1 - 26), X.p(hx1 - 6, kHeroY1 - 6), col(0, 0.02f, 0.08f, 0.7f * a), X.d(4));
    textPlain(F.badge, 11, hx1 - cw - 14, kHeroY1 - 22.5f, cnt, C(kWhite, a));
    for (int side = -1; side <= 1; side += 2) {
      const float cx = side < 0 ? hx0 + 24 : hx1 - 24, cy = (kHeroY0 + kHeroY1) * 0.5f;
      Hit h = hit(cx - 20, cy - 26, cx + 20, cy + 26);
      DL->AddCircleFilled(X.p(cx, cy), X.d(17), col(0, 0.03f, 0.12f, (h.hover ? 0.8f : 0.5f) * a), 24);
      DL->AddTriangleFilled(X.p(cx + side * 7, cy), X.p(cx - side * 5, cy - 8), X.p(cx - side * 5, cy + 8), C(kWhite, a));
      if (h.click) {
        U.imgIdx = (U.imgIdx + side + int(imgs.size())) % int(imgs.size());
        U.imgFade = 0.2f;
      }
    }
  }
  // the strip
  const float sy0 = kHeroY1 + 14, sh = 62, sw = sh * 16.0f / 9.0f;
  float sx = hx0;
  for (int i = 0; i < int(imgs.size()) && i < 5; ++i) {
    const bool cur = i == U.imgIdx;
    Hit h = hit(sx, sy0, sx + sw, sy0 + sh);
    pushClip(sx, sy0, sx + sw, sy0 + sh);
    coverImage(e, imgs[size_t(i)], 720, sx, sy0, sx + sw, sy0 + sh, a * (cur ? 1.0f : 0.6f));
    popClip();
    DL->AddRect(X.p(sx, sy0), X.p(sx + sw, sy0 + sh), cur ? C(kGold, a) : col(0.55f, 0.75f, 1.0f, 0.5f * a), 0, 0,
                std::max(1.0f, X.d(cur ? 2.0f : 1.0f)));
    if (h.click) {
      U.imgIdx = i;
      U.imgFade = 0.2f;
    }
    sx += sw + 10;
  }
  if (imgs.size() > 1) {
    textPlain(F.smallF, 11.5f, hx0, sy0 + sh + 12, "LB / RB, LEFT / RIGHT OR CLICK TO BROWSE THE IMAGES", C(kSilverDim, a));
  }

  // ── right: the info column ──
  const float x0 = kDetX0 + slide * 0.5f, x1 = kDetX1;
  float y = 128;
  std::vector<std::string> tl = wrap(F.title, kSzTitle, ML::upper(e.name), x1 - x0);
  if (tl.size() > 2) {
    tl.resize(2);
    tl[1] = ellipsize(F.title, kSzTitle, tl[1] + " \xE2\x80\xA6", x1 - x0);
  }
  for (const auto& l : tl) {
    textTitle(F.title, kSzTitle, x0, y, l, a);
    y += kSzTitle * 1.12f;
  }
  y += 4;
  // meta: author, version, category, size
  std::string meta;
  if (!e.author.empty()) meta = "by " + e.author;
  auto add = [&](const std::string& s) {
    if (!s.empty()) meta += (meta.empty() ? "" : "   \xC2\xB7   ") + s;
  };
  add(e.builtin ? std::string("Built into the game") : (e.version.empty() ? std::string() : "Version " + e.version));
  for (const auto& c : ML::categories())
    if (e.category == c.key) add(c.label);
  uint64_t bytes = 0;
  if (!e.builtin && folderSize(e.id, e.root, &bytes)) add(ML::sizeLabel(bytes));
  textPlain(F.label, 13, x0, y, ellipsize(F.label, 13, meta, x1 - x0), C(kSilver, a));
  y += 24;
  // badges
  {
    float bx = x0;
    if (e.builtin) bx += badge(bx, y, "BUILT-IN", col(0.55f, 0.38f, 0.06f, 0.92f * a), col(1.0f, 0.93f, 0.70f, a), C(kGold, a)) + 6;
    if (e.enabled && !e.failed) bx += badge(bx, y, "ENABLED", col(0.06f, 0.36f, 0.20f, 0.92f * a), col(0.80f, 1.0f, 0.86f, a), C(kOk, a)) + 6;
    if (!e.enabled) bx += badge(bx, y, "DISABLED", col(0.12f, 0.16f, 0.26f, 0.92f * a), col(0.80f, 0.84f, 0.92f, a), C(kSilverDim, a)) + 6;
    if (e.enabled && e.failed) bx += badge(bx, y, "PROBLEM", col(0.62f, 0.06f, 0.08f, 0.92f * a), col(1.0f, 0.88f, 0.80f, a), C(kGold, 0.8f * a)) + 6;
    if (e.mounted) bx += badge(bx, y, "LOADED NOW", col(0.08f, 0.24f, 0.52f, 0.92f * a), col(0.85f, 0.92f, 1.0f, a), C(kBlueHi, a)) + 6;
    if (e.needsRestart) bx += badge(bx, y, "NEEDS RESTART", col(0.70f, 0.48f, 0.04f, 0.94f * a), col(0.12f, 0.06f, 0.0f, a), C(kGold, a)) + 6;
    if (!e.conflicts.empty()) bx += badge(bx, y, "CONFLICT", col(0.62f, 0.06f, 0.08f, 0.92f * a), col(1.0f, 0.88f, 0.80f, a), C(kGold, 0.8f * a)) + 6;
    if (e.isZip) badge(bx, y, "ZIP", col(0.12f, 0.16f, 0.26f, 0.92f * a), col(0.80f, 0.84f, 0.92f, a), C(kSilverDim, a));
  }
  y += 28;
  // actions
  const std::vector<DetItem> items = detItems(e);
  if (U.detFocus >= int(items.size())) U.detFocus = int(items.size()) - 1;
  {
    std::string lab;
    bool can = true;
    if (e.builtin && !e.hasToggle) {
      lab = "ALWAYS ON";
      can = false;
    } else {
      lab = e.enabled ? "DISABLE" : "ENABLE";
      lab += e.builtin ? "" : " MOD";
    }
    bool clicked = false;
    button(x0, y, 236, cA, lab, U.detFocus == 0, true, can, t, &clicked);
    if (clicked) {
      U.detFocus = 0;
      activateDetItem(e, {0, 0});
    }
    if (!e.nexusUrl.empty()) {
      bool c2 = false;
      button(x0 + 252, y, 236, cX, "NEXUS MODS PAGE", U.detFocus == 1, false, true, t, &c2);
      if (c2) {
        U.detFocus = 1;
        activateDetItem(e, {1, 0});
      }
    }
  }
  y += 54;
  // the scrolling part: problem, description, conflicts, suits
  const float sy0b = y, sy1b = kBodyY1;
  pushClip(x0 - 8, sy0b, x1 + 8, sy1b);
  float cy = sy0b - U.detScroll;
  float focusTop = 0, focusBot = 0;
  if (e.failed) {
    const std::vector<std::string> rl = wrap(F.body, 13.5f, "This mod is switched off for this run: " + (e.reason.empty() ? e.status : e.reason), x1 - x0 - 24);
    const float bh = float(rl.size()) * 19 + 16;
    DL->AddRectFilled(X.p(x0, cy), X.p(x1, cy + bh), col(0.45f, 0.03f, 0.06f, 0.75f * a), X.d(4));
    DL->AddRect(X.p(x0, cy), X.p(x1, cy + bh), col(1.0f, 0.45f, 0.45f, 0.8f * a), X.d(4), 0, std::max(1.0f, X.d(1.0f)));
    float ly = cy + 8;
    for (const auto& l : rl) { textPlain(F.body, 13.5f, x0 + 12, ly, l, col(1.0f, 0.88f, 0.86f, a)); ly += 19; }
    cy += bh + 14;
  }
  {
    const std::string d = e.description.empty() ? std::string("No description.") : e.description;
    for (const auto& l : wrap(F.body, kSzBody, d, x1 - x0 - 6)) {
      textPlain(F.body, kSzBody, x0, cy, l, col(0.88f, 0.91f, 0.97f, a));
      cy += 19.5f;
    }
    cy += 12;
  }
  if (!e.conflicts.empty()) {
    textTitle(F.tab, 14, x0, cy, "CONFLICTS", a);
    cy += 24;
    for (const auto& c : e.conflicts) {
      const bool w = c.wins;
      // a marker: gold up-triangle = this mod wins, red down-triangle = the other one wins
      if (w) DL->AddTriangleFilled(X.p(x0 + 6, cy + 3), X.p(x0 + 12, cy + 13), X.p(x0, cy + 13), C(kGold, a));
      else DL->AddTriangleFilled(X.p(x0, cy + 3), X.p(x0 + 12, cy + 3), X.p(x0 + 6, cy + 13), col(1.0f, 0.35f, 0.32f, a));
      const std::string head = (w ? "WINS OVER " : "LOSES TO ") + ML::upper(c.otherName.empty() ? c.otherId : c.otherName);
      textPlain(F.label, 12.5f, x0 + 20, cy, ellipsize(F.label, 12.5f, head, x1 - x0 - 24), w ? C(kGold, a) : col(1.0f, 0.62f, 0.58f, a));
      cy += 18;
      textPlain(F.smallF, 11.5f, x0 + 20, cy, ellipsize(F.smallF, 11.5f, c.what, x1 - x0 - 24), C(kSilverDim, a));
      cy += 22;
    }
    textPlain(F.smallF, 11.5f, x0, cy, "Change who wins in LOAD ORDER: the mod higher in the list wins.", C(kSilver, a));
    cy += 26;
  }
  if (!e.suits.empty()) {
    textTitle(F.tab, 14, x0, cy, "SUITS IN THIS PACK", a);
    cy += 24;
    for (int i = 0; i < int(e.suits.size()); ++i) {
      const ML::LibSuit& s = e.suits[size_t(i)];
      const int fi = (e.nexusUrl.empty() ? 1 : 2) + i;
      const bool focus = U.detFocus == fi;
      const float ry0 = cy - 4, ry1 = cy + 28;
      Hit h = hit(x0 - 6, ry0, x1, ry1);
      if (h.hover && U.mouseMoved) U.detFocus = fi;
      if (focus) {
        selBand(x0 - 8, ry0, x1 + 8, ry1, 1.0f);
        focusTop = ry0 + U.detScroll - sy0b;
        focusBot = ry1 + U.detScroll - sy0b;
      }
      checkbox(x0 + 2, cy + 1, 20, s.included, focus, a);
      text(F.label, 13.5f, x0 + 34, cy + 2, ML::upper(s.name), C(kWhite, a), C(kSilver, a), 1.0f);
      if (s.active) {
        const float nw = textW(F.label, 13.5f, ML::upper(s.name));
        badge(x0 + 44 + nw, cy + 3, "IN THIS SESSION", col(0.08f, 0.24f, 0.52f, 0.92f * a), col(0.85f, 0.92f, 1.0f, a), C(kBlueHi, a));
      }
      if (h.click) {
        U.detFocus = fi;
        activateDetItem(e, {2, i});
      }
      cy += 34;
    }
    // where the suit is worn: the game's own costume screen (the fortress lane), never here
    const float hy = cy + 2;
    DL->AddRectFilled(X.p(x0, hy), X.p(x1, hy + 34), col(0.10f, 0.22f, 0.50f, 0.55f * a), X.d(4));
    shield(x0 + 18, hy + 17, 9, a);
    textPlain(F.label, 13, x0 + 36, hy + 9, "Wear a suit from the Fortress of Solitude > Costumes.", C(kWhite, a));
    cy += 46;
  }
  const float contentH = cy + U.detScroll - sy0b;
  popClip();
  // scroll: wheel / right stick / PgUp-PgDn; keep a focused suit visible
  const float viewH = sy1b - sy0b;
  if (inside(x0 - 8, sy0b, x1 + 24, sy1b) && U.wheel != 0) {
    U.detScrollT -= float(U.wheel) * 60.0f;
    U.wheel = 0;
  }
  U.detScrollT += U.rsY * dt * 600.0f;
  if (focusBot > focusTop) {
    if (focusTop < U.detScrollT) U.detScrollT = focusTop - 6;
    if (focusBot > U.detScrollT + viewH) U.detScrollT = focusBot - viewH + 6;
  }
  const float maxS = std::max(0.0f, contentH - viewH + 8);
  U.detScrollT = clampf(U.detScrollT, 0, maxS);
  U.detScroll = approach(U.detScroll, U.detScrollT, 14.0f, dt);
  if (maxS > 1) {
    scrollBar(3, x1 + 9, sy0b, sy1b, viewH, maxS, U.detScroll, U.detScrollT, a);
    if (U.detScroll < maxS - 2)
      DL->AddRectFilledMultiColor(X.p(x0 - 8, sy1b - 22), X.p(x1 + 8, sy1b), col(0.01f, 0.06f, 0.20f, 0),
                                  col(0.01f, 0.06f, 0.20f, 0), col(0.01f, 0.06f, 0.20f, 0.95f), col(0.01f, 0.06f, 0.20f, 0.95f));
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// LOAD ORDER
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kLoX0 = 40, kLoX1 = 1224, kLoHeadY = 128, kLoY0 = 158, kLoRowH = 44, kLoY1 = 598;
// columns
constexpr float kCHandle = 52, kCNum = 74, kCOn = 112, kCThumb = 150, kCName = 236, kCCat = 700, kCVer = 860,
                kCConf = 960, kCStat = 1120;

void drawLoadOrder(double t, float dt) {
  const std::vector<int> items = loItems();
  const int n = int(items.size());
  int nMods = 0;
  for (int i : items) nMods += U.snap.mods[size_t(i)].builtin ? 0 : 1;
  if (U.loFocus >= n) U.loFocus = std::max(0, n - 1);
  // header row
  const ImU32 hc = C(kSilverDim);
  textPlain(F.badge, 10.5f, kCNum, kLoHeadY + 6, "#", hc);
  textPlain(F.badge, 10.5f, kCOn - 2, kLoHeadY + 6, "ON", hc);
  textPlain(F.badge, 10.5f, kCName, kLoHeadY + 6, "MOD", hc);
  textPlain(F.badge, 10.5f, kCCat, kLoHeadY + 6, "CATEGORY", hc);
  textPlain(F.badge, 10.5f, kCVer, kLoHeadY + 6, "VERSION", hc);
  textPlain(F.badge, 10.5f, kCConf, kLoHeadY + 6, "CONFLICTS", hc);
  textPlain(F.badge, 10.5f, kCStat, kLoHeadY + 6, "STATUS", hc);
  DL->AddLine(X.p(kLoX0, kLoY0 - 6), X.p(kLoX1, kLoY0 - 6), col(0.6f, 0.8f, 1.0f, 0.35f), std::max(1.0f, X.d(1.0f)));
  // row geometry: mods, then a separator, then the built-ins
  auto rowY = [&](int k) {
    float y = float(k) * kLoRowH;
    if (k >= nMods) y += 34;   // the built-in section header
    return y;
  };
  const float contentH = rowY(n - 1) + kLoRowH;
  const float viewH = kLoY1 - kLoY0;
  static int s_lastLoFocus = -1;
  if (!U.dragging && U.loFocus != s_lastLoFocus) {
    s_lastLoFocus = U.loFocus;
    const float fy0 = rowY(U.loFocus), fy1 = fy0 + kLoRowH;
    if (fy0 - U.loScrollT < 0) U.loScrollT = fy0;
    if (fy1 - U.loScrollT > viewH) U.loScrollT = fy1 - viewH;
  }
  if (inside(kLoX0, kLoY0, kLoX1 + 24, kLoY1) && U.wheel != 0) {
    U.loScrollT -= float(U.wheel) * kLoRowH * 1.5f;
    U.wheel = 0;
  }
  U.loScrollT = clampf(U.loScrollT, 0, std::max(0.0f, contentH - viewH));
  U.loScroll = approach(U.loScroll, U.loScrollT, 14.0f, dt);

  // mouse drag: press on a mod row arms, moving > 6 units drags, release drops
  int dropAt = -1;
  if (U.dragging) {
    U.dragY = U.my;
    const float rel = U.dragY - U.dragGrabDy - kLoY0 + U.loScroll + kLoRowH * 0.5f;
    dropAt = std::clamp(int(std::floor(rel / kLoRowH)), 0, std::max(0, nMods - 1));
    // auto-scroll near the edges
    if (U.my < kLoY0 + 20) U.loScrollT -= dt * 400;
    if (U.my > kLoY1 - 20) U.loScrollT += dt * 400;
  }

  pushClip(kLoX0 - 6, kLoY0 - 2, kLoX1 + 6, kLoY1);
  const ML::LibEntry* focused = nullptr;
  for (int k = 0; k < n; ++k) {
    const ML::LibEntry& e = U.snap.mods[size_t(items[size_t(k)])];
    const bool isDragged = U.dragging && e.id == U.dragId;
    float y0 = kLoY0 + rowY(k) - U.loScroll;
    int shown = k + 1;   // the number this row will have
    if (U.dragging && !e.builtin && !isDragged) {
      // make room where the dragged row would land
      int from = -1;
      for (int q = 0; q < nMods; ++q)
        if (U.snap.mods[size_t(items[size_t(q)])].id == U.dragId) from = q;
      if (from >= 0 && dropAt >= 0) {
        if (from < k && k <= dropAt) { y0 -= kLoRowH; --shown; }
        if (dropAt <= k && k < from) { y0 += kLoRowH; ++shown; }
      }
    }
    if (k == nMods && nMods < n) {
      const float sy = kLoY0 + rowY(k) - U.loScroll - 30;
      textPlain(F.badge, 10.5f, kCName, sy + 9, "BUILT-IN FEATURES  \xC2\xB7  PART OF THE PC PORT, ALWAYS LOADED BELOW YOUR MODS", C(kGold, 0.85f));
      DL->AddLine(X.p(kLoX0, sy + 26), X.p(kLoX1, sy + 26), col(kGold.r, kGold.g, kGold.b, 0.30f), std::max(1.0f, X.d(1.0f)));
    }
    if (isDragged) continue;   // drawn last, on top
    if (y0 > kLoY1 || y0 + kLoRowH < kLoY0 - 2) continue;
    const float y1 = y0 + kLoRowH - 4;
    const bool focus = U.loFocus == k && !U.dragging;
    if (focus) focused = &e;
    Hit h;
    h.hover = inside(kLoX0, y0, kLoX1, y1);
    if (h.hover) U.hovered = true;
    if (h.hover && U.mouseMoved && !U.dragging) U.loFocus = k;
    // stripes + focus
    if (k % 2 == 0) DL->AddRectFilled(X.p(kLoX0, y0), X.p(kLoX1, y1), col(0.3f, 0.5f, 1.0f, 0.05f));
    if (focus) {
      selBand(kLoX0 - 4, y0, kLoX1 + 4, y1, 1.0f);
      if (U.grabbed) {
        DL->AddRect(X.p(kLoX0, y0), X.p(kLoX1, y1), C(kGold), X.d(3), 0, X.d(2.0f));
      }
    }
    // the drag handle / lock
    if (!e.builtin) {
      for (int l = 0; l < 3; ++l)
        DL->AddRectFilled(X.p(kCHandle - 8, y0 + 13 + l * 5.5f), X.p(kCHandle + 6, y0 + 15 + l * 5.5f),
                          (focus || h.hover) ? C(kGold) : C(kSilverDim, 0.8f));
      char num[16];
      std::snprintf(num, sizeof num, "%d", shown);
      text(F.card, 14, kCNum, y0 + 11, num, C(kWhite), C(kSilver), 1.0f);
    } else {
      DL->AddRectFilled(X.p(kCHandle - 6, y0 + 19), X.p(kCHandle + 6, y0 + 30), C(kGold, 0.9f), X.d(2));
      DL->AddCircle(X.p(kCHandle, y0 + 19), X.d(4.2f), C(kGold, 0.9f), 16, X.d(1.8f));
    }
    // enabled checkbox
    if (!e.builtin || e.hasToggle) {
      checkbox(kCOn, y0 + 9, 20, e.enabled, focus);
      Hit hc = hit(kCOn - 4, y0 + 5, kCOn + 24, y0 + 33);
      if (hc.click) {
        U.loFocus = k;
        toggleMod(e);
      }
    } else {
      textPlain(F.badge, 10, kCOn - 6, y0 + 14, "ALWAYS", C(kGold, 0.85f));
    }
    // thumbnail + name + author
    pushClip(kCThumb, y0 + 3, kCThumb + 64, y0 + 39);
    coverImage(e, e.preview, 720, kCThumb, y0 + 3, kCThumb + 64, y0 + 39, (e.enabled || e.builtin) ? 1.0f : 0.5f);
    popClip();
    DL->AddRect(X.p(kCThumb, y0 + 3), X.p(kCThumb + 64, y0 + 39), col(0.55f, 0.75f, 1.0f, 0.45f), 0, 0, std::max(1.0f, X.d(1.0f)));
    const std::string nm = ellipsize(F.card, 14, ML::upper(e.name), kCCat - kCName - 20);
    if (focus) textTitle(F.card, 14, kCName, y0 + 4, nm);
    else text(F.card, 14, kCName, y0 + 4, nm, (e.enabled || e.builtin) ? C(kWhite) : C(kSilver, 0.7f),
              (e.enabled || e.builtin) ? C(kSilver) : C(kSilverDim, 0.7f), 1.0f);
    textPlain(F.smallF, 11.5f, kCName, y0 + 23, ellipsize(F.smallF, 11.5f, e.author.empty() ? "" : "by " + e.author, kCCat - kCName - 20),
              C(kSilverDim));
    // category
    std::string catLabel = "Other";
    for (const auto& c : ML::categories())
      if (e.category == c.key) catLabel = c.label;
    catIcon(e.category, kCCat + 8, y0 + 19, 6.5f, C(kSilver, 0.85f));
    textPlain(F.label, 12.5f, kCCat + 22, y0 + 11, ellipsize(F.label, 12.5f, catLabel, kCVer - kCCat - 30), C(kSilver));
    textPlain(F.label, 12.5f, kCVer, y0 + 11, ellipsize(F.label, 12.5f, e.builtin ? "built in" : e.version, kCConf - kCVer - 10), C(kSilver));
    // conflict markers
    {
      int wins = 0, loses = 0;
      for (const auto& c : e.conflicts) (c.wins ? wins : loses)++;
      float cx = kCConf;
      if (wins) {
        DL->AddTriangleFilled(X.p(cx + 6, y0 + 13), X.p(cx + 12, y0 + 23), X.p(cx, y0 + 23), C(kGold));
        const std::string s = "WINS " + std::to_string(wins);
        textPlain(F.badge, 11, cx + 16, y0 + 13, s, C(kGold));
        cx += 24 + textW(F.badge, 11, s) + 10;
      }
      if (loses) {
        DL->AddTriangleFilled(X.p(cx, y0 + 13), X.p(cx + 12, y0 + 13), X.p(cx + 6, y0 + 23), col(1.0f, 0.35f, 0.32f));
        textPlain(F.badge, 11, cx + 16, y0 + 13, "LOSES " + std::to_string(loses), col(1.0f, 0.62f, 0.58f));
      }
      if (!wins && !loses) textPlain(F.smallF, 12, kCConf, y0 + 12, "\xE2\x80\x94", C(kSilverDim, 0.7f));
    }
    // status
    {
      std::string st;
      ImU32 sc;
      if (e.failed) { st = "PROBLEM"; sc = col(1.0f, 0.55f, 0.52f); }
      else if (e.needsRestart) { st = e.enabled ? "ON AFTER RESTART" : "OFF AFTER RESTART"; sc = C(kGold); }
      else if (e.mounted) { st = "LOADED"; sc = C(kOk); }
      else { st = "OFF"; sc = C(kSilverDim); }
      textPlain(F.badge, 11, kCStat, y0 + 13, ellipsize(F.badge, 11, st, kLoX1 - kCStat - 6), sc);
    }
    // press on a mod row (not on its checkbox, which took its click above) arms a drag
    h.click = h.hover && U.clickL;
    if (h.click) U.clickL = false;
    if (h.click && !e.builtin) {
      U.loFocus = k;
      U.dragArmed = true;
      U.dragId = e.id;
      U.dragFromY = U.my;
      U.dragGrabDy = U.my - y0;
    } else if (h.click) {
      U.loFocus = k;
    }
  }
  // the dragged row, lifted, under the mouse
  if (U.dragging) {
    const ML::LibEntry* de = findMod(U.dragId);
    if (de) {
      const float y0 = U.dragY - U.dragGrabDy, y1 = y0 + kLoRowH - 4;
      DL->AddRectFilled(X.p(kLoX0 + 4, y0 + 6), X.p(kLoX1 + 4, y1 + 6), col(0, 0, 0, 0.45f), X.d(4));
      ImVec2 p[8];
      const int pn = chamfer(kLoX0, y0, kLoX1, y1, 9, 0, 9, 0, p);
      gradPoly(p, pn, col(0.16f, 0.38f, 0.80f, 0.96f), col(0.05f, 0.16f, 0.46f, 0.96f));
      DL->AddPolyline(p, pn, C(kGold), ImDrawFlags_Closed, X.d(2.0f));
      for (int l = 0; l < 3; ++l)
        DL->AddRectFilled(X.p(kCHandle - 8, y0 + 13 + l * 5.5f), X.p(kCHandle + 6, y0 + 15 + l * 5.5f), C(kGold));
      checkbox(kCOn, y0 + 9, 20, de->enabled, true);
      pushClip(kCThumb, y0 + 3, kCThumb + 64, y0 + 39);
      coverImage(*de, de->preview, 720, kCThumb, y0 + 3, kCThumb + 64, y0 + 39);
      popClip();
      textTitle(F.card, 14, kCName, y0 + 11, ellipsize(F.card, 14, ML::upper(de->name), kCCat - kCName - 20));
      char num[16];
      std::snprintf(num, sizeof num, "%d", dropAt + 1);
      text(F.card, 14, kCNum, y0 + 11, num, C(kGold), C(kGoldDeep), 1.0f);
    }
  }
  popClip();
  scrollBar(2, kLoX1 + 9, kLoY0, kLoY1, viewH, std::max(0.0f, contentH - viewH), U.loScroll, U.loScrollT);
  // the focused mod's conflicts, spelled out
  {
    const float y0 = kLoY1 + 8;
    DL->AddLine(X.p(kLoX0, y0 - 4), X.p(kLoX1, y0 - 4), col(0.6f, 0.8f, 1.0f, 0.25f), std::max(1.0f, X.d(1.0f)));
    std::string line;
    if (focused && !focused->conflicts.empty()) {
      const auto& c = focused->conflicts.front();
      line = ML::upper(focused->name) + (c.wins ? " WINS " : " LOSES ") + c.what + (c.wins ? " OVER " : " TO ") +
             ML::upper(c.otherName.empty() ? c.otherId : c.otherName);
      if (focused->conflicts.size() > 1) line += "  (+" + std::to_string(focused->conflicts.size() - 1) + " MORE)";
    } else {
      line = "THE MOD HIGHER IN THE LIST WINS WHEN TWO MODS CHANGE THE SAME FILE.";
    }
    textPlain(F.label, 12, kLoX0 + 8, y0 + 6, ellipsize(F.label, 12, line, kLoX1 - kLoX0 - 16),
              focused && !focused->conflicts.empty() ? C(kGold) : C(kSilver));
  }
  // finish a drag
  if (U.dragArmed && !U.dragging && U.lDown && std::fabs(U.my - U.dragFromY) > 6) U.dragging = true;
  if (U.releaseL || !U.lDown) {
    if (U.dragging && dropAt >= 0) {
      int from = -1;
      for (int q = 0; q < nMods; ++q)
        if (U.snap.mods[size_t(items[size_t(q)])].id == U.dragId) from = q;
      if (from >= 0 && from != dropAt) {
        const int steps = dropAt - from;
        const std::string id = U.dragId;
        for (int s = 0; s < std::abs(steps); ++s) {
          std::string msg;
          if (!ML::move(id, steps < 0 ? -1 : 1, &msg)) { toast(msg); break; }
        }
        refresh();
        U.loFocus = dropAt;
        toast("LOAD ORDER SAVED \xC2\xB7 APPLIES AFTER A RESTART");
      }
    } else if (U.dragArmed && !U.dragging) {
      // a plain click on a row: just focus (already done)
    }
    U.dragging = false;
    U.dragArmed = false;
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// GET MORE MODS: an explicitly clicked Nexus Mods link and the mods folder. No catalogue, no
// download buttons (docs/MODS.md requirement 5).
// ═════════════════════════════════════════════════════════════════════════════════════════
void drawGetMore(double t) {
  const float x0 = 64, x1 = 1216, y0 = 136, y1 = 630;
  glass(x0, y0, x1, y1, 20, 0.9f);
  // ── left: how to install, and the two actions ──
  const float lx = x0 + 44;
  shield(lx + 26, y0 + 50, 24);
  textTitle(F.title, kSzTitle, lx + 66, y0 + 26, "FIND MORE MODS");
  textPlain(F.body, kSzBody, lx + 66, y0 + 66, "Mods are made by the community. Browse them on Nexus Mods, then drop them in.",
            C(kSilver));
  const char* steps[3][2] = {
      {"1", "DOWNLOAD A MOD"}, {"2", "PUT IT IN YOUR MODS FOLDER"}, {"3", "RESTART THE GAME"}};
  const char* stepNotes[3] = {"A mod is a folder or a .zip file.", "OPEN MODS FOLDER below takes you there.",
                              "New mods are switched on. Manage them in LIBRARY and LOAD ORDER."};
  float sy = y0 + 106;
  for (int i = 0; i < 3; ++i) {
    DL->AddCircleFilled(X.p(lx + 26, sy + 15), X.d(15), C(kRed), 24);
    DL->AddCircle(X.p(lx + 26, sy + 15), X.d(15), C(kGold), 24, X.d(1.6f));
    textPlain(F.card, 15, lx + 26 - textW(F.card, 15, steps[i][0]) * 0.5f, sy + 5, steps[i][0], C(kWhite));
    textTitle(F.label, 14, lx + 58, sy + 1, steps[i][1]);
    textPlain(F.smallF, 12.5f, lx + 58, sy + 19, stepNotes[i], C(kSilver));
    sy += 46;
  }
  float by = sy + 8;
  bool c1 = false, c2 = false, c3 = false;
  button(lx, by, 400, cA, "BROWSE NEXUS MODS", U.gmFocus == 0, true, true, t, &c1);
  textPlain(F.smallF, kSzSmall, lx + 6, by + 46, std::string("Opens ") + ML::nexusHomeUrl() + " in your web browser.",
            C(kSilver));
  by += 70;
  button(lx, by, 400, cX, "OPEN MODS FOLDER", U.gmFocus == 1, false, true, t, &c2);
  textPlain(F.smallF, kSzSmall, lx + 6, by + 46, ellipsize(F.smallF, kSzSmall, U.snap.modsDir, 560), C(kGold, 0.9f));
  by += 70;
  button(lx, by, 400, cY, "RESTART NOW", U.gmFocus == 2, false, U.snap.canRestart, t, &c3);
  textPlain(F.smallF, kSzSmall, lx + 6, by + 46, "Loads the mods you just put in the folder.", C(kSilver));
  if (c1) { U.gmFocus = 0; std::string m; if (ML::openUrl(ML::nexusHomeUrl(), &m)) toast("NEXUS MODS OPENED IN YOUR BROWSER"); else toast(m); }
  if (c2) { U.gmFocus = 1; std::string m; if (ML::openModsFolder(&m)) toast("MODS FOLDER OPENED"); else toast(m); }
  if (c3) { U.gmFocus = 2; askRestart(); }

  // ── right: your library at a glance ──
  const float rx0 = 700, rx1 = x1 - 40, ry0 = y0 + 30, ry1 = y0 + 380;
  glass(rx0, ry0, rx1, ry1, 14, 1.0f, true);
  textTitle(F.tab, 15, rx0 + 22, ry0 + 16, "YOUR LIBRARY");
  // a collage of the first previews
  {
    float cx = rx0 + 22;
    const float cw = (rx1 - rx0 - 44 - 16) / 3.0f, ch = cw * 9.0f / 16.0f, cy = ry0 + 46;
    int shown = 0;
    for (const auto& e : U.snap.mods) {
      if (e.builtin || shown == 3) continue;
      pushClip(cx, cy, cx + cw, cy + ch);
      coverImage(e, e.preview, 720, cx, cy, cx + cw, cy + ch);
      popClip();
      DL->AddRect(X.p(cx, cy), X.p(cx + cw, cy + ch), col(0.6f, 0.8f, 1.0f, 0.5f), 0, 0, std::max(1.0f, X.d(1.0f)));
      cx += cw + 8;
      ++shown;
    }
    if (!shown) textPlain(F.smallF, kSzSmall, rx0 + 22, cy + 20, "No mods installed yet.", C(kSilver));
  }
  // the numbers
  int conflicts = 0, pending = 0, problems = 0, builtins = 0;
  for (const auto& e : U.snap.mods) {
    if (e.builtin) { ++builtins; continue; }
    conflicts += e.conflicts.empty() ? 0 : 1;
    pending += e.needsRestart ? 1 : 0;
    problems += e.failed ? 1 : 0;
  }
  (void)pending;
  struct Stat { const char* label; std::string n; Rgb c; };
  const Stat stats[6] = {{"INSTALLED", std::to_string(U.snap.installed), kWhite},
                         {"ENABLED", std::to_string(U.snap.enabledCount), kGold},
                         {"IN CONFLICT", std::to_string(conflicts), {1.0f, 0.55f, 0.50f}},
                         {"RESTART NEEDED", U.snap.restartRequired ? "YES" : "NO", kGold},
                         {"WITH A PROBLEM", std::to_string(problems), {1.0f, 0.55f, 0.50f}},
                         {"BUILT-IN FEATURES", std::to_string(builtins), kBlueHi}};
  const float sx0 = rx0 + 22, colW = (rx1 - rx0 - 44) / 2.0f;
  for (int i = 0; i < 6; ++i) {
    const float x = sx0 + float(i % 2) * colW, y = ry0 + 190 + float(i / 2) * 58;
    const std::string& n = stats[i].n;
    text(F.title, 26, x, y, n, C(stats[i].c), col(stats[i].c.r * 0.7f, stats[i].c.g * 0.7f, stats[i].c.b * 0.75f), 1.2f);
    textPlain(F.badge, 10.5f, x + textW(F.title, 26, n) + 10, y + 13, stats[i].label, C(kSilver));
  }
  // safety notes, under the panel
  float ny = ry1 + 16;
  for (const char* note : {"Only install mods you trust. Mods never change your game files: delete a mod's folder to remove it.",
                           "Nexus Mods is a separate website; this game has no account, catalogue or download service of its own."}) {
    for (const auto& l : wrap(F.smallF, 11.5f, note, rx1 - rx0)) {
      textPlain(F.smallF, 11.5f, rx0, ny, l, C(kSilverDim));
      ny += 16;
    }
    ny += 6;
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// RESTART modal
// ═════════════════════════════════════════════════════════════════════════════════════════
void drawConfirm(double t) {
  gLayer = 1;
  DL->AddRectFilled(ImVec2(0, 0), ImVec2(float(X.bbW), float(X.bbH)), col(0, 0.01f, 0.05f, 0.55f));
  const float x0 = 380, x1 = 900, y0 = 250, y1 = 440;
  {
    // an opaque backing: the details text behind the modal must not ghost through the glass
    ImVec2 bp[8];
    const int bn = chamfer(x0, y0, x1, y1, 18, 18 * 0.35f, 18, 18 * 0.35f, bp);
    gradPoly(bp, bn, col(0.03f, 0.10f, 0.30f, 1.0f), col(0.01f, 0.04f, 0.15f, 1.0f));
  }
  glass(x0, y0, x1, y1, 18, 1.0f, true);
  chrome(x0 + 70, y0 - 5, x1 - 70, y0 + 7, true);
  textTitle(F.title, 24, 640 - textW(F.title, 24, "RESTART NOW?") * 0.5f, y0 + 26, "RESTART NOW?");
  const bool inGame = U.ctx == "IG";
  // two short lines: what restarts, and what the player loses
  const std::string l1 = U.snap.restartRequired ? "The game restarts with your mod changes."
                                                : "The game restarts and loads the mods in your mods folder now.";
  const std::string l2 = inGame ? "Progress since your last save is lost." : "You return to the title screen.";
  float ly = y0 + 70;
  for (const std::string& para : {l1, l2})
    for (const auto& l : wrap(F.body, kSzBody, para, x1 - x0 - 80)) {
      textPlain(F.body, kSzBody, 640 - textW(F.body, kSzBody, l) * 0.5f, ly, l, C(kSilver));
      ly += 21;
    }
  bool c1 = false, c2 = false;
  // A presses the focused button and B is NOT NOW (handleConfirm), so A sits on the focused one and B on NOT NOW,
  // both glyphs when NOT NOW is focused (Astra E3 r1); a disabled RESTART NOW never has the focus (r2)
  if (!U.snap.canRestart) U.confirmFocus = 1;
  const bool restartFocused = U.confirmFocus == 0;
  button(x0 + 40, y1 - 66, 210, restartFocused ? cA : cCount, "RESTART NOW", restartFocused, true, U.snap.canRestart,
         t, &c1);
  button(x1 - 250, y1 - 66, 210, restartFocused ? cB : cA, "NOT NOW", !restartFocused, false, true, t, &c2,
         restartFocused ? cCount : cB);
  if (c1) {
    std::string m;
    if (!ML::restartNow(&m)) toast(m);
    U.confirm = false;
  }
  if (c2) U.confirm = false;
  gLayer = 0;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Input: keyboard (SDL events + the harness channel), pad, mouse -> actions
// ═════════════════════════════════════════════════════════════════════════════════════════
std::vector<Act> gActs;
std::string gTyped;
int gBackspace = 0;

// Lane E E3: the keys follow the PC controls (kernel/pc_controls.hpp hostKeyAct: the menu bindings, Enter / Esc as
// failsafes, then the host-only keys; the harness channel keeps the iter-450S test keys). The library's own keys
// come after: Ctrl+F (before the bindings, so it searches even though F is MenuY), Slash = search, F12 = screenshot.
Act keyAct(int sc, uint16_t mods, bool fromHarness) {
  if (sc == SDL_SCANCODE_F && (mods & SDL_KMOD_CTRL)) return aSearch;
  switch (PC::hostKeyAct(uint16_t(sc), (mods & SDL_KMOD_SHIFT) != 0, PC::hostKeymap(fromHarness),
                         PC::config().bindings)) {
    case PC::HostAct::Up: return aUp;
    case PC::HostAct::Down: return aDown;
    case PC::HostAct::Left: return aLeft;
    case PC::HostAct::Right: return aRight;
    case PC::HostAct::Accept: return aAccept;
    case PC::HostAct::Back: return aBack;
    case PC::HostAct::X: return aX;
    case PC::HostAct::Y: return aY;
    case PC::HostAct::LB: return aLB;
    case PC::HostAct::RB: return aRB;
    case PC::HostAct::LT: return aLT;
    case PC::HostAct::RT: return aRT;
    case PC::HostAct::View: return aBack;   // MAP's key = the pad's BACK / VIEW, which closes here too (Astra E3 r1)
    default: break;
  }
  if (sc == SDL_SCANCODE_SLASH) return aSearch;
  return sc == SDL_SCANCODE_F12 ? aShot : aNone;
}

bool isRepeatable(Act a) { return a == aUp || a == aDown || a == aLeft || a == aRight; }

void onKey(int sc, bool down, bool repeat, uint16_t mods, bool fromHarness) {
  if (!down) return;
  gLastWasPad = false;
  // Ctrl+F is the search accelerator everywhere, typing included: consumed before the text path, once per press
  // (Astra E3 r2: a held or repeated harness Ctrl+F typed 'f')
  if (sc == SDL_SCANCODE_F && (mods & SDL_KMOD_CTRL)) {
    if (!repeat && !U.typing) gActs.push_back(aSearch);
    return;
  }
  if (U.typing) {
    // typing in the search box: letters are text, a few keys edit / leave
    if (sc == SDL_SCANCODE_BACKSPACE) { ++gBackspace; return; }
    if (sc == SDL_SCANCODE_ESCAPE) { if (U.search.empty()) U.typing = false; else U.search.clear(); return; }
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_TAB) {
      U.typing = false;
      if (sc != SDL_SCANCODE_DOWN) { U.zone = 1; U.gridFocus = 0; }
      else { U.railFocus = 1; }
      return;
    }
    if (fromHarness && !(mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {   // the harness channel has no text events: its
                                                                     // letters type (not under Ctrl / Alt)
      if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) gTyped.push_back(char('a' + (sc - SDL_SCANCODE_A)));
      else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) gTyped.push_back(char('1' + (sc - SDL_SCANCODE_1)));
      else if (sc == SDL_SCANCODE_0) gTyped.push_back('0');
      else if (sc == SDL_SCANCODE_SPACE) gTyped.push_back(' ');
    }
    return;
  }
  const Act a = keyAct(sc, mods, fromHarness);
  if (a == aNone) return;
  if (repeat && !isRepeatable(a)) return;
  gActs.push_back(a);
}

void pollHarnessKeys(double t) {
  const uint8_t* hk = sr::kernel::automation::harnessKeys();
  if (!hk) return;
  const uint16_t mods = PC::harnessKeyMods(hk);   // its own held Shift / Ctrl (Shift+Tab, Ctrl+F; Astra E3 r1)
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

void pollPad(double t) {
  HI::PadState ps;
  if (!HI::readActivePad(&ps)) {
    U.padPrev = 0;
    U.rsY = 0;
    return;
  }
  uint16_t b = ps.buttons;
  // the left stick drives the D-pad (past a generous dead zone)
  if (ps.ly > 16000) b |= 0x0001;
  if (ps.ly < -16000) b |= 0x0002;
  if (ps.lx < -16000) b |= 0x0004;
  if (ps.lx > 16000) b |= 0x0008;
  const uint16_t edge = uint16_t(b & ~U.padPrev);
  if (b) gLastWasPad = true;
  const Act dirs[4] = {aUp, aDown, aLeft, aRight};
  for (int i = 0; i < 4; ++i) {
    const uint16_t bit = uint16_t(1u << i);
    if (edge & bit) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.40;
    } else if ((b & bit) && t >= U.padRepeatAt[i]) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.085;
    }
  }
  if (edge & 0x1000) gActs.push_back(aAccept);
  if (edge & 0x2000) gActs.push_back(aBack);
  if (edge & 0x4000) gActs.push_back(aX);
  if (edge & 0x8000) gActs.push_back(aY);
  if (edge & 0x0100) gActs.push_back(aLB);
  if (edge & 0x0200) gActs.push_back(aRB);
  if (edge & 0x0020) gActs.push_back(aBack);   // BACK closes too
  const bool lt = ps.lt > 128, rt = ps.rt > 128;
  if (lt && !U.ltPrev) gActs.push_back(aLT);
  if (rt && !U.rtPrev) gActs.push_back(aRT);
  U.ltPrev = lt;
  U.rtPrev = rt;
  U.padPrev = b;
  U.rsY = std::fabs(float(ps.ry)) > 9000 ? -float(ps.ry) / 32767.0f : 0.0f;
  if (U.rsY != 0) gLastWasPad = true;
}

// At open: whatever is already held (the A that pressed MODS) is not a new press.
void primeInputs() {
  HI::PadState ps;
  U.padPrev = HI::readActivePad(&ps) ? ps.buttons : 0;
  U.ltPrev = U.rtPrev = true;
  if (const uint8_t* hk = sr::kernel::automation::harnessKeys()) std::memcpy(U.hkPrev, hk, 512);
  else std::memset(U.hkPrev, 0, sizeof U.hkPrev);
}

void gatherInput(double t) {
  gActs.clear();
  gTyped.clear();
  gBackspace = 0;
  U.mouseMoved = U.clickL = U.clickR = U.releaseL = false;
  U.wheel = 0;
  for (const ML::InputEvent& ev : ML::drainInput()) {
    switch (ev.kind) {
      case ML::InputEvent::Key: onKey(ev.scancode, ev.down, ev.repeat, ev.mods, false); break;
      case ML::InputEvent::Text: if (U.typing) gTyped += ev.text; break;
      case ML::InputEvent::MouseMove:
        if (std::fabs(ev.x - U.mx) > 0.01f || std::fabs(ev.y - U.my) > 0.01f) U.mouseMoved = true;
        U.mx = ev.x; U.my = ev.y; U.mouseSeen = true;
        break;
      case ML::InputEvent::MouseDown:
        U.mx = ev.x; U.my = ev.y; U.mouseSeen = true;
        if (ev.button == 1) { U.clickL = true; U.lDown = true; }
        if (ev.button == 3) U.clickR = true;
        gLastWasPad = false;
        break;
      case ML::InputEvent::MouseUp:
        if (ev.button == 1) { U.releaseL = true; U.lDown = false; }
        break;
      case ML::InputEvent::Wheel: U.wheel += ev.wheel; gLastWasPad = false; break;
    }
  }
  pollHarnessKeys(t);
  pollPad(t);
  if (U.clickR) gActs.push_back(aBack);
  // text editing
  if (U.typing) {
    for (char c : gTyped)
      if (static_cast<unsigned char>(c) >= 0x20 && U.search.size() < 40) U.search.push_back(c);
    while (gBackspace-- > 0 && !U.search.empty()) {
      do U.search.pop_back();
      while (!U.search.empty() && (static_cast<unsigned char>(U.search.back()) & 0xC0) == 0x80);
    }
    if (!gTyped.empty()) { U.gridFocus = 0; U.gridScrollT = 0; }
  }
}

// ── actions per page ─────────────────────────────────────────────────────────────────────
void switchTab(int d) {
  const int nt = (U.tab + d + kTabCount) % kTabCount;
  U.tabSlide = d > 0 ? 40.0f : -40.0f;
  U.tab = nt;
  U.details = false;
  U.typing = false;
  U.grabbed = false;
}

void stepCategory(int d) {
  const int n = int(ML::categories().size());
  U.cat = (U.cat + d + n) % n;
  U.gridFocus = 0;
  U.gridScrollT = 0;
  if (U.zone == 0 && U.railFocus > 0) U.railFocus = U.cat + 1;
}

void handleConfirm(Act a) {
  if (!U.snap.canRestart) U.confirmFocus = 1;   // RESTART NOW disabled: focus and A stay on NOT NOW
  switch (a) {
    case aLeft: case aUp: U.confirmFocus = U.snap.canRestart ? 0 : 1; break;
    case aRight: case aDown: U.confirmFocus = 1; break;
    case aAccept:
      if (U.confirmFocus == 0 && U.snap.canRestart) {
        std::string m;
        if (!ML::restartNow(&m)) toast(m);
      }
      U.confirm = false;
      break;
    case aBack: U.confirm = false; break;
    default: break;
  }
}

void handleLibrary(Act a) {
  const std::vector<int> items = gridItems();
  const int n = int(items.size());
  const int nCats = int(ML::categories().size());
  if (U.details) {
    const ML::LibEntry* e = findMod(U.detId);
    if (!e) { U.details = false; return; }
    const auto di = detItems(*e);
    const int nImg = int(detImages(*e).size());
    switch (a) {
      case aUp: U.detFocus = std::max(0, U.detFocus - 1); break;
      case aDown: U.detFocus = std::min(int(di.size()) - 1, U.detFocus + 1); break;
      case aLeft: case aLB: if (nImg > 1) { U.imgIdx = (U.imgIdx - 1 + nImg) % nImg; U.imgFade = 0.2f; } break;
      case aRight: case aRB: if (nImg > 1) { U.imgIdx = (U.imgIdx + 1) % nImg; U.imgFade = 0.2f; } break;
      case aAccept: activateDetItem(*e, di[size_t(std::clamp(U.detFocus, 0, int(di.size()) - 1))]); break;
      case aX:
        if (!e->nexusUrl.empty()) activateDetItem(*e, {1, 0});
        else activateDetItem(*e, {0, 0});
        break;
      case aY: askRestart(); break;
      case aLT: U.detScrollT -= 120; break;
      case aRT: U.detScrollT += 120; break;
      case aBack: U.details = false; break;
      default: break;
    }
    return;
  }
  switch (a) {
    case aLB: switchTab(-1); return;
    case aRB: switchTab(1); return;
    case aLT: stepCategory(-1); return;
    case aRT: stepCategory(1); return;
    case aY: askRestart(); return;
    case aSearch: U.zone = 0; U.railFocus = 0; U.typing = true; return;
    default: break;
  }
  if (U.zone == 0) {
    switch (a) {
      case aUp: U.railFocus = std::max(0, U.railFocus - 1); break;
      case aDown: U.railFocus = std::min(nCats, U.railFocus + 1); break;
      case aRight: if (n > 0) { U.zone = 1; U.gridFocus = std::min(U.gridFocus, n - 1); } break;
      case aAccept:
        if (U.railFocus == 0) U.typing = true;
        else if (n > 0) { U.zone = 1; U.gridFocus = 0; }
        break;
      case aBack: beginClose(); break;
      default: break;
    }
    if (U.railFocus > 0 && U.cat != U.railFocus - 1) {   // moving through the rail filters live
      U.cat = U.railFocus - 1;
      U.gridFocus = 0;
      U.gridScrollT = 0;
    }
    return;
  }
  // the grid
  switch (a) {
    case aLeft: case aRight: case aUp: case aDown: {
      if (n == 0) { U.zone = 0; U.railFocus = U.cat + 1; break; }
      const int dx = a == aLeft ? -1 : (a == aRight ? 1 : 0), dy = a == aUp ? -1 : (a == aDown ? 1 : 0);
      const int ni = ML::gridMove(U.gridFocus, n, kCols, dx, dy);
      if (ni < 0 && dx < 0) { U.zone = 0; U.railFocus = U.cat + 1; }
      else if (ni < 0 && dy < 0) { /* top row: stay */ }
      else U.gridFocus = ni;
      break;
    }
    case aAccept: if (n > 0) openDetails(U.snap.mods[size_t(items[size_t(U.gridFocus)])].id); break;
    case aX: if (n > 0) toggleMod(U.snap.mods[size_t(items[size_t(U.gridFocus)])]); break;
    case aBack: beginClose(); break;
    default: break;
  }
}

void handleLoadOrder(Act a) {
  const std::vector<int> items = loItems();
  const int n = int(items.size());
  int nMods = 0;
  for (int i : items) nMods += U.snap.mods[size_t(i)].builtin ? 0 : 1;
  if (n == 0) {
    if (a == aBack) beginClose();
    if (a == aLB) switchTab(-1);
    if (a == aRB) switchTab(1);
    return;
  }
  const ML::LibEntry& e = U.snap.mods[size_t(items[size_t(std::clamp(U.loFocus, 0, n - 1))])];
  switch (a) {
    case aUp:
      if (U.grabbed && !e.builtin) {
        if (U.loFocus > 0) { moveMod(e.id, -1); U.loFocus--; }
      } else U.loFocus = std::max(0, U.loFocus - 1);
      break;
    case aDown:
      if (U.grabbed && !e.builtin) {
        if (U.loFocus < nMods - 1) { moveMod(e.id, 1); U.loFocus++; }
      } else U.loFocus = std::min(n - 1, U.loFocus + 1);
      break;
    case aAccept: if (!U.grabbed) toggleMod(e); else { U.grabbed = false; toast("LOAD ORDER SAVED \xC2\xB7 APPLIES AFTER A RESTART"); } break;
    case aX:
      if (e.builtin) { toast("BUILT-IN FEATURES ALWAYS LOAD BELOW YOUR MODS"); break; }
      U.grabbed = !U.grabbed;
      if (!U.grabbed) toast("LOAD ORDER SAVED \xC2\xB7 APPLIES AFTER A RESTART");
      break;
    case aY: askRestart(); break;
    case aLB: switchTab(-1); break;
    case aRB: switchTab(1); break;
    case aBack: if (U.grabbed) { U.grabbed = false; toast("LOAD ORDER SAVED \xC2\xB7 APPLIES AFTER A RESTART"); } else beginClose(); break;
    default: break;
  }
}

void handleGetMore(Act a) {
  std::string m;
  switch (a) {
    case aUp: U.gmFocus = std::max(0, U.gmFocus - 1); break;
    case aDown: U.gmFocus = std::min(2, U.gmFocus + 1); break;
    case aAccept:
      if (U.gmFocus == 0) { if (ML::openUrl(ML::nexusHomeUrl(), &m)) toast("NEXUS MODS OPENED IN YOUR BROWSER"); else toast(m); }
      else if (U.gmFocus == 1) { if (ML::openModsFolder(&m)) toast("MODS FOLDER OPENED"); else toast(m); }
      else askRestart();
      break;
    case aX: if (ML::openModsFolder(&m)) toast("MODS FOLDER OPENED"); else toast(m); break;
    case aY: askRestart(); break;
    case aLB: switchTab(-1); break;
    case aRB: switchTab(1); break;
    case aBack: beginClose(); break;
    default: break;
  }
}

void buildPrompts() {
  gPrompts.clear();
  gPromptsLeft.clear();
  if (U.confirm) {
    gPrompts = {{cA, "Select", aAccept}, {cB, "Not now", aBack}};
    return;
  }
  if (U.tab == kLibrary) {
    if (U.typing) {
      // the search box takes fixed keys (onKey's typing path: Enter / Keypad Enter / Down / Tab = done, Esc = clear
      // or leave), and a rebound MenuConfirm / MenuBack letter types there: the caps name Enter / Esc (Astra E3 r3)
      gPrompts = {{cA, "Done", aAccept, "Enter"}, {cB, "Clear", aBack, "Esc"}};
      return;
    }
    if (U.details) {
      const ML::LibEntry* e = findMod(U.detId);
      std::string what = "Select";
      if (e) {
        const auto di = detItems(*e);
        const DetItem& it = di[size_t(std::clamp(U.detFocus, 0, int(di.size()) - 1))];
        if (it.kind == 0) what = (e->builtin && !e->hasToggle) ? "Always on" : (e->enabled ? "Disable" : "Enable");
        else if (it.kind == 1) what = "Open Nexus page";
        else what = "Include suit";
      }
      gPrompts = {{cA, what, aAccept}};
      if (U.snap.restartRequired) gPrompts.push_back({cY, "Restart now", aY});
      gPrompts.push_back({cB, "Back", aBack});
      if (e && detImages(*e).size() > 1) gPromptsLeft.push_back({cLB, "Images", aNone});
      gPromptsLeft.push_back({cRS, "Scroll", aNone});
      return;
    }
    if (U.zone == 0) gPrompts = {{cA, U.railFocus == 0 ? "Search" : "Browse", aAccept}};
    else gPrompts = {{cA, "Details", aAccept}, {cX, "Enable / disable", aX}};
    if (U.snap.restartRequired) gPrompts.push_back({cY, "Restart now", aY});
    gPrompts.push_back({cB, "Close", aBack});
    gPromptsLeft = {{cLB, "Tabs", aNone}, {cLT, "Category", aNone}};
  } else if (U.tab == kLoadOrder) {
    if (U.grabbed) {
      gPrompts = {{cDpad, "Move", aNone}, {cA, "Drop", aAccept}};
      gMoveLeftRight = false;   // a grabbed mod moves up / down the load order only
    } else {
      gPrompts = {{cA, "Enable / disable", aAccept}, {cX, "Move", aX}};
      if (U.snap.restartRequired) gPrompts.push_back({cY, "Restart now", aY});
      gPrompts.push_back({cB, "Close", aBack});
    }
    gPromptsLeft = {{cLB, "Tabs", aNone}};
  } else {
    gPrompts = {{cA, U.gmFocus == 2 ? "Restart now" : "Open", aAccept}};
    if (U.gmFocus != 2) gPrompts.push_back({cY, "Restart now", aY});   // always offered here
    gPrompts.push_back({cB, "Close", aBack});
    gPromptsLeft = {{cLB, "Tabs", aNone}};
  }
}

void dispatch(Act a) {
  if (a == aShot) {
    U.shotWanted = !ML::shotDir().empty();
    return;
  }
  if (U.confirm) { handleConfirm(a); return; }
  if (U.tab == kLibrary) handleLibrary(a);
  else if (U.tab == kLoadOrder) handleLoadOrder(a);
  else handleGetMore(a);
}

// ── the composited-frame capture (SR_MOD_LIBRARY_SHOTS, F12) is done by d3d12_overlay.cpp
std::atomic<bool> gShotPending{false};

}  // namespace

bool shotRequested() { return gShotPending.exchange(false); }

bool wantsFrame() {
  if (U.shown) return true;
  if (ML::isOpen() && ML::openPage() == ML::OverlayPage::Mods) return true;
  if (ML::autoOpenSeconds() >= 0 && !U.autoOpened) return true;
  return false;
}

bool render(const FrameCtx& f) {
  const double t = nowSec();
  if (U.firstFrameT < 0) U.firstFrameT = t;
  // developer aid: open by itself after N seconds (look checks without a menu roll)
  if (!U.autoOpened && ML::autoOpenSeconds() >= 0 && t - U.firstFrameT >= ML::autoOpenSeconds()) {
    U.autoOpened = true;
    ML::requestOpen("auto");
  }
  std::string ctx;
  if (ML::openPage() == ML::OverlayPage::Mods && ML::takeOpenRequest(&ctx)) {
    U.shown = true;
    U.closing = false;
    U.ctx = ctx;
    U.snapDirty = true;
    U.details = false;
    U.typing = false;
    U.confirm = false;
    U.focusInit = false;
    U.openA = 0;
    primeInputs();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] shown (%s, %dx%d)", ctx.c_str(), f.bbW, f.bbH);
  }
  if (!U.shown) return false;
  if (!ensureContext(f.device, f.queue)) {
    // never strand the player behind an overlay that cannot draw
    U.shown = false;
    ML::setShown(false);
    return false;
  }
  ++G.frame;
  collectGrave();
  const float dt = U.lastT > 0 ? float(std::clamp(t - U.lastT, 0.001, 0.1)) : 1.0f / 60.0f;
  U.lastT = t;
  // the design transform: the game's 16:9 image inside the back buffer
  {
    const float W = float(f.bbW), H = float(f.bbH);
    float fw, fh;
    if (W * 720.0f <= H * 1280.0f) { fw = W; fh = W * 720.0f / 1280.0f; }
    else { fh = H; fw = H * 1280.0f / 720.0f; }
    X.ox = (W - fw) * 0.5f;
    X.oy = (H - fh) * 0.5f;
    X.s = fh / 720.0f;
    X.bbW = f.bbW;
    X.bbH = f.bbH;
  }
  if (std::fabs(F.scale - X.s) > 0.001f) {
    shutdownBackend();   // the atlas texture changes: a fresh backend uploads the new one
    buildFonts(X.s);
  }
  if (!ensureBackend(f.rtvFormat)) {
    U.shown = false;
    ML::setShown(false);
    return false;
  }
  if (U.snapDirty) refresh();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(f.bbW), float(f.bbH));
  io.DeltaTime = dt;
  ImGui_ImplDX12_NewFrame();
  ImGui::NewFrame();
  DL = ImGui::GetBackgroundDrawList();
  gClip.clear();
  U.hovered = false;

  // input
  gatherInput(t);
  if (!U.closing) {
    for (Act a : gActs) dispatch(a);
  }
  // prompt set: a developer pin (SR_MOD_LIBRARY_PROMPTS, look checks), else the input layer's
  // set (the player's pin / the device used last), else the library's own last device
  static const int s_promptPin = [] {
    const char* e = std::getenv("SR_MOD_LIBRARY_PROMPTS");
    if (!e) return -1;
    const std::string v = e;
    return v == "xbox" ? int(psXbox) : v == "ps4" ? int(psPS4) : v == "ps5" ? int(psPS5)
         : v == "switch" ? int(psSwitch) : v == "keyboard" ? int(psKeyboard) : -1;
  }();
  if (s_promptPin >= 0) {
    gPromptSet = PromptSet(s_promptPin);
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

  // animation
  U.openA = approach(U.openA, U.closing ? 0.0f : 1.0f, 12.0f, dt);
  if (!U.closing && U.openA > 0.995f) U.openA = 1.0f;
  U.detailsA = approach(U.detailsA, U.details ? 1.0f : 0.0f, 14.0f, dt);
  U.tabSlide = approach(U.tabSlide, 0.0f, 14.0f, dt);
  const float a = smooth01(U.openA);
  gAlpha = a;

  // the page: the frame zooms in slightly as it opens
  const float zoom = 0.97f + 0.03f * a;
  {
    const float fw = 1280.0f * X.s, fh = 720.0f * X.s;
    const float cx = X.ox + fw * 0.5f, cy = X.oy + fh * 0.5f;
    X.ox = cx - fw * 0.5f * zoom;
    X.oy = cy - fh * 0.5f * zoom;
    X.s *= zoom;
  }
  gAlpha = a;
  drawBackdrop();
  drawFrame(t);
  drawHeader(t);
  {
    // the page, logged when it changes (rolls follow it without a frame)
    static int s_loggedTab = -1;
    if (U.tab != s_loggedTab) {
      s_loggedTab = U.tab;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] tab %s", kTabNames[U.tab]);
    }
  }
  // the body (slides in after a tab change)
  {
    const float keepOx = X.ox;
    X.ox += U.tabSlide * X.s;
    std::string left;
    if (U.tab == kLibrary) {
      if (U.details) {
        const ML::LibEntry* e = findMod(U.detId);
        left = "LIBRARY  /  " + (e ? ML::upper(e->name) : std::string());
      } else {
        const auto items = gridItems();
        int nm = 0, nb = 0;
        for (int i : items) (U.snap.mods[size_t(i)].builtin ? nb : nm)++;
        left = ML::upper(ML::categories()[size_t(U.cat)].label) + "  \xC2\xB7  " + std::to_string(nm) +
               (nm == 1 ? " MOD" : " MODS");
        if (nb) left += "  \xC2\xB7  " + std::to_string(nb) + " BUILT-IN";
        if (!U.search.empty()) left += "  \xC2\xB7  SEARCH \"" + ML::upper(U.search) + "\"";
      }
    } else if (U.tab == kLoadOrder) {
      left = "LOAD ORDER  \xC2\xB7  TOP OF THE LIST WINS";
      if (!U.snap.canSave) left += "  \xC2\xB7  CHANGES ARE NOT SAVED IN THIS RUN";
    } else {
      left = "GET MORE MODS";
    }
    X.ox = keepOx;
    drawStatusLine(t, ellipsize(F.label, kSzLabel, left, U.snap.restartRequired ? 640.0f : 1150.0f));
    X.ox += U.tabSlide * X.s;
    if (U.tab == kLibrary) {
      if (U.details || U.detailsA > 0.02f) {
        if (U.details) drawDetails(t, dt);
        else {
          // closing the details: fade the grid back in
          drawLibrary(t, dt);
        }
      } else {
        drawLibrary(t, dt);
      }
    } else if (U.tab == kLoadOrder) {
      drawLoadOrder(t, dt);
    } else {
      drawGetMore(t);
    }
    X.ox = keepOx;
  }
  buildPrompts();
  gPromptClick = aNone;
  drawPromptBar();
  drawToast(t);
  if (U.confirm) drawConfirm(t);
  if (gPromptClick != aNone && !U.closing) dispatch(Act(gPromptClick));
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
  ++U.frames;
  if (U.closing && U.openA < 0.01f) {
    U.shown = false;
    U.closing = false;
    releaseImages();
    ML::setShown(false);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] hidden (images released)");
  }
  return true;
}

}  // namespace modlibui
}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
