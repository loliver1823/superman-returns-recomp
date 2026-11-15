// iter-466 gfx (PORT FEATURE): THE PC SETTINGS SCREEN. Contract: settings_ui.hpp,
// docs/GRAPHICS_SETTINGS.md. Dear ImGui is only the drawing layer (the shared overlay kit); the
// look, the layout, focus, navigation and input are our own code below.
//
// Charter: a host settings screen. It reads no guest memory, writes none and calls no guest code;
// the only guest-visible effect is the neutral pad while it is open (the mod library's routing).
#ifdef _WIN32

#include "settings_ui.hpp"

#include "achievements_ui.hpp"   // lane E L-a: the ACHIEVEMENTS page draws through this slot
#include "overlay_kit.hpp"
#include "res_scale.hpp"
#include "../app/log.hpp"
#include "../kernel/host_automation.hpp"
#include "../kernel/host_input.hpp"
#include "../kernel/mod_library.hpp"
#include "../kernel/pc_settings.hpp"
#include "../kernel/pc_controls.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sr {
namespace gfx {
namespace settingsui {

namespace {
using namespace sr::gfx::overlay;
namespace ML = sr::kernel::modlib;
namespace HI = sr::kernel::hostinput;
namespace PS = sr::kernel::pcsettings;

// ═════════════════════════════════════════════════════════════════════════════════════════
// Layout (1280x720 design units)
// ═════════════════════════════════════════════════════════════════════════════════════════
constexpr float kFX0 = 22, kFY0 = 14, kFX1 = 1258, kFY1 = 706;
constexpr float kListX0 = 40, kListX1 = 792, kListY0 = 130, kListY1 = 606;
constexpr float kCtlX0 = 482, kCtlX1 = 776;
constexpr float kPaneX0 = 810, kPaneX1 = 1236, kPaneY0 = 130, kPaneY1 = 650;
constexpr float kRowH = 34, kHeadH = 32;
constexpr float kBtnY0 = 616, kBtnY1 = 646;
constexpr double kConfirmSecs = 15.0;

enum Act { aNone = 0, aUp, aDown, aLeft, aRight, aAccept, aBack, aX, aY, aLB, aRB, aLT, aRT, aView, aShot };
enum Modal { mNone = 0, mConfirmDisplay, mRestart, mUnsaved, mReset };

struct Row {
  bool header = false;
  std::string section;
  PS::Option opt;
};

// ═════════════════════════════════════════════════════════════════════════════════════════
// State
// ═════════════════════════════════════════════════════════════════════════════════════════
struct Ui {
  bool shown = false, closing = false;
  float openA = 0;
  double lastT = 0;
  std::string ctx;
  int tab = 0;
  float tabSlide = 0;
  std::vector<PS::Option> cat;
  double catT = -100;
  std::vector<Row> rows;      // the current tab's rows (headers + options)
  int focus = -1;             // index into rows (an option row)
  int focusPerTab[int(PS::Tab::Count)] = {-1, -1, -1, -1, -1, -1, -1};
  float scroll = 0, scrollT = 0;
  std::map<std::string, std::string> saved, pending;   // id -> value
  // the drop-down
  bool dd = false;
  std::string ddId;
  int ddFocus = 0;
  float ddScroll = 0;
  // slider drag
  std::string dragId;
  // modal
  Modal modal = mNone;
  int modalFocus = 0;
  double confirmT0 = 0;
  std::map<std::string, std::string> confirmOld;   // display values before the APPLY
  std::vector<std::string> restartIds;
  bool closeAfterModal = false;
  // peek (hold RT / P / the eye button): the overlay fades so the game shows
  bool peekPad = false, peekKey = false, peekMouse = false;
  int peekSc = 0;   // the key that holds the preview (it owns it until ITS release; Astra E3 r2)
  float peekA = 0;
  // toast
  std::string toast;
  double toastT = -100;
  // mouse (stage coords)
  float mx = -1000, my = -1000;
  bool clickL = false, clickR = false, releaseL = false, lDown = false, mouseMoved = false;
  int wheel = 0;
  bool hovered = false;
  // pad / harness keyboard edges
  uint16_t padPrev = 0;
  bool ltPrev = false, rtPrev = false;
  double padRepeatAt[4] = {0, 0, 0, 0};
  uint8_t hkPrev[512] = {0};
  double hkRepeatAt[512] = {0};
  bool shotWanted = false;
  // the performance overlay
  double fpsT0 = 0;
  int fpsFrames = 0;
  float fps = 0, frameMs = 0, worstMs = 0, worstAcc = 0;
  double lastPresentT = 0;
  // iter-466 controls2: the key binding rows (the remapper)
  int bindSlot = 0;           // the focused chip of the focused binding row (n = the "+" chip)
  bool capOn = false;         // capturing the next key / mouse button
  std::string capId;
  int capSlot = -1;
  double capT0 = 0;
} U;

std::atomic<bool> gShotPending{false};
std::vector<Act> gActs;
int gLayer = 0;   // 0 page, 1 modal / drop-down

void toastMsg(const std::string& s) {
  U.toast = s;
  U.toastT = nowSec();
}

PS::Tab curTab() { return PS::Tab(U.tab); }

const PS::Option* opt(const std::string& id) { return PS::find(U.cat, id); }

std::string pend(const std::string& id) {
  auto it = U.pending.find(id);
  return it == U.pending.end() ? PS::value(id) : it->second;
}

std::string presetNow() {
  return PS::matchPreset([](const std::string& k) { return pend(k); });
}

void refreshCatalogue() {
  U.cat = PS::catalogue();
  U.catT = nowSec();
}

void buildRows() {
  U.rows.clear();
  std::string sec;
  for (const PS::Option& o : U.cat) {
    if (o.tab != curTab()) continue;
    if (o.section != sec) {
      sec = o.section;
      Row h;
      h.header = true;
      h.section = sec;
      U.rows.push_back(h);
    }
    Row r;
    r.opt = o;
    U.rows.push_back(r);
  }
  if (U.focus < 0 || U.focus >= int(U.rows.size()) || U.rows[size_t(U.focus)].header) {
    U.focus = -1;
    for (int i = 0; i < int(U.rows.size()); ++i)
      if (!U.rows[size_t(i)].header) {
        U.focus = i;
        break;
      }
  }
}

void loadValues() {
  U.saved.clear();
  U.pending.clear();
  for (const PS::Option& o : U.cat) {
    if (o.kind == PS::Kind::Action) continue;
    const std::string v = o.id == "preset" ? std::string() : PS::storedValue(o.id);
    U.saved[o.id] = v;
    U.pending[o.id] = v;
  }
}

bool changed(const std::string& id) {
  auto a = U.saved.find(id), b = U.pending.find(id);
  return a != U.saved.end() && b != U.pending.end() && a->second != b->second;
}
int changedCount() {
  int n = 0;
  for (const auto& kv : U.pending)
    if (kv.first != "preset" && changed(kv.first)) n++;
  return n;
}
bool restartPending(const std::string& id) {
  const PS::Option* o = opt(id);
  return o && o->apply == PS::Apply::Restart && pend(id) != PS::bootValue(id);
}

void setPending(const std::string& id, const std::string& v) {
  const PS::Option* o = opt(id);
  if (!o || !o->available) return;
  if (id == "preset") {
    for (const auto& kv : PS::presetValues(v)) {
      const PS::Option* m = opt(kv.first);
      if (m) setPending(kv.first, kv.second);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] preset %s", v.c_str());
    return;
  }
  if (pend(id) == v) return;
  U.pending[id] = v;
  std::string err;
  PS::preview(id, v, &err);   // live options apply now; the rest wait for APPLY
  // rolls attribute their [PM4_GPU_TIME] / [PRESENT_CHAIN_TIME] windows to these lines
  if (o->apply == PS::Apply::Live && U.dragId.empty())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] preview %s = %s", id.c_str(), v.c_str());
  // a display-mode change rebuilds the RESOLUTION / REFRESH lists
  if (id == "display_mode" || id == "display_index" || id == "resolution") {
    refreshCatalogue();
    buildRows();
  }
}

// ── values shown ──────────────────────────────────────────────────────────────────────────
std::string choiceLabel(const PS::Option& o, const std::string& v) {
  for (const PS::Choice& c : o.choices)
    if (c.value == v) return c.label;
  if (o.id == "resolution") {
    unsigned w = 0, h = 0;
    if (std::sscanf(v.c_str(), "%ux%u", &w, &h) == 2) {
      char b[48];
      std::snprintf(b, sizeof b, "%u x %u", w, h);
      return b;
    }
  }
  if (o.id == "target_fps" && !v.empty()) return v;
  return upperAscii(v);
}
std::string sliderText(const PS::Option& o, float f) {
  char b[32];
  if (o.decimals > 0) std::snprintf(b, sizeof b, "%.*f", o.decimals, double(f));
  else std::snprintf(b, sizeof b, "%d", int(std::lround(f)));
  return std::string(b) + o.unit;
}
std::string sliderValue(const PS::Option& o, float f) {
  char b[32];
  if (o.decimals > 0) std::snprintf(b, sizeof b, "%.*f", o.decimals, double(f));
  else std::snprintf(b, sizeof b, "%d", int(std::lround(f)));
  return b;
}
float sliderSnap(const PS::Option& o, float f) {
  f = clampf(f, o.lo, o.hi);
  if (o.step > 0) f = o.lo + std::round((f - o.lo) / o.step) * o.step;
  return clampf(f, o.lo, o.hi);
}
std::string displayedValue(const PS::Option& o) {
  const std::string v = o.id == "preset" ? presetNow() : pend(o.id);
  if (o.kind == PS::Kind::Toggle) return v == "on" ? "ON" : "OFF";
  if (o.kind == PS::Kind::Slider) return sliderText(o, float(std::atof(v.c_str())));
  if (o.kind == PS::Kind::Action) return o.choices.empty() ? "RUN" : o.choices[0].label;
  if (o.kind == PS::Kind::Binding) return upperAscii(v);
  return choiceLabel(o, v);
}

// ── changing a row ────────────────────────────────────────────────────────────────────────
void stepChoice(const PS::Option& o, int d) {
  if (o.choices.empty()) return;
  const std::string cur = o.id == "preset" ? presetNow() : pend(o.id);
  int i = -1;
  for (int k = 0; k < int(o.choices.size()); ++k)
    if (o.choices[size_t(k)].value == cur) i = k;
  if (i < 0) i = d > 0 ? -1 : int(o.choices.size());
  const int n = std::clamp(i + d, 0, int(o.choices.size()) - 1);
  if (n != i) setPending(o.id, o.choices[size_t(n)].value);
}
void stepSlider(const PS::Option& o, int d) {
  const float f = float(std::atof(pend(o.id).c_str()));
  setPending(o.id, sliderValue(o, sliderSnap(o, f + float(d) * o.step)));
}
void toggle(const PS::Option& o) { setPending(o.id, pend(o.id) == "on" ? "off" : "on"); }

// ── iter-466 controls2: key binding rows ──────────────────────────────────────────────────
namespace PC = sr::kernel::pccontrols;
std::vector<std::string> bindList(const std::string& id) {
  std::vector<std::string> v;
  const std::string t = pend(id);
  size_t a = 0;
  while (a <= t.size()) {
    size_t b = t.find(',', a);
    if (b == std::string::npos) b = t.size();
    std::string x = t.substr(a, b - a);
    while (!x.empty() && x.front() == ' ') x.erase(x.begin());
    while (!x.empty() && x.back() == ' ') x.pop_back();
    if (!x.empty() && x != "none") v.push_back(x);
    a = b + 1;
  }
  return v;
}
std::string joinList(const std::vector<std::string>& v) {
  std::string t;
  for (const std::string& x : v) t += (t.empty() ? "" : ", ") + x;
  return t.empty() ? "none" : t;
}
// The inputs of this row that another action in the same context uses too (pending values).
std::vector<std::pair<std::string, std::string>> bindConflicts(const std::string& id) {
  std::vector<std::pair<std::string, std::string>> out;   // (input, the other action's label)
  const int me = PS::bindingAction(id);
  if (me < 0) return out;
  PC::Bindings b;
  for (const PS::Option& o : U.cat) {
    const int a = PS::bindingAction(o.id);
    if (a < 0) continue;
    uint16_t ins[PC::kInputsPerAction];
    if (PC::parseBindingText(pend(o.id), ins))
      for (int k = 0; k < PC::kInputsPerAction; ++k) b.in[a][k] = ins[k];
  }
  for (const PC::Conflict& c : PC::findConflicts(b)) {
    if (c.a == me) out.push_back({PC::inputName(c.input), PC::actionLabel(c.b)});
    else if (c.b == me) out.push_back({PC::inputName(c.input), PC::actionLabel(c.a)});
  }
  return out;
}
void startCapture(const PS::Option& o, int slot) {
  U.capOn = true;
  U.capId = o.id;
  U.capSlot = slot;
  U.capT0 = nowSec();
}
void endCapture(const std::string& name) {
  if (!U.capOn) return;
  U.capOn = false;
  if (name.empty()) return;
  std::vector<std::string> v = bindList(U.capId);
  for (size_t k = 0; k < v.size();) {   // one input once per action
    if (v[k] == name) {
      v.erase(v.begin() + long(k));
      if (int(k) < U.capSlot) U.capSlot--;
    } else {
      ++k;
    }
  }
  if (U.capSlot >= 0 && U.capSlot < int(v.size())) v[size_t(U.capSlot)] = name;
  else if (int(v.size()) < PC::kInputsPerAction) v.push_back(name);
  setPending(U.capId, joinList(v));
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] bind %s slot %d = %s -> %s", U.capId.c_str(), U.capSlot,
              name.c_str(), pend(U.capId).c_str());
}
void clearBinding(const PS::Option& o, int slot) {
  std::vector<std::string> v = bindList(o.id);
  if (slot < 0 || slot >= int(v.size())) return;
  v.erase(v.begin() + slot);
  setPending(o.id, joinList(v));
  U.bindSlot = std::min(U.bindSlot, int(v.size()));
}

void beginClose();

void runAction(const PS::Option& o) {
  // lane E L-b: VIEW PAD LAYOUT (the game's own diagram screen) and DEV TOOLS (a development build's pages) leave the
  // screen: only with nothing unapplied
  const bool leaves = o.id == "view_pad_layout" || o.id == "dev_tools";
  if (leaves && changedCount() > 0) {
    toastMsg("APPLY OR REVERT YOUR CHANGES FIRST");
    return;
  }
  std::string msg;
  const bool ok = PS::runAction(o.id, &msg);
  toastMsg(upperAscii(msg));
  if (ok && leaves) beginClose();
  // a CHEATS row changes the game's state on its next tick: the rows re-read it (labels show ON / OFF)
  if (ok && o.tab == PS::Tab::Cheats) U.catT = -100;
}

void openDropdown(const PS::Option& o) {
  if (o.choices.empty()) return;
  U.dd = true;
  U.ddId = o.id;
  const std::string cur = o.id == "preset" ? presetNow() : pend(o.id);
  U.ddFocus = 0;
  for (int k = 0; k < int(o.choices.size()); ++k)
    if (o.choices[size_t(k)].value == cur) U.ddFocus = k;
  U.ddScroll = -1;   // centre on the focus at the first draw
}

// ── APPLY / REVERT / RESET ────────────────────────────────────────────────────────────────
void revertAll() {
  for (auto& kv : U.pending) kv.second = U.saved[kv.first];
  PS::revertPreviews();
  refreshCatalogue();
  buildRows();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] revert");
}

void applyAll() {
  std::vector<std::string> ids;
  for (const auto& kv : U.pending)
    if (kv.first != "preset" && changed(kv.first)) ids.push_back(kv.first);
  if (ids.empty()) {
    toastMsg("NO CHANGES TO APPLY");
    return;
  }
  // display rows first (one live window request), in screen order
  std::map<std::string, std::string> old;
  std::vector<std::string> restart, failed;
  for (const PS::Option& o : U.cat) {
    if (std::find(ids.begin(), ids.end(), o.id) == ids.end()) continue;
    if (o.apply == PS::Apply::Confirm) old[o.id] = U.saved[o.id];
    std::string err;
    if (!PS::commit(o.id, U.pending[o.id], &err)) {
      failed.push_back(o.label);
      continue;
    }
    U.saved[o.id] = U.pending[o.id];
    if (o.apply == PS::Apply::Restart && restartPending(o.id)) restart.push_back(o.id);
  }
  refreshCatalogue();
  buildRows();
  U.restartIds = restart;
  if (!failed.empty()) toastMsg("NOT SAVED: " + upperAscii(failed.front()));
  else if (old.empty()) toastMsg("SETTINGS APPLIED");
  if (!old.empty()) {
    U.confirmOld = old;
    U.confirmT0 = nowSec();
    U.modal = mConfirmDisplay;
    U.modalFocus = 0;
  } else if (!restart.empty()) {
    U.modal = mRestart;
    U.modalFocus = 0;
  }
}

void revertDisplay(const char* why) {
  for (const auto& kv : U.confirmOld) {
    std::string err;
    PS::commit(kv.first, kv.second, &err);
    U.saved[kv.first] = kv.second;
    U.pending[kv.first] = kv.second;
  }
  U.confirmOld.clear();
  refreshCatalogue();
  buildRows();
  toastMsg("DISPLAY SETTINGS REVERTED");
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] display settings reverted (%s)", why);
}

void resetTab() {
  for (const PS::Option& o : U.cat)
    if (o.tab == curTab() && o.available && o.kind != PS::Kind::Action && o.id != "preset" &&
        !o.defaultValue.empty())
      setPending(o.id, o.defaultValue);
  toastMsg(std::string(PS::tabName(curTab())) + " RESET TO DEFAULTS - APPLY TO KEEP");
}

void beginClose() {
  if (U.closing) return;
  U.closing = true;
  U.dd = false;
  U.modal = mNone;
  U.dragId.clear();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] close");
}

void askClose() {
  if (changedCount() > 0) {
    U.modal = mUnsaved;
    U.modalFocus = 0;
    return;
  }
  beginClose();
}

// A tab is shown when the catalogue has a row on it (a lane's tab waits for its rows). lane E L-b: CHEATS only in
// game (at the main menu nothing is running).
bool tabVisible(int i) {
  if (PS::Tab(i) == PS::Tab::Cheats && U.ctx != "IG") return false;
  for (const PS::Option& o : U.cat)
    if (int(o.tab) == i) return true;
  return false;
}

// d = +-1: the next / previous shown tab; otherwise jump to tab U.tab + d.
void switchTab(int d) {
  const int n = int(PS::Tab::Count);
  int t = (U.tab + d + n) % n;
  if (d == 1 || d == -1)
    for (int k = 0; k < n && !tabVisible(t); ++k) t = (t + d + n) % n;
  if (t == U.tab) return;
  U.focusPerTab[U.tab] = U.focus;
  U.tab = t;
  U.tabSlide = d > 0 ? 40.0f : -40.0f;
  U.focus = U.focusPerTab[U.tab];
  U.scroll = U.scrollT = 0;
  U.dd = false;
  buildRows();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] tab %s", PS::tabName(curTab()));
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Hit testing (immediate mode)
// ═════════════════════════════════════════════════════════════════════════════════════════
bool inside(float x0, float y0, float x1, float y1) {
  if ((U.modal != mNone || U.dd) && gLayer == 0) return false;
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
  h.hover = inside(x0, y0, x1, y1);
  if (h.hover) U.hovered = true;
  h.click = h.hover && U.clickL;
  if (h.click) U.clickL = false;
  return h;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Widgets
// ═════════════════════════════════════════════════════════════════════════════════════════
// A PC button in the game's style: chamfered glass, gold rim when focused or hovered.
bool button(float x0, float y0, float x1, float y1, const std::string& label, bool focus, bool primary,
            bool enabled, double t, Ctl prompt = cCount, Ctl prompt2 = cCount) {
  Hit h = enabled ? hit(x0, y0, x1, y1) : Hit{};
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 7, 0, 7, 0, p);
  const float a = enabled ? 1.0f : 0.4f;
  if (primary)
    gradPoly(p, n, col(0.80f, 0.12f, 0.14f, 0.95f * a), col(0.46f, 0.03f, 0.07f, 0.95f * a));
  else
    gradPoly(p, n, col(0.12f, 0.30f, 0.66f, 0.92f * a), col(0.04f, 0.12f, 0.34f, 0.94f * a));
  if (h.hover) gradPoly(p, n, col(1, 1, 1, 0.10f), col(1, 1, 1, 0.02f));
  DL->AddPolyline(p, n, (focus || h.hover) && enabled ? C(kGold) : col(0.55f, 0.74f, 1.0f, 0.55f * a),
                  ImDrawFlags_Closed, std::max(1.0f, X.d(focus ? 1.8f : 1.1f)));
  if (focus && enabled) focusFrame(x0, y0, x1, y1, 7, 0.7f, t);
  // the glyphs and the label fit inside the button (Astra E3 r2: two rebound caps in a 150 px modal button): the
  // glyphs shrink when they alone would leave under 40 px for the label, which ellipsizes to what is left
  const float room = std::max(0.0f, x1 - x0 - 16);
  auto glyphsW = [&](float gh) {
    float w = 0;
    if (prompt != cCount) w = glyphW(prompt, gh) + 7;
    if (prompt2 != cCount) w += glyphW(prompt2, gh) + 4;
    return w;
  };
  float gh = 17;
  float gw = glyphsW(gh);
  if (gw > room - 40 && gw > 0) {
    gh = std::max(9.0f, gh * std::max(0.0f, room - 40) / gw);
    gw = glyphsW(gh);
  }
  // still too wide at the smallest size (Astra E3 r3): the second glyph goes, then the first; nothing is drawn
  // outside the button
  if (gw > room && prompt2 != cCount) {
    prompt2 = cCount;
    gw = glyphsW(gh);
  }
  if (gw > room) {
    prompt = cCount;
    gw = 0;
  }
  const float textRoom = room - gw;
  const std::string shown = textRoom >= textW(F.card, 13.5f, "...") ? ellipsize(F.card, 13.5f, label, textRoom)
                                                                     : std::string();
  const float tw = textW(F.card, 13.5f, shown);
  const float tx = (x0 + x1) * 0.5f - (tw + gw) * 0.5f;
  if (prompt != cCount) {
    const float w1 = glyphAt(prompt, tx, (y0 + y1) * 0.5f, gh);
    if (prompt2 != cCount) glyphAt(prompt2, tx + w1 + 4, (y0 + y1) * 0.5f, gh);
  }
  text(F.card, 13.5f, tx + gw, (y0 + y1) * 0.5f - 8.5f, shown, C(kWhite, a), C(kSilver, a), 1.0f);
  return h.click;
}

// The performance bar: 0..3 filled segments.
void perfBars(float x, float y, const std::string& perf) {
  const int lvl = perf == "HIGH" ? 3 : perf == "MEDIUM" ? 2 : perf == "LOW" ? 1 : 0;
  for (int i = 0; i < 3; ++i) {
    const float x0 = x + float(i) * 13.0f;
    const bool on = i < lvl;
    const Rgb c = lvl == 3 ? kRed : lvl == 2 ? kGold : kOk;
    DL->AddRectFilled(X.p(x0, y), X.p(x0 + 10, y + 10), on ? C(c) : col(0.2f, 0.3f, 0.5f, 0.6f), X.d(1.5f));
  }
}

// A drop-down box (closed): the value and a chevron.
void choiceBox(const PS::Option& o, float y0, float y1, bool focus, bool enabled, double t, bool* openIt) {
  const float x0 = kCtlX0, x1 = kCtlX1;
  Hit h = enabled ? hit(x0, y0, x1, y1) : Hit{};
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 5, 0, 5, 0, p);
  const float a = enabled ? 1.0f : 0.45f;
  gradPoly(p, n, col(0.03f, 0.10f, 0.30f, 0.92f * a), col(0.01f, 0.05f, 0.18f, 0.95f * a));
  DL->AddPolyline(p, n, (focus || h.hover) && enabled ? C(kGold) : col(0.50f, 0.70f, 1.0f, 0.55f * a),
                  ImDrawFlags_Closed, std::max(1.0f, X.d(focus ? 1.6f : 1.0f)));
  // left / right arrows (pad and keyboard change the value in place)
  if (focus && enabled) {
    const float cy = (y0 + y1) * 0.5f;
    DL->AddTriangleFilled(X.p(x0 - 13, cy), X.p(x0 - 6, cy - 5), X.p(x0 - 6, cy + 5), C(kGold, 0.9f));
    DL->AddTriangleFilled(X.p(x1 + 13, cy), X.p(x1 + 6, cy - 5), X.p(x1 + 6, cy + 5), C(kGold, 0.9f));
  }
  const std::string v = ellipsize(F.label, kSzLabel, displayedValue(o), x1 - x0 - 40);
  text(F.label, kSzLabel, x0 + 10, (y0 + y1) * 0.5f - 8, v, C(kWhite, a), C(kSilver, a), 1.0f);
  // the chevron
  const float cx = x1 - 15, cy = (y0 + y1) * 0.5f;
  DL->AddTriangleFilled(X.p(cx - 5, cy - 2.5f), X.p(cx + 5, cy - 2.5f), X.p(cx, cy + 3.5f),
                        h.hover ? C(kGold) : C(kSilver, a));
  DL->AddLine(X.p(x1 - 30, y0 + 5), X.p(x1 - 30, y1 - 5), col(0.5f, 0.7f, 1.0f, 0.35f * a), std::max(1.0f, X.d(1)));
  if (h.click && openIt) *openIt = true;
  (void)t;
}

// A slider: track, filled part (red -> gold), the thumb, and its value in a box.
void slider(const PS::Option& o, float y0, float y1, bool focus, bool enabled) {
  const float tx0 = kCtlX0 + 6, tx1 = kCtlX1 - 74, cy = (y0 + y1) * 0.5f;
  const float a = enabled ? 1.0f : 0.45f;
  const float f = float(std::atof(pend(o.id).c_str()));
  const float k = o.hi > o.lo ? clampf((f - o.lo) / (o.hi - o.lo), 0, 1) : 0;
  Hit h = enabled ? hit(tx0 - 8, y0, tx1 + 8, y1) : Hit{};
  if (h.click) U.dragId = o.id;
  if (U.dragId == o.id) {
    if (!U.lDown) U.dragId.clear();
    else {
      const float nk = clampf((U.mx - tx0) / (tx1 - tx0), 0, 1);
      setPending(o.id, sliderValue(o, sliderSnap(o, o.lo + nk * (o.hi - o.lo))));
    }
  }
  // track
  DL->AddRectFilled(X.p(tx0, cy - 3), X.p(tx1, cy + 3), col(0.02f, 0.06f, 0.20f, 0.95f * a), X.d(3));
  DL->AddRect(X.p(tx0, cy - 3), X.p(tx1, cy + 3), col(0.5f, 0.7f, 1.0f, 0.5f * a), X.d(3), 0, std::max(1.0f, X.d(0.8f)));
  // fill from the default (a centred slider, e.g. brightness) or from the left
  const float dflt = float(std::atof(o.defaultValue.c_str()));
  const float kd = (o.lo < 0 && o.hi > 0) || (o.id == "gamma") ? clampf((dflt - o.lo) / (o.hi - o.lo), 0, 1) : 0.0f;
  const float fa = tx0 + (tx1 - tx0) * std::min(k, kd), fb = tx0 + (tx1 - tx0) * std::max(k, kd);
  if (fb > fa)
    DL->AddRectFilledMultiColor(X.p(fa, cy - 2.2f), X.p(fb, cy + 2.2f), C(kRed, a), C(kGold, a), C(kGold, a), C(kRed, a));
  if (kd > 0) DL->AddLine(X.p(tx0 + (tx1 - tx0) * kd, cy - 6), X.p(tx0 + (tx1 - tx0) * kd, cy + 6),
                          col(0.8f, 0.9f, 1.0f, 0.5f * a), std::max(1.0f, X.d(1)));
  // ticks
  if (o.step > 0 && (o.hi - o.lo) / o.step <= 24)
    for (float v = o.lo; v <= o.hi + 1e-4f; v += o.step) {
      const float x = tx0 + (tx1 - tx0) * (v - o.lo) / (o.hi - o.lo);
      DL->AddLine(X.p(x, cy + 5), X.p(x, cy + 8), col(0.6f, 0.75f, 1.0f, 0.35f * a), std::max(1.0f, X.d(0.8f)));
    }
  // thumb: a chamfered crystal
  const float x = tx0 + (tx1 - tx0) * k;
  ImVec2 p[8];
  const int n = chamfer(x - 6, cy - 9, x + 6, cy + 9, 3, 3, 3, 3, p);
  const bool hot = focus || h.hover || U.dragId == o.id;
  gradPoly(p, n, hot ? col(1.0f, 0.88f, 0.45f, a) : col(0.86f, 0.92f, 1.0f, a),
           hot ? col(0.82f, 0.55f, 0.12f, a) : col(0.50f, 0.62f, 0.85f, a));
  DL->AddPolyline(p, n, col(0.05f, 0.08f, 0.2f, 0.8f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(0.9f)));
  // value box
  const float bx0 = kCtlX1 - 62, bx1 = kCtlX1;
  ImVec2 b[8];
  const int bn = chamfer(bx0, y0 + 2, bx1, y1 - 2, 4, 0, 4, 0, b);
  gradPoly(b, bn, col(0.03f, 0.10f, 0.30f, 0.92f * a), col(0.01f, 0.05f, 0.18f, 0.95f * a));
  DL->AddPolyline(b, bn, focus && enabled ? C(kGold) : col(0.5f, 0.7f, 1.0f, 0.5f * a), ImDrawFlags_Closed,
                  std::max(1.0f, X.d(1)));
  const std::string s = sliderText(o, f);
  text(F.label, kSzLabel, (bx0 + bx1) * 0.5f - textW(F.label, kSzLabel, s) * 0.5f, (y0 + y1) * 0.5f - 8, s,
       C(kWhite, a), C(kSilver, a), 1.0f);
  if (focus && enabled) {
    DL->AddTriangleFilled(X.p(kCtlX0 - 13, cy), X.p(kCtlX0 - 6, cy - 5), X.p(kCtlX0 - 6, cy + 5), C(kGold, 0.9f));
  }
}

// The binding row: one chip per bound input (red when another action in the same context uses it),
// then "+" while fewer than four. Click a chip to rebind it (or "+" to add): the next key / mouse
// button is captured.
void bindingChips(const PS::Option& o, int row, float y0, float y1, bool focus, double t) {
  const std::vector<std::string> v = bindList(o.id);
  std::vector<std::string> bad;
  for (const auto& c : bindConflicts(o.id)) bad.push_back(c.first);
  float x = kCtlX0;
  const int n = int(v.size());
  const int chips = n < PC::kInputsPerAction ? n + 1 : n;
  for (int k = 0; k < chips; ++k) {
    const bool plus = k == n;
    const bool cap = U.capOn && U.capId == o.id && (U.capSlot == k || (plus && U.capSlot >= n));
    std::string lab = plus ? "+" : upperAscii(v[size_t(k)]);
    if (cap) lab = (std::fmod(t, 0.8) < 0.4) ? "PRESS..." : "";
    const float w = std::max(plus ? 26.0f : 40.0f, textW(F.label, 12.5f, cap ? "PRESS..." : lab) + 16);
    if (x + w > kListX1 - 6) break;
    const bool isBad = !plus && std::find(bad.begin(), bad.end(), v[size_t(k)]) != bad.end();
    Hit h = hit(x, y0 + 1, x + w, y1 - 1);
    ImVec2 p[8];
    const int pn = chamfer(x, y0 + 1, x + w, y1 - 1, 4, 0, 4, 0, p);
    if (isBad) gradPoly(p, pn, col(0.62f, 0.07f, 0.09f, 0.95f), col(0.36f, 0.03f, 0.05f, 0.95f));
    else gradPoly(p, pn, col(0.03f, 0.10f, 0.30f, 0.92f), col(0.01f, 0.05f, 0.18f, 0.95f));
    const bool hot = (focus && U.bindSlot == k) || h.hover || cap;
    DL->AddPolyline(p, pn, hot ? C(kGold) : col(0.50f, 0.70f, 1.0f, 0.55f), ImDrawFlags_Closed,
                    std::max(1.0f, X.d(hot ? 1.6f : 1.0f)));
    text(F.label, 12.5f, x + (w - textW(F.label, 12.5f, lab)) * 0.5f, (y0 + y1) * 0.5f - 7.5f, lab,
         C(kWhite), C(kSilver), 1.0f);
    if (h.click && !U.capOn) {
      U.focus = row;
      U.bindSlot = k;
      startCapture(o, k);
    }
    x += w + 6;
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Page
// ═════════════════════════════════════════════════════════════════════════════════════════
void drawBackdrop() {
  const ImVec2 a(0, 0), b(float(X.bbW), float(X.bbH));
  DL->AddRectFilledMultiColor(a, b, col(0.0f, 0.03f, 0.12f, 0.62f), col(0.0f, 0.03f, 0.12f, 0.62f),
                              col(0.0f, 0.01f, 0.05f, 0.80f), col(0.0f, 0.01f, 0.05f, 0.80f));
}

void drawFrame() {
  ImVec2 pts[8];
  const int n = chamfer(kFX0, kFY0, kFX1, kFY1, 26, 10, 26, 10, pts);
  gradPoly(pts, n, col(0.05f, 0.16f, 0.44f, 0.955f), col(0.01f, 0.05f, 0.18f, 0.97f));
  for (int i = 0; i < 9; ++i) {
    const float x = kFX0 + 60 + float(i) * 140.0f + 20.0f * std::sin(float(i) * 1.7f);
    const float w = 18 + 10 * std::sin(float(i) * 2.3f);
    DL->AddRectFilledMultiColor(X.p(x, kFY0 + 70), X.p(x + w, kFY1 - 60), col(0.5f, 0.7f, 1.0f, 0.0f),
                                col(0.5f, 0.7f, 1.0f, 0.03f), col(0.5f, 0.7f, 1.0f, 0.0f), col(0.5f, 0.7f, 1.0f, 0.0f));
  }
  DL->AddPolyline(pts, n, col(0.48f, 0.70f, 1.0f, 0.55f), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  chrome(kFX0 + 160, kFY0 - 6, kFX1 - 160, kFY0 + 8, true);
  chrome(kFX0 + 160, kFY1 - 8, kFX1 - 160, kFY1 + 6, false);
  const float y = 92;
  const ImU32 e0 = col(0.6f, 0.8f, 1.0f, 0.0f), e1 = col(0.6f, 0.8f, 1.0f, 0.55f);
  DL->AddRectFilledMultiColor(X.p(kFX0 + 20, y), X.p(640, y + 1.2f), e0, e1, e1, e0);
  DL->AddRectFilledMultiColor(X.p(640, y), X.p(kFX1 - 20, y + 1.2f), e1, e0, e0, e1);
}

void drawHeader(double t) {
  shield(62, 52, 24);
  textTitle(F.display, kSzDisplay, 96, 26, "OPTIONS");   // lane E L-b: one OPTIONS screen (owner q14d)
  float x = 380;
  const float ty = 44;
  glyph(cLB, x - 14 - glyphW(cLB, 18) * 0.5f, ty + 11, 18);
  for (int i = 0; i < int(PS::Tab::Count); ++i) {
    if (!tabVisible(i)) continue;
    const std::string name = PS::tabName(PS::Tab(i));
    const float w = textW(F.tab, kSzTab, name);
    const bool act = U.tab == i;
    Hit h = hit(x - 10, ty - 8, x + w + 10, ty + 30);
    if (h.click && U.tab != i) switchTab(i - U.tab);
    if (act) {
      DL->AddRectFilledMultiColor(X.p(x - 6, ty + 25), X.p(x + w + 6, ty + 29), C(kRed), C(kRed), C(kRedDeep), C(kRedDeep));
      DL->AddRectFilled(X.p(x - 6, ty + 24), X.p(x + w + 6, ty + 25.2f), C(kGold));
      DL->AddRectFilledMultiColor(X.p(x - 10, ty - 6), X.p(x + w + 10, ty + 24), col(0.6f, 0.8f, 1.0f, 0.0f),
                                  col(0.6f, 0.8f, 1.0f, 0.0f), col(0.6f, 0.8f, 1.0f, 0.16f), col(0.6f, 0.8f, 1.0f, 0.16f));
      textTitle(F.tab, kSzTab, x, ty, name);
    } else {
      const float a = h.hover ? 0.95f : 0.62f;
      text(F.tab, kSzTab, x, ty, name, col(0.80f, 0.86f, 0.96f, a), col(0.60f, 0.68f, 0.82f, a), 1.0f);
    }
    x += w + 34;
  }
  glyphAt(cRB, x - 22, ty + 11, 18);
  // right: where the screen was opened from
  const std::string where = U.ctx == "IG" ? "PAUSED" : "MAIN MENU";
  text(F.label, kSzLabel, kFX1 - 34 - textW(F.label, kSzLabel, where), 46, where, C(kSilver), C(kSilverDim), 1.0f);
  (void)t;
}

void drawStatusLine(double t) {
  std::string left;
  const auto& d = sr::gfx::resscale::liveDisplay();
  switch (curTab()) {
    case PS::Tab::Display: {
      char b[160];
      const auto& s = sr::gfx::resscale::get();
      std::snprintf(b, sizeof b, "NOW  \xC2\xB7  %s  \xC2\xB7  OUTPUT %d x %d  \xC2\xB7  RENDERING %u x %u",
                    d.mode == sr::gfx::resscale::DisplayMode::Windowed ? "WINDOWED"
                    : d.mode == sr::gfx::resscale::DisplayMode::Borderless ? "BORDERLESS" : "EXCLUSIVE",
                    X.bbW, X.bbH, 1280u * s.x, 720u * s.y);
      left = b;
      break;
    }
    case PS::Tab::Graphics: left = "GRAPHICS  \xC2\xB7  PRESET " + presetNow(); break;
    case PS::Tab::World: left = "WORLD  \xC2\xB7  METROPOLIS"; break;
    case PS::Tab::Audio: left = "AUDIO  \xC2\xB7  THE OUTPUT. THE GAME'S MUSIC AND EFFECTS VOLUMES ARE ON THE GAME TAB"; break;
    default: left = "CONTROLS  \xC2\xB7  MOUSE AND BUTTON PROMPTS"; break;
  }
  text(F.label, kSzLabel, 44, 100, ellipsize(F.label, kSzLabel, left, 700), C(kSilver), C(kSilverDim), 1.0f);
  // right: unsaved changes / restart banner
  const int nc = changedCount();
  int nr = 0;
  for (const PS::Option& o : U.cat)
    if (o.apply == PS::Apply::Restart && o.kind != PS::Kind::Action && restartPending(o.id)) nr++;
  if (!nc && !nr) return;
  char msg[96];
  if (nc) std::snprintf(msg, sizeof msg, "%d UNSAVED CHANGE%s", nc, nc == 1 ? "" : "S");
  else std::snprintf(msg, sizeof msg, "RESTART TO APPLY %d SETTING%s", nr, nr == 1 ? "" : "S");
  const float w = textW(F.badge, 11.5f, msg) + 28, x1 = kFX1 - 34, x0 = x1 - w, y0 = 96, y1 = 118;
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x1, y1, 7, 0, 7, 0, p);
  const float pulse = 0.75f + 0.25f * float(std::sin(t * 2.6));
  if (nc) gradPoly(p, n, col(0.62f, 0.44f, 0.08f, 0.92f), col(0.36f, 0.22f, 0.02f, 0.95f));
  else gradPoly(p, n, col(0.70f, 0.08f, 0.10f, 0.92f), col(0.38f, 0.02f, 0.05f, 0.95f));
  DL->AddPolyline(p, n, col(kGold.r, kGold.g, kGold.b, pulse), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  textPlain(F.badge, 11.5f, x0 + 14, y0 + 5.5f, msg, col(1.0f, 0.93f, 0.75f));
}

float rowY(int i) {
  float y = kListY0 + 4 - U.scroll;
  for (int k = 0; k < i; ++k) y += U.rows[size_t(k)].header ? kHeadH : kRowH;
  return y;
}
float contentH() {
  float h = 8;
  for (const Row& r : U.rows) h += r.header ? kHeadH : kRowH;
  return h;
}

void drawList(double t) {
  // the panel
  glass(kListX0 - 8, kListY0 - 6, kListX1 + 8, kListY1 + 4, 14, 0.75f);
  pushClip(kListX0 - 6, kListY0 - 4, kListX1 + 6, kListY1 + 2);
  const float viewH = kListY1 - kListY0, maxS = std::max(0.0f, contentH() - viewH);
  U.scrollT = clampf(U.scrollT, 0, maxS);
  for (int i = 0; i < int(U.rows.size()); ++i) {
    const Row& r = U.rows[size_t(i)];
    const float y0 = rowY(i), h = r.header ? kHeadH : kRowH, y1 = y0 + h;
    if (y1 < kListY0 - 40 || y0 > kListY1 + 40) continue;
    if (r.header) {
      text(F.tab, 13.5f, kListX0 + 14, y0 + 9, r.section, C(kGold), C(kGoldDeep), 1.0f);
      const float lx = kListX0 + 22 + textW(F.tab, 13.5f, r.section);
      DL->AddRectFilledMultiColor(X.p(lx, y0 + 17), X.p(kListX1 - 10, y0 + 18), col(0.9f, 0.75f, 0.3f, 0.55f),
                                  col(0.9f, 0.75f, 0.3f, 0.0f), col(0.9f, 0.75f, 0.3f, 0.0f), col(0.9f, 0.75f, 0.3f, 0.55f));
      continue;
    }
    const PS::Option& o = r.opt;
    const bool focus = i == U.focus;
    const bool en = o.available;
    const bool rowHover = inside(kListX0, y0 + 1, kListX1, y1 - 1);
    if (rowHover) U.hovered = true;
    if (focus) selBand(kListX0, y0 + 2, kListX1, y1 - 2, 1.0f);
    else if (rowHover) selBand(kListX0, y0 + 2, kListX1, y1 - 2, 0.35f);
    const float a = en ? 1.0f : 0.62f;
    // the changed marker
    if (changed(o.id)) DL->AddCircleFilled(X.p(kListX0 + 9, (y0 + y1) * 0.5f), X.d(3.2f), C(kGold), 10);
    std::string lab = o.label;
    const float lx = kListX0 + 22;
    text(F.card, kSzCard, lx, y0 + 9, lab, focus ? C(kWhite, a) : col(0.86f, 0.90f, 0.98f, a),
         col(0.62f, 0.70f, 0.84f, a), 1.0f);
    float bx = lx + textW(F.card, kSzCard, lab) + 10;
    if (o.apply == PS::Apply::Restart && o.kind != PS::Kind::Action && en) {
      const bool pendR = restartPending(o.id);
      bx += badge(bx, y0 + 9, "RESTART", pendR ? col(0.62f, 0.06f, 0.08f, 0.95f) : col(0.06f, 0.12f, 0.30f, 0.9f),
                  pendR ? col(1, 0.92f, 0.75f) : C(kSilver), pendR ? C(kGold) : col(0.5f, 0.7f, 1.0f, 0.6f)) + 6;
    }
    // the control
    const float cy0 = y0 + 4, cy1 = y1 - 4;
    if (!en) {
      badge(kCtlX0, y0 + 9, o.unavailableBadge.empty() ? "NOT AVAILABLE" : o.unavailableBadge,
            col(0.08f, 0.12f, 0.24f, 0.9f), C(kSilver), col(0.4f, 0.5f, 0.7f, 0.6f));
    } else if (o.kind == PS::Kind::Choice) {
      bool openIt = false;
      choiceBox(o, cy0, cy1, focus, en, t, &openIt);
      if (openIt) {
        U.focus = i;
        openDropdown(o);
      }
    } else if (o.kind == PS::Kind::Slider) {
      slider(o, cy0, cy1, focus, en);
    } else if (o.kind == PS::Kind::Toggle) {
      const bool on = pend(o.id) == "on";
      Hit ch = hit(kCtlX0, cy0, kCtlX0 + 120, cy1);
      checkbox(kCtlX0 + 2, (cy0 + cy1) * 0.5f - 10, 20, on, focus || ch.hover);
      text(F.label, kSzLabel, kCtlX0 + 32, (cy0 + cy1) * 0.5f - 8, on ? "ON" : "OFF", C(kWhite), C(kSilver), 1.0f);
      if (ch.click) {
        U.focus = i;
        toggle(o);
      }
    } else if (o.kind == PS::Kind::Binding) {
      bindingChips(o, i, cy0, cy1, focus, t);
    } else if (o.kind == PS::Kind::Action) {
      if (button(kCtlX0, cy0, kCtlX0 + 150, cy1, displayedValue(o), focus, false, en, t)) {
        U.focus = i;
        runAction(o);
      }
    }
    // a click anywhere else on the row (the control took its own) focuses it
    if (rowHover && U.clickL) {
      U.clickL = false;
      U.focus = i;
    }
  }
  popClip();
  if (maxS > 1 && U.scroll < maxS - 1) {   // more below: the rows fade out under the edge
    DL->AddRectFilledMultiColor(X.p(kListX0 - 6, kListY1 - 26), X.p(kListX1 + 6, kListY1 + 2), col(0.02f, 0.08f, 0.26f, 0.0f),
                                col(0.02f, 0.08f, 0.26f, 0.0f), col(0.02f, 0.07f, 0.24f, 0.95f), col(0.02f, 0.07f, 0.24f, 0.95f));
  }
  // scroll indicator
  if (maxS > 1) {
    const float x = kListX1 + 2, trackH = kListY1 - kListY0, thumbH = std::max(30.0f, trackH * viewH / (viewH + maxS));
    const float ty = kListY0 + (trackH - thumbH) * clampf(U.scroll / maxS, 0, 1);
    DL->AddRectFilled(X.p(x, kListY0), X.p(x + 4, kListY1), col(0.01f, 0.04f, 0.14f, 0.7f), X.d(2));
    DL->AddRectFilled(X.p(x, ty), X.p(x + 4, ty + thumbH), col(0.78f, 0.86f, 0.98f, 0.8f), X.d(2));
  }
}

void ensureFocusVisible() {
  if (U.focus < 0) return;
  const float y0 = rowY(U.focus) + U.scroll, y1 = y0 + kRowH;
  // a header right above the focus stays in view too
  const float top = (U.focus > 0 && U.rows[size_t(U.focus - 1)].header) ? y0 - kHeadH : y0;
  const float viewH = kListY1 - kListY0 - 8;
  if (top - kListY0 - 4 < U.scrollT) U.scrollT = top - kListY0 - 4;
  if (y1 - kListY0 - 4 > U.scrollT + viewH) U.scrollT = y1 - kListY0 - 4 - viewH;
  U.scrollT = std::max(0.0f, U.scrollT);
}

void drawPane(double t) {
  glass(kPaneX0, kPaneY0 - 6, kPaneX1, kPaneY1, 16, 0.9f, true);
  if (U.focus < 0 || U.focus >= int(U.rows.size())) return;
  const PS::Option& o = U.rows[size_t(U.focus)].opt;
  float y = kPaneY0 + 12;
  for (const std::string& l : wrap(F.title, 21, o.label, kPaneX1 - kPaneX0 - 40)) {
    textTitle(F.title, 21, kPaneX0 + 20, y, l);
    y += 27;
  }
  // gold rule
  DL->AddRectFilledMultiColor(X.p(kPaneX0 + 20, y + 4), X.p(kPaneX1 - 20, y + 5.2f), C(kGold, 0.8f), C(kGold, 0.0f),
                              C(kGold, 0.0f), C(kGold, 0.8f));
  y += 16;
  for (const std::string& l : wrap(F.body, kSzBody, o.desc, kPaneX1 - kPaneX0 - 40)) {
    textPlain(F.body, kSzBody, kPaneX0 + 20, y, l, C(kSilver));
    y += 20;
  }
  y += 12;
  auto meta = [&](const char* k, const std::string& v, Rgb c) {
    text(F.badge, 11, kPaneX0 + 20, y + 2, k, C(kSilverDim), C(kSilverDim), 0.0f);
    text(F.label, 13.5f, kPaneX0 + 150, y, v, C(c), C(c), 1.0f);
    y += 24;
  };
  // performance
  text(F.badge, 11, kPaneX0 + 20, y + 2, "PERFORMANCE", C(kSilverDim), C(kSilverDim), 0.0f);
  if (o.perf != "-") {
    perfBars(kPaneX0 + 150, y + 3, o.perf);
    text(F.label, 13.5f, kPaneX0 + 196, y, o.perf == "NONE" ? "NO COST" : o.perf + " COST", C(kWhite), C(kSilver), 1.0f);
  }
  y += 24;
  std::string applies = o.apply == PS::Apply::Restart ? "APPLY SAVES IT - AFTER A RESTART"
                        : o.apply == PS::Apply::Confirm ? "ON APPLY - KEEP OR REVERT"
                                                        : "AT ONCE - APPLY SAVES IT";
  if (o.kind == PS::Kind::Action) applies = "WHEN PRESSED";
  if (o.kind == PS::Kind::Binding) applies = "AT ONCE - APPLY SAVES IT";
  meta("APPLIES", applies, o.apply == PS::Apply::Restart ? kGold : kWhite);
  meta(o.kind == PS::Kind::Binding ? "GAME CONTROL" : "XBOX 360", upperAscii(o.xbox), kWhite);
  if (o.kind == PS::Kind::Binding) {
    for (const auto& c : bindConflicts(o.id)) {
      text(F.label, 13, kPaneX0 + 20, y, upperAscii(c.first) + " ALSO: " + upperAscii(c.second),
           col(1.0f, 0.55f, 0.45f), col(0.9f, 0.4f, 0.35f), 1.0f);
      y += 19;
    }
  }
  if (!o.defaultValue.empty() && o.kind != PS::Kind::Action && o.id != "preset") {
    std::string dv = o.defaultValue;
    if (o.kind == PS::Kind::Choice) dv = choiceLabel(o, dv);
    else if (o.kind == PS::Kind::Toggle) dv = dv == "on" ? "ON" : "OFF";
    else if (o.kind == PS::Kind::Slider) dv = sliderText(o, float(std::atof(dv.c_str())));
    meta("DEFAULT", dv, kWhite);
  }
  if (changed(o.id)) {
    // what is applied now (the row shows the unapplied value)
    const std::string sv = U.saved[o.id];
    std::string sl = sv;
    if (o.kind == PS::Kind::Choice) sl = choiceLabel(o, sv);
    else if (o.kind == PS::Kind::Toggle) sl = sv == "on" ? "ON" : "OFF";
    else if (o.kind == PS::Kind::Slider) sl = sliderText(o, float(std::atof(sv.c_str())));
    meta("SAVED VALUE", sl, kGold);
  }
  if (!o.available) {
    y += 6;
    for (const std::string& l : wrap(F.label, 13, "NOT AVAILABLE. " + upperAscii(o.unavailableWhy), kPaneX1 - kPaneX0 - 40)) {
      text(F.label, 13, kPaneX0 + 20, y, l, col(1.0f, 0.55f, 0.45f), col(0.9f, 0.4f, 0.35f), 1.0f);
      y += 19;
    }
  }
  // the peek button (hold to see the game): only where the picture is what the rows change
  if (curTab() != PS::Tab::Display && curTab() != PS::Tab::Graphics) {
    U.peekMouse = false;
    return;
  }
  const float bx0 = kPaneX1 - 150, bx1 = kPaneX1 - 18, by0 = kPaneY1 - 44, by1 = kPaneY1 - 16;
  Hit h = hit(bx0, by0, bx1, by1);
  U.peekMouse = h.hover && U.lDown;
  ImVec2 p[8];
  const int n = chamfer(bx0, by0, bx1, by1, 6, 0, 6, 0, p);
  gradPoly(p, n, col(0.10f, 0.26f, 0.60f, 0.9f), col(0.03f, 0.10f, 0.30f, 0.92f));
  DL->AddPolyline(p, n, h.hover ? C(kGold) : col(0.5f, 0.7f, 1.0f, 0.5f), ImDrawFlags_Closed, std::max(1.0f, X.d(1)));
  // an eye
  const float ex = bx0 + 18, ey = (by0 + by1) * 0.5f;
  DL->AddEllipse(X.p(ex, ey), ImVec2(X.d(8), X.d(4.5f)), C(kWhite), 0, 20, std::max(1.0f, X.d(1.2f)));
  DL->AddCircleFilled(X.p(ex, ey), X.d(2.2f), C(kWhite), 10);
  text(F.badge, 11, bx0 + 32, by0 + 8, "HOLD TO PREVIEW", C(kWhite), C(kSilver), 1.0f);
  (void)t;
}

void drawButtons(double t) {
  const int nc = changedCount();
  if (button(kListX0 - 8, kBtnY0, kListX0 + 210, kBtnY1, "RESET TO DEFAULTS", false, false, true, t, cX)) {
    U.modal = mReset;
    U.modalFocus = 1;   // CANCEL first
  }
  // lane E E3: REVERT shows its control too (the pad's BACK / VIEW; the keyboard key bound to MAP)
  if (button(kListX1 - 330, kBtnY0, kListX1 - 175, kBtnY1, "REVERT", false, false, nc > 0, t, cView)) revertAll();
  if (button(kListX1 - 160, kBtnY0, kListX1 + 8, kBtnY1, "APPLY", false, true, nc > 0, t, cY)) applyAll();
}

struct Prompt {
  Ctl c;
  std::string label;
  Act act;
  const char* key = nullptr;   // a fixed keyboard cap for a mode that takes fixed keys (setKeyCapOverride)
};

void drawPromptBar() {
  std::vector<Prompt> right, left;
  if (U.capOn) {
    // a key capture takes fixed keys (onKey: the next key is the binding, Esc cancels; pad B cancels; a mouse button
    // is the binding, so the prompt is not clickable): the cap names Esc, never a rebound MenuBack (Astra E3 r3)
    right = {{cB, "Cancel", aNone, "Esc"}};
  } else if (U.modal != mNone) {
    right = {{cA, "Select", aAccept}, {cB, "Back", aBack}};
  } else if (U.dd) {
    right = {{cA, "Choose", aAccept}, {cB, "Cancel", aBack}};
  } else {
    std::string a = "Change";
    if (U.focus >= 0 && U.focus < int(U.rows.size())) {
      const PS::Option& o = U.rows[size_t(U.focus)].opt;
      if (!o.available) a = "";
      else if (o.kind == PS::Kind::Toggle) a = "Toggle";
      else if (o.kind == PS::Kind::Action) a = "Run";
      else if (o.kind == PS::Kind::Choice) a = "Open list";
      else if (o.kind == PS::Kind::Binding) a = "Bind key";
      else a = "Adjust";
    }
    const bool bindRow = U.focus >= 0 && U.focus < int(U.rows.size()) &&
                         U.rows[size_t(U.focus)].opt.kind == PS::Kind::Binding;
    right = {{cX, bindRow ? "Clear key" : "Reset to defaults", aX}};
    if (changedCount() > 0) right.push_back({cY, "Apply", aY});
    right.push_back({cB, "Back", aBack});
    if (!a.empty()) right.insert(right.begin(), Prompt{cA, a, aAccept});
    left = {{cLB, "Tabs", aNone}, {cDpad, "Move", aNone}};
    gMoveLeftRight = true;   // MOVE here is 4-way: rows, and LEFT / RIGHT adjust a value or pick a binding slot
    if (curTab() == PS::Tab::Display || curTab() == PS::Tab::Graphics)
      left.push_back({cRT, "Hold: preview", aNone});
  }
  float x = kFX1 - 40;
  const float y = 668;
  for (int i = int(right.size()) - 1; i >= 0; --i) {
    const Prompt& p = right[size_t(i)];
    const std::string l = upperAscii(p.label);
    if (p.key) setKeyCapOverride(p.c, p.key);
    const float w = textW(F.card, 14, l);
    const float gw = std::max(22.0f, glyphW(p.c, 22));
    const float x0 = x - w - gw - 10;
    Hit h = hit(x0 - 6, y - 6, x + 6, y + 24);
    if (h.hover && p.act != aNone) selBand(x0 - 14, y - 6, x + 14, y + 24, 0.8f);
    textTitle(F.card, 14, x0, y + 1, l);
    glyph(p.c, x - gw * 0.5f, y + 9, 22);
    if (p.key) setKeyCapOverride(p.c, nullptr);
    if (h.click && p.act != aNone) gActs.push_back(p.act);
    x = x0 - 26;
  }
  float lx = kFX0 + 40;
  for (const Prompt& p : left) {
    const std::string l = upperAscii(p.label);
    float gx = lx;
    if (p.c == cLB) {
      // lane E E3: the two tab glyphs are clickable (LB = previous tab, RB = next), one action per click
      for (const Ctl g : {cLB, cRB}) {
        const float w = glyphW(g, 20);
        const Hit h = hit(gx - 4, y - 6, gx + w + 4, y + 24);
        if (h.hover) selBand(gx - 8, y - 6, gx + w + 8, y + 24, 0.8f);
        glyphAt(g, gx, y + 9, 20);
        if (h.click) gActs.push_back(g == cLB ? aLB : aRB);
        gx += w + (g == cLB ? 6 : 10);
      }
    } else {
      gx += glyphAt(p.c, gx, y + 9, 20) + 10;
    }
    text(F.smallF, 12.5f, gx, y + 1.5f, l, C(kSilver), C(kSilverDim), 1.0f);
    lx = gx + textW(F.smallF, 12.5f, l) + 24;
  }
}

void drawDropdown(double t) {
  if (!U.dd) return;
  const PS::Option* o = opt(U.ddId);
  int ri = -1;
  for (int i = 0; i < int(U.rows.size()); ++i)
    if (!U.rows[size_t(i)].header && U.rows[size_t(i)].opt.id == U.ddId) ri = i;
  if (!o || ri < 0) {
    U.dd = false;
    return;
  }
  gLayer = 1;
  const int n = int(o->choices.size());
  const float ih = 28, vis = float(std::min(n, 9));
  const float h = vis * ih + 8;
  const float by1 = rowY(ri) + kRowH - 4;
  float y0 = by1 + 2;
  if (y0 + h > kFY1 - 50) y0 = rowY(ri) + 4 - h - 2;
  const float x0 = kCtlX0, x1 = kCtlX1;
  // dim the page a touch
  ImVec2 bp[8];
  const int bn = chamfer(x0, y0, x1, y0 + h, 6, 0, 6, 0, bp);
  gradPoly(bp, bn, col(0.04f, 0.13f, 0.38f, 1.0f), col(0.02f, 0.06f, 0.22f, 1.0f));
  DL->AddPolyline(bp, bn, C(kGold), ImDrawFlags_Closed, std::max(1.0f, X.d(1.4f)));
  const float maxS = std::max(0.0f, float(n) * ih - (h - 8));
  if (U.ddScroll < 0) U.ddScroll = clampf(float(U.ddFocus) * ih - (h - 8) * 0.5f + ih * 0.5f, 0, maxS);
  if (inside(x0, y0, x1, y0 + h) && U.wheel) {
    U.ddScroll = clampf(U.ddScroll - float(U.wheel) * ih * 2, 0, maxS);
    U.wheel = 0;
  }
  // keep the focus visible
  if (float(U.ddFocus) * ih < U.ddScroll) U.ddScroll = float(U.ddFocus) * ih;
  if (float(U.ddFocus + 1) * ih > U.ddScroll + h - 8) U.ddScroll = float(U.ddFocus + 1) * ih - (h - 8);
  pushClip(x0 + 2, y0 + 4, x1 - 2, y0 + h - 4);
  const std::string cur = o->id == "preset" ? presetNow() : pend(o->id);
  for (int k = 0; k < n; ++k) {
    const float iy0 = y0 + 4 + float(k) * ih - U.ddScroll, iy1 = iy0 + ih;
    if (iy1 < y0 || iy0 > y0 + h) continue;
    Hit hh = hit(x0 + 2, iy0, x1 - 2, iy1);
    if (hh.hover && U.mouseMoved) U.ddFocus = k;
    if (k == U.ddFocus) selBand(x0 + 2, iy0 + 1, x1 - 2, iy1 - 1, 1.0f);
    const PS::Choice& c = o->choices[size_t(k)];
    const bool sel = c.value == cur;
    if (sel) checkMark(x0 + 8, iy0 + 6, 16, C(kGold), 2.0f);
    text(F.label, kSzLabel, x0 + 30, iy0 + 6, ellipsize(F.label, kSzLabel, c.label, x1 - x0 - 40),
         sel ? C(kGold) : C(kWhite), sel ? C(kGoldDeep) : C(kSilver), 1.0f);
    if (hh.click) {
      setPending(o->id, c.value);
      U.dd = false;
    }
  }
  popClip();
  // a click outside closes it
  if (U.clickL && !inside(x0, y0, x1, y0 + h)) {
    U.clickL = false;
    U.dd = false;
  }
  gLayer = 0;
  (void)t;
}

void drawModal(double t) {
  if (U.modal == mNone) return;
  gLayer = 1;
  DL->AddRectFilled(ImVec2(0, 0), ImVec2(float(X.bbW), float(X.bbH)), col(0, 0.01f, 0.05f, 0.55f));
  const float x0 = 360, x1 = 920, y0 = 236, y1 = 464;
  {
    ImVec2 bp[8];
    const int bn = chamfer(x0, y0, x1, y1, 18, 18 * 0.35f, 18, 18 * 0.35f, bp);
    gradPoly(bp, bn, col(0.03f, 0.10f, 0.30f, 1.0f), col(0.01f, 0.04f, 0.15f, 1.0f));
  }
  glass(x0, y0, x1, y1, 18, 1.0f, true);
  chrome(x0 + 70, y0 - 5, x1 - 70, y0 + 7, true);
  std::string title, l1, l2;
  std::vector<std::pair<std::string, int>> btns;   // label, action id
  if (U.modal == mConfirmDisplay) {
    const double left = kConfirmSecs - (t - U.confirmT0);
    if (left <= 0) {
      revertDisplay("timeout");
      U.modal = U.restartIds.empty() ? mNone : mRestart;
      gLayer = 0;
      return;
    }
    title = "KEEP THESE DISPLAY SETTINGS?";
    char b[64];
    std::snprintf(b, sizeof b, "Reverting in %d s.", int(std::ceil(left)));
    l1 = b;
    l2 = "If the picture is wrong, wait or choose REVERT.";
    btns = {{"KEEP", 1}, {"REVERT", 2}};
  } else if (U.modal == mRestart) {
    title = "RESTART NOW?";
    std::string names;
    for (const std::string& id : U.restartIds)
      if (const PS::Option* o = opt(id)) names += (names.empty() ? "" : ", ") + o->label;
    l1 = "These apply after a restart: " + names + ".";
    l2 = U.ctx == "IG" ? "Progress since your last save is lost." : "You return to the title screen.";
    btns = {{"RESTART NOW", 3}, {"LATER", 4}};
  } else if (U.modal == mUnsaved) {
    title = "APPLY CHANGES?";
    char b[64];
    std::snprintf(b, sizeof b, "%d setting%s changed and not applied.", changedCount(), changedCount() == 1 ? "" : "s");
    l1 = b;
    l2 = "";
    btns = {{"APPLY", 5}, {"DISCARD", 6}, {"CANCEL", 7}};
  } else if (U.modal == mReset) {
    title = "RESET TO DEFAULTS?";
    l1 = std::string("Every setting on the ") + PS::tabName(curTab()) + " tab returns to its default.";
    l2 = "Nothing is saved until you APPLY.";
    btns = {{"RESET", 8}, {"CANCEL", 9}};
  }
  textTitle(F.title, 24, 640 - textW(F.title, 24, title) * 0.5f, y0 + 26, title);
  float ly = y0 + 72;
  for (const std::string& para : {l1, l2})
    for (const auto& l : wrap(F.body, kSzBody, para, x1 - x0 - 80)) {
      textPlain(F.body, kSzBody, 640 - textW(F.body, kSzBody, l) * 0.5f, ly, l, C(kSilver));
      ly += 21;
    }
  const int nb = int(btns.size());
  // lane E E3: each button that a control presses shows that control - A on the focused one (A presses the focused
  // button), B on the one B triggers (dispatch: REVERT / LATER / CANCEL), both when they are the same button
  // (Astra E3 r1). The buttons widen (equally, within the modal) to the widest one's worst case - its label with A,
  // plus B on the cancel button - so long rebound caps never crowd the label (Astra E3 r3).
  const int cancelIdx = U.modal == mUnsaved ? 2 : 1;
  const float gap = 20;
  float bw = nb == 3 ? 150.0f : 200.0f;
  for (int i = 0; i < nb; ++i) {
    float need = glyphW(cA, 17) + 7 + textW(F.card, 13.5f, btns[size_t(i)].first) + 24;
    if (i == cancelIdx) need += glyphW(cB, 17) + 4;
    bw = std::max(bw, need);
  }
  bw = std::min(bw, (x1 - x0 - 80 - float(nb - 1) * gap) / float(nb));
  float bx = 640 - (float(nb) * bw + float(nb - 1) * gap) * 0.5f;
  U.modalFocus = std::clamp(U.modalFocus, 0, nb - 1);
  int clicked = -1;
  for (int i = 0; i < nb; ++i) {
    const bool focused = i == U.modalFocus, cancels = i == cancelIdx;
    const Ctl g = focused ? cA : (cancels ? cB : cCount);
    const Ctl g2 = focused && cancels ? cB : cCount;
    if (button(bx, y1 - 64, bx + bw, y1 - 30, btns[size_t(i)].first, focused, i == 0, true, t, g, g2))
      clicked = btns[size_t(i)].second;
    bx += bw + gap;
  }
  // countdown bar
  if (U.modal == mConfirmDisplay) {
    const float k = clampf(float((t - U.confirmT0) / kConfirmSecs), 0, 1);
    DL->AddRectFilled(X.p(x0 + 40, y1 - 20), X.p(x1 - 40, y1 - 16), col(0.1f, 0.2f, 0.45f, 0.9f), X.d(2));
    DL->AddRectFilled(X.p(x0 + 40, y1 - 20), X.p(x0 + 40 + (x1 - x0 - 80) * (1 - k), y1 - 16), C(kGold), X.d(2));
  }
  gLayer = 0;
  // the modal's own focus action (A) arrives as clicked = the focused button
  if (clicked < 0) return;
  switch (clicked) {
    case 1:   // KEEP
      U.confirmOld.clear();
      U.modal = U.restartIds.empty() ? mNone : mRestart;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] display settings kept");
      break;
    case 2:   // REVERT
      revertDisplay("player");
      U.modal = U.restartIds.empty() ? mNone : mRestart;
      break;
    case 3: {   // RESTART NOW
      std::string m;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] RESTART NOW");
      if (!ML::restartNow(&m)) toastMsg(upperAscii(m));
      U.modal = mNone;
      break;
    }
    case 4:
      U.modal = mNone;
      U.restartIds.clear();
      if (U.closeAfterModal) beginClose();
      break;
    case 5:
      U.modal = mNone;
      U.closeAfterModal = true;
      applyAll();
      if (U.modal == mNone) beginClose();
      break;
    case 6:
      U.modal = mNone;
      revertAll();
      beginClose();
      break;
    case 7: U.modal = mNone; break;
    case 8:
      U.modal = mNone;
      resetTab();
      break;
    case 9: U.modal = mNone; break;
    default: break;
  }
}

void drawToast(double t) {
  const double age = t - U.toastT;
  if (age > 2.4 || U.toast.empty() || U.modal != mNone) return;
  const float a = float(std::min(1.0, std::min(age / 0.15, (2.4 - age) / 0.4)));
  const float w = std::min(kPaneX1 - kPaneX0 - 40, textW(F.label, 13, U.toast) + 48);
  const float cx = (kPaneX0 + kPaneX1) * 0.5f;
  const float x0 = cx - w * 0.5f, y0 = kPaneY1 - 96 - 6 * (1 - a), y1 = y0 + 30;
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x0 + w, y1, 9, 0, 9, 0, p);
  gradPoly(p, n, col(0.10f, 0.28f, 0.66f, 0.96f * a), col(0.03f, 0.10f, 0.32f, 0.96f * a));
  DL->AddPolyline(p, n, col(kGold.r, kGold.g, kGold.b, 0.9f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(1.2f)));
  const std::string s = ellipsize(F.label, 13, U.toast, w - 24);
  text(F.label, 13, cx - textW(F.label, 13, s) * 0.5f, y0 + 6.5f, s, C(kWhite, a), C(kSilver, a), 1.0f);
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Input
// ═════════════════════════════════════════════════════════════════════════════════════════
// Lane E E3: the keys follow the PC controls (kernel/pc_controls.hpp hostKeyAct: the menu bindings, Enter / Esc as
// failsafes, then the host-only keys; the harness channel keeps the iter-450S test keys). F12 = screenshot.
// RT is hold-to-preview here (onKey: the key the rule gives RT, the one the "Hold: preview" cap names).
Act keyAct(PC::HostAct ha, int sc) {
  switch (ha) {
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
    case PC::HostAct::View: return aView;   // the menus' BACK / VIEW (MAP's key, default M) = REVERT
    default: break;
  }
  return sc == SDL_SCANCODE_F12 ? aShot : aNone;
}
bool isRepeatable(Act a) { return a == aUp || a == aDown || a == aLeft || a == aRight; }

void onKey(int sc, bool down, bool repeat, uint16_t mods, bool fromHarness = false) {
  // The key holding the preview owns it until its own release, whatever a binding change or a capture does meanwhile;
  // its repeats are consumed (Astra E3 r2: a release reclassified by new bindings left the screen invisible).
  if (U.peekSc != 0 && sc == U.peekSc) {
    if (!down) {
      U.peekKey = false;
      U.peekSc = 0;
    }
    return;
  }
  if (U.capOn) {   // a binding row is capturing: the key is the binding (Esc cancels)
    if (!down || repeat) return;
    gLastWasPad = false;
    endCapture(sc == SDL_SCANCODE_ESCAPE ? std::string() : PC::inputName(uint16_t(sc)));
    return;
  }
  const PC::Keymap km = PC::hostKeymap(fromHarness);
  const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
  // Hold to preview = RT's key under the one rule, P too in the test keymap (pc_controls.hpp hostPreviewKey; Astra
  // E3 r1: a binding wins, in a modal too)
  if (PC::hostPreviewKey(uint16_t(sc), shift, km, PC::config().bindings)) {
    if (down && !repeat && U.peekSc == 0) {   // a second preview key while one holds it: nothing
      U.peekKey = true;
      U.peekSc = sc;
    }
    return;
  }
  const PC::HostAct ha = PC::hostKeyAct(uint16_t(sc), shift, km, PC::config().bindings);
  if (!down) return;
  gLastWasPad = false;
  const Act a = keyAct(ha, sc);
  if (a == aNone) return;
  if (repeat && !isRepeatable(a)) return;
  gActs.push_back(a);
}

void pollHarnessKeys(double t) {
  const uint8_t* hk = sr::kernel::automation::harnessKeys();
  if (!hk) return;
  const uint16_t mods = PC::harnessKeyMods(hk);   // its own held Shift / Ctrl / Alt (Astra E3 r1)
  for (int sc = 1; sc < 512; ++sc) {
    const bool d = hk[sc] != 0, p = U.hkPrev[sc] != 0;
    if (d && !p) {
      onKey(sc, true, false, mods, true);
      U.hkRepeatAt[sc] = t + 0.40;
    } else if (d && p && t >= U.hkRepeatAt[sc]) {
      onKey(sc, true, true, mods, true);
      U.hkRepeatAt[sc] = t + 0.09;
    } else if (!d && p) {
      onKey(sc, false, false, mods, true);
    }
    U.hkPrev[sc] = hk[sc];
  }
}

void pollPad(double t) {
  HI::PadState ps;
  if (!HI::readActivePad(&ps)) {
    U.padPrev = 0;
    U.peekPad = false;
    return;
  }
  uint16_t b = ps.buttons;
  if (ps.ly > 16000) b |= 0x0001;
  if (ps.ly < -16000) b |= 0x0002;
  if (ps.lx < -16000) b |= 0x0004;
  if (ps.lx > 16000) b |= 0x0008;
  const uint16_t edge = uint16_t(b & ~U.padPrev);
  if (b) gLastWasPad = true;
  if (U.capOn) {   // a controller cannot be bound here (it keeps the game's own buttons): B cancels
    if (edge & 0x2000) endCapture(std::string());
    U.padPrev = b;
    return;
  }
  const Act dirs[4] = {aUp, aDown, aLeft, aRight};
  for (int i = 0; i < 4; ++i) {
    const uint16_t bit = uint16_t(1u << i);
    if (edge & bit) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.40;
    } else if ((b & bit) && t >= U.padRepeatAt[i]) {
      gActs.push_back(dirs[i]);
      U.padRepeatAt[i] = t + 0.07;
    }
  }
  if (edge & 0x1000) gActs.push_back(aAccept);
  if (edge & 0x2000) gActs.push_back(aBack);
  if (edge & 0x4000) gActs.push_back(aX);
  if (edge & 0x8000) gActs.push_back(aY);
  if (edge & 0x0100) gActs.push_back(aLB);
  if (edge & 0x0200) gActs.push_back(aRB);
  if (edge & 0x0010) gActs.push_back(aY);      // START = APPLY
  if (edge & 0x0020) gActs.push_back(aView);   // BACK / VIEW = REVERT
  const bool lt = ps.lt > 128, rt = ps.rt > 128;
  if (lt && !U.ltPrev) gActs.push_back(aLT);
  U.peekPad = rt;
  U.ltPrev = lt;
  U.rtPrev = rt;
  U.padPrev = b;
}

void primeInputs() {
  HI::PadState ps;
  U.peekKey = false;
  U.peekSc = 0;
  U.padPrev = HI::readActivePad(&ps) ? ps.buttons : 0;
  U.ltPrev = U.rtPrev = true;
  if (const uint8_t* hk = sr::kernel::automation::harnessKeys()) std::memcpy(U.hkPrev, hk, 512);
  else std::memset(U.hkPrev, 0, sizeof U.hkPrev);
}

void gatherInput(double t) {
  gActs.clear();
  U.clickL = U.clickR = U.releaseL = U.mouseMoved = false;
  U.wheel = 0;
  for (const ML::InputEvent& ev : ML::drainInput()) {
    switch (ev.kind) {
      case ML::InputEvent::Key: onKey(ev.scancode, ev.down, ev.repeat, ev.mods); break;
      case ML::InputEvent::MouseMove:
        if (std::fabs(ev.x - U.mx) > 0.01f || std::fabs(ev.y - U.my) > 0.01f) U.mouseMoved = true;
        U.mx = ev.x;
        U.my = ev.y;
        break;
      case ML::InputEvent::MouseDown:
        U.mx = ev.x;
        U.my = ev.y;
        if (U.capOn) {   // the mouse button is the binding (SDL: 1 left, 2 middle, 3 right, 4/5 X)
          static const char* const kB[] = {"", "Mouse1", "Mouse3", "Mouse2", "Mouse4", "Mouse5"};
          if (ev.button >= 1 && ev.button <= 5) endCapture(kB[ev.button]);
          gLastWasPad = false;
          break;
        }
        if (ev.button == 1) {
          U.clickL = true;
          U.lDown = true;
        }
        if (ev.button == 3) U.clickR = true;
        gLastWasPad = false;
        break;
      case ML::InputEvent::MouseUp:
        if (ev.button == 1) {
          U.releaseL = true;
          U.lDown = false;
        }
        break;
      case ML::InputEvent::Wheel:
        if (U.capOn) {
          endCapture(ev.wheel > 0 ? "WheelUp" : "WheelDown");
          break;
        }
        U.wheel += ev.wheel;
        gLastWasPad = false;
        break;
      default: break;
    }
  }
  pollHarnessKeys(t);
  pollPad(t);
  if (U.capOn && t - U.capT0 > 8.0) {
    endCapture(std::string());
    toastMsg("NO KEY PRESSED - BINDING UNCHANGED");
  }
  if (U.clickR && !U.capOn) gActs.push_back(aBack);
}

void moveFocus(int d) {
  int i = U.focus;
  for (int k = 0; k < int(U.rows.size()); ++k) {
    i += d;
    if (i < 0 || i >= int(U.rows.size())) return;
    if (!U.rows[size_t(i)].header) {
      U.focus = i;
      ensureFocusVisible();
      return;
    }
  }
}

void dispatch(Act a) {
  if (a == aShot) {
    U.shotWanted = !ML::shotDir().empty();
    return;
  }
  if (U.modal != mNone) {
    if (a == aLeft || a == aUp) U.modalFocus--;
    else if (a == aRight || a == aDown) U.modalFocus++;
    else if (a == aAccept) {
      // press the focused button: synthesise its click on the next draw
      // (drawModal maps the focus to its action)
      const Modal m = U.modal;
      const int f = U.modalFocus;
      int act = -1;
      if (m == mConfirmDisplay) act = f == 0 ? 1 : 2;
      else if (m == mRestart) act = f == 0 ? 3 : 4;
      else if (m == mUnsaved) act = f == 0 ? 5 : f == 1 ? 6 : 7;
      else if (m == mReset) act = f == 0 ? 8 : 9;
      switch (act) {
        case 1: U.confirmOld.clear(); U.modal = U.restartIds.empty() ? mNone : mRestart; break;
        case 2: revertDisplay("player"); U.modal = U.restartIds.empty() ? mNone : mRestart; break;
        case 3: {
          std::string msg;
          SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] RESTART NOW");
          if (!ML::restartNow(&msg)) toastMsg(upperAscii(msg));
          U.modal = mNone;
          break;
        }
        case 4: U.modal = mNone; U.restartIds.clear(); if (U.closeAfterModal) beginClose(); break;
        case 5: U.modal = mNone; U.closeAfterModal = true; applyAll(); if (U.modal == mNone) beginClose(); break;
        case 6: U.modal = mNone; revertAll(); beginClose(); break;
        case 7: U.modal = mNone; break;
        case 8: U.modal = mNone; resetTab(); break;
        case 9: U.modal = mNone; break;
        default: break;
      }
    } else if (a == aBack) {
      if (U.modal == mConfirmDisplay) {
        revertDisplay("player");
        U.modal = U.restartIds.empty() ? mNone : mRestart;
      } else if (U.modal == mRestart) {
        U.modal = mNone;
        U.restartIds.clear();
        if (U.closeAfterModal) beginClose();
      } else {
        U.modal = mNone;
      }
    }
    return;
  }
  if (U.dd) {
    const PS::Option* o = opt(U.ddId);
    const int n = o ? int(o->choices.size()) : 0;
    if (a == aUp) U.ddFocus = std::max(0, U.ddFocus - 1);
    else if (a == aDown) U.ddFocus = std::min(n - 1, U.ddFocus + 1);
    else if (a == aAccept && o && n) {
      setPending(o->id, o->choices[size_t(std::clamp(U.ddFocus, 0, n - 1))].value);
      U.dd = false;
    } else if (a == aBack) U.dd = false;
    return;
  }
  const PS::Option* o = (U.focus >= 0 && U.focus < int(U.rows.size())) ? &U.rows[size_t(U.focus)].opt : nullptr;
  if (o && o->kind == PS::Kind::Binding && o->available) {
    const int n = int(bindList(o->id).size());
    const int last = n < PC::kInputsPerAction ? n : n - 1;
    if (a == aLeft || a == aRight) {
      U.bindSlot = std::clamp(U.bindSlot + (a == aLeft ? -1 : 1), 0, std::max(0, last));
      return;
    }
    if (a == aAccept) {
      startCapture(*o, std::min(U.bindSlot, last));
      return;
    }
    if (a == aX) {
      clearBinding(*o, U.bindSlot);
      return;
    }
  }
  if (a == aUp || a == aDown) U.bindSlot = 0;
  switch (a) {
    case aUp: moveFocus(-1); break;
    case aDown: moveFocus(1); break;
    case aLeft:
    case aRight:
      if (o && o->available) {
        const int d = a == aLeft ? -1 : 1;
        if (o->kind == PS::Kind::Choice) stepChoice(*o, d);
        else if (o->kind == PS::Kind::Slider) stepSlider(*o, d);
        else if (o->kind == PS::Kind::Toggle) toggle(*o);
      }
      break;
    case aAccept:
      if (o && o->available) {
        if (o->kind == PS::Kind::Choice) openDropdown(*o);
        else if (o->kind == PS::Kind::Toggle) toggle(*o);
        else if (o->kind == PS::Kind::Action) runAction(*o);
      }
      break;
    case aBack: askClose(); break;
    case aX: U.modal = mReset; U.modalFocus = 1; break;   // CANCEL first
    case aY: applyAll(); break;
    case aView: if (changedCount()) revertAll(); break;
    case aLB: switchTab(-1); break;
    case aRB: switchTab(1); break;
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Performance overlay
// ═════════════════════════════════════════════════════════════════════════════════════════
void tickFps(double t) {
  if (U.lastPresentT > 0) {
    const float ms = float((t - U.lastPresentT) * 1000.0);
    U.worstAcc = std::max(U.worstAcc, ms);
  }
  U.lastPresentT = t;
  U.fpsFrames++;
  if (t - U.fpsT0 >= 0.5) {
    U.fps = float(U.fpsFrames / (t - U.fpsT0));
    U.frameMs = U.fps > 0 ? 1000.0f / U.fps : 0;
    U.worstMs = U.worstAcc;
    U.worstAcc = 0;
    U.fpsFrames = 0;
    U.fpsT0 = t;
  }
}

void drawPerfOverlay(int mode) {
  char a[64], b[96], c[96];
  std::snprintf(a, sizeof a, "%.0f FPS", double(U.fps));
  std::snprintf(b, sizeof b, "%.1f MS  (WORST %.1f)", double(U.frameMs), double(U.worstMs));
  const auto& s = sr::gfx::resscale::get();
  std::snprintf(c, sizeof c, "RENDER %u x %u  OUTPUT %d x %d", 1280u * s.x, 720u * s.y, X.bbW, X.bbH);
  const float w = mode == 2 ? std::max(textW(F.label, 12, c), textW(F.label, 12, b)) + 24 : textW(F.card, 15, a) + 24;
  const float h = mode == 2 ? 62 : 26;
  const float x1 = 1270, x0 = x1 - w, y0 = 10;
  DL->AddRectFilled(X.p(x0, y0), X.p(x1, y0 + h), colRaw(0.01f, 0.04f, 0.14f, 0.72f), X.d(4));
  DL->AddRect(X.p(x0, y0), X.p(x1, y0 + h), colRaw(0.5f, 0.7f, 1.0f, 0.5f), X.d(4), 0, std::max(1.0f, X.d(1)));
  const ImU32 fc = U.fps >= 58 ? colRaw(0.5f, 0.95f, 0.6f, 1) : U.fps >= 29 ? colRaw(1, 0.85f, 0.35f, 1) : colRaw(1, 0.45f, 0.4f, 1);
  DL->AddText(F.card, fontPx(15), X.p(x0 + 12, y0 + 5), fc, a);
  if (mode == 2) {
    DL->AddText(F.label, fontPx(12), X.p(x0 + 12, y0 + 26), colRaw(0.85f, 0.9f, 1, 1), b);
    DL->AddText(F.label, fontPx(12), X.p(x0 + 12, y0 + 42), colRaw(0.7f, 0.78f, 0.9f, 1), c);
  }
}

}  // namespace

bool shotRequested() {
  const bool mine = gShotPending.exchange(false);
  const bool ach = achui::shotRequested();   // lane E L-a: the ACHIEVEMENTS page's F12
  return mine || ach;
}

bool wantsFrame(bool otherOverlay) {
  // lane E L-a: the ACHIEVEMENTS page (gfx/achievements_ui.hpp) draws through this slot; asked every present
  const bool ach = achui::wantsFrame();
  if (U.shown || ach) return true;
  if (ML::isOpen() && ML::openPage() == ML::OverlayPage::Settings) return true;
  return !otherOverlay && PS::perfOverlayMode() > 0;
}

bool render(const FrameCtx& f, bool otherOverlay) {
  // lane E L-a: while the ACHIEVEMENTS page is up (or asked for) it owns this frame - its own state, input and drawing
  // (one host overlay at a time: the mod library's routing)
  if (!U.shown && achui::wantsFrame()) return achui::render(f, otherOverlay);
  const double t = nowSec();
  std::string ctx;
  if (ML::openPage() == ML::OverlayPage::Settings && ML::takeOpenRequest(&ctx)) {
    U.shown = true;
    U.closing = false;
    U.ctx = ctx;
    U.openA = 0;
    U.dd = false;
    U.modal = mNone;
    U.closeAfterModal = false;
    U.dragId.clear();
    U.restartIds.clear();
    U.tab = 0;   // every open starts on the first shown tab (GAME; lane E L-b), first row
    U.focus = -1;
    for (int& f2 : U.focusPerTab) f2 = -1;
    U.scroll = U.scrollT = 0;
    refreshCatalogue();
    for (int i = 0; i < int(PS::Tab::Count); ++i)
      if (tabVisible(i)) {
        U.tab = i;
        break;
      }
    loadValues();
    buildRows();
    primeInputs();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] shown (%s, %dx%d)", ctx.c_str(), f.bbW, f.bbH);
  }
  const int perf = otherOverlay ? 0 : PS::perfOverlayMode();
  if (perf > 0) tickFps(t);
  if (!U.shown && perf == 0) return false;
  if (!ensureContext(f.device, f.queue)) {
    if (U.shown) {
      U.shown = false;
      ML::setShown(false);
    }
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
    if (U.shown) {
      U.shown = false;
      ML::setShown(false);
    }
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

  if (U.shown) {
    U.hovered = false;
    gatherInput(t);
    // prompts: a developer pin (SR_MOD_LIBRARY_PROMPTS), else the input layer's set
    static const int s_pin = [] {
      const char* e = std::getenv("SR_MOD_LIBRARY_PROMPTS");
      if (!e) return -1;
      const std::string v = e;
      return v == "xbox" ? int(psXbox) : v == "ps4" ? int(psPS4) : v == "ps5" ? int(psPS5)
           : v == "switch" ? int(psSwitch) : v == "keyboard" ? int(psKeyboard) : -1;
    }();
    if (s_pin >= 0) gPromptSet = PromptSet(s_pin);
    else if (HI::glyphsEnabled()) {
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
    // the catalogue's live parts (availability, monitors) once a second
    if (t - U.catT > 1.0 && U.modal == mNone && !U.dd && U.dragId.empty()) {
      refreshCatalogue();
      buildRows();
    }
    // mouse wheel scrolls the list
    if (U.wheel && U.modal == mNone && !U.dd && inside(kListX0, kListY0, kListX1 + 10, kListY1)) {
      U.scrollT -= float(U.wheel) * kRowH * 2;
      U.wheel = 0;
    }
    const std::vector<Act> acts = gActs;
    gActs.clear();
    if (!U.closing)
      for (Act a : acts) dispatch(a);
    U.openA = approach(U.openA, U.closing ? 0.0f : 1.0f, 12.0f, dt);
    if (!U.closing && U.openA > 0.995f) U.openA = 1.0f;
    U.tabSlide = approach(U.tabSlide, 0.0f, 14.0f, dt);
    U.scroll = approach(U.scroll, U.scrollT, 18.0f, dt);
    const bool peek = (U.peekPad || U.peekKey || U.peekMouse) && U.modal == mNone && !U.dd;
    U.peekA = approach(U.peekA, peek ? 1.0f : 0.0f, 14.0f, dt);
    const float a = smooth01(U.openA) * (1.0f - U.peekA);
    gAlpha = a;
    const float zoom = 0.97f + 0.03f * smooth01(U.openA);
    {
      const float fw = 1280.0f * X.s, fh = 720.0f * X.s;
      const float cx = X.ox + fw * 0.5f, cy = X.oy + fh * 0.5f;
      X.ox = cx - fw * 0.5f * zoom;
      X.oy = cy - fh * 0.5f * zoom;
      X.s *= zoom;
    }
    drawBackdrop();
    drawFrame();
    drawHeader(t);
    drawStatusLine(t);
    {
      const float keep = X.ox;
      X.ox += U.tabSlide * X.s;
      drawList(t);
      drawPane(t);
      X.ox = keep;
    }
    drawButtons(t);
    drawToast(t);
    drawDropdown(t);
    drawModal(t);
    {
      const int keepLayer = gLayer;
      gLayer = (U.modal != mNone || U.dd) ? 1 : 0;
      drawPromptBar();
      gLayer = keepLayer;
    }
    // clicks on the prompt bar arrive as actions for the next frame
    const std::vector<Act> late = gActs;
    gActs.clear();
    if (!U.closing)
      for (Act x : late) dispatch(x);
    if (U.peekA > 0.5f) {
      gAlpha = 1.0f;
      const std::string s = "PREVIEW";
      text(F.tab, 16, 640 - textW(F.tab, 16, s) * 0.5f, 24, s, C(kGold), C(kGoldDeep), 1.0f);
    }
    ML::setMouseHover(U.hovered);
    gAlpha = 1.0f;
  }
  if (perf > 0 && !(U.shown && U.peekA < 0.5f && U.openA > 0.5f)) {
    // the perf overlay, in the game's 16:9 image (not while the screen covers it)
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
    drawPerfOverlay(perf);
  }

  ImGui::Render();
  ID3D12DescriptorHeap* heaps[] = {G.heap.Get()};
  pumpUploads(f.cl);
  f.cl->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), f.cl);
  if (U.shotWanted) {
    U.shotWanted = false;
    gShotPending.store(true);
  }
  if (U.shown && U.closing && U.openA < 0.01f) {
    U.shown = false;
    U.closing = false;
    PS::revertPreviews();   // nothing unapplied survives a close
    ML::setShown(false);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] hidden");
  }
  return true;
}

}  // namespace settingsui
}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
