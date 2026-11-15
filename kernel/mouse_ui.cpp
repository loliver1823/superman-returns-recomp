// iter-464 PLATFORM FEATURE: the in-game mouse cursor. Contract: kernel/mouse_ui.hpp,
// design + evidence: docs/MOUSE_CURSOR.md.
//
// Charter (docs/NATURAL_PATH_CHARTER.md 2a, USER DECISION 2026-10-06): a PORT FEATURE acting on the
// player's mouse - it calls the menus' own functions instead of pressing pad buttons:
//   * mouseUiAfterAptPump READS Apt state on the guest thread (fault-free), then calls at most one
//     menu function per pump (kernel/apt_call.hpp: the game's own AS invoke sequence): the
//     surface's focus step, gEH.HandleEvent(CROSS / TRIANGLE / SQUARE / START) - the game's input
//     router, as its Key listener calls it - or a slider's / toggle's own value method;
//   * it presses nothing: the pad report never carries the mouse;
//   * the overlay is drawn by the host after the frame dump (gfx/d3d12_overlay.cpp).
// It rides apt_parse_trace.cpp's override of sub_826D2718 (docs/OVERRIDES.json: port-feature);
// off on natural rolls (automation::portFeaturesOn()).
//
// Apt object model used here (IDA ida-w5 + the three Xenia golden dumps, MOUSE_CURSOR.md 3):
//   value object      +0 vtable, +4 flags: low 7 bits = type, bit 27 = live.
//                     1 string (+8 -> string obj, chars at +8), 5 bool (+8 byte), 6 number
//                     (+8 f32), 7 int (+8 i32), 12 MovieClip wrapper, 14 Array, 17 _global,
//                     19 Object, 34/35 functions, 3 undefined
//   property table    plain objects/arrays/functions: obj+8 (vt[2] = sub_826819C0);
//                     MovieClip wrapper (vt 0x82086FF0): *(*(w+0x20)+12) (vt[2] = sub_82680448).
//                     {+0 cap (pow2), +4 entries[{name*, value*}], +8 __proto__, +12 prototype}
//                     (AsPropertyGet_sub_8268C228 special-cases those two names). Entry name =
//                     string obj: +6 u16 hash (AptStringHashFNV1a16CI_sub_82681C40), +8 chars;
//                     empty slot 0, deleted 0x829311B4 (AsPropertyProbe_sub_82684768)
//   Array (type 14)   +0x20 element array, +0x24 capacity, +0x28 count
//   AS _global        *(0x829A1588) (AptAS2ResolveNamedVariable_sub_826B1040)
//   MovieClip wrapper +0x18 next sibling, +0x1C parent wrapper, +0x20 display instance D
//   display instance  D+4 -> state S, D+8 flags (bits 26.. = char type: 5 sprite, 9 movie,
//                     2 text, 1/10 shape; 0x3C000000 = skip), D+28 display list (head**)
//   state S           S+4 char def, S+8 matrix* (a,b,c,d,tx,ty; null = identity),
//                     S+24 bit31 = _visible (getter case 8), S+22 u16 >= 0x8000 = placed,
//                     S+80 text bounds; shape bounds at chardef+16
//   bounds            AptGetCharWorldBounds_sub_8268E750 / AptComputeCharBoundsRecurse_sub_8268E548
// Menu framework (AS in MAIN, docs/MOUSE_CURSOR.md 4): BistroFW_EventHandler.HandleEvent walks
// gSM.mScreens from the END down to index 1 and stops at the first screen whose HandleInput
// returns false; ScrBackground_IG / ScrForeground_IG / Main_Hud (no save slots) / ScrVideoClips
// return true (pass-through). Inputs are dropped while gSM.mInputLockMgr.mLocks is non-empty.
#include "mouse_ui.hpp"

#include "guest_context.hpp"
#include "host_automation.hpp"
#include "mod_library.hpp"   // iter-465: the mod library overlay takes the mouse while open
#include "apt_call.hpp"      // iter-466 controls2: the menus' own methods, called directly
#include "pc_controls.hpp"   // SDL3: relativeMouseOn (relative mode is per window)
#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

namespace {

using Clock = std::chrono::steady_clock;
double nowSec() {
  static const Clock::time_point t0 = Clock::now();
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

bool logOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MOUSE_UI_LOG");
    return e && e[0] == '1';
  }();
  return v;
}

// ── the menu model ──────────────────────────────────────────────────────────────────────
constexpr float kStageW = 1280.0f, kStageH = 720.0f;
constexpr int kMaxItems = 64;

enum ItemKind : uint8_t { kItemButton = 0, kItemSlider = 1, kItemToggle = 2, kItemThumb = 3,
                          kItemIcon = 4,
                          // a value row of a host-built popup page (Dev Tools, port settings):
                          // LEFT / RIGHT adjust it in place (markValueRows)
                          kItemValue = 5 };
enum NavKind : uint8_t {
  kNavList = 0,      // CafeFW_FocusManager: UP = previous, DOWN = next, wraps
  kNavRow = 1,       // IconMenu: LEFT / RIGHT, no wrap
  kNavGrid = 2,      // ThumbViewer: LEFT / RIGHT +-1, UP / DOWN +-cols
};

struct RectF {
  float x0 = 3.0e38f, y0 = 3.0e38f, x1 = -3.0e38f, y1 = -3.0e38f;
  bool empty() const { return x1 < x0 || y1 < y0; }
  bool contains(float x, float y) const { return !empty() && x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

struct MenuItem {
  RectF r;                        // stage coords
  uint8_t kind = kItemButton;
  bool enabled = false;
  int index = -1;                 // the game's own index (focus manager slot, icon, thumb)
  uint32_t obj = 0;               // the item's AS object (focus-manager items: slider / toggle)
  // Slider / toggle parts.
  RectF leftArrow, rightArrow, track;
  float value = 0, vmin = 0, vmax = 0, step = 1;
  bool sliderValid = false;
};

// Lane E E5 (RELEASE_PLAN step 5): a clickable entry of the target screen's button-hint bar ("A SELECT  B BACK"):
// the entry's stage rect (its icon and its label) and the key its button sends through the game's router, from the
// icon name the custom front end keeps on the ButtonIcon (tools/apt/mods/custom_frontend/65_buttonicon_id.patch).
struct HintHit {
  RectF r;
  int key = 0;          // the AS key code (hintIconKey); entries with no known button are not kept
  uint32_t label = 0;   // lane E E5b: its LabelText (a PartText), for the hover glow
};
constexpr int kMaxHints = 8;

struct MenuModel {
  bool valid = false;             // a snapshot was taken
  bool active = false;            // an interactive menu surface takes input
  bool clickAnywhere = false;     // the title's PRESS START: any click = START
  bool locked = false;            // gSM.mInputLockMgr holds a lock (inputs are dropped)
  int n = 0;
  MenuItem items[kMaxItems];
  int selected = -1;              // the game's index of the focused item
  int count = 0;                  // list: m_nInputs (wrap modulus); grid: columns
  uint8_t nav = kNavList;
  uint32_t navObj = 0;            // the object whose methods move the focus (focus manager,
                                  // IconMenu, ThumbViewer)
  char what[64] = {0};
  double t = 0;
  uint64_t seq = 0;
  uint32_t screen = 0;            // lane E E1: the target screen's Screen object (0 = none)
  int nHints = 0;                 // lane E E5: the clickable hints of that screen (fillHintHits)
  HintHit hints[kMaxHints];
  bool hintsUnknown = false;      // lane E E5: a read of the snapshot was refused, so no hint is published
};

struct MouseState {
  float wx = 0, wy = 0;           // window coords
  int winW = 1280, winH = 720;
  bool inside = false;
  bool lDown = false;
  int wheel = 0;                  // accumulated notches (+ = up)
  int lClicks = 0, rClicks = 0, mClicks = 0;
  double lastClick = -1e9;
  bool awake = false;             // the mouse is the active input device
  uint64_t moveSeq = 0;           // real motion events (hover acts only on these)
  double lastMove = -1e9;         // nowSec() of the last real motion
  bool skipMotion = true;         // the next motion follows a window enter / focus gain
  float accum = 0;                // movement since the cursor fell asleep (px)
};

std::mutex g_mu;
MouseState g_mouse;
MenuModel g_model;
std::atomic<bool> g_wantSnapshot{false};
std::atomic<uint64_t> g_snapSeq{0};
// Harness mouse feed (SR_MOUSE_FEED): real mouse/window events are ignored and the window
// counts as focused for the mouse while it is set.
std::atomic<bool> g_feedActive{false};
// SDL relative mouse mode is on: the mouse belongs to mouse-look (the PC controls lane,
// docs/PC_CONTROLS.md 4; hand-off in docs/MOUSE_CURSOR.md 6), so the cursor ignores it.
std::atomic<bool> g_relative{false};
// iter-464 bgruns input isolation (kernel/host_automation.hpp): the REAL mouse counts only where
// real input is allowed - players always, automated runs only with SR_HARNESS_REAL_INPUT=1. An
// automated run drives the cursor through the harness feed instead.
bool realMouseAllowed() { return automation::policy().realInput; }
bool focusedForMouse() {
  if (g_relative.load(std::memory_order_relaxed)) return false;
  return g_feedActive.load(std::memory_order_relaxed) ||
         (realMouseAllowed() && hostWindowFocused());
}

// ── guest reads (fault-free) ────────────────────────────────────────────────────────────
struct Reader {
  const uint8_t* (*map)(void* user, uint32_t va, uint32_t n);
  void* user;
  int budget = 60000;             // reads per snapshot (garbage pointers cannot loop forever)
  // Pages already proven readable in this snapshot (direct-mapped by page number), so the
  // readability predicate runs once per 4 KB page, not once per read.
  uint32_t okPage[64] = {};
  const uint8_t* okBase[64] = {};
  // Lane E E1 (Astra r2-r4): every read of this snapshot refused for a reason other than budget, from the menu scan
  // on: an address in the first 64 KB (null included), a span past 4 GB, or one the map does not cover. The census
  // logs only snapshots with none (censusSnapshotComplete). Every caller tests a pointer for null before it reads
  // through it, so a read there is a garbage pointer or one that wrapped: nothing the reader follows lives in the
  // first 64 KB (guest allocations are 64 KB aligned and never at 0) and every offset it adds is below 64 KB, so
  // base + offset past 4 GB always lands there (Astra r4: 0xFFFFFFF8 + 8 = 0). Not counted: the reader's own
  // whole-page probe below, which falls back to the exact span (the one deliberate optional probe).
  int refused = 0;
  uint32_t firstRefused = 0;
  void refuse(uint32_t va) {
    if (refused++ == 0) firstRefused = va;
  }

  const uint8_t* at(uint32_t va, uint32_t n) {
    if (--budget < 0) return nullptr;
    if (va < 0x10000u || va > 0xFFFFFFFFu - n) {
      refuse(va);
      return nullptr;
    }
    const uint32_t pg = va >> 12;
    if ((va + n - 1) >> 12 == pg) {
      const uint32_t k = pg & 63u;
      if (okPage[k] == pg + 1 && okBase[k]) return okBase[k] + (va & 0xFFFu);
      const uint8_t* p = map(user, pg << 12, 0x1000);
      if (!p) return exact(va, n);  // a partly readable page: ask for the exact span
      okPage[k] = pg + 1;
      okBase[k] = p;
      return p + (va & 0xFFFu);
    }
    return exact(va, n);
  }
  const uint8_t* exact(uint32_t va, uint32_t n) {
    const uint8_t* p = map(user, va, n);
    if (!p) refuse(va);
    return p;
  }
  bool u32(uint32_t va, uint32_t* o) {
    const uint8_t* p = at(va, 4);
    if (!p) return false;
    *o = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    return true;
  }
  uint32_t u32or0(uint32_t va) { uint32_t v = 0; return u32(va, &v) ? v : 0; }
  bool u16(uint32_t va, uint16_t* o) {
    const uint8_t* p = at(va, 2);
    if (!p) return false;
    *o = uint16_t((p[0] << 8) | p[1]);
    return true;
  }
  bool f32(uint32_t va, float* o) {
    uint32_t v;
    if (!u32(va, &v)) return false;
    std::memcpy(o, &v, 4);
    return std::isfinite(*o);
  }
  bool strEqCI(uint32_t va, const char* s) {
    for (size_t i = 0; i < 128; ++i) {
      const uint8_t* p = at(va + uint32_t(i), 1);
      if (!p) return false;
      char a = char(*p), b = s[i];
      if (a >= 'A' && a <= 'Z') a = char(a + 32);
      if (b >= 'A' && b <= 'Z') b = char(b + 32);
      if (a != b) return false;
      if (a == 0) return true;
    }
    return false;
  }
  std::string cstr(uint32_t va, size_t max = 63) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
      const uint8_t* p = at(va + uint32_t(i), 1);
      if (!p || !*p) break;
      s.push_back(char(*p));
    }
    return s;
  }
};

constexpr uint32_t kTString = 1, kTBool = 5, kTNumber = 6, kTInt = 7, kTClip = 12,
                   kTArray = 14, kTGlobal = 17, kTObject = 19;
constexpr uint32_t kClipVtable = 0x82086FF0u;
constexpr uint32_t kDeletedName = 0x829311B4u;
constexpr uint32_t kGlobalHolder = 0x829A1588u;

bool asType(Reader& r, uint32_t o, uint32_t* type) {
  uint32_t f;
  if (!o || !r.u32(o + 4, &f)) return false;
  if (((f >> 27) & 1u) == 0) return false;
  *type = f & 0x7Fu;
  return true;
}

bool isClip(Reader& r, uint32_t o) {
  uint32_t t, vt;
  return asType(r, o, &t) && t == kTClip && r.u32(o, &vt) && vt == kClipVtable;
}

uint32_t propTable(Reader& r, uint32_t o) {
  uint32_t t;
  if (!asType(r, o, &t)) return 0;
  if (t == kTClip) {
    if (!isClip(r, o)) return 0;
    const uint32_t inst = r.u32or0(o + 0x20);
    return inst ? r.u32or0(inst + 12) : 0;
  }
  return o + 8;
}

uint32_t ownProp(Reader& r, uint32_t tbl, const char* name) {
  uint32_t cap = 0, ents = 0;
  if (!tbl || !r.u32(tbl, &cap) || !r.u32(tbl + 4, &ents)) return 0;
  if (cap == 0 || cap > 4096 || (cap & (cap - 1)) || !ents) return 0;
  const uint16_t h = mouseUiAptHash16(name);
  // The runtime probes a window around hash & (cap-1); a full scan finds the same entry.
  for (uint32_t i = 0; i < cap; ++i) {
    uint32_t nm = 0;
    if (!r.u32(ents + 8 * i, &nm) || !nm || nm == kDeletedName) continue;
    uint16_t nh = 0;
    if (!r.u16(nm + 6, &nh)) continue;
    if (nh != 0 && nh != h) continue;   // the hash is cached lazily (0 = not computed yet)
    if (!r.strEqCI(nm + 8, name)) continue;
    return r.u32or0(ents + 8 * i + 4);
  }
  return 0;
}

// Property through the __proto__ chain.
uint32_t prop(Reader& r, uint32_t o, const char* name) {
  uint32_t tbl = propTable(r, o);
  for (int depth = 0; tbl && depth < 8; ++depth) {
    if (const uint32_t v = ownProp(r, tbl, name)) return v;
    const uint32_t proto = r.u32or0(tbl + 8);
    tbl = proto ? propTable(r, proto) : 0;
  }
  return 0;
}

uint32_t protoOf(Reader& r, uint32_t o) {
  const uint32_t tbl = propTable(r, o);
  return tbl ? r.u32or0(tbl + 8) : 0;
}

// Is `proto` on o's __proto__ chain?
bool hasProto(Reader& r, uint32_t o, uint32_t proto) {
  if (!proto) return false;
  uint32_t p = protoOf(r, o);
  for (int i = 0; p && i < 8; ++i, p = protoOf(r, p))
    if (p == proto) return true;
  return false;
}

// _global.<className>.prototype (property-table +12 of the function object).
uint32_t classProto(Reader& r, uint32_t G, const char* className) {
  const uint32_t fn = prop(r, G, className);
  const uint32_t tbl = fn ? propTable(r, fn) : 0;
  return tbl ? r.u32or0(tbl + 12) : 0;
}

bool asNumber(Reader& r, uint32_t v, double* out) {
  uint32_t t;
  if (!asType(r, v, &t)) return false;
  if (t == kTInt) {
    uint32_t x;
    if (!r.u32(v + 8, &x)) return false;
    *out = double(int32_t(x));
    return true;
  }
  if (t == kTNumber) {
    float f;
    if (!r.f32(v + 8, &f)) return false;
    *out = f;
    return true;
  }
  if (t == kTBool) {
    const uint8_t* p = r.at(v + 8, 1);
    if (!p) return false;
    *out = *p ? 1.0 : 0.0;
    return true;
  }
  return false;
}

bool asBool(Reader& r, uint32_t v, bool dflt) {
  double d;
  return asNumber(r, v, &d) ? d != 0.0 : dflt;
}

std::vector<uint32_t> asArray(Reader& r, uint32_t a, size_t maxN = kMaxItems) {
  std::vector<uint32_t> out;
  uint32_t t;
  if (!asType(r, a, &t) || t != kTArray) return out;
  const uint32_t base = r.u32or0(a + 0x20);
  uint32_t cnt = 0;
  if (!r.u32(a + 0x28, &cnt) || cnt > 4096) return out;
  if (cnt && !base) return out;
  for (uint32_t i = 0; i < cnt && out.size() < maxN; ++i) out.push_back(r.u32or0(base + 4 * i));
  return out;
}

uint32_t arrayCount(Reader& r, uint32_t a) {
  uint32_t t, cnt = 0;
  if (!asType(r, a, &t) || t != kTArray || !r.u32(a + 0x28, &cnt)) return 0;
  return cnt;
}

// ── display geometry ────────────────────────────────────────────────────────────────────
struct Mat { float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0; };
// A·B: B applied first (AptMatrixConcat_sub_82684F38).
Mat mul(const Mat& A, const Mat& B) {
  Mat o;
  o.a = A.a * B.a + A.c * B.b;
  o.b = A.b * B.a + A.d * B.b;
  o.c = A.a * B.c + A.c * B.d;
  o.d = A.b * B.c + A.d * B.d;
  o.tx = A.a * B.tx + A.c * B.ty + A.tx;
  o.ty = A.b * B.tx + A.d * B.ty + A.ty;
  return o;
}

bool clipState(Reader& r, uint32_t w, uint32_t* inst, uint32_t* st) {
  *inst = r.u32or0(w + 0x20);
  *st = *inst ? r.u32or0(*inst + 4) : 0;
  return *inst && *st;
}

Mat localMat(Reader& r, uint32_t st) {
  Mat m;
  const uint32_t mp = r.u32or0(st + 8);
  if (!mp) return m;
  float v[6];
  for (int i = 0; i < 6; ++i)
    if (!r.f32(mp + 4 * i, &v[i])) return Mat{};
  m.a = v[0]; m.b = v[1]; m.c = v[2]; m.d = v[3]; m.tx = v[4]; m.ty = v[5];
  return m;
}

// A clip state's matrix as stored is usable: none (identity) or six finite floats. localMat reads a refused or
// non-finite one as identity, which would put a clickable rect where the hint is not (E3-E6 r5).
bool matReadable(Reader& r, uint32_t st) {
  const uint32_t mp = r.u32or0(st + 8);
  float v;
  for (int i = 0; mp && i < 6; ++i)
    if (!r.f32(mp + 4 * i, &v)) return false;
  return true;
}

bool clipVisible(Reader& r, uint32_t w) {
  uint32_t inst, st, f = 0;
  if (!clipState(r, w, &inst, &st) || !r.u32(st + 24, &f)) return false;
  return (f >> 31) != 0;
}

bool chainVisible(Reader& r, uint32_t w) {
  for (int i = 0; w && i < 64; ++i) {
    if (!clipVisible(r, w)) return false;
    w = r.u32or0(w + 0x1C);
  }
  return true;
}

bool isDescendant(Reader& r, uint32_t w, uint32_t ancestor) {
  for (int i = 0; w && i < 64; ++i) {
    if (w == ancestor) return true;
    w = r.u32or0(w + 0x1C);
  }
  return false;
}

void addRect(RectF* o, const Mat& M, float rx0, float ry0, float rx1, float ry1) {
  const float xs[4] = {rx0, rx1, rx0, rx1}, ys[4] = {ry0, ry0, ry1, ry1};
  for (int k = 0; k < 4; ++k) {
    const float x = M.a * xs[k] + M.c * ys[k] + M.tx;
    const float y = M.b * xs[k] + M.d * ys[k] + M.ty;
    o->x0 = std::min(o->x0, x); o->y0 = std::min(o->y0, y);
    o->x1 = std::max(o->x1, x); o->y1 = std::max(o->y1, y);
  }
}

// AptComputeCharBoundsRecurse_sub_8268E548, visible geometry only. selfLocal: in w's own space (its matrix not applied;
// hintExtent). badMat (when given) is set when a clip that contributes has a matrix that cannot be read (localMat then
// substitutes identity; hintExtent withholds such a hint: E3-E6 r6).
void boundsRec(Reader& r, uint32_t w, const Mat& parent, RectF* out, int depth, bool selfLocal = false,
               bool* badMat = nullptr) {
  if (depth > 24) return;
  uint32_t inst, st;
  if (!clipState(r, w, &inst, &st)) return;
  uint32_t fl = 0, vis = 0;
  if (!r.u32(inst + 8, &fl) || (fl & 0xFC000000u) == 0x3C000000u) return;
  if (!r.u32(st + 24, &vis) || (vis >> 31) == 0) return;
  if (badMat && !selfLocal && !matReadable(r, st)) *badMat = true;
  const Mat M = selfLocal ? parent : mul(parent, localMat(r, st));
  const int t = int32_t(fl) >> 26;
  if (t == 5 || t == 9) {
    const uint32_t listHead = r.u32or0(inst + 28);
    uint32_t c = listHead ? r.u32or0(listHead) : 0;
    for (int n = 0; c && n < 512; ++n) {
      uint32_t cf = 0, cfl = 0;
      uint16_t placed = 0;
      const uint32_t ci = r.u32or0(c + 0x20);
      const uint32_t cst = ci ? r.u32or0(ci + 4) : 0;
      if (r.u32(c + 4, &cf) && ((cf >> 27) & 1u) && ci && cst && r.u32(ci + 8, &cfl) &&
          (cfl & 0xFC000000u) != 0x3C000000u && r.u16(cst + 22, &placed) && placed >= 0x8000u)
        boundsRec(r, c, M, out, depth + 1, false, badMat);
      c = r.u32or0(c + 0x18);
    }
  } else if (t == 2) {
    float b[4];
    for (int k = 0; k < 4; ++k)
      if (!r.f32(st + 80 + 4 * k, &b[k])) return;
    addRect(out, M, b[0], b[1], b[2], b[3]);
  } else if (t != 8) {
    const uint32_t cd = r.u32or0(st + 4);
    uint32_t ct = 0;
    if (!cd || !r.u32(cd, &ct) || (ct != 1 && ct != 10)) return;
    float b[4];
    for (int k = 0; k < 4; ++k)
      if (!r.f32(cd + 16 + 4 * k, &b[k])) return;
    addRect(out, M, b[0], b[1], b[2], b[3]);
  }
}

// The parent's world matrix, root first: world(child) = world(parent)·M(child), the order the
// renderer composes (boundsRec above uses the same order for children).
Mat parentWorld(Reader& r, uint32_t w) {
  uint32_t chain[64];
  int n = 0;
  for (uint32_t p = r.u32or0(w + 0x1C); p && n < 64; p = r.u32or0(p + 0x1C)) chain[n++] = p;
  Mat W;
  for (int i = n - 1; i >= 0; --i) {
    uint32_t inst, st;
    if (!clipState(r, chain[i], &inst, &st)) return Mat{};
    W = mul(W, localMat(r, st));
  }
  return W;
}

RectF worldBounds(Reader& r, uint32_t w) {
  RectF out;
  if (isClip(r, w)) boundsRec(r, w, parentWorld(r, w), &out, 0);
  return out;
}

RectF clipRectIfVisible(Reader& r, uint32_t w) {
  return (w && isClip(r, w) && chainVisible(r, w)) ? worldBounds(r, w) : RectF{};
}

const uint8_t* guestMap(void*, uint32_t va, uint32_t n) {
  if (!isGuestVAInRange(va) || !isGuestVAInRange(va + n - 1)) return nullptr;
  const uint8_t* p = guestVAToHost(va);
  return (p && hostRangeReadableCached(p, n)) ? p : nullptr;
}

// ── surface builders ────────────────────────────────────────────────────────────────────
struct Classes {
  uint32_t bg = 0, fg = 0, hud = 0, video = 0, slider = 0, toggle = 0;
};

// A focus-manager list: items in m_arrItems, selection m_selectedItemIdx, UP/DOWN with wrap.
// `under` (optional) keeps only items inside that clip (gFM keeps stale entries of earlier
// screens: clearAllItems is never called on it).
bool buildFromFocusManager(Reader& r, const Classes& C, uint32_t fm, uint32_t under,
                           MenuModel* m) {
  const uint32_t arr = prop(r, fm, "m_arrItems");
  const std::vector<uint32_t> items = asArray(r, arr, kMaxItems);
  double sel = -1, nInputs = 0;
  asNumber(r, prop(r, fm, "m_selectedItemIdx"), &sel);
  asNumber(r, prop(r, fm, "m_nInputs"), &nInputs);
  m->n = 0;
  int usable = 0;
  for (size_t i = 0; i < items.size() && m->n < kMaxItems; ++i) {
    const uint32_t w = items[i];
    if (!w || !isClip(r, w) || (under && !isDescendant(r, w, under))) continue;
    if (!chainVisible(r, w)) continue;
    MenuItem it;
    it.index = int(i);
    it.obj = w;
    it.r = worldBounds(r, w);
    it.enabled = !it.r.empty() && asBool(r, prop(r, w, "m_bEnabled"), true) &&
                 asBool(r, prop(r, w, "m_bVisible"), true);
    if (hasProto(r, w, C.slider)) {
      it.kind = kItemSlider;
      it.leftArrow = clipRectIfVisible(r, prop(r, w, "mLeftArrow"));
      it.rightArrow = clipRectIfVisible(r, prop(r, w, "mRightArrow"));
      const RectF s1 = clipRectIfVisible(r, prop(r, w, "mSection1"));
      const RectF s10 = clipRectIfVisible(r, prop(r, w, "mSection10"));
      double v = 0, lo = 0, hi = 0, st = 1;
      if (!s1.empty() && !s10.empty() && asNumber(r, prop(r, w, "mCurrentValue"), &v) &&
          asNumber(r, prop(r, w, "mMinValue"), &lo) && asNumber(r, prop(r, w, "mMaxValue"), &hi) &&
          asNumber(r, prop(r, w, "mStepValue"), &st) && hi > lo && st > 0) {
        it.track.x0 = s1.x0; it.track.x1 = s10.x1;
        it.track.y0 = std::min(s1.y0, s10.y0); it.track.y1 = std::max(s1.y1, s10.y1);
        it.value = float(v); it.vmin = float(lo); it.vmax = float(hi); it.step = float(st);
        it.sliderValid = true;
      }
    } else if (hasProto(r, w, C.toggle)) {
      it.kind = kItemToggle;
      it.leftArrow = clipRectIfVisible(r, prop(r, w, "mLeftArrow"));
      it.rightArrow = clipRectIfVisible(r, prop(r, w, "mRightArrow"));
    }
    if (it.enabled) usable++;
    m->items[m->n++] = it;
  }
  m->selected = int(sel);
  m->count = std::max<int>(int(nInputs), int(items.size()));
  m->nav = kNavList;
  m->navObj = fm;
  return usable > 0;
}

// IconMenu (Fortress / Main menu / Bonus / Control room): mIconHolder.icon_<i>,
// mSelectedIndex, LEFT / RIGHT without wrap (IconMenu Next/Previous, MAIN 0x2aa6 / 0x2b65).
bool buildFromIconMenu(Reader& r, uint32_t menu, MenuModel* m) {
  const uint32_t holder = prop(r, menu, "mIconHolder");
  double sel = -1, nVis = 0;
  if (!holder || !asNumber(r, prop(r, menu, "mSelectedIndex"), &sel)) return false;
  asNumber(r, prop(r, menu, "mVisibleIcons"), &nVis);
  m->n = 0;
  int usable = 0;
  for (int i = 0; i < kMaxItems && i < 32; ++i) {
    char nm[16];
    std::snprintf(nm, sizeof nm, "icon_%d", i);
    const uint32_t w = prop(r, holder, nm);
    if (!w) break;
    if (nVis > 0 && i >= int(nVis)) break;
    MenuItem it;
    it.index = i;
    it.kind = kItemIcon;
    it.r = clipRectIfVisible(r, w);
    it.enabled = !it.r.empty();
    if (it.enabled) usable++;
    m->items[m->n++] = it;
  }
  m->selected = int(sel);
  m->count = m->n;
  m->nav = kNavRow;
  m->navObj = menu;
  return usable > 0;
}

// ThumbViewer (Costumes / Trophies / Concept art / Video clips): mThumbsHolder.ThumbClip<k>,
// mActiveThumb, 4-way MoveHighlight on a grid of ThumbViewer.mColumns (a class static).
bool buildFromThumbViewer(Reader& r, uint32_t G, uint32_t tv, MenuModel* m) {
  const uint32_t holder = prop(r, tv, "mThumbsHolder");
  double sel = -1, cols = 0;
  if (!holder || !asNumber(r, prop(r, tv, "mActiveThumb"), &sel)) return false;
  const uint32_t cls = prop(r, G, "ThumbViewer");
  if (!cls || !asNumber(r, prop(r, cls, "mColumns"), &cols) || cols < 1) return false;
  const uint32_t clips = prop(r, tv, "mThumbClips");
  const uint32_t nThumbs = arrayCount(r, clips);
  m->n = 0;
  int usable = 0;
  for (uint32_t k = 0; k < nThumbs && m->n < kMaxItems && k < 512; ++k) {
    char nm[24];
    std::snprintf(nm, sizeof nm, "ThumbClip%u", k);
    const uint32_t w = prop(r, holder, nm);
    if (!w) continue;
    MenuItem it;
    it.index = int(k);
    it.kind = kItemThumb;
    it.r = clipRectIfVisible(r, w);
    // Only thumbs on screen (the holder scrolls under a mask; off-screen rows are clipped).
    it.enabled = !it.r.empty() && it.r.y1 > 0 && it.r.y0 < kStageH;
    if (!it.enabled) continue;
    usable++;
    m->items[m->n++] = it;
  }
  m->selected = int(sel);
  m->count = int(cols);
  m->nav = kNavGrid;
  m->navObj = tv;
  return usable > 0;
}

// Host-built pages shown in the game's popup (the Dev Tools pages and any port settings page
// built the same way: docs/CUSTOM_FRONTEND.md 5.7, devtools_ui.aptasm). The popup's callback
// context (ScrGenPop `mParams.context`, alive until the popup's ExitScreen) carries `kinds`,
// an Array indexed by row = the focus-manager index: 1 toggle, 2 slider; LEFT / RIGHT adjust
// those rows in place (SRDevTools.PopupKey). Plain popups (the game's own, the front end's
// panels) have no `kinds`, so nothing changes for them.
int markValueRows(Reader& r, uint32_t screen, MenuModel* m) {
  const uint32_t params = prop(r, screen, "mParams");
  const uint32_t ctx = params ? prop(r, params, "context") : 0;
  const uint32_t kinds = ctx ? prop(r, ctx, "kinds") : 0;
  uint32_t t = 0;
  if (!kinds || !asType(r, kinds, &t) || t != kTArray) return 0;
  const std::vector<uint32_t> ks = asArray(r, kinds, kMaxItems);
  int marked = 0;
  for (int i = 0; i < m->n; ++i) {
    MenuItem& it = m->items[i];
    if (it.kind != kItemButton || it.index < 0) continue;
    uint32_t v = it.index < int(ks.size()) ? ks[it.index] : 0;
    if (!v) {                     // a sparse array keeps a numeric key as a named member
      char nm[12];
      std::snprintf(nm, sizeof nm, "%d", it.index);
      v = ownProp(r, propTable(r, kinds), nm);
    }
    double k = 0;
    if (v && asNumber(r, v, &k) && (k == 1.0 || k == 2.0)) {
      it.kind = kItemValue;
      marked++;
    }
  }
  return marked;
}

uint32_t findPropWith(Reader& r, uint32_t so, const char* const* names, const char* field) {
  for (int i = 0; names[i]; ++i) {
    const uint32_t v = prop(r, so, names[i]);
    if (v && prop(r, v, field)) return v;
  }
  return 0;
}

bool scanMenus(Reader& r, MenuModel* m) {
  const uint32_t G = r.u32or0(kGlobalHolder);
  uint32_t gt = 0;
  if (!asType(r, G, &gt) || gt != kTGlobal) return false;
  Classes C;
  C.bg = classProto(r, G, "ScrBackground_IG");
  C.fg = classProto(r, G, "ScrForeground_IG");
  C.hud = classProto(r, G, "Main_Hud");
  C.video = classProto(r, G, "ScrVideoClips");
  C.slider = classProto(r, G, "Slider");
  C.toggle = classProto(r, G, "Toggle");
  const uint32_t sm = prop(r, G, "gSM");
  if (!sm) return false;
  // Input locks (MAIN 0xeab6: HandleEvent drops every input while one is held).
  const uint32_t ilm = prop(r, sm, "mInputLockMgr");
  m->locked = ilm && arrayCount(r, prop(r, ilm, "mLocks")) > 0;
  const std::vector<uint32_t> screens = asArray(r, prop(r, sm, "mScreens"), 32);
  // HandleEvent's walk: from the END down to index 1; the first screen that does not pass
  // the input through is the one that handles it.
  for (int i = int(screens.size()) - 1; i >= 1; --i) {
    const uint32_t mc = prop(r, screens[i], "mMovieClip");
    const uint32_t so = mc ? prop(r, mc, "Screen") : 0;
    if (!so || !isClip(r, so)) continue;
    const uint32_t slots = prop(r, so, "mSlots");
    const bool slotsUp = slots && isClip(r, slots) && chainVisible(r, slots);
    if (hasProto(r, so, C.bg) || hasProto(r, so, C.fg) || hasProto(r, so, C.video)) continue;
    if (hasProto(r, so, C.hud) && !slotsUp) continue;
    // This screen takes the input.
    if (!chainVisible(r, so)) return true;          // hidden (an FMV over the title): no menu
    double lvl = -1;
    asNumber(r, prop(r, screens[i], "mLevel"), &lvl);
    MenuModel tmp;
    tmp.locked = m->locked;
    bool ok = false;
    const char* kind = "";
    if (slotsUp) {
      const uint32_t fm = prop(r, slots, "mFM");
      ok = fm && buildFromFocusManager(r, C, fm, slots, &tmp);
      kind = "slots";
    }
    if (!ok) {
      if (const uint32_t fm = prop(r, so, "mFM")) {
        ok = buildFromFocusManager(r, C, fm, 0, &tmp);
        kind = "popup";
        if (ok && markValueRows(r, so, &tmp) > 0) kind = "page";
      }
    }
    if (!ok) {
      static const char* const kIconNames[] = {"mIconMenu", "mMenu", "mIcons", nullptr};
      if (const uint32_t im = findPropWith(r, so, kIconNames, "mIconHolder")) {
        ok = buildFromIconMenu(r, im, &tmp);
        kind = "iconmenu";
      }
    }
    if (!ok) {
      if (const uint32_t tv = prop(r, so, "mThumbViewer")) {
        ok = buildFromThumbViewer(r, G, tv, &tmp);
        kind = "thumbs";
      }
    }
    if (!ok) {
      if (const uint32_t gfm = prop(r, G, "gFM")) {
        ok = buildFromFocusManager(r, C, gfm, so, &tmp);
        kind = "gFM";
      }
    }
    if (!ok && prop(r, so, "readyForInput")) {
      // The title (ScrPressStart.HandleInput: START or CROSS once readyForInput).
      tmp.clickAnywhere = asBool(r, prop(r, so, "readyForInput"), false);
      ok = tmp.clickAnywhere;
      kind = "title";
    }
    if (!ok) kind = "screen";          // a scroll / back-only screen (lists, map, credits)
    *m = tmp;
    m->active = true;
    m->screen = so;
    std::snprintf(m->what, sizeof m->what, "%s@L%d", kind, int(lvl));
    return true;
  }
  return true;   // valid snapshot, nothing interactive (gameplay, cutscenes)
}

// ── lane E (RELEASE_PLAN step 5, E1): a screen's button-hint bar ─────────────────────────
// The "A SELECT / B BACK" bar (data/ui_ast_extracted BUTTONHINTS + MAIN, dumped by tools/apt/apt2txt.py):
// ButtonHints.ShowHints(label1, icon1, ...) -> AddHint(label, icon) attaches "Hint<mHintCount>" (ButtonHint_id) whose
// "Icon" is a ButtonIcon and whose "LabelText" is a PartText (SetText: mText). A DisplayWell (the popup chrome)
// forwards ShowHints to its mHintHolder; ScrGenPop calls this.mDisplayWell.ShowHints(Generic_SELECT, "A") only - it
// loads Generic_BACK and never shows it, whatever canCancel says.
// The icon is NOT readable from AS state: ButtonIcon.SetIcon(icon) passes mFilePath ('../common/ButtonHints/', a
// prototype constant) + platform + '/Hint' + icon + '.swf' straight to mIconHolder.loadMovie and stores nothing
// (dump_BUTTONICON; Astra E1 r1), so the census names a hint by its label only.
struct HintEntry {
  RectF r;                 // stage coords (empty when hidden): the hint clip's world bounds (the census prints them)
  RectF hit;               // E3-E6 r4: the clickable extent (hintExtent; empty = not clickable)
  bool visible = false;
  char label[40] = {0};    // the label as set (localized)
  char icon[16] = {0};     // lane E E5: ButtonIcon.srIcon ("A", "B", ...; custom front end only), "" = unknown
  uint32_t labelObj = 0;   // lane E E5b: the LabelText object (the label's own text was read through it)
};
struct HintBar {
  int n = 0;
  HintEntry h[12];
};

std::string asString(Reader& r, uint32_t v) {
  uint32_t t;
  if (!asType(r, v, &t) || t != kTString) return {};
  const uint32_t so = r.u32or0(v + 8);
  return so ? r.cstr(so + 8, 95) : std::string();
}

// E3-E6 r4 (the PC mouse roll of 2026-10-23): a hint's clickable extent. Its world bounds span the label field's whole
// AUTHORED width (366 stage px for every hint of the popup bar), so the hints of one bar overlapped (BACK 708..1075
// over SELECT 817..1183) and a click on BACK's centre landed on SELECT. ButtonHints.AddHint (MAIN's class, the only
// layout every bar uses) lays a hint out as: LabelText.SetAutoSize("left") - the text starts at the label's left -
// then Icon._x = LabelText.GetWidth() + 5 and the hint is Icon._x + 32 wide (the next hint to the left ends
// mHintSpacing before it). So the extent is the hint's bounds IN ITS OWN SPACE cut at the hint-local x Icon._x + 32,
// then put through the hint's world matrix (E3-E6 r5, Astra r4 P2: cutting the world box on x alone kept a rotated
// hint's whole authored width on y and kept the wrong side of a reflected one) - the game's own measure of the
// rendered label, read from its icon's position. Empty (not clickable) when the hint is hidden, the Icon is not a
// clip or its state cannot be read (a refused read also withholds every hint), or the cut leaves nothing.
RectF hintExtent(Reader& r, uint32_t hint, const RectF& bounds) {
  RectF out;
  const uint32_t icon = prop(r, hint, "Icon");
  uint32_t hi = 0, hs = 0, ii = 0, is = 0;
  if (bounds.empty() || !icon || !isClip(r, icon) || !clipState(r, hint, &hi, &hs) || !clipState(r, icon, &ii, &is))
    return out;
  if (!matReadable(r, hs) || !matReadable(r, is)) return out;
  int depth = 0;   // every ancestor matrix parentWorld composes (the same walk: at most 64, a stateless one ends it)
  for (uint32_t p = r.u32or0(hint + 0x1C); p && depth < 64; p = r.u32or0(p + 0x1C), ++depth) {
    uint32_t pi = 0, ps = 0;
    if (!clipState(r, p, &pi, &ps)) break;
    if (!matReadable(r, ps)) return out;
  }
  RectF local;
  bool badMat = false;
  boundsRec(r, hint, Mat{}, &local, 0, /*selfLocal=*/true, &badMat);
  const float right = localMat(r, is).tx + 32.0f;
  const float x1 = std::min(local.x1, right);
  if (badMat || local.empty() || !std::isfinite(right) || !(x1 > local.x0) || !(local.y1 > local.y0)) return out;
  // The world transform must keep the cut rect an axis-aligned rect - a scale, a reflection or a quarter turn - and
  // be nonsingular (E3-E6 r6, Astra r5 P2: a = b = c = d = 1 maps it to a diagonal line whose box would take clicks
  // off the line; any other rotation's box is larger than the rect). The game never turns a hint bar.
  const Mat W = mul(parentWorld(r, hint), localMat(r, hs));
  // within 1e-3 of the matrix's scale: float noise of a composed chain, never a visible turn (well under a pixel)
  const float sc = std::fabs(W.a) + std::fabs(W.b) + std::fabs(W.c) + std::fabs(W.d);
  const bool keepsAxes = std::fabs(W.b) + std::fabs(W.c) <= 1e-3f * sc || std::fabs(W.a) + std::fabs(W.d) <= 1e-3f * sc;
  const float det = W.a * W.d - W.b * W.c;
  if (!keepsAxes || !std::isfinite(det) || !(std::fabs(det) > 1e-6f)) return out;
  addRect(&out, W, local.x0, local.y0, x1, local.y1);
  return (out.x1 > out.x0 && out.y1 > out.y0 && std::isfinite(out.x0) && std::isfinite(out.y0) &&
          std::isfinite(out.x1) && std::isfinite(out.y1)) ? out : RectF{};
}

void readHintHolder(Reader& r, uint32_t holder, HintBar* out) {
  double cnt = 0;
  // !(cnt >= 1) also refuses NaN; the count is clamped before it becomes an int (Astra E1 r1)
  if (!holder || !asNumber(r, prop(r, holder, "mHintCount"), &cnt) || !(cnt >= 1)) return;
  const int n = cnt >= 10 ? 10 : int(cnt);
  for (int i = 0; i < n && out->n < 12; ++i) {
    char nm[12];
    std::snprintf(nm, sizeof nm, "Hint%d", i);
    const uint32_t hint = prop(r, holder, nm);
    if (!hint || !isClip(r, hint)) continue;
    HintEntry& e = out->h[out->n];
    e = HintEntry{};
    const uint32_t labelText = prop(r, hint, "LabelText");
    const std::string label = asString(r, prop(r, labelText, "mText"));
    std::snprintf(e.label, sizeof e.label, "%s", label.c_str());
    e.labelObj = labelText;
    const std::string icon = asString(r, prop(r, prop(r, hint, "Icon"), "srIcon"));
    std::snprintf(e.icon, sizeof e.icon, "%s", icon.c_str());
    e.visible = chainVisible(r, hint);
    if (e.visible) {
      e.r = worldBounds(r, hint);
      e.hit = hintExtent(r, hint, e.r);
    }
    out->n++;
  }
}

// Every hint holder of a screen: its mDisplayWell's holder, then any DisplayWell / ButtonHints clip among the screen
// itself and its display-list descendants (3 levels, at most 64 clips; the reader's budget bounds the rest), each
// once. Matched by prototype (one walk per candidate), never by a string property (a full table scan each).
// fn(holder) returns true to stop the walk (lane E E5b r3: the glow's membership check stops at its label).
template <class Fn>
void forEachHintHolder(Reader& r, uint32_t G, uint32_t so, Fn&& fn) {
  if (!so) return;
  const uint32_t dwProto = classProto(r, G, "DisplayWell"), bhProto = classProto(r, G, "ButtonHints");
  uint32_t seen[8];
  int nSeen = 0;
  bool stop = false;
  auto take = [&](uint32_t holder) {
    if (!holder || stop) return;
    for (int i = 0; i < nSeen; ++i)
      if (seen[i] == holder) return;
    if (nSeen < 8) seen[nSeen++] = holder;
    stop = fn(holder);
  };
  auto consider = [&](uint32_t c) {
    if (stop) return;
    if (dwProto && hasProto(r, c, dwProto)) take(prop(r, c, "mHintHolder"));
    else if (bhProto && hasProto(r, c, bhProto)) take(c);
  };
  if (const uint32_t dw = prop(r, so, "mDisplayWell")) consider(dw);
  consider(so);
  uint32_t q[64];
  int depth[64], qh = 0, qt = 0;
  q[qt] = so;
  depth[qt++] = 0;
  while (qh < qt && !stop) {
    const uint32_t w = q[qh];
    const int d = depth[qh++];
    const uint32_t inst = r.u32or0(w + 0x20);
    uint32_t fl = 0;
    if (!inst || !r.u32(inst + 8, &fl)) continue;
    const int t = int32_t(fl) >> 26;
    if (t != 5 && t != 9) continue;   // only sprites / movies have a display list
    const uint32_t head = r.u32or0(inst + 28);
    uint32_t c = head ? r.u32or0(head) : 0;
    for (int n = 0; c && n < 256 && !stop; ++n) {
      if (isClip(r, c)) {
        consider(c);
        if (stop) break;   // nothing is read after the match (Astra E5b r6: a budget that ends there still judges)
        if (d + 1 < 3 && qt < 64) {
          q[qt] = c;
          depth[qt++] = d + 1;
        }
      }
      c = r.u32or0(c + 0x18);
    }
  }
}

// Every hint bar of a screen (forEachHintHolder, the whole walk).
void readHintBars(Reader& r, uint32_t G, uint32_t so, HintBar* out) {
  out->n = 0;
  forEachHintHolder(r, G, so, [&](uint32_t holder) {
    readHintHolder(r, holder, out);
    return false;
  });
}

// ── lane E E1: the menu census (SR_MENU_CENSUS=1; a diagnostic, observe-only, default OFF) ────────────
// One [MENU_CENSUS] line each time the menu on screen changes: the target screen's class (mLinkageName), the
// reader's surface kind and level, a popup's canCancel, the item count and the game's own focused index, and every
// hint (label, stage rect). It reads guest memory only (the fault-free reader above), calls nothing, and runs
// every 3rd Apt pump whether or not the mouse is in use; when SR_MENU_CENSUS is unset it does nothing at all (no
// state, no reads). A snapshot with any refused read (budget, or a refused address: censusSnapshotComplete) is
// incomplete: it is never logged or used to decide what changed (Astra E1 r1-r3); the next complete line reports
// how many were skipped, and [MENU_CENSUS_SKIP] reports a long run of them. docs/MENU_INVENTORY.md is filled from
// these lines.
bool censusOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MENU_CENSUS");
    return e && e[0] == '1';
  }();
  return v;
}

std::string censusLine(Reader& r, const MenuModel& m) {
  std::string screen = "-", cancel = "-", page = "-";
  HintBar hb;
  if (m.active && m.screen) {
    const uint32_t G = r.u32or0(kGlobalHolder);
    screen = asString(r, prop(r, m.screen, "mLinkageName"));
    if (screen.empty()) screen = "?";
    if (const uint32_t params = prop(r, m.screen, "mParams")) {
      double cc = 0;
      if (asNumber(r, prop(r, params, "canCancel"), &cc)) cancel = cc != 0 ? "1" : "0";
      // the popup's own title (ScrGenPop pages differ by it: MAIN MENU, SETTINGS, MORE OPTIONS, a Dev Tools page)
      page = asString(r, prop(r, params, "title"));
      if (page.empty()) page = "-";
      if (page.size() > 40) page.resize(40);
    }
    readHintBars(r, G, m.screen, &hb);
  }
  char buf[320];
  std::snprintf(buf, sizeof buf, "screen=%s page='%s' what=%s cancel=%s items=%d sel=%d locked=%d title=%d hints=%d",
                screen.c_str(), page.c_str(), m.active ? m.what : "-", cancel.c_str(), m.active ? m.n : 0,
                m.active ? m.selected : -1, m.locked ? 1 : 0, m.clickAnywhere ? 1 : 0, hb.n);
  std::string s = buf;
  for (int i = 0; i < hb.n; ++i) {
    const HintEntry& e = hb.h[i];
    char ic[24] = "";
    if (e.icon[0]) std::snprintf(ic, sizeof ic, " icon=%s", e.icon);   // lane E E5 (custom front end only)
    if (e.visible && !e.r.empty())
      std::snprintf(buf, sizeof buf, " ['%s'%s (%.0f,%.0f)-(%.0f,%.0f)]", e.label, ic, e.r.x0, e.r.y0, e.r.x1, e.r.y1);
    else
      std::snprintf(buf, sizeof buf, " ['%s'%s hidden]", e.label, ic);
    s += buf;
  }
  return s;
}

// A census line is complete when no read of the WHOLE snapshot - the menu scan that built the model (screen, focus,
// visibility) and the census's own reads - was refused: not for budget (Reader::at decrements past 0 on a refusal),
// not for an unmapped or invalid address (Reader::refused). A refused read would otherwise read as an absent
// property, sel=-1, "no menu", an empty string or a hidden hint (Astra E1 r1-r3).
bool censusSnapshotComplete(const Reader& r) {
  return r.budget >= 0 && r.refused == 0;
}

// One census snapshot. false = nothing to census (the menu system does not exist yet and no read was refused);
// otherwise *line and *complete - a scan that failed BECAUSE a read was refused is an incomplete snapshot, counted
// and reported like any other (Astra E1 r4), never silently dropped.
bool censusSnapshot(Reader& r, std::string* line, bool* complete) {
  MenuModel m;
  const bool scanned = scanMenus(r, &m);
  if (!scanned && censusSnapshotComplete(r)) return false;
  *line = scanned ? censusLine(r, m) : std::string();
  *complete = scanned && censusSnapshotComplete(r);
  return true;
}

// The census's acceptance rule, one place (censusTick and the tests): an incomplete line is never emitted and never
// becomes the remembered one; a complete line is emitted once per change. prevScreen = the screen of the previously
// EMITTED line (a hint for reading consecutive lines; parents come from the scripted traversal, not from here).
struct CensusState {
  std::string last, lastScreen = "-";
  uint64_t seq = 0, incomplete = 0;
};
bool censusAccept(CensusState& st, bool complete, const std::string& line, std::string* prevScreen) {
  if (!complete) {
    ++st.incomplete;
    return false;
  }
  if (line == st.last) return false;
  st.last = line;
  ++st.seq;
  *prevScreen = st.lastScreen;
  const size_t a = line.find("screen="), b = line.find(' ', a);
  st.lastScreen = a == std::string::npos ? std::string("?")
                                          : line.substr(a + 7, b == std::string::npos ? std::string::npos : b - a - 7);
  return true;
}

void censusTick() {
  if (!censusOn()) return;   // before any census state exists
  static unsigned tick = 0;
  static CensusState st;
  if ((tick++ % 3) != 0) return;
  Reader r{guestMap, nullptr};
  std::string line;
  bool complete = false;
  if (!censusSnapshot(r, &line, &complete)) return;
  std::string prev;
  const uint64_t skipped = st.incomplete;
  if (!censusAccept(st, complete, line, &prev)) {
    // A run of incomplete snapshots is reported at its 1st, 10th, 100th, ... (a census that never completes must be
    // diagnosable from the log). Its own tag: no [MENU_CENSUS] pattern matches it.
    uint64_t p = 1;
    while (p < st.incomplete && p <= st.incomplete / 10) p *= 10;
    if (!complete && p == st.incomplete)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MENU_CENSUS_SKIP] %llu incomplete snapshot(s) since the last line; this one: %d refused read(s), "
                  "the first at 0x%08X, reads_left=%d",
                  (unsigned long long)st.incomplete, r.refused, r.firstRefused, r.budget);
    return;
  }
  st.incomplete = 0;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MENU_CENSUS] #%llu %s prev=%s reads_left=%d incomplete_skipped=%llu",
              (unsigned long long)st.seq, line.c_str(), prev.c_str(), r.budget, (unsigned long long)skipped);
}

// ── the direct calls (iter-466 controls2, USER DECISION 2026-10-06) ─────────────────────
// The cursor no longer presses pad buttons: it calls the menus' own methods (charter 2a). The
// D-pad direction tokens below only name the way the focus has to move (navKey).
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;

// The AS key codes the game's input router takes (BistroFW_EventHandler.HandleEvent, MAIN 0xe94e;
// the Key listener's onKeyDown passes Key.getCode()): docs/MOUSE_CURSOR.md 2.
constexpr int kKeyLeft = 37, kKeyRight = 39, kKeyStart = 301, kKeyCross = 302, kKeyTriangle = 304,
              kKeySquare = 305;
constexpr int kKeySelect = 300, kKeyCircle = 303;   // lane E E5: SELECT (pad BACK), CIRCLE (pad Y)

// Lane E E5: the key a hint's button sends (docs/MOUSE_CURSOR.md 2: CROSS = A, TRIANGLE = B, SQUARE = X, CIRCLE = Y),
// from its ButtonIcon name (case-insensitive); 0 = not a button a click can press (Dpad, sticks, triggers ...).
int hintIconKey(const char* icon) {
  if (!icon || !icon[0]) return 0;
  auto is = [&](const char* s) {
    for (size_t i = 0;; ++i) {
      char a = icon[i], b = s[i];
      if (a >= 'A' && a <= 'Z') a = char(a + 32);
      if (b >= 'A' && b <= 'Z') b = char(b + 32);
      if (a != b) return false;
      if (!a) return true;
    }
  };
  if (is("A")) return kKeyCross;
  if (is("B")) return kKeyTriangle;
  if (is("X")) return kKeySquare;
  if (is("Y")) return kKeyCircle;
  if (is("Start")) return kKeyStart;
  if (is("Select")) return kKeySelect;
  return 0;
}

// Clickable hint extents never overlap when they are the game's own layout (hintExtent); two that still do cannot be
// told apart and are both dropped - no hint is better than the wrong button (nothing is ever trimmed).
void dropAmbiguousHints(MenuModel* m) {
  bool drop[kMaxHints] = {};
  for (int i = 0; i < m->nHints; ++i)
    for (int j = i + 1; j < m->nHints; ++j) {
      const RectF& a = m->hints[i].r;
      const RectF& b = m->hints[j].r;
      if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) drop[i] = drop[j] = true;
    }
  int n = 0;
  for (int i = 0; i < m->nHints; ++i)
    if (!drop[i]) m->hints[n++] = m->hints[i];
  m->nHints = n;
}

// The target screen's clickable hints into m->hints: every visible entry with a clickable extent (hintExtent) and a
// known button; overlapping ones are dropped (dropAmbiguousHints).
void fillHintHits(Reader& r, MenuModel* m) {
  m->nHints = 0;
  if (!m->active || !m->screen) return;
  HintBar hb;
  readHintBars(r, r.u32or0(kGlobalHolder), m->screen, &hb);
  for (int i = 0; i < hb.n && m->nHints < kMaxHints; ++i) {
    const HintEntry& e = hb.h[i];
    const int key = hintIconKey(e.icon);
    if (!e.visible || e.hit.empty() || !key) continue;
    m->hints[m->nHints].r = e.hit;
    m->hints[m->nHints].key = key;
    m->hints[m->nHints].label = e.labelObj;
    m->nHints++;
  }
  dropAmbiguousHints(m);
}

// The mouse's snapshot (takeSnapshot and the tests): the menu scan, then the clickable hints, with ONE reader. The
// hints are published only when no read of the whole snapshot was refused (censusSnapshotComplete: budget, unmapped
// or invalid addresses - partial bounds must never be clickable; Astra E5 r1); otherwise hintsUnknown (logged), and
// a click acts on the rows alone, exactly as before E5 (no click is held: a held click could land on another screen;
// Astra E5 r2).
MenuModel mouseSnapshot(Reader& r) {
  MenuModel m;
  m.valid = scanMenus(r, &m);
  if (m.valid) fillHintHits(r, &m);
  if (!censusSnapshotComplete(r)) {
    m.nHints = 0;
    m.hintsUnknown = true;
  }
  return m;
}

// The key of the hint under the cursor (stage coords), 0 = none.
int hintKeyAt(const MenuModel& m, float sx, float sy) {
  for (int i = 0; i < m.nHints; ++i)
    if (m.hints[i].r.contains(sx, sy)) return m.hints[i].key;
  return 0;
}

// ── lane E E5b (owner Q1 (a), 2026-10-23): the hovered hint glows ─────────────────────────────────────
// The hovered hint's label turns the cursor's gold and back through the game's own PartText.SetFontColor. The GUEST
// owns the glow (Astra E5b r7: no host-side record of a colour or of an object's identity can survive the guest
// freeing a label and reusing its address). The custom front end's PARTTEXT patch (tools/apt/mods/custom_frontend/
// 66_parttext_glow.patch) gives SetFontColor two values only the cursor uses, both above every colour:
//   - kGlowOn: the label keeps its own colour on itself (this.srOwn, once) and shows kGlowColor;
//   - kGlowRestore: the label puts back exactly what IT recorded and forgets it; with no record it changes nothing;
//   - any other value (the game's own SetFontColor) voids the record: the game's colour stands.
// So a restore can only ever undo this cursor's own glow on that very object: a label the game recoloured, or another
// object at the same address, has no record and is left as it is. The host never reads or remembers a colour. Before
// each call it finds the label on a CLEAN path (a live hint label of some screen, labelOnAnyScreen), so it never calls
// a freed object:
//   - an off (a restore) runs before the pump's main call; an on only in a pump with no other call and no input lock
//     (never delays a click, a focus step or a slider; a restore may run during a lock);
//   - the state changes only after the call succeeded. A refused restore keeps the glow and is retried after 2, 4, ...
//     at most 64 pumps, until it is done or the label is gone (no clean path finds it: nothing left to restore);
//   - while it stays hovered, the label must still CARRY the guest's record (srOwn): an object replaced at the same
//     address, or one the game recoloured (the record voided), has none and gets the on again (Astra E5b r8) - under
//     the same clean-path, lock and one-call rules, and without fighting the game: re-sent ons in a row wait 2, 4,
//     ... at most 64 pumps; the record is read on every such pump (only the calls wait), and the wait starts over
//     only once the record has held for kGlowHeldReset (64) pumps in a row - so a label the game keeps recolouring,
//     even every other pump (Astra E5b r9), costs one call every 64 pumps at most;
//   - the glow's reads use their own readers (never a snapshot's budget, refusals or the census); a refused read on
//     the path = "cannot judge": nothing happens that pump. Only hints with a recorded button (srIcon: the custom
//     front end, the same patched movies) are hovered, so the patched SetFontColor is the one called.
constexpr uint32_t kGlowColor = 0xFFC83C;   // what kGlowOn shows (66_parttext_glow.patch's constant)
constexpr int32_t kGlowOn = 0x1000002;      // the cursor's glow (above every 24-bit colour)
constexpr int32_t kGlowRestore = 0x1000001; // the cursor's restore
struct Glow {
  uint32_t label = 0;       // the glowing LabelText (0 = none)
  int fails = 0;            // refused restores in a row
  uint64_t retryAt = 0;     // the glow pump the next restore may run in (after a refused one)
  int reapplies = 0;        // ons re-sent in a row while hovered (the record kept going: a game that recolours it)
  uint64_t reapplyAt = 0;   // the glow pump the next re-sent on may run in
  int held = 0;             // hovered pumps in a row that saw the guest's record (the wait resets at kGlowHeldReset)
};
Glow g_glow;   // the guest thread only (mouseUiAfterAptPump), under g_mu with the rest of the mouse state
enum class Tri : uint8_t { No, Yes, Unknown };

// Does this hint holder list `label` as one of its hints' LabelText? Only what identifies a hint label is read (the
// count, Hint<i>, its LabelText pointer): no text, icon, visibility or geometry (Astra E5b r2).
bool holderHasLabel(Reader& r, uint32_t holder, uint32_t label) {
  double cnt = 0;
  if (!holder || !asNumber(r, prop(r, holder, "mHintCount"), &cnt) || !(cnt >= 1)) return false;
  const int n = cnt >= 10 ? 10 : int(cnt);   // readHintHolder's clamp
  for (int i = 0; i < n; ++i) {
    char nm[12];
    std::snprintf(nm, sizeof nm, "Hint%d", i);
    const uint32_t hint = prop(r, holder, nm);
    if (hint && isClip(r, hint) && prop(r, hint, "LabelText") == label) return true;
  }
  return false;   // a match is judged by its path's reads (labelOnAnyScreen)
}

// Is `label` the LabelText of a hint of any screen's bars (the whole screen stack, the top first: the hovered label
// is usually there)? A Yes needs a CLEAN path: the stack's reads (the global holder, gSM, mScreens) and that screen's
// own walk up to the match, with no refused read and within the budget. Any refusal on the path - an unreadable own
// entry that could override an inherited mHintCount, Hint<i>, LabelText, mDisplayWell or a class lookup - could make a
// prototype's value look like a match (Astra E5b r5). A refusal in ANOTHER screen's walk does not touch this one
// (Astra E5b r3). No needs the whole stack read with no refusal; anything else is Unknown.
int g_glowMatchBudget = -1;   // the reader's budget at the last clean match (the tests: nothing is read after it)
Tri labelOnAnyScreen(Reader& r, uint32_t label) {
  if (!label) return Tri::No;
  const int refused0 = r.refused;
  const uint32_t G = r.u32or0(kGlobalHolder);
  const uint32_t sm = G ? prop(r, G, "gSM") : 0;
  const std::vector<uint32_t> screens = asArray(r, sm ? prop(r, sm, "mScreens") : 0, 32);
  const bool stackClean = r.refused == refused0 && r.budget >= 0;
  bool unsure = false;
  for (size_t i = screens.size(); i-- > 0;) {
    const int pre = r.refused;
    const uint32_t mc = prop(r, screens[i], "mMovieClip");
    const uint32_t so = mc ? prop(r, mc, "Screen") : 0;
    if (!so || !isClip(r, so)) continue;
    bool found = false;
    int atMatch = -1;
    forEachHintHolder(r, G, so, [&](uint32_t holder) {
      found = holderHasLabel(r, holder, label);
      if (found) atMatch = r.budget;
      return found;
    });
    if (!found) continue;
    if (stackClean && r.refused == pre && r.budget >= 0) {
      g_glowMatchBudget = atMatch;
      return Tri::Yes;
    }
    unsure = true;   // a match on a path with a refusal: not judged
  }
  if (unsure || !sm || r.refused != refused0 || r.budget < 0) return Tri::Unknown;
  return Tri::No;
}

// Does `label` carry the guest's glow record (66_parttext_glow.patch: this.srOwn, set by the on, deleted by the restore
// and by any game SetFontColor)? Yes / No from a lookup with no refused read; anything else Unknown (then nothing is
// re-sent).
Tri labelHasGlowRecord(Reader& r, uint32_t label) {
  const int refused0 = r.refused;
  const uint32_t v = label ? prop(r, label, "srOwn") : 0;
  if (r.refused != refused0 || r.budget < 0) return Tri::Unknown;
  return v ? Tri::Yes : Tri::No;
}

enum class GlowAct : uint8_t { None, Off, Drop, On };
// want = the hovered hint's label (0: none, or the hover is not allowed now); standing = labelOnAnyScreen of g.label
// (only meaningful when g.label != 0); allowOn = no other call this pump and no input lock.
GlowAct glowDecide(const Glow& g, uint32_t want, Tri standing, bool allowOn) {
  if (g.label) {
    if (standing == Tri::Unknown) return GlowAct::None;   // cannot judge: keep it for a pump that can
    if (standing == Tri::No) return GlowAct::Drop;        // gone: nothing left to restore, nothing is called
    return want == g.label ? GlowAct::None : GlowAct::Off;
  }
  return want && allowOn ? GlowAct::On : GlowAct::None;
}

// The glow's guest boundary (injected, so the tests drive whole pump sequences): production = this pump's reads on the
// guest map and PartText.SetFontColor(value) through aptcall::callMethod; the tests = a script or the fixture heap.
struct GlowIo {
  void* user = nullptr;
  Tri (*standing)(void* user, uint32_t label) = nullptr;                // labelOnAnyScreen
  Tri (*glowing)(void* user, uint32_t label) = nullptr;                 // labelHasGlowRecord
  bool (*call)(void* user, uint32_t label, int32_t value) = nullptr;    // SetFontColor(value); false = not called
};
struct GlowStep {
  GlowAct act = GlowAct::None;
  bool called = false, ok = false;
  uint32_t obj = 0;
};
// The wait after the n-th refused restore in a row: 2, 4, 8, ... 64 pumps (bounded: a refusing guest costs one
// call every 64 pumps at most; the glow is never given up while its label is still there).
// E5b r13 (Astra E5b r11): the counters themselves saturate at the cap, so no run of refusals or re-applies can
// overflow them (the wait is 64 pumps from the 6th on either way).
constexpr int kGlowBackoffCap = 6;
uint64_t glowBackoff(int fails) {
  return uint64_t(1) << (fails < kGlowBackoffCap ? fails : kGlowBackoffCap);
}
// The record must hold this many hovered pumps in a row before the re-apply wait starts over (a game that recolours
// the label every other pump would otherwise reset it each time: Astra E5b r9).
constexpr int kGlowHeldReset = 64;
static_assert(kGlowHeldReset == 64 && kGlowBackoffCap == 6, "the glow tests pin 64 / 63 and the 2..64 waits by literals");
// One glow pump. g = the glow (the caller's copy of g_glow, committed back after); want = the hovered hint's label
// (glowWanted); allowOn = no other call this pump and no input lock; pump = this glow pump's number.
GlowStep glowStep(Glow& g, uint32_t want, bool allowOn, uint64_t pump, const GlowIo& io) {
  GlowStep s;
  if (!g.label && !want) return s;                                 // the common case: no reads at all
  if (g.label && want != g.label && pump < g.retryAt) return s;    // a refused restore waits (no reads)
  const Tri standing = g.label ? io.standing(io.user, g.label) : Tri::No;
  s.act = glowDecide(g, want, standing, allowOn);
  switch (s.act) {
    case GlowAct::None:
      if (g.label && want == g.label && standing == Tri::Yes) {   // hovered again: wanted, no restore pending
        g.fails = 0;
        g.retryAt = 0;
        // still hovered: does the object there still carry the guest's record? Not (a new object at the same address,
        // or the game recoloured it): the on again, as for a new hover (allowOn: no other call, no lock), with a
        // growing wait between re-sent ons in a row. The record is read every such pump (one read; only the calls
        // wait); the wait starts over once the record has held for kGlowHeldReset pumps in a row.
        if (allowOn && io.glowing) {
          const Tri rec = io.glowing(io.user, g.label);
          if (rec == Tri::Yes && g.held < kGlowHeldReset) ++g.held;   // saturates: never overflows
          if (rec == Tri::Yes && g.held >= kGlowHeldReset) {
            g.reapplies = 0;
            g.reapplyAt = 0;   // already <= pump here (a held run starts after the last on, whose wait is <= 64 =
                               // kGlowHeldReset); cleared for clarity
          }
          if (rec == Tri::No) g.held = 0;
          if (rec == Tri::No && pump >= g.reapplyAt) {
            s.act = GlowAct::On;
            s.obj = g.label;
            s.called = true;
            s.ok = io.call(io.user, g.label, kGlowOn);
            if (g.reapplies < kGlowBackoffCap) ++g.reapplies;   // saturates (E5b r13)
            g.reapplyAt = pump + glowBackoff(g.reapplies);
          }
        }
      }
      return s;
    case GlowAct::Drop:
      s.obj = g.label;
      g = Glow{};
      return s;
    case GlowAct::On:
      if (io.standing(io.user, want) != Tri::Yes) {   // a live hint label on a clean path, or no call
        s.act = GlowAct::None;
        return s;
      }
      s.obj = want;
      s.called = true;
      s.ok = io.call(io.user, want, kGlowOn);
      if (s.ok) g = Glow{want, 0, 0};   // a refused on changes nothing: the next allowed pump tries again
      return s;
    case GlowAct::Off:
      s.obj = g.label;
      s.called = true;
      s.ok = io.call(io.user, g.label, kGlowRestore);
      if (s.ok) {
        g = Glow{};
      } else {   // kept: the label still holds what it recorded
        if (g.fails < kGlowBackoffCap) ++g.fails;   // saturates (E5b r13)
        g.retryAt = pump + glowBackoff(g.fails);
      }
      return s;
  }
  return s;
}

// The hovered hint's label from THIS pump's complete snapshot (0 = none, or the cursor cannot hover now); *locked =
// the game holds an input lock (no new glow then).
uint32_t glowWanted(const MenuModel& m, const MouseState& ms, bool tookSnapshot, bool focused, bool libOpen,
                    bool* locked) {
  *locked = m.locked;
  if (!(tookSnapshot && m.valid && m.active && !m.hintsUnknown && ms.awake && ms.inside && focused && !libOpen))
    return 0;
  float sx, sy;
  mouseUiWindowToStage(ms.wx, ms.wy, ms.winW, ms.winH, &sx, &sy);
  for (int i = 0; i < m.nHints; ++i)
    if (m.hints[i].label && m.hints[i].r.contains(sx, sy)) return m.hints[i].label;
  return 0;
}

enum Action : uint8_t { kActNone = 0, kActA, kActLeft, kActRight, kActSlider };

// What the cursor does next (decided under g_mu, called after it is released).
struct Plan {
  // kStart (lane E E5c): the game's own START through the PC action layer (pccontrols::pulseStart), the title's
  // click; when the action layer is off, gEH.HandleEvent(key) as kEvent
  enum Kind : uint8_t { kNone = 0, kMethod, kEvent, kStart } kind = kNone;
  uint32_t obj = 0;            // kMethod: the target object
  const char* method = "";     // kMethod: its method
  std::string strArg;          // kMethod: one string argument (MoveHighlight), "" = none
  bool hasInt = false;         // kMethod: one integer argument instead (lane E E5b: SetFontColor)
  int32_t intArg = 0;
  int key = 0;                 // kEvent / kStart: gEH.HandleEvent(key, controller) (kStart: the fallback)
  const char* why = "";
};

struct Driver {
  int goal = -1;              // the game index to focus
  Action act = kActNone;      // what to do once it is focused
  int waitPumps = 0;          // after a call: pumps to wait for the game to react
  int lastSelected = -9;
  int stuck = 0;
  int lastHover = -2;
  uint64_t lastMoveSeq = 0;   // hover acts only after real mouse motion
  bool dragging = false;
  int dragIndex = -1;
  float lastValue = -1e9f;    // slider: the value when the last step was called
  int valueWait = 0;
  uint64_t calls = 0;
} g_drv;

// The item under the cursor: of the items whose rect contains it, the one whose centre is
// nearest (popup rows overlap by their highlight bars).
int hoveredSlot(const MenuModel& m, float sx, float sy) {
  int best = -1;
  float bestD = 3.0e38f;
  for (int i = 0; i < m.n; ++i) {
    const MenuItem& it = m.items[i];
    if (!it.enabled || !it.r.contains(sx, sy)) continue;
    const float cx = 0.5f * (it.r.x0 + it.r.x1), cy = 0.5f * (it.r.y0 + it.r.y1);
    const float d = (cx - sx) * (cx - sx) + (cy - sy) * (cy - sy);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

int slotOfIndex(const MenuModel& m, int index) {
  for (int i = 0; i < m.n; ++i)
    if (m.items[i].index == index) return i;
  return -1;
}

// The next D-pad press that moves the game's selection from m.selected toward `goal`.
uint16_t navKey(const MenuModel& m, int goal) {
  const int sel = m.selected;
  switch (m.nav) {
    case kNavRow:
      return goal > sel ? kPadRight : kPadLeft;
    case kNavGrid: {
      const int cols = std::max(1, m.count);
      if (sel < 0) return kPadRight;
      const int gr = goal / cols, gc = goal % cols, sr0 = sel / cols, sc = sel % cols;
      if (gc != sc) return gc > sc ? kPadRight : kPadLeft;
      return gr > sr0 ? kPadDown : kPadUp;
    }
    case kNavList:
    default: {
      const int n = std::max(m.count, 1);
      if (sel < 0 || sel >= n || goal >= n) return goal > sel ? kPadDown : kPadUp;
      const int fwd = (goal - sel + n) % n, bwd = (sel - goal + n) % n;
      return fwd <= bwd ? kPadDown : kPadUp;   // the list wraps (selectNextItem 0x70a2)
    }
  }
}

}  // namespace

// ── public: on/off ──────────────────────────────────────────────────────────────────────
// USER 2026-10-02: no player toggle - the cursor is always available (it hides itself on
// pad/keyboard use and outside menus). SR_MOUSE_CURSOR=0|1 is a developer A/B gate only.
// Automated rolls run the SAME configuration as players (the cursor is on); the bgruns input
// isolation keeps a hand on the desk from steering them (realMouseAllowed), and their mouse
// steps arrive through the harness feed. So "zero impact when untouched" holds by
// construction in every natural roll: no mouse event, no Apt read, no pad bit, no overlay.
bool mouseUiEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MOUSE_CURSOR");
    return !(e && e[0] == '0');
  }();
  return v;
}

// ── public: pure helpers ────────────────────────────────────────────────────────────────
// The game image (a 16:9 frame at any internal resolution) fills the largest 16:9 rectangle
// centred in the output, with bars on the other axis: render/resscale's present fit
// (gfx/d3d12_pm4_shmem.cpp fitRect, aspect kept). A 16:9 window gives the whole client area,
// so the mapping is the plain scale there (today's fixed 1280x720 window: identity).
void mouseUiFitRect(int outW, int outH, float* x, float* y, float* w, float* h) {
  const float W = float(std::max(outW, 1)), H = float(std::max(outH, 1));
  if (W * kStageH <= H * kStageW) {      // width-limited (taller than 16:9): bars top/bottom
    *w = W;
    *h = W * kStageH / kStageW;
  } else {                               // height-limited (wider): bars left/right
    *h = H;
    *w = H * kStageW / kStageH;
  }
  *x = 0.5f * (W - *w);
  *y = 0.5f * (H - *h);
}

void mouseUiWindowToStage(float wx, float wy, int winW, int winH, float* sx, float* sy) {
  float fx, fy, fw, fh;
  mouseUiFitRect(winW, winH, &fx, &fy, &fw, &fh);
  *sx = (wx - fx) * kStageW / fw;
  *sy = (wy - fy) * kStageH / fh;
}

void mouseUiStageToWindow(float sx, float sy, int winW, int winH, float* wx, float* wy) {
  float fx, fy, fw, fh;
  mouseUiFitRect(winW, winH, &fx, &fy, &fw, &fh);
  *wx = fx + sx * fw / kStageW;
  *wy = fy + sy * fh / kStageH;
}

uint16_t mouseUiAptHash16(const char* s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) {
    uint32_t c = uint8_t(*s);
    if (c - 65u <= 25u) c += 32u;
    h = (h ^ c) * 16777619u;
  }
  const uint16_t r = uint16_t(h);
  return r ? r : 17767;
}

// The Slider fills section i (1..n) iff i <= (value - vmin) * n / (vmax - vmin)
// (Slider.UpdateValue, MAIN 0x11832), so a click inside section i means value i: ceil.
int mouseUiSliderSteps(float px, float x0, float x1, float vmin, float vmax, float step,
                       float value) {
  if (!(x1 > x0) || !(vmax > vmin) || !(step > 0)) return 0;
  const float k = std::clamp((px - x0) / (x1 - x0), 0.0f, 1.0f);
  const float nSteps = (vmax - vmin) / step;
  const float target = vmin + std::ceil(k * nSteps - 1e-4f) * step;
  return int(std::lround((std::clamp(target, vmin, vmax) - value) / step));
}

// ── public: SDL events (main thread) ────────────────────────────────────────────────────
namespace {
void onMouseEvent(const SDL_Event& e, int winW, int winH);
}  // namespace

// SDL3: window events are top-level event types (SDL2: SDL_WINDOWEVENT + e.window.event).
static bool isWindowEvent(Uint32 t) {
  return t == SDL_EVENT_WINDOW_MOUSE_ENTER || t == SDL_EVENT_WINDOW_MOUSE_LEAVE ||
         t == SDL_EVENT_WINDOW_FOCUS_GAINED || t == SDL_EVENT_WINDOW_FOCUS_LOST;
}

void mouseUiOnSdlEvent(const SDL_Event& e, int winW, int winH) {
  if (e.type != SDL_EVENT_MOUSE_MOTION && e.type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
      e.type != SDL_EVENT_MOUSE_BUTTON_UP && e.type != SDL_EVENT_MOUSE_WHEEL && !isWindowEvent(e.type))
    return;
  if (!mouseUiEnabled()) return;
  if (g_feedActive.load(std::memory_order_relaxed)) return;   // a harness feed owns the mouse
  if (!realMouseAllowed()) return;      // automated run: the desk mouse never steers it
  // HAND-OFF with mouse-look (docs/MOUSE_CURSOR.md 6): while SDL relative mode is on, the mouse
  // is the camera's, so the cursor sleeps and ignores it (motion, buttons, wheel).
  if (!isWindowEvent(e.type) && pccontrols::relativeMouseOn()) {
    mouseUiSetRelativeMode(true);
    return;
  }
  if (!isWindowEvent(e.type)) mouseUiSetRelativeMode(false);
  onMouseEvent(e, winW, winH);
}

void mouseUiSetRelativeMode(bool on) {
  const bool was = g_relative.exchange(on, std::memory_order_relaxed);
  if (was == on) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (on) {
    // the camera takes the mouse: the cursor sleeps and drops anything it had queued
    g_mouse.awake = false;
    g_mouse.lDown = false;
    g_mouse.lClicks = g_mouse.rClicks = g_mouse.mClicks = 0;
    g_mouse.wheel = 0;
    g_mouse.accum = 0;
  } else {
    // back to the absolute cursor: SDL restores / warps the pointer, which is not a move
    g_mouse.skipMotion = true;
  }
  if (logOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] relative mouse %s (mouse-look %s the mouse)",
                on ? "ON" : "OFF", on ? "owns" : "released");
}

bool mouseUiParseFeedLine(const char* line, MouseFeedCmd* out) {
  if (!line || !out) return false;
  char verb[16] = {0}, arg[16] = {0};
  float x = 0, y = 0;
  *out = MouseFeedCmd{};
  if (std::sscanf(line, " %15s", verb) != 1) return false;
  auto button = [](const char* a) {
    if (std::strcmp(a, "left") == 0) return int(SDL_BUTTON_LEFT);
    if (std::strcmp(a, "middle") == 0) return int(SDL_BUTTON_MIDDLE);
    if (std::strcmp(a, "right") == 0) return int(SDL_BUTTON_RIGHT);
    return 0;
  };
  if (std::strcmp(verb, "move") == 0) {
    if (std::sscanf(line, " %*s %f %f", &x, &y) != 2) return false;
    if (!(x >= -1.0f && x <= kStageW + 1.0f && y >= -1.0f && y <= kStageH + 1.0f)) return false;
    out->kind = 1; out->x = x; out->y = y;
    return true;
  }
  if (std::strcmp(verb, "down") == 0 || std::strcmp(verb, "up") == 0) {
    if (std::sscanf(line, " %*s %15s", arg) != 1) return false;
    const int b = button(arg);
    if (!b) return false;
    out->kind = verb[0] == 'd' ? 2 : 3; out->button = b;
    return true;
  }
  if (std::strcmp(verb, "wheel") == 0) {
    int n = 0;
    if (std::sscanf(line, " %*s %d", &n) != 1 || n == 0 || n < -8 || n > 8) return false;
    out->kind = 4; out->x = float(n);
    return true;
  }
  return false;
}

void mouseUiPollFeed(int winW, int winH) {
  struct Feed { bool init = false; std::string path; long long off = 0; std::string pend;
                float wx = -1, wy = -1; };
  static Feed f;
  if (!f.init) {
    f.init = true;
    const char* p = std::getenv("SR_MOUSE_FEED");
    if (!p || !*p || (p[0] == '0' && p[1] == 0) || !mouseUiEnabled()) return;
    f.path = p;
    g_feedActive.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_mouse.inside = true;
      g_mouse.skipMotion = false;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] harness feed ON: %s (real mouse ignored)",
                f.path.c_str());
  }
  if (f.path.empty()) return;
  std::FILE* fp = std::fopen(f.path.c_str(), "rb");
  if (!fp) return;
  char buf[8192];
  if (_fseeki64(fp, f.off, SEEK_SET) == 0) {
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
      f.pend.append(buf, n);
      f.off += (long long)n;
      if (f.pend.size() > (1u << 20)) f.pend.clear();   // never grow without bound
    }
  }
  std::fclose(fp);
  size_t nl;
  while ((nl = f.pend.find('\n')) != std::string::npos) {
    std::string line = f.pend.substr(0, nl);
    f.pend.erase(0, nl + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    MouseFeedCmd c;
    if (!mouseUiParseFeedLine(line.c_str(), &c)) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MOUSE_UI] feed: bad line '%s'", line.c_str());
      continue;
    }
    if (logOn()) SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] feed: %s", line.c_str());
    SDL_Event e{};
    const int w = std::max(winW, 1), h = std::max(winH, 1);
    switch (c.kind) {
      case 1: {
        float wx, wy;
        mouseUiStageToWindow(c.x, c.y, w, h, &wx, &wy);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.which = 0;
        // Whole pixels, as SDL2's int fields carried (SDL3's are float): a feed moves the cursor
        // to the same positions it always did.
        e.motion.x = float(std::lround(wx));
        e.motion.y = float(std::lround(wy));
        // the first move comes "from" 12 px up-left, as the desktop path does
        e.motion.xrel = f.wx < 0 ? 12.0f : float(std::lround(wx - f.wx));
        e.motion.yrel = f.wy < 0 ? 12.0f : float(std::lround(wy - f.wy));
        f.wx = wx;
        f.wy = wy;
        break;
      }
      case 2:
      case 3:
        e.type = c.kind == 2 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.which = 0;
        e.button.button = uint8_t(c.button);
        e.button.x = float(std::lround(f.wx < 0 ? 0.0f : f.wx));
        e.button.y = float(std::lround(f.wy < 0 ? 0.0f : f.wy));
        break;
      case 4:
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.which = 0;
        e.wheel.y = c.x;
        e.wheel.integer_y = int(c.x);
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        break;
      default:
        continue;
    }
    onMouseEvent(e, winW, winH);
  }
}

namespace {
void onMouseEvent(const SDL_Event& e, int winW, int winH) {
  std::lock_guard<std::mutex> lk(g_mu);
  MouseState& m = g_mouse;
  m.winW = winW;
  m.winH = winH;
  const double t = nowSec();
  // iter-465 modlib: while the mod library overlay is open it owns the mouse. The cursor keeps
  // its position (it is still drawn, over the library), but nothing is queued for the Apt menu
  // underneath: no click, wheel or hover move reaches the game.
  if (modlib::wantsMouse() && !isWindowEvent(e.type)) {
    float sx = 0, sy = 0;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
      if (e.motion.which == SDL_TOUCH_MOUSEID) return;
      m.wx = float(e.motion.x);
      m.wy = float(e.motion.y);
      m.inside = true;
      if (m.skipMotion) { m.skipMotion = false; return; }
      if (e.motion.xrel != 0.0f || e.motion.yrel != 0.0f) {
        m.lastMove = t;
        m.awake = true;
      }
      mouseUiWindowToStage(m.wx, m.wy, winW, winH, &sx, &sy);
      modlib::onMouse(1, sx, sy, 0, 0);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
      if (e.button.which == SDL_TOUCH_MOUSEID) return;
      m.wx = float(e.button.x);
      m.wy = float(e.button.y);
      m.inside = true;
      m.awake = true;
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) { m.lDown = true; m.lastClick = t; }
      if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) m.lDown = false;
      mouseUiWindowToStage(m.wx, m.wy, winW, winH, &sx, &sy);
      const int b = e.button.button == SDL_BUTTON_LEFT ? 1 : (e.button.button == SDL_BUTTON_RIGHT ? 3 : 2);
      modlib::onMouse(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 2 : 3, sx, sy, b, 0);
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      if (e.wheel.which == SDL_TOUCH_MOUSEID) return;
      int y = e.wheel.integer_y;   // SDL3: whole ticks (SDL2's int y); wheel.y is the float amount
      if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
      m.awake = true;
      mouseUiWindowToStage(m.wx, m.wy, winW, winH, &sx, &sy);
      modlib::onMouse(4, sx, sy, 0, y);
    }
    m.lClicks = m.rClicks = m.mClicks = 0;
    m.wheel = 0;
    return;
  }
  switch (e.type) {
    // Windows synthesizes a motion on enter / focus gain (the cursor did not move): the
    // next motion event never wakes the cursor and never counts as a hover move.
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      m.inside = false;
      break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      m.inside = true;
      m.skipMotion = true;
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      m.skipMotion = true;
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      m.lDown = false;
      m.awake = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (e.motion.which == SDL_TOUCH_MOUSEID) break;
      m.wx = float(e.motion.x);
      m.wy = float(e.motion.y);
      m.inside = true;
      if (m.skipMotion) { m.skipMotion = false; break; }
      if (e.motion.xrel != 0.0f || e.motion.yrel != 0.0f) {
        m.moveSeq++;
        m.lastMove = t;
        // Waking needs a deliberate move (a few pixels), not a single-pixel jitter.
        if (!m.awake) {
          m.accum += std::fabs(float(e.motion.xrel)) + std::fabs(float(e.motion.yrel));
          if (m.accum >= 6.0f) { m.awake = true; m.accum = 0; }
        }
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.which == SDL_TOUCH_MOUSEID) break;
      m.wx = float(e.button.x);
      m.wy = float(e.button.y);
      m.inside = true;
      m.awake = true;
      if (e.button.button == SDL_BUTTON_LEFT) { m.lDown = true; m.lClicks++; m.lastClick = t; }
      else if (e.button.button == SDL_BUTTON_RIGHT) m.rClicks++;
      else if (e.button.button == SDL_BUTTON_MIDDLE) m.mClicks++;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e.button.which == SDL_TOUCH_MOUSEID) break;
      if (e.button.button == SDL_BUTTON_LEFT) m.lDown = false;
      break;
    case SDL_EVENT_MOUSE_WHEEL: {
      if (e.wheel.which == SDL_TOUCH_MOUSEID) break;
      int y = e.wheel.integer_y;   // SDL3: whole ticks (SDL2's int y)
      if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
      m.wheel = std::clamp(m.wheel + y, -8, 8);
      m.awake = true;
      break;
    }
    default:
      break;
  }
  if (m.awake) g_wantSnapshot.store(true, std::memory_order_relaxed);
}
}  // namespace

bool mouseUiWantHideOsCursor() {
  if (!mouseUiEnabled()) return false;
  if (g_feedActive.load(std::memory_order_relaxed)) return false;   // harness: OS cursor untouched
  if (!realMouseAllowed()) return false;   // automated run: the user's OS cursor is left alone
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_mouse.inside || !hostWindowFocused()) return false;
  const double t = nowSec();
  if (modlib::wantsMouse() && g_mouse.awake) return true;   // iter-465: over the mod library
  // In a menu our cursor replaces the OS one; elsewhere (gameplay, cutscenes, movies) the OS
  // cursor hides once the mouse has been still for 2 s, and comes back when it moves.
  if (g_mouse.awake && g_model.valid && g_model.active && (t - g_model.t) < 1.0) return true;
  return (t - g_mouse.lastMove) > 2.0;
}

// ── public: the Apt snapshot (guest thread) ─────────────────────────────────────────────
namespace {
bool takeSnapshot() {   // true = a new model this pump (lane E E5b: the glow judges only from this pump's reads)
  if (!g_wantSnapshot.load(std::memory_order_relaxed)) return false;   // mouse untouched: nothing
  if (!mouseUiEnabled()) return false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_mouse.awake) {
      g_wantSnapshot.store(false, std::memory_order_relaxed);
      return false;
    }
  }
  Reader r{guestMap, nullptr};
  MenuModel m = mouseSnapshot(r);   // lane E E5: the scan and the clickable hints
  m.t = nowSec();
  m.seq = g_snapSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  std::lock_guard<std::mutex> lk(g_mu);
  bool changed = m.valid != g_model.valid || m.active != g_model.active ||
                 m.n != g_model.n || m.selected != g_model.selected || m.nHints != g_model.nHints ||
                 m.hintsUnknown != g_model.hintsUnknown || m.screen != g_model.screen ||
                 m.locked != g_model.locked || std::strcmp(m.what, g_model.what) != 0;
  // lane E E5: a moved or replaced hint logs again too - against the hints LAST LOGGED, so a slow drift (under
  // half a pixel per snapshot) cannot leave the logged targets stale (Astra E5 r2)
  static HintHit s_loggedHints[kMaxHints];
  static int s_loggedN = -1;
  changed = changed || m.nHints != s_loggedN;
  for (int i = 0; !changed && i < m.nHints; ++i) {
    const HintHit& a = m.hints[i];
    const HintHit& b = s_loggedHints[i];
    changed = a.key != b.key || std::fabs(a.r.x0 - b.r.x0) > 0.5f || std::fabs(a.r.y0 - b.r.y0) > 0.5f ||
              std::fabs(a.r.x1 - b.r.x1) > 0.5f || std::fabs(a.r.y1 - b.r.y1) > 0.5f;
  }
  // A re-laid-out page (a popup re-shown with another body, a scrolled grid) logs again too,
  // so the slot lines always describe what is on screen (sr_drive `clickitem` aims by them).
  for (int i = 0; !changed && i < m.n; ++i) {
    const MenuItem& a = m.items[i];
    const MenuItem& b = g_model.items[i];
    changed = a.index != b.index || a.kind != b.kind || a.enabled != b.enabled ||
              std::fabs(a.r.x0 - b.r.x0) > 0.5f || std::fabs(a.r.y0 - b.r.y0) > 0.5f ||
              std::fabs(a.r.x1 - b.r.x1) > 0.5f || std::fabs(a.r.y1 - b.r.y1) > 0.5f;
  }
  g_model = m;
  if (logOn() && changed) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[MOUSE_UI] snapshot #%llu valid=%d active=%d what='%s' items=%d sel=%d count=%d "
                "nav=%d locked=%d title=%d reads_left=%d",
                (unsigned long long)m.seq, m.valid ? 1 : 0, m.active ? 1 : 0, m.what, m.n,
                m.selected, m.count, int(m.nav), m.locked ? 1 : 0, m.clickAnywhere ? 1 : 0,
                r.budget);
    for (int i = 0; i < m.n && i < 24; ++i) {
      const MenuItem& it = m.items[i];
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MOUSE_UI]   slot %d index=%d kind=%d en=%d rect=(%.1f,%.1f)-(%.1f,%.1f)%s",
                  i, it.index, int(it.kind), it.enabled ? 1 : 0, it.r.x0, it.r.y0, it.r.x1,
                  it.r.y1, it.sliderValid ? " slider" : "");
    }
    if (m.hintsUnknown) SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI]   hints unknown (a read was refused)");
    s_loggedN = m.nHints;
    for (int i = 0; i < m.nHints; ++i) s_loggedHints[i] = m.hints[i];
    for (int i = 0; i < m.nHints; ++i)   // lane E E5: a scripted click aims at a hint by these
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI]   hint %d key=%d rect=(%.1f,%.1f)-(%.1f,%.1f)", i,
                  m.hints[i].key, m.hints[i].r.x0, m.hints[i].r.y0, m.hints[i].r.x1, m.hints[i].r.y1);
  }
  return true;
}
}  // namespace

namespace {
// Player 1's controller index (what Key.getController() reports for the pad that pressed).
// E5c r2: the game's front-end pad / keyboard input is armed - ActivePadListener (dword_8296EDB4) +16, which only the
// input layer's own INTERFACE_START / INTERFACE_A set (sub_822A3350) and MenuButtonRepeatTick_sub_8214FC78 waits for.
// 1 / 0, or -1 when it cannot be read (no listener yet). The live proof of the title click logs it before and after.
int frontEndInputArmed() {
  Reader r{guestMap, nullptr};
  const uint32_t lst = r.u32or0(0x8296EDB4u);
  uint32_t v = 0;
  if (!lst || !r.u32(lst + 16, &v)) return -1;
  return (v >> 24) != 0 ? 1 : 0;   // the byte at +16 (big-endian)
}

int playerController() {
  Reader r{guestMap, nullptr};
  const uint32_t pad = r.u32or0(0x8296E8C8u);
  const uint32_t idx = pad ? r.u32or0(pad + 12) : 0;
  return idx > 3 ? 0 : int(idx);
}

// The focus step toward `goal` as a call of the surface's own method.
Plan focusStep(const MenuModel& m, int goal) {
  Plan p;
  const uint16_t dir = navKey(m, goal);
  p.kind = Plan::kMethod;
  p.obj = m.navObj;
  p.why = "focus";
  switch (m.nav) {
    case kNavRow:     // IconMenu.Next / Previous (its own RIGHT / LEFT handlers, ICONMENU 0x2a6a)
      p.method = dir == kPadRight ? "Next" : "Previous";
      break;
    case kNavGrid:    // ThumbViewer.MoveHighlight (what the screens' HandleInput calls)
      p.method = "MoveHighlight";
      p.strArg = dir == kPadRight ? "right" : dir == kPadLeft ? "left" : dir == kPadUp ? "up" : "down";
      break;
    case kNavList:    // CafeFW_FocusManager.selectNextItem / selectPreviousItem (MAIN 0x70a2 / 0x72a2)
    default:
      p.method = dir == kPadDown ? "selectNextItem" : "selectPreviousItem";
      break;
  }
  if (!p.obj) p.kind = Plan::kNone;
  return p;
}
Plan eventPlan(int key, const char* why) {
  Plan p;
  p.kind = Plan::kEvent;
  p.key = key;
  p.why = why;
  return p;
}
Plan methodPlan(uint32_t obj, const char* method, const char* why) {
  Plan p;
  p.kind = obj ? Plan::kMethod : Plan::kNone;
  p.obj = obj;
  p.method = method;
  p.why = why;
  return p;
}

// What the cursor does this pump (under g_mu). At most one call per pump; the closed loop re-reads
// the game's own selection / value from the next snapshot.
Plan decide() {
  MouseState& ms = g_mouse;
  Driver& in = g_drv;
  auto idle = [&] { const uint64_t keep = in.lastMoveSeq; in = Driver{}; in.lastMoveSeq = keep; };
  // iter-465 modlib: the mod library overlay owns the mouse while open.
  if (modlib::isOpen()) {
    idle();
    ms.lClicks = ms.rClicks = ms.mClicks = 0;
    ms.wheel = 0;
    return {};
  }
  if (!ms.awake || !focusedForMouse()) {
    idle();
    return {};
  }
  const MenuModel& m = g_model;
  const bool fresh = m.valid && (nowSec() - m.t) < 0.75;
  if (!fresh || !m.active) {
    ms.lClicks = ms.rClicks = ms.mClicks = 0;
    ms.wheel = 0;
    in.goal = -1;
    in.dragging = false;
    return {};
  }
  if (m.locked) return {};             // the game drops input now; keep the intent
  float sx, sy;
  mouseUiWindowToStage(ms.wx, ms.wy, ms.winW, ms.winH, &sx, &sy);
  const int hovSlot = ms.inside ? hoveredSlot(m, sx, sy) : -1;
  const int hovIndex = hovSlot >= 0 ? m.items[hovSlot].index : -1;

  if (ms.rClicks > 0) {                // right click = the menu's back (TRIANGLE)
    ms.rClicks--;
    in.goal = -1;
    in.dragging = false;
    return eventPlan(kKeyTriangle, "right click = back");
  }
  if (ms.mClicks > 0) {                // middle click = SQUARE (e.g. wear a costume)
    ms.mClicks--;
    return eventPlan(kKeySquare, "middle click = SQUARE");
  }
  if (ms.wheel != 0) {                 // the wheel: one focus step per notch
    const bool up = ms.wheel > 0;
    if (hovSlot >= 0 && m.items[hovSlot].kind == kItemValue) {
      // over a host page's value row: adjust it once the row has the focus
      if (m.selected == hovIndex && in.goal < 0) {
        ms.wheel += up ? -1 : 1;
        in.lastSelected = m.selected;
        in.waitPumps = 4;
        return eventPlan(up ? kKeyRight : kKeyLeft, "wheel on a value row");
      }
      if (in.goal < 0) { in.goal = hovIndex; in.act = kActNone; in.stuck = 0; }
    } else {
      ms.wheel += up ? -1 : 1;
      in.goal = -1;
      Plan p;
      p.kind = m.navObj ? Plan::kMethod : Plan::kNone;
      p.obj = m.navObj;
      p.why = "wheel";
      if (m.nav == kNavRow) p.method = up ? "Previous" : "Next";
      else if (m.nav == kNavGrid) { p.method = "MoveHighlight"; p.strArg = up ? "up" : "down"; }
      else p.method = up ? "selectPreviousItem" : "selectNextItem";
      return p;
    }
  }
  if (m.clickAnywhere) {               // the title: any click = START
    if (ms.lClicks > 0) {
      ms.lClicks = 0;
      // lane E E5c: the game's own START (the PC action layer), never only an Apt key: a gEH.HandleEvent(301)
      // starts the boot flow but leaves the front end's pad / keyboard input unarmed (pccontrols::pulseStart)
      Plan p = eventPlan(kKeyStart, "title click = START");
      p.kind = Plan::kStart;
      return p;
    }
    return {};
  }
  // A left click picks the item under the cursor and what to do once it has focus.
  if (ms.lClicks > 0) {
    ms.lClicks = 0;
    // lane E E5: a click on a hint (its icon or its label) is that button: its key through the game's router, once
    // per click; nothing under it is clicked as well (a BACK hint never also confirms a row)
    if (const int hk = ms.inside ? hintKeyAt(m, sx, sy) : 0) {
      in.goal = -1;
      in.act = kActNone;
      in.dragging = false;
      in.lastSelected = m.selected;
      in.waitPumps = 8;
      return eventPlan(hk, "hint click");
    }
    if (hovSlot >= 0) {
      const MenuItem& it = m.items[hovSlot];
      in.goal = it.index;
      in.stuck = 0;
      in.dragging = false;
      if (it.kind == kItemSlider || it.kind == kItemToggle) {
        if (it.leftArrow.contains(sx, sy)) in.act = kActLeft;
        else if (it.rightArrow.contains(sx, sy)) in.act = kActRight;
        else if (it.kind == kItemSlider && it.sliderValid && it.track.contains(sx, it.track.y0)) {
          in.act = kActSlider;
          in.dragging = true;
          in.dragIndex = it.index;
        } else {
          in.act = it.kind == kItemToggle ? kActRight : kActNone;   // toggles wrap: next value
        }
      } else if (it.kind == kItemValue) {
        in.act = sx < 0.5f * (it.r.x0 + it.r.x1) ? kActLeft : kActRight;
      } else {
        in.act = kActA;
      }
    }
  }
  if (!ms.lDown && in.act != kActSlider) in.dragging = false;
  // Hover moves focus, but only on REAL mouse motion (a still cursor never fights the keyboard).
  const bool moved = ms.moveSeq != in.lastMoveSeq;
  in.lastMoveSeq = ms.moveSeq;
  if (moved && hovIndex >= 0 && hovIndex != m.selected && in.goal < 0 && !in.dragging) {
    in.goal = hovIndex;
    in.act = kActNone;
    in.stuck = 0;
  }
  in.lastHover = hovIndex;
  // Wait for the game to react to the previous call (closed loop on its own selection).
  if (in.waitPumps > 0 && m.selected == in.lastSelected) {
    in.waitPumps--;
    return {};
  }
  in.waitPumps = 0;
  if (in.goal >= 0) {
    const int gs = slotOfIndex(m, in.goal);
    if (gs < 0) {                       // the item went away
      in.goal = -1;
      in.act = kActNone;
      return {};
    }
    if (m.selected != in.goal) {
      if (in.lastSelected != m.selected) in.stuck = 0;
      else if (++in.stuck > 6) {         // the game refuses to move there
        in.goal = -1;
        in.act = kActNone;
        in.stuck = 0;
        return {};
      }
      in.lastSelected = m.selected;
      in.waitPumps = 8;
      return focusStep(m, in.goal);
    }
    // Focused: the click's action, through the item's / the router's own function.
    const Action act = in.act;
    const MenuItem& it = m.items[gs];
    in.goal = -1;
    in.act = kActNone;
    in.lastSelected = m.selected;
    switch (act) {
      case kActA:
        in.waitPumps = 8;
        return eventPlan(kKeyCross, "click = confirm");
      case kActLeft:
      case kActRight: {
        const bool right = act == kActRight;
        in.waitPumps = 4;
        if (it.kind == kItemSlider) return methodPlan(it.obj, right ? "increaseValue" : "decreaseValue", "slider arrow");
        if (it.kind == kItemToggle) return methodPlan(it.obj, right ? "nextValue" : "prevValue", "toggle");
        return eventPlan(right ? kKeyRight : kKeyLeft, "value row");
      }
      case kActSlider: in.act = kActNone; break;   // the drag loop below sets the value
      default: break;
    }
  }
  // Slider drag (or a click on its track): the slider's own increase / decrease until its own
  // mCurrentValue is the one under the cursor.
  if (in.dragging && m.selected == in.dragIndex) {
    const int s = slotOfIndex(m, in.dragIndex);
    if (s >= 0 && m.items[s].sliderValid) {
      const MenuItem& it = m.items[s];
      const int steps = mouseUiSliderSteps(sx, it.track.x0, it.track.x1, it.vmin, it.vmax,
                                           it.step, it.value);
      if (in.valueWait > 0 && it.value == in.lastValue) {
        in.valueWait--;
        return {};
      }
      in.valueWait = 0;
      if (steps != 0) {
        in.lastSelected = m.selected;
        in.lastValue = it.value;
        in.valueWait = 6;
        return methodPlan(it.obj, steps > 0 ? "increaseValue" : "decreaseValue", "slider drag");
      }
    }
    if (!ms.lDown) in.dragging = false;
  }
  return {};
}
}  // namespace

// ── public: the Apt snapshot and the cursor's calls (guest thread, after the executor pump) ──
static void glowPump(PPCContext& ctx, uint8_t* base, bool tookSnapshot, bool mainCall);

void mouseUiAfterAptPump(PPCContext& ctx, uint8_t* base) {
  censusTick();   // lane E E1: SR_MENU_CENSUS only (observe-only)
  const bool took = takeSnapshot();
  // The calls are a PORT FEATURE (charter 2a, docs/OVERRIDES.json): players always, automated
  // rolls only with SR_PORT_FEATURES=1 (natural rolls make none).
  if (!mouseUiEnabled() || !automation::portFeaturesOn()) return;
  // E5c r2: a title click's START pulse ends with the title (never carried into the next screen); the moment the
  // front end's own input becomes armed is logged once (the live proof: armed=0 at the click, then this line)
  {
    static bool s_wasTitle = false, s_armedLogged = false;
    bool isTitle;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      isTitle = g_model.valid && g_model.clickAnywhere;
    }
    if (s_wasTitle && !isTitle) pccontrols::cancelStartPulse();
    s_wasTitle = isTitle;
    if (!s_armedLogged && logOn() && frontEndInputArmed() == 1) {
      s_armedLogged = true;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MOUSE_UI] front-end pad / keyboard input armed (ActivePadListener +16 = 1: the game's own "
                  "INTERFACE_START / INTERFACE_A)");
    }
  }
  Plan p;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_mouse.awake) g_drv.goal = -1;
    else p = decide();
  }
  glowPump(ctx, base, took, p.kind != Plan::kNone);   // lane E E5b: an off before the call, an on only without one
  if (p.kind == Plan::kNone) return;
  bool ok = false;
  char argText[32] = {};
  if (p.kind == Plan::kStart) {
    const int armedBefore = frontEndInputArmed();
    if (pccontrols::pulseStart(pccontrols::kStartPulsePolls)) {
      ++g_drv.calls;
      if (logOn())
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[MOUSE_UI] press START (%s: the game's own START, %d pad polls / %d ms through the PC action "
                    "layer; front-end input armed=%d) sel=%d '%s'", p.why, pccontrols::kStartPulsePolls,
                    pccontrols::kStartPulseMaxMs, armedBefore, g_model.selected, g_model.what);
      return;
    }
  }
  if (p.kind == Plan::kEvent || p.kind == Plan::kStart) {   // kStart here: the action layer is off
    // the game's input router, as its Key listener calls it (onKeyDown: HandleEvent(code, ctrl))
    std::snprintf(argText, sizeof argText, "%d", p.key);
    ok = aptcall::callPath(ctx, base, "gEH", "HandleEvent",
                           {aptcall::Arg::Int(p.key), aptcall::Arg::Int(playerController())});
  } else {
    std::vector<aptcall::Arg> args;
    if (p.hasInt) {
      args.push_back(aptcall::Arg::Int(p.intArg));
      std::snprintf(argText, sizeof argText, "0x%06X", unsigned(p.intArg));
    } else {
      if (!p.strArg.empty()) args.push_back(aptcall::Arg::Str(p.strArg));
      std::snprintf(argText, sizeof argText, "%s", p.strArg.c_str());
    }
    ok = aptcall::callMethod(ctx, base, p.obj, p.method, args);
  }
  ++g_drv.calls;
  if (logOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] call %s(%s) (%s) obj=%08X %s sel=%d '%s'",
                (p.kind == Plan::kEvent || p.kind == Plan::kStart) ? "gEH.HandleEvent" : p.method, argText, p.why,
                p.obj,
                ok ? "done" : "NOT CALLED", g_model.selected, g_model.what);
}

// Lane E E5b: the hover glow's pump (guest thread, after decide()). The hover target comes only from THIS pump's
// complete snapshot; the glowing label's standing from this pump's own reads (labelOnAnyScreen).
static void glowPump(PPCContext& ctx, uint8_t* base, bool tookSnapshot, bool mainCall) {
  static uint64_t s_pump = 0;   // the guest thread only
  Glow g;
  uint32_t want = 0;
  bool locked = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g = g_glow;
    want = glowWanted(g_model, g_mouse, tookSnapshot, focusedForMouse(), modlib::isOpen(), &locked);
  }
  struct Guest {
    PPCContext* ctx;
    uint8_t* base;
  } guest{&ctx, base};
  GlowIo io;
  io.user = &guest;
  io.standing = [](void*, uint32_t label) {
    Reader r{guestMap, nullptr};
    return labelOnAnyScreen(r, label);
  };
  io.glowing = [](void*, uint32_t label) {
    Reader r{guestMap, nullptr};
    const Tri rec = labelHasGlowRecord(r, label);
    // E5b r13 (the step-5 live-receipt rule): srOwn exists only when the PATCHED PartText.SetFontColor ran (the glow
    // patch is a class patch inside MAIN); the first record seen present is the receipt (tools/main_receipt.py e5b)
    static std::atomic<bool> s_seen{false};
    if (rec == Tri::Yes && logOn() && !s_seen.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MOUSE_UI] glow record seen label=%08X (srOwn: the patched PartText.SetFontColor kept the label's "
                  "own colour)", label);
    return rec;
  };
  io.call = [](void* u, uint32_t label, int32_t value) {
    Guest& gu = *static_cast<Guest*>(u);
    return aptcall::callMethod(*gu.ctx, gu.base, label, "SetFontColor", {aptcall::Arg::Int(value)});
  };
  const GlowStep s = glowStep(g, want, !mainCall && !locked, ++s_pump, io);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_glow = g;   // only this thread writes g_glow
  }
  // tools/hint_glow_grade.py reads these lines (every on is followed by its label's off or drop)
  if (logOn() && s.act == GlowAct::Drop)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] glow dropped label=%08X (gone: no hint label on any screen)", s.obj);
  if (logOn() && s.called)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] glow %s label=%08X %s%s", s.act == GlowAct::On ? "on" : "off", s.obj,
                s.ok ? "done" : "NOT CALLED", !s.ok && s.act == GlowAct::Off ? " (kept; retried later)" : "");
}

// ── public: the pad poll tells the cursor about other input (poll thread) ─────────────────────
void mouseUiNoteOtherInput(bool active) {
  if (!active || !mouseUiEnabled()) return;
  // Pad or keyboard in use: the cursor sleeps AT ONCE (hidden from the next drawn frame, whatever
  // the port-feature state) until the mouse moves again, and nothing it queued is done.
  std::lock_guard<std::mutex> lk(g_mu);
  MouseState& ms = g_mouse;
  if (ms.awake && logOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] pad/keyboard input -> cursor sleeps");
  ms.awake = false;
  ms.lClicks = ms.rClicks = ms.mClicks = 0;
  ms.wheel = 0;
  const uint64_t keep = g_drv.lastMoveSeq;
  g_drv = Driver{};
  g_drv.lastMoveSeq = keep;
}

// ── public: overlay (render thread) ─────────────────────────────────────────────────────
CursorOverlayState mouseUiOverlayState(int bbW, int bbH) {
  CursorOverlayState s;
  if (!mouseUiEnabled()) return s;
  std::lock_guard<std::mutex> lk(g_mu);
  const MouseState& ms = g_mouse;
  const MenuModel& m = g_model;
  const double t = nowSec();
  // iter-465 modlib: over the mod library the cursor shows whenever the mouse is in use, and its
  // hover ring follows the library's own items.
  const bool lib = modlib::wantsMouse();
  if (lib) {
    if (!ms.awake || !ms.inside || !focusedForMouse()) return s;
  } else if (!ms.awake || !ms.inside || !m.valid || !m.active || (t - m.t) > 1.0) {
    return s;
  }
  if (!focusedForMouse()) return s;
  s.visible = true;
  s.x = ms.wx * float(bbW) / float(std::max(ms.winW, 1));
  s.y = ms.wy * float(bbH) / float(std::max(ms.winH, 1));
  // scaled with the game image, not the window: the fit rect's height / 720 (4K -> 3x)
  float fx, fy, fw, fh;
  mouseUiFitRect(bbW, bbH, &fx, &fy, &fw, &fh);
  s.scale = std::max(0.5f, fh / kStageH);
  float sx, sy;
  mouseUiWindowToStage(ms.wx, ms.wy, ms.winW, ms.winH, &sx, &sy);
  s.hover = lib ? (modlib::mouseHover() ? 1.0f : 0.0f)
                : ((m.clickAnywhere || hoveredSlot(m, sx, sy) >= 0) ? 1.0f : 0.0f);
  const double since = t - ms.lastClick;
  s.pulse = (since >= 0 && since < 0.4) ? float(since) : -1.0f;
  s.pressed = ms.lDown ? 1.0f : 0.0f;
  s.timeSec = float(std::fmod(t, 1000.0));
  return s;
}

// ── a synthetic Apt heap for the self test (the layout of the header comment) ───────────
namespace {
struct FakeHeap {
  static constexpr uint32_t kBase = 0x40000000u;
  std::vector<uint8_t> mem = std::vector<uint8_t>(1 << 20, 0);
  uint32_t top = kBase + 0x100;
  uint8_t globalCell[4] = {0, 0, 0, 0};

  uint32_t alloc(uint32_t n) {
    const uint32_t a = (top + 15u) & ~15u;
    top = a + n;
    return a;
  }
  void w32(uint32_t va, uint32_t v) {
    uint8_t* p = &mem[va - kBase];
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
  }
  void w16(uint32_t va, uint16_t v) {
    mem[va - kBase] = uint8_t(v >> 8);
    mem[va - kBase + 1] = uint8_t(v);
  }
  void wf(uint32_t va, float f) { uint32_t v; std::memcpy(&v, &f, 4); w32(va, v); }
  uint32_t u32(uint32_t va) const {
    const uint8_t* p = &mem[va - kBase];
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  uint32_t str(const char* s, bool hashed = true) {
    const uint32_t n = uint32_t(std::strlen(s));
    const uint32_t a = alloc(8 + n + 1);
    if (hashed) w16(a + 6, mouseUiAptHash16(s));
    std::memcpy(&mem[a + 8 - kBase], s, n + 1);
    return a;
  }
  uint32_t hdr(uint32_t a, uint32_t type, uint32_t vt = 0x82087164u) {
    w32(a, vt);
    w32(a + 4, (1u << 27) | (1u << 31) | type);
    return a;
  }
  // A property table {cap, entries, __proto__, prototype} at `tbl`.
  void table(uint32_t tbl, const std::vector<std::pair<const char*, uint32_t>>& ps,
             uint32_t proto = 0, uint32_t prototype = 0) {
    uint32_t cap = 8;
    while (cap < ps.size() * 2) cap <<= 1;
    const uint32_t ents = alloc(8 * cap);
    w32(tbl, cap);
    w32(tbl + 4, ents);
    w32(tbl + 8, proto);
    w32(tbl + 12, prototype);
    // Place by hash like the runtime; the reader scans, so any slot works. Leave slot 0
    // deleted to exercise the skip.
    w32(ents, 0x829311B4u);
    uint32_t slot = 1;
    for (const auto& kv : ps) {
      w32(ents + 8 * slot, str(kv.first, slot % 2 == 0));   // odd slots: hash not cached yet
      w32(ents + 8 * slot + 4, kv.second);
      slot++;
    }
  }
  uint32_t obj(uint32_t type, const std::vector<std::pair<const char*, uint32_t>>& ps,
               uint32_t proto = 0, uint32_t prototype = 0) {
    const uint32_t a = hdr(alloc(0x30), type);
    table(a + 8, ps, proto, prototype);
    return a;
  }
  uint32_t intv(int v) { const uint32_t a = hdr(alloc(12), kTInt); w32(a + 8, uint32_t(v)); return a; }
  // A string value (type 1: +8 -> the string object, its chars at +8).
  uint32_t strv(const char* s) { const uint32_t a = hdr(alloc(12), kTString); w32(a + 8, str(s)); return a; }
  // Make `child` the first entry of `parent`'s display list (instance +28 -> head cell -> child wrapper).
  void displayChild(uint32_t parent, uint32_t child) {
    const uint32_t inst = u32(parent + 0x20), cell = alloc(4);
    w32(inst + 28, cell);
    w32(cell, child);
  }
  uint32_t boolv(bool v) { const uint32_t a = hdr(alloc(12), kTBool); mem[a + 8 - kBase] = v; return a; }
  // A number value (type 6: a big-endian float at +8).
  uint32_t numv(float v) {
    const uint32_t a = hdr(alloc(12), kTNumber);
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    w32(a + 8, bits);
    return a;
  }
  uint32_t arr(const std::vector<uint32_t>& xs) {
    const uint32_t a = hdr(alloc(0x30), kTArray);
    table(a + 8, {});
    const uint32_t el = alloc(4 * uint32_t(xs.size() + 1));
    for (size_t i = 0; i < xs.size(); ++i) w32(el + 4 * uint32_t(i), xs[i]);
    w32(a + 0x20, el);
    w32(a + 0x24, uint32_t(xs.size()));
    w32(a + 0x28, uint32_t(xs.size()));
    return a;
  }
  // An Array whose numeric keys live as named members (a sparse array), count 0.
  uint32_t arrNamed(const std::vector<std::pair<const char*, uint32_t>>& ps) {
    const uint32_t a = arr({});
    table(a + 8, ps);
    return a;
  }
  // A MovieClip wrapper: shape (charType 1, bounds b) or sprite (charType 5), local
  // translate (tx, ty) and scale s, visible flag, own property table.
  uint32_t clip(uint32_t parent, int charType, float tx, float ty, float s, bool visible,
                const float* b, const std::vector<std::pair<const char*, uint32_t>>& ps,
                uint32_t proto = 0) {
    const uint32_t w = hdr(alloc(0x24), kTClip, kClipVtable);
    const uint32_t inst = alloc(0x40), st = alloc(0x70), mtx = alloc(24), tbl = alloc(16);
    w32(w + 0x1C, parent);
    w32(w + 0x20, inst);
    w32(inst + 4, st);
    w32(inst + 8, uint32_t(charType) << 26);
    w32(inst + 12, tbl);
    table(tbl, ps, proto);
    w32(st + 8, mtx);
    wf(mtx, s); wf(mtx + 4, 0); wf(mtx + 8, 0); wf(mtx + 12, s); wf(mtx + 16, tx); wf(mtx + 20, ty);
    w16(st + 22, 0x8000);
    w32(st + 24, visible ? 0x80000000u : 0u);
    if (charType == 1 && b) {
      const uint32_t cd = alloc(32);
      w32(cd, 1);
      for (int k = 0; k < 4; ++k) wf(cd + 16 + 4 * k, b[k]);
      w32(st + 4, cd);
    }
    return w;
  }
  static const uint8_t* map(void* user, uint32_t va, uint32_t n) {
    FakeHeap* h = static_cast<FakeHeap*>(user);
    if (va == kGlobalHolder && n <= 4) return h->globalCell;
    if (va < kBase || uint64_t(va) + n > uint64_t(kBase) + h->mem.size()) return nullptr;
    return &h->mem[va - kBase];
  }
};

// Builds the LOAD A GAME? state of the nosound golden: BG(L20) / ScrPressStart(L50) /
// FG(L75) / ScrGenPop popup (L900) with two rows; the popup sits under a parent scaled 2x
// (to prove the matrix order) and the first slot of every table is a deleted entry.
// pageKinds: 0 = a plain popup; 1 = a host page (mParams.context.kinds = [0, 2]: row 1 is a
// slider); 2 = the same with `kinds` sparse (row 1 as the named member "1" = 1, a toggle);
// 3 = a context object without `kinds` (the front end's own panels).
// hintMode (lane E E1): 0 = no hint bar; 1 = the popup's mDisplayWell.mHintHolder holds Hint0 (A 'SELECT') and Hint1
// (B 'BACK'); 2 = the same ButtonHints instance only as a display-list child of the screen (no mDisplayWell); 3 = mode
// 1 with Hint1 hidden; 4 = an absurd mHintCount; 5 = Hint0's label string at unmapped memory; 6 = the focus index's
// value at unmapped memory; 7 = Hint0's label string at a low non-null address; 8 = at 0xFFFFFFF8 (its chars at +8
// wrap to 0); 9 = mode 1 with the custom front end's icon names (ButtonIcon.srIcon: SELECT = "A", BACK = "B");
// 10 = mode 9 with BACK's name at unmapped memory; 11 = mode 9 with SELECT's label already in the glow colour;
// 12 / 13: the E5b covered-screen modes (below); 14 = mode 9 laid out as the PC roll's popup bar (E3-E6 r4): each
// label field 183 local (366 stage) px wide so the hints' bounds overlap, the icons at Icon._x = 44 (SELECT) / 32
// (BACK) and BACK placed where AddHint puts it (408 - (32 + 32) - 6). From mode 9 on, each Icon is a clip child of
// its hint at Icon._x (8 unless mode 14: the extent then equals the 40 px shape).
// probe: called with a fresh reader over the live heap and its mouse snapshot (lane E E5b's guest reads). *census =
// censusLine() of the snapshot; *complete =
// censusSnapshotComplete(); *out = mouseSnapshot() (the scan and the hints, one reader, as takeSnapshot).
using HeapProbe = void (*)(const Reader& fresh, const MenuModel& m, void* user);
// lane E E5b r3: what the last fakeHeapTest built, for probes that need addresses a snapshot does not publish
uint32_t t_hintLabels[2] = {};   // the popup's SELECT / BACK LabelText (0 = no hint bar)
uint32_t t_coveredLabel = 0;     // hintMode 12: the covered screen's hint label (0 = none)
FakeHeap* t_heap = nullptr;      // the heap a probe runs on (lane E E5b r7: probes may change it between glow pumps)
uint32_t t_protoHidden = 0;      // hintMode 14: an object with an unreadable own entry and mFontColor only on its proto
uint32_t t_protoPlain = 0;       // hintMode 14: the same without the unreadable entry (mFontColor 0x123456 from the proto)
// hintMode 15: as 12, but the covered holder's own table has an unreadable entry (it could be an own mHintCount = 0) and
// mHintCount = 1 only on a prototype between it and ButtonHints: its Hint0 / LabelText match is on an unclean path
int fakeHeapTest(bool withPopup, bool hudOnly, bool locked, MenuModel* out, int pageKinds = 0, int hintMode = 0,
                 std::string* census = nullptr, int budget = 60000, bool* complete = nullptr,
                 HeapProbe probe = nullptr, void* probeUser = nullptr) {
  FakeHeap h;
  t_hintLabels[0] = t_hintLabels[1] = t_coveredLabel = t_protoHidden = t_protoPlain = 0;
  const uint32_t baseProto = h.obj(kTObject, {});
  auto cls = [&](const char* nm) {
    const uint32_t proto = h.obj(kTObject, {}, baseProto);
    const uint32_t fn = h.hdr(h.alloc(0x30), 35);
    h.table(fn + 8, {}, 0, proto);
    return std::make_pair(fn, proto);
  };
  auto bg = cls("ScrBackground_IG"), fg = cls("ScrForeground_IG"), ps = cls("ScrPressStart"),
       gp = cls("ScrGenPop"), hud = cls("Main_Hud"), dwc = cls("DisplayWell"), bhc = cls("ButtonHints");
  h.table(gp.second + 8, {{"mLinkageName", h.strv("ScrGenPop")}}, baseProto);   // ScrGenPop.prototype.mLinkageName
  const float row[4] = {0, -1, 300, 19};   // a 300 x 20 row (scaled 2x below = 600 x 40)
  auto screenEntry = [&](uint32_t proto, int level, std::vector<std::pair<const char*, uint32_t>> props,
                         float scale, uint32_t* screenOut) {
    const uint32_t root = h.clip(0, 5, 0, 0, 1, true, nullptr, {});
    const uint32_t mc = h.clip(root, 5, 0, 0, scale, true, nullptr, {});
    const uint32_t so = h.clip(mc, 5, 0, 0, 1, true, nullptr, props, proto);
    h.table(h.u32(h.u32(mc + 0x20) + 12), {{"Screen", so}});   // mc.Screen = so
    if (screenOut) *screenOut = so;
    return h.obj(kTObject, {{"mMovieClip", mc}, {"mLevel", h.intv(level)}});
  };
  std::vector<uint32_t> screens;
  uint32_t psSo = 0;   // ScrPressStart's Screen (hintMode 12 puts a hint bar there, under the popup)
  screens.push_back(screenEntry(bg.second, 20, {}, 1, nullptr));
  if (hudOnly) {
    screens.push_back(screenEntry(fg.second, 75, {}, 1, nullptr));
    screens.push_back(screenEntry(hud.second, 10, {}, 1, nullptr));
  } else {
    screens.push_back(screenEntry(ps.second, 50, {{"readyForInput", h.boolv(true)}}, 1, &psSo));
    screens.push_back(screenEntry(fg.second, 75, {}, 1, nullptr));
  }
  if (withPopup) {
    uint32_t so = 0;
    const uint32_t entry = screenEntry(gp.second, 900, {}, 2.0f, &so);
    const uint32_t b0 = h.clip(so, 1, 169.55f, 185.4f, 1, true, row, {{"m_bEnabled", h.boolv(true)}});
    const uint32_t b1 = h.clip(so, 1, 169.55f, 201.4f, 1, true, row, {{"m_bEnabled", h.boolv(true)}});
    // hintMode 6 (Astra E1 r3): the focus index's value points at unmapped memory - a scan read, not a census one
    const uint32_t fm = h.obj(kTObject, {{"m_arrItems", h.arr({b0, b1})},
                                         {"m_selectedItemIdx", hintMode == 6 ? 0x7F000000u : h.intv(1)},
                                         {"m_nInputs", h.intv(2)}});
    const uint32_t soTbl = h.u32(h.u32(so + 0x20) + 12);
    std::vector<std::pair<const char*, uint32_t>> soProps = {{"mFM", fm}};
    if (pageKinds) {
      uint32_t ctx = 0;
      if (pageKinds == 1) ctx = h.obj(kTObject, {{"kinds", h.arr({h.intv(0), h.intv(2)})}});
      else if (pageKinds == 2) ctx = h.obj(kTObject, {{"kinds", h.arrNamed({{"1", h.intv(1)}})}});
      else ctx = h.obj(kTObject, {{"pendingId", h.intv(7)}});
      soProps.push_back({"mParams", h.obj(kTObject, {{"context", ctx}, {"canCancel", h.boolv(true)},
                                                     {"title", h.strv("MAIN MENU")}})});
    }
    if (hintMode) {
      // A ButtonHints instance with two hints (a 40 x 20 shape each at local (420,330) / (480,330), under the 2x
      // parent = stage (840,660)-(920,700) / (960,660)-(1040,700)).
      const uint32_t holder = h.clip(so, 5, 0, 0, 1, true, nullptr, {}, bhc.second);
      const float hb[4] = {0, 0, 40, 20};
      // As retail: ButtonIcon.mFilePath is only the prefix constant (SetIcon hands the full path to loadMovie).
      // badLabel (hintMode 5): the label's string value points at unmapped memory (a stale pointer); hintMode 7: at
      // a non-null address inside the first page (a garbage pointer the reader refuses by range); hintMode 8: at
      // 0xFFFFFFF8, so the chars' address (+8) wraps to 0
      // hintMode 9 / 10: the custom front end's icon names (SELECT = "A", BACK = "B"); 10: BACK's name string at
      // unmapped memory (a refused read in the hint collection); 11: SELECT's label already gold; 12: as 9, plus a
      // hint bar on the covered ScrPressStart whose label shows our gold (a glow a popup then covered); 13: as 12,
      // plus a third popup hint at unmapped memory (a refused read the membership walk meets before the covered label);
      // 14 (E3-E6 r4): as 9, laid out as the PC roll's popup bar
      const bool named = hintMode >= 9 && hintMode <= 14;   // the custom front end's icons (clips)
      auto iconProps = [&](const char* label) {
        std::vector<std::pair<const char*, uint32_t>> ps = {{"mFilePath", h.strv("../common/ButtonHints/")}};
        if (!named) return ps;
        const uint32_t name = h.strv(label[0] == 'S' ? "A" : "B");
        if (hintMode == 10 && label[0] == 'B') h.w32(name + 8, 0x7F000000u);
        ps.push_back({"srIcon", name});
        return ps;
      };
      const float wide[4] = {0, 0, 183, 20};   // hintMode 14: the label field's authored width
      auto hint = [&](float x, const char* label, bool vis, bool badLabel, float iconX) {
        const uint32_t text = h.strv(label);
        if (badLabel) h.w32(text + 8, hintMode == 7 ? 0x10u : hintMode == 8 ? 0xFFFFFFF8u : 0x7F000000u);
        const uint32_t lt = named ? h.obj(kTObject, {{"mText", text},
                                                     {"mFontColor", h.intv(label[0] == 'S'
                                                          ? (hintMode == 11 ? int(kGlowColor) : 0xFFFFFF)
                                                          : 0xC0C0C0)},
                                                     {"mTextField", h.obj(kTObject, {})}})   // PartText's own field
                                  : h.obj(kTObject, {{"mText", text}});
        t_hintLabels[label[0] == 'S' ? 0 : 1] = lt;
        if (!named)
          return h.clip(holder, 1, x, 330, 1, vis, hb, {{"Icon", h.obj(kTObject, iconProps(label))}, {"LabelText", lt}});
        const uint32_t c = h.clip(holder, 1, x, 330, 1, vis, hintMode == 14 ? wide : hb, {});
        const uint32_t ic = h.clip(c, 5, iconX, 0, 1, true, nullptr, iconProps(label));
        h.table(h.u32(h.u32(c + 0x20) + 12), {{"Icon", ic}, {"LabelText", lt}});
        return c;
      };
      const uint32_t h0 = hint(hintMode == 14 ? 408 : 420, "SELECT", true,
                               hintMode == 5 || hintMode == 7 || hintMode == 8, hintMode == 14 ? 44 : 8);
      const uint32_t h1 = hint(hintMode == 14 ? 338 : 480, "BACK", hintMode != 3, false, hintMode == 14 ? 32 : 8);
      // 4: an absurd count (clamped to 10); 13: a third hint whose clip is unmapped
      const uint32_t count = hintMode == 4 ? h.numv(1e30f) : h.intv(hintMode == 13 ? 3 : 2);
      std::vector<std::pair<const char*, uint32_t>> holderProps = {{"mHintCount", count}, {"Hint0", h0}, {"Hint1", h1}};
      if (hintMode == 13) holderProps.push_back({"Hint2", 0x7F000000u});
      h.table(h.u32(h.u32(holder + 0x20) + 12), holderProps, bhc.second);
      if (hintMode == 2) {
        h.displayChild(so, holder);
      } else {
        const uint32_t dw = h.clip(so, 5, 0, 0, 1, true, nullptr, {{"mHintHolder", holder}}, dwc.second);
        soProps.push_back({"mDisplayWell", dw});
      }
    }
    h.table(soTbl, soProps, gp.second);
    screens.push_back(entry);
    if ((hintMode >= 12 && hintMode <= 16) && psSo) {   // the covered bar
      const uint32_t holder2 = h.clip(psSo, 5, 0, 0, 1, true, nullptr, {}, bhc.second);
      const float hb2[4] = {0, 0, 40, 20};
      // 14: the label's own table has an entry whose name is unmapped (table slot 1), before mText / mFontColor
      std::vector<std::pair<const char*, uint32_t>> ltProps = {{"mText", h.strv("START")},
                                                              {"mFontColor", h.intv(int(kGlowColor))},
                                                              {"mTextField", h.obj(kTObject, {})}};
      if (hintMode == 14) ltProps.insert(ltProps.begin(), {"srUnreadable", h.intv(0)});
      const uint32_t lt = h.obj(kTObject, ltProps);
      if (hintMode == 14) {
        h.w32(h.u32(lt + 8 + 4) + 8 * 1, 0x7F000000u);   // slot 1's name pointer -> unmapped
        const uint32_t protoObj = h.obj(kTObject, {{"mFontColor", h.intv(0x123456)}});
        t_protoHidden = h.obj(kTObject, {{"srUnreadable", h.intv(0)}}, protoObj);
        h.w32(h.u32(t_protoHidden + 8 + 4) + 8 * 1, 0x7F000000u);
        t_protoPlain = h.obj(kTObject, {{"srOther", h.intv(0)}}, protoObj);
      }
      const uint32_t c = h.clip(holder2, 1, 100, 330, 1, true, hb2, {{"LabelText", lt}});
      const uint32_t holder2Tbl = h.u32(h.u32(holder2 + 0x20) + 12);
      if (hintMode == 15) {
        const uint32_t countProto = h.obj(kTObject, {{"mHintCount", h.intv(1)}}, bhc.second);
        h.table(holder2Tbl, {{"srUnreadable", h.intv(0)}, {"Hint0", c}}, countProto);
        h.w32(h.u32(holder2Tbl + 4) + 8 * 1, 0x7F000000u);   // slot 1's name pointer -> unmapped
      } else {
        h.table(holder2Tbl, {{"mHintCount", h.intv(1)}, {"Hint0", c}}, bhc.second);
      }
      h.displayChild(psSo, holder2);
      t_coveredLabel = lt;
    }
  }
  const uint32_t lockMgr = h.obj(kTObject, {{"mLocks", h.arr(locked ? std::vector<uint32_t>{h.intv(1)}
                                                                    : std::vector<uint32_t>{})}});
  // hintMode 16 (Astra E5b r5): as 12, plus an unreadable entry in gSM's own table (a stack-level refusal that no screen
  // walk reads again)
  std::vector<std::pair<const char*, uint32_t>> smProps = {{"mScreens", h.arr(screens)}, {"mInputLockMgr", lockMgr}};
  if (hintMode == 16) smProps.insert(smProps.begin(), {"srUnreadable", h.intv(0)});
  const uint32_t sm = h.obj(kTObject, smProps);
  if (hintMode == 16) h.w32(h.u32(sm + 8 + 4) + 8 * 1, 0x7F000000u);
  const uint32_t G = h.obj(kTGlobal, {{"gSM", sm}, {"ScrBackground_IG", bg.first},
                                      {"ScrForeground_IG", fg.first}, {"ScrPressStart", ps.first},
                                      {"ScrGenPop", gp.first}, {"Main_Hud", hud.first},
                                      {"DisplayWell", dwc.first}, {"ButtonHints", bhc.first}});
  h.globalCell[0] = uint8_t(G >> 24); h.globalCell[1] = uint8_t(G >> 16);
  h.globalCell[2] = uint8_t(G >> 8); h.globalCell[3] = uint8_t(G);
  Reader r{&FakeHeap::map, &h};
  r.budget = budget;
  MenuModel m;
  const bool ok = scanMenus(r, &m);
  m.valid = ok;                      // as mouseUiAfterAptPump does
  if (census) *census = censusLine(r, m);
  if (complete) *complete = censusSnapshotComplete(r);
  if (out) {
    // lane E E5: the mouse's own snapshot as takeSnapshot takes it - the scan and the hints with one reader
    Reader r2{&FakeHeap::map, &h};
    r2.budget = budget;
    *out = mouseSnapshot(r2);
  }
  if (probe) {   // lane E E5b: reads over the live heap after the snapshot (a fresh reader each time it copies)
    MenuModel pm;
    Reader r3{&FakeHeap::map, &h};
    pm = mouseSnapshot(r3);
    const Reader fresh{&FakeHeap::map, &h};
    t_heap = &h;
    probe(fresh, pm, probeUser);
    t_heap = nullptr;
  }
  return ok ? 0 : 1;
}
}  // namespace

// ── self test (difftest --mouse-ui-test) ────────────────────────────────────────────────
int mouseUiSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok " : "FAIL:", what);
    if (!ok) fails++;
  };
  check(mouseUiAptHash16("FlowMgr") == mouseUiAptHash16("flowmgr"), "hash is case-insensitive");
  check(mouseUiAptHash16("") == uint16_t(2166136261u & 0xFFFFu), "hash of '' = FNV basis low 16");
  float sx, sy;
  mouseUiWindowToStage(640, 360, 1280, 720, &sx, &sy);
  check(sx == 640 && sy == 360, "window->stage identity at 1280x720");
  mouseUiWindowToStage(960, 540, 1920, 1080, &sx, &sy);
  check(sx == 640 && sy == 360, "window->stage scales 1080p");
  // Not 16:9: the game image is the centred 16:9 fit (render/resscale's present fit).
  mouseUiWindowToStage(0, 60, 1920, 1200, &sx, &sy);
  check(std::fabs(sx) < 1e-3f && std::fabs(sy) < 1e-3f, "16:10: bars top/bottom, (0,60) = stage origin");
  mouseUiWindowToStage(1920, 1140, 1920, 1200, &sx, &sy);
  check(std::fabs(sx - 1280) < 1e-3f && std::fabs(sy - 720) < 1e-3f, "16:10: (1920,1140) = stage corner");
  mouseUiWindowToStage(320 + 960, 540, 2560, 1080, &sx, &sy);
  check(std::fabs(sx - 640) < 1e-3f && std::fabs(sy - 360) < 1e-3f, "21:9: bars left/right, centre");
  {
    float wx, wy;
    mouseUiStageToWindow(339.1f, 401.8f, 2560, 1080, &wx, &wy);
    mouseUiWindowToStage(wx, wy, 2560, 1080, &sx, &sy);
    check(std::fabs(sx - 339.1f) < 1e-3f && std::fabs(sy - 401.8f) < 1e-3f, "stage->window->stage round trip");
    float fx, fy, fw, fh;
    mouseUiFitRect(3840, 2160, &fx, &fy, &fw, &fh);
    check(fx == 0 && fy == 0 && fw == 3840 && fh == 2160, "4K: the fit is the whole output (cursor scale 3)");
  }
  // Slider track 489.8 .. 659.8 (10 sections of 17, SLIDER char5), range 0..10 step 1.
  const float x0 = 489.8f, x1 = 659.8f;
  check(mouseUiSliderSteps(x1 + 5, x0, x1, 0, 10, 1, 10) == 0, "slider: right of the track at max");
  check(mouseUiSliderSteps(x0 - 5, x0, x1, 0, 10, 1, 10) == -10, "slider: left of the track = 0");
  check(mouseUiSliderSteps(x0 + 17 * 4.5f, x0, x1, 0, 10, 1, 3) == 2, "slider: inside section 5 = 5");
  check(mouseUiSliderSteps(x0 + 17 * 0.2f, x0, x1, 0, 10, 1, 0) == 1, "slider: section 1 = 1");
  check(mouseUiSliderSteps(x0 + 17 * 9.9f, x0, x1, 0, 10, 1, 0) == 10, "slider: section 10 = 10");
  check(mouseUiSliderSteps(x0 + 17 * 2.0f + 0.01f, x0, x1, 0, 10, 1, 2) == 1,
        "slider: just past section 2's edge = 3");
  check(mouseUiSliderSteps(100, 0, 0, 0, 10, 1, 5) == 0, "slider: degenerate track = no steps");
  // Harness feed grammar (SR_MOUSE_FEED).
  {
    MouseFeedCmd c;
    check(mouseUiParseFeedLine("move 640.5 388", &c) && c.kind == 1 && c.x == 640.5f && c.y == 388,
          "feed: move x y");
    check(mouseUiParseFeedLine("down left", &c) && c.kind == 2 && c.button == SDL_BUTTON_LEFT &&
              mouseUiParseFeedLine("up right", &c) && c.kind == 3 && c.button == SDL_BUTTON_RIGHT,
          "feed: down / up buttons");
    check(mouseUiParseFeedLine("wheel -2", &c) && c.kind == 4 && c.x == -2, "feed: wheel n");
    check(!mouseUiParseFeedLine("move 640", &c) && !mouseUiParseFeedLine("move 9999 10", &c) &&
              !mouseUiParseFeedLine("down thumb", &c) && !mouseUiParseFeedLine("wheel 0", &c) &&
              !mouseUiParseFeedLine("click 1 2", &c) && !mouseUiParseFeedLine("", &c),
          "feed: malformed lines are refused");
  }
  // Navigation.
  MenuModel m;
  m.nav = kNavList; m.count = 9; m.selected = 0;
  check(navKey(m, 3) == kPadDown, "list: 0 -> 3 = DOWN");
  check(navKey(m, 8) == kPadUp, "list wraps: 0 -> 8 = UP");
  m.selected = 7;
  check(navKey(m, 1) == kPadDown, "list wraps: 7 -> 1 = DOWN");
  m.nav = kNavRow; m.count = 4; m.selected = 2;
  check(navKey(m, 0) == kPadLeft && navKey(m, 3) == kPadRight, "icon row: LEFT/RIGHT, no wrap");
  m.nav = kNavGrid; m.count = 4; m.selected = 5;    // row 1 col 1
  check(navKey(m, 7) == kPadRight, "grid: same row = RIGHT");
  check(navKey(m, 1) == kPadUp, "grid: same column = UP");
  check(navKey(m, 8) == kPadLeft, "grid: column first (8 = row 2 col 0) = LEFT");
  // Hover picks the nearest centre among overlapping rows (ScrGenPop golden rows).
  MenuModel p;
  p.n = 2;
  p.items[0].r = RectF{339.1f, 369.8f, 940.9f, 409.8f}; p.items[0].enabled = true; p.items[0].index = 0;
  p.items[1].r = RectF{339.1f, 401.8f, 940.9f, 441.8f}; p.items[1].enabled = true; p.items[1].index = 1;
  check(hoveredSlot(p, 600, 380) == 0, "hover: row 0");
  check(hoveredSlot(p, 600, 405) == 0 && hoveredSlot(p, 600, 408) == 1, "hover: overlap goes to the nearer row");
  check(hoveredSlot(p, 600, 430) == 1, "hover: row 1");
  check(hoveredSlot(p, 100, 430) == -1, "hover: outside");
  p.items[1].enabled = false;
  check(hoveredSlot(p, 600, 430) == -1, "hover: a disabled row is not hit");
  // The reader on a synthetic Apt heap (hash tables with deleted + uncached slots,
  // prototypes, arrays, the screen walk, matrices, bounds).
  MenuModel fm;
  check(fakeHeapTest(true, false, false, &fm) == 0 && fm.valid, "heap: snapshot valid");
  check(fm.active && std::strncmp(fm.what, "popup@L900", 10) == 0,
        "heap: the popup (last screen, not pass-through) takes the input");
  check(fm.n == 2 && fm.selected == 1 && fm.count == 2 && fm.nav == kNavList,
        "heap: two rows, selection 1, list navigation");
  // Rows: shape (0,-1)-(300,19) at (169.55,185.4)/(169.55,201.4) under a 2x parent.
  check(fm.n == 2 && std::fabs(fm.items[0].r.x0 - 339.1f) < 0.01f &&
            std::fabs(fm.items[0].r.y0 - 368.8f) < 0.01f &&
            std::fabs(fm.items[0].r.x1 - 939.1f) < 0.01f &&
            std::fabs(fm.items[1].r.y1 - 440.8f) < 0.01f,
        "heap: world bounds = parent(2x) . local (child first)");
  check(fakeHeapTest(false, false, false, &fm) == 0 && fm.active && fm.clickAnywhere &&
            std::strncmp(fm.what, "title@L50", 9) == 0,
        "heap: no popup -> FG passes through -> the title (click anywhere)");
  check(fakeHeapTest(false, true, false, &fm) == 0 && fm.valid && !fm.active,
        "heap: gameplay (BG/FG/HUD all pass through) -> no menu, cursor hidden");
  check(fakeHeapTest(true, false, true, &fm) == 0 && fm.active && fm.locked,
        "heap: an input lock is reported");
  // Host-built pages (Dev Tools / port settings) on the game's popup: value rows.
  check(fakeHeapTest(true, false, false, &fm, 1) == 0 && std::strncmp(fm.what, "page@L900", 9) == 0 &&
            fm.n == 2 && fm.items[0].kind == kItemButton && fm.items[1].kind == kItemValue,
        "heap: a host page's kinds[1] = 2 (slider) makes row 1 a value row, row 0 stays a button");
  check(fakeHeapTest(true, false, false, &fm, 2) == 0 && std::strncmp(fm.what, "page@L900", 9) == 0 &&
            fm.n == 2 && fm.items[1].kind == kItemValue,
        "heap: a sparse kinds (named member \"1\" = 1, toggle) is read too");
  check(fakeHeapTest(true, false, false, &fm, 3) == 0 && std::strncmp(fm.what, "popup@L900", 10) == 0 &&
            fm.items[0].kind == kItemButton && fm.items[1].kind == kItemButton,
        "heap: a popup context without kinds = plain popup (the front end's own panels)");
  // Lane E E1: the hint bar + the census line (M6).
  {
    std::string c1, c2, c3, c0, cg;
    fakeHeapTest(true, false, false, &fm, 3, 1, &c1);
    std::printf("      census: %s\n", c1.c_str());
    check(c1.find("screen=ScrGenPop page='MAIN MENU' what=popup@L900 cancel=1 items=2 sel=1") == 0,
          "M6a census: the screen class (mLinkageName), the popup's title, surface, canCancel, items, the focus");
    check(c1.find("hints=2 ['SELECT' (840,660)-(920,700)] ['BACK' (960,660)-(1040,700)]") != std::string::npos,
          "M6b census: mDisplayWell.mHintHolder's hints - LabelText.mText and world rects (no invented icon names)");
    fakeHeapTest(true, false, false, &fm, 0, 2, &c2);
    check(c2.find("page='-'") != std::string::npos && c2.find("cancel=-") != std::string::npos &&
              c2.find("hints=2 ['SELECT' (840,660)-(920,700)] ['BACK'") != std::string::npos,
          "M6c census: a ButtonHints clip found by the display-list walk (no mDisplayWell); no mParams = cancel '-'");
    fakeHeapTest(true, false, false, &fm, 0, 3, &c3);
    check(c3.find("['SELECT' (840,660)-(920,700)] ['BACK' hidden]") != std::string::npos,
          "M6d census: a hidden hint is listed as hidden (no rect)");
    fakeHeapTest(true, false, false, &fm, 0, 0, &c0);
    check(c0.find("hints=0") != std::string::npos && c0.find('[') == std::string::npos,
          "M6e census: no hint bar -> hints=0 (no invented entries)");
    fakeHeapTest(false, true, false, &fm, 0, 0, &cg);
    check(cg.find("screen=- page='-' what=- cancel=- items=0 sel=-1") == 0, "M6f census: gameplay = no menu");
    // M6g (Astra E1 r1): an absurd mHintCount is clamped before it becomes an int (the two real hints still read)
    std::string c4;
    fakeHeapTest(true, false, false, &fm, 3, 4, &c4);
    check(c4.find("hints=2 ['SELECT' (840,660)-(920,700)] ['BACK' (960,660)-(1040,700)]") != std::string::npos,
          "M6g census: mHintCount = 1e30 is clamped to 10 (no undefined conversion); the two hints read");
    // M6h (Astra E1 r1): a snapshot that ran out of read budget is incomplete (never logged or used for dedupe)
    bool full = false, part = true;
    std::string c5, c6;
    fakeHeapTest(true, false, false, &fm, 3, 1, &c5, 60000, &full);
    fakeHeapTest(true, false, false, &fm, 3, 1, &c6, 40, &part);
    check(full && !part, "M6h census: a budget-exhausted snapshot is reported incomplete; a full one complete");
    // M6i (Astra E1 r2): an unmapped read inside the census (a label pointing at unmapped memory) is incomplete too
    bool stale = true;
    std::string c7;
    fakeHeapTest(true, false, false, &fm, 3, 5, &c7, 60000, &stale);
    check(!stale, "M6i census: a read the map refuses (not budget, not null) makes the snapshot incomplete");
    // M6k (Astra E1 r3): a refused read in the MENU SCAN (the focus index the line reports) is incomplete too, not an
    // accepted sel=-1
    bool scanStale = true;
    std::string c8;
    fakeHeapTest(true, false, false, &fm, 3, 6, &c8, 60000, &scanStale);
    check(c8.find("sel=-1") != std::string::npos && !scanStale,
          "M6k census: an unreadable m_selectedItemIdx (a scan read) makes the snapshot incomplete");
    // M6l (Astra E1 r3): a non-null address the reader refuses by range (below the first page) counts as refused
    bool lowStale = true;
    std::string c9;
    fakeHeapTest(true, false, false, &fm, 3, 7, &c9, 60000, &lowStale);
    check(!lowStale, "M6l census: a garbage low pointer (refused by range, not by the map) makes it incomplete");
    // M6m (Astra E1 r4): a pointer whose base + offset wraps past 4 GB to 0 is refused and counted, not taken as null
    bool wrapStale = true;
    std::string c10;
    fakeHeapTest(true, false, false, &fm, 3, 8, &c10, 60000, &wrapStale);
    check(!wrapStale, "M6m census: 0xFFFFFFF8 + 8 (wraps to 0) makes the snapshot incomplete");
    // M6n (Astra E1 r4): a menu scan that fails because a read was refused (the global holder points at unmapped
    // memory) is an incomplete snapshot (counted, reported); one that finds no menu system yet is nothing to census
    {
      FakeHeap h;
      auto setGlobal = [&](uint32_t g) {
        h.globalCell[0] = uint8_t(g >> 24); h.globalCell[1] = uint8_t(g >> 16);
        h.globalCell[2] = uint8_t(g >> 8); h.globalCell[3] = uint8_t(g);
      };
      setGlobal(0x7F000000u);
      Reader rBad{&FakeHeap::map, &h};
      std::string lBad;
      bool cBad = true;
      const bool anyBad = censusSnapshot(rBad, &lBad, &cBad);
      setGlobal(0);
      Reader rNone{&FakeHeap::map, &h};
      std::string lNone;
      bool cNone = true;
      const bool anyNone = censusSnapshot(rNone, &lNone, &cNone);
      check(anyBad && !cBad && !anyNone,
            "M6n census: a scan failed by a refused read is an incomplete snapshot; no menu system yet is none");
    }
    // M6j (Astra E1 r2): the production acceptance rule - an incomplete line is never emitted and never remembered;
    // a complete one is emitted once per change
    {
      CensusState st;
      std::string prev;
      const bool e1 = censusAccept(st, true, "screen=A x", &prev);
      const bool e2 = censusAccept(st, false, "screen=B y", &prev);
      const bool e3 = censusAccept(st, true, "screen=A x", &prev);
      const bool e4 = censusAccept(st, true, "screen=B y", &prev);
      check(e1 && !e2 && st.incomplete == 1 && !e3 && e4 && st.last == "screen=B y" && prev == "A" && st.seq == 2,
            "M6j census: incomplete lines neither emit nor replace the remembered line; changes emit once");
    }
  }
  // Lane E E5: clickable hints. The custom front end keeps each ButtonIcon's name (65_buttonicon_id.patch); a click
  // on a hint sends that button's key through the game's router, once, and nothing under it.
  {
    check(hintIconKey("A") == kKeyCross && hintIconKey("b") == kKeyTriangle && hintIconKey("X") == kKeySquare &&
              hintIconKey("Y") == kKeyCircle && hintIconKey("Start") == kKeyStart &&
              hintIconKey("Select") == kKeySelect && hintIconKey("Dpad") == 0 && hintIconKey("") == 0 &&
              hintIconKey(nullptr) == 0 && hintIconKey("AB") == 0,
          "M7a hint keys: A CROSS, B TRIANGLE, X SQUARE, Y CIRCLE, START, SELECT; anything else none");
    MenuModel hm;
    std::string c11;
    fakeHeapTest(true, false, false, &hm, 3, 9, &c11);
    check(c11.find("hints=2 ['SELECT' icon=A (840,660)-(920,700)] ['BACK' icon=B (960,660)-(1040,700)]") !=
              std::string::npos,
          "M7b census: each hint names its button when the custom front end recorded it");
    check(hm.nHints == 2 && hm.hints[0].key == kKeyCross && hm.hints[1].key == kKeyTriangle &&
              hm.hints[1].r.x0 == 960 && hm.hints[1].r.y0 == 660 && hm.hints[1].r.x1 == 1040 && hm.hints[1].r.y1 == 700,
          "M7c snapshot: two clickable hints, A = CROSS over SELECT, B = TRIANGLE over BACK, at their stage rects");
    check(hintKeyAt(hm, 1000, 680) == kKeyTriangle && hintKeyAt(hm, 880, 680) == kKeyCross &&
              hintKeyAt(hm, 940, 680) == 0 && hintKeyAt(hm, 600, 390) == 0,
          "M7d hit test: on BACK = TRIANGLE, on SELECT = CROSS, between them / on a row = none");
    // M7k (E3-E6 r4): the PC roll's popup bar - label fields 366 stage px wide, so the hints' BOUNDS overlap (the
    // census keeps them) - gives clickable extents from each label's left to its icon slot's end (AddHint's layout:
    // Icon._x + 32), which do not overlap: BACK's centre is TRIANGLE, the r3 click point (891.6) SELECT
    {
      MenuModel wm;
      std::string cw;
      fakeHeapTest(true, false, false, &wm, 3, 14, &cw);
      const float cx = (wm.nHints == 2) ? (wm.hints[1].r.x0 + wm.hints[1].r.x1) / 2 : 0;
      check(wm.nHints == 2 && wm.hints[0].key == kKeyCross && wm.hints[0].r.x0 == 816 && wm.hints[0].r.x1 == 968 &&
                wm.hints[1].key == kKeyTriangle && wm.hints[1].r.x0 == 676 && wm.hints[1].r.x1 == 804 &&
                hintKeyAt(wm, cx, 680) == kKeyTriangle && hintKeyAt(wm, 891.6f, 680) == kKeyCross &&
                hintKeyAt(wm, 1000, 680) == 0 && hintKeyAt(wm, 810, 680) == 0,
            "M7k the PC roll's bar: SELECT 816..968 and BACK 676..804 (label left to icon end), no overlap; BACK's "
            "centre = TRIANGLE, 891.6 = SELECT, past SELECT's icon / in the spacing = none");
      check(cw.find("['SELECT' icon=A (816,660)-(1182,700)] ['BACK' icon=B (676,660)-(1042,700)]") != std::string::npos,
            "M7k the census keeps the game's own (overlapping) bounds");
      MenuModel am;   // two clickable rects that still overlap cannot be told apart: both dropped, a third kept
      am.nHints = 3;
      am.hints[0].r = RectF{100, 100, 300, 140};
      am.hints[0].key = kKeyCross;
      am.hints[1].r = RectF{250, 110, 400, 150};
      am.hints[1].key = kKeyTriangle;
      am.hints[2].r = RectF{100, 300, 300, 340};
      am.hints[2].key = kKeySquare;
      dropAmbiguousHints(&am);
      check(am.nHints == 1 && am.hints[0].key == kKeySquare && am.hints[0].r.x1 == 300,
            "M7k overlapping clickable rects are both dropped (never trimmed); a non-overlapping one is kept as is");
    }
    // M7l (E3-E6 r5, Astra r4 P2): the extent is cut in the hint's own space and then transformed. A hint whose label
    // field is 0..183 x 0..20 local with its Icon at _x 44 (the cut at 76): scaled 2 at (816,660) -> 816..968; a
    // reflected one (x' = 1000 - 2x) keeps the label's side 848..1000 (the world-x cut gave 634..848); one rotated 90
    // degrees loses the authored excess on y (100..252, not 100..466); an Icon that is not a clip, or an icon slot
    // ending left of the label: not clickable
    {
      FakeHeap h;
      const float field[4] = {0, 0, 183, 20};
      auto mk = [&](float a, float b, float c, float d, float tx, float ty, bool iconClip, float iconX,
                    bool iconNaN = false, uint32_t parent = 0) {
        const uint32_t hint = h.clip(parent, 1, 0, 0, 1, true, field, {});
        const uint32_t m = h.u32(h.u32(h.u32(hint + 0x20) + 4) + 8);
        h.wf(m, a); h.wf(m + 4, b); h.wf(m + 8, c); h.wf(m + 12, d); h.wf(m + 16, tx); h.wf(m + 20, ty);
        const uint32_t ic = iconClip ? h.clip(hint, 5, iconX, 0, 1, true, nullptr, {}) : h.obj(kTObject, {});
        if (iconNaN) h.wf(h.u32(h.u32(h.u32(ic + 0x20) + 4) + 8) + 12, std::nanf(""));
        h.table(h.u32(h.u32(hint + 0x20) + 12), {{"Icon", ic}});
        return hint;
      };
      const uint32_t badParent = h.clip(0, 5, 0, 0, 1, true, nullptr, {});
      h.wf(h.u32(h.u32(h.u32(badParent + 0x20) + 4) + 8) + 16, std::nanf(""));
      const uint32_t plain = mk(2, 0, 0, 2, 816, 660, true, 44), mirror = mk(-2, 0, 0, 2, 1000, 660, true, 44),
                     turned = mk(0, 2, -2, 0, 500, 100, true, 44), noIcon = mk(2, 0, 0, 2, 816, 660, false, 44),
                     leftIcon = mk(2, 0, 0, 2, 816, 660, true, -40), flat = mk(2, 0, 0, 0, 816, 660, true, 44),
                     nanM = mk(std::nanf(""), 0, 0, 2, 816, 660, true, 44),
                     nanIcon = mk(2, 0, 0, 2, 816, 660, true, 44, true),
                     nanAbove = mk(2, 0, 0, 2, 816, 660, true, 44, false, badParent);
      Reader rd{&FakeHeap::map, &h};
      auto ext = [&](uint32_t w) { return hintExtent(rd, w, worldBounds(rd, w)); };
      const RectF p = ext(plain), q = ext(mirror), t = ext(turned);
      check(p.x0 == 816 && p.x1 == 968 && p.y0 == 660 && p.y1 == 700,
            "M7l the extent of an upright hint: 816..968 x 660..700 (label left to icon end)");
      check(q.x0 == 848 && q.x1 == 1000 && q.y0 == 660 && q.y1 == 700,
            "M7l a reflected hint keeps the label's side: 848..1000 (cut in local space, then transformed)");
      check(t.x0 == 460 && t.x1 == 500 && t.y0 == 100 && t.y1 == 252,
            "M7l a hint rotated 90 degrees: 460..500 x 100..252 (the authored excess is cut on y too)");
      check(ext(noIcon).empty() && ext(leftIcon).empty(),
            "M7l an Icon that is not a clip, or an icon slot ending left of the label: not clickable");
      // E3-E6 r6 (Astra r5 P2): a = b = c = d = 1 (singular, a diagonal line) and a 45-degree turn: not clickable
      const uint32_t diag = mk(1, 1, 1, 1, 500, 100, true, 44), turn45 = mk(0.7071068f, 0.7071068f, -0.7071068f,
                                                                           0.7071068f, 500, 100, true, 44);
      // a SPRITE hint (its bounds come from its children): the label field as a display-list child; with a NaN in
      // the label's matrix (localMat: identity) it is withheld, with a sound one it is the upright extent
      auto sprite = [&](bool nanLabel) {
        const uint32_t sh = h.clip(0, 5, 816, 660, 2, true, nullptr, {});
        const uint32_t lab = h.clip(sh, 1, 0, 0, 1, true, field, {});
        if (nanLabel) h.wf(h.u32(h.u32(h.u32(lab + 0x20) + 4) + 8) + 16, std::nanf(""));
        h.displayChild(sh, lab);
        const uint32_t ic = h.clip(sh, 5, 44, 0, 1, true, nullptr, {});
        h.table(h.u32(h.u32(sh + 0x20) + 12), {{"Icon", ic}, {"LabelText", lab}});
        return sh;
      };
      const uint32_t spriteOk = sprite(false), spriteNaN = sprite(true);
      const RectF so = ext(spriteOk);
      // and float noise off the axes (b = 1e-6 at scale 2) is still upright: 816..968
      const uint32_t noisy = mk(2, 1e-6f, -1e-6f, 2, 816, 660, true, 44);
      const RectF nz = ext(noisy);
      check(ext(diag).empty() && hintExtent(rd, diag, RectF{0, 0, 2000, 2000}).empty() &&
                hintExtent(rd, turn45, RectF{0, 0, 2000, 2000}).empty() && nz.x0 > 815.9f && nz.x1 < 968.1f &&
                nz.x1 > 967.9f,
            "M7l a = b = c = d = 1 (a diagonal line) and a 45-degree turn, given visible bounds: not clickable; float "
            "noise off the axes is still the upright extent");
      check(so.x0 == 816 && so.x1 == 968 && so.y0 == 660 && so.y1 == 700 &&
                hintExtent(rd, spriteNaN, RectF{0, 0, 2000, 2000}).empty(),
            "M7l a sprite hint: its label child's bounds give 816..968 x 660..700; a NaN in that child's matrix "
            "withholds the hint");
      check(ext(flat).empty() && hintExtent(rd, nanM, RectF{0, 0, 2000, 2000}).empty() &&
                hintExtent(rd, nanIcon, RectF{0, 0, 2000, 2000}).empty() &&
                hintExtent(rd, nanAbove, RectF{0, 0, 2000, 2000}).empty(),
            "M7l a singular matrix (a line), or a NaN in the hint's, its Icon's or an ancestor's matrix (read as "
            "identity by localMat; given visible bounds): not clickable");
    }
    MenuModel nm;
    fakeHeapTest(true, false, false, &nm, 3, 1);
    check(nm.nHints == 0 && !nm.hintsUnknown,
          "M7e no recorded icon (retail ButtonIcon, port features off): no clickable hint");
    // M7g (Astra E5 r1): one refused read in the hint collection (BACK's name) withholds EVERY hint, SELECT too
    MenuModel rm;
    fakeHeapTest(true, false, false, &rm, 3, 10);
    check(rm.valid && rm.hintsUnknown && rm.nHints == 0,
          "M7g a refused read while collecting hints: no hint is published (partial hints are never clickable)");
    // M7f: decide() - a BACK hint click is TRIANGLE exactly once (the next pump does nothing), never CROSS; a click on
    // a row still focuses / confirms it.
    {
      std::lock_guard<std::mutex> lk(g_mu);
      // Astra E5 r1: every global decide() reads is saved, set and restored (relative mode off, the feed on)
      const bool feedWas = g_feedActive.load(), relWas = g_relative.load();
      const Driver drvWas = g_drv;
      const MouseState mouseWas = g_mouse;
      const MenuModel modelWas = g_model;
      g_feedActive.store(true);
      g_relative.store(false);
      const bool pre = !modlib::isOpen() && focusedForMouse();
      g_drv = Driver{};
      g_model = hm;
      g_model.t = nowSec();
      g_mouse = MouseState{};
      g_mouse.awake = true;
      g_mouse.inside = true;
      g_mouse.winW = 1280;
      g_mouse.winH = 720;
      g_mouse.wx = 1000;
      g_mouse.wy = 680;
      g_mouse.lClicks = 1;
      const Plan p1 = decide();
      const Plan p2 = decide();
      const MenuItem& row = hm.items[slotOfIndex(hm, hm.selected)];
      g_drv = Driver{};
      g_mouse.wx = 0.5f * (row.r.x0 + row.r.x1);
      g_mouse.wy = 0.5f * (row.r.y0 + row.r.y1);
      g_mouse.lClicks = 1;
      g_model.t = nowSec();
      const Plan p3 = decide();
      check(pre && p1.kind == Plan::kEvent && p1.key == kKeyTriangle && std::strcmp(p1.why, "hint click") == 0 &&
                p2.kind == Plan::kNone && p3.kind == Plan::kEvent && p3.key == kKeyCross,
            "M7f click: BACK hint = TRIANGLE once (nothing on the next pump, no CROSS); a click on the row confirms it");
      // M7i (lane E E5b, owner Q1 (a); Astra E5b r1-r7): the hover glow. The guest owns it (66_parttext_glow.patch:
      // kGlowOn records the label's own colour on the label, kGlowRestore puts back only that object's record); the
      // host decides from this pump's clean-path membership alone. The decision table first.
      {
        using GA = GlowAct;
        const Glow none{}, on{0x40001000u, 0, 0};
        check(glowDecide(none, 0x40002000u, Tri::No, true) == GA::On &&
                  glowDecide(none, 0x40002000u, Tri::No, false) == GA::None &&   // another call / a lock: later
                  glowDecide(none, 0, Tri::No, true) == GA::None &&
                  glowDecide(on, on.label, Tri::Yes, true) == GA::None &&       // still hovered and still there
                  glowDecide(on, 0, Tri::Yes, false) == GA::Off &&              // off goes even with a main call
                  glowDecide(on, 0x40002000u, Tri::Yes, true) == GA::Off &&     // another hint: this one off first
                  glowDecide(on, on.label, Tri::No, true) == GA::Drop &&        // gone: nothing to restore
                  glowDecide(on, 0, Tri::No, true) == GA::Drop &&
                  glowDecide(on, 0, Tri::Unknown, true) == GA::None,            // cannot judge: keep
              "M7i glow decisions: on only without another call; off first; gone = forget; unknown = wait");
        // the membership the host trusts: a bar label on a clean path of the stack
        struct Seen {
          Tri backOn = Tri::Unknown, bogusOn = Tri::Unknown, tinyOn = Tri::No;
        };
        Seen s9;
        auto probe = [](const Reader& fresh, const MenuModel& pm, void* u) {
          Seen& s = *static_cast<Seen*>(u);
          if (pm.nHints < 2) return;
          Reader a = fresh, b = fresh, c = fresh;
          s.backOn = labelOnAnyScreen(a, pm.hints[1].label);
          s.bogusOn = labelOnAnyScreen(b, pm.screen);        // a live object that is no hint label
          c.budget = 20;
          s.tinyOn = labelOnAnyScreen(c, pm.hints[1].label);   // the reads ran out: cannot judge
        };
        fakeHeapTest(true, false, false, nullptr, 3, 9, nullptr, 60000, nullptr, probe, &s9);
        check(s9.backOn == Tri::Yes && s9.bogusOn == Tri::No && s9.tinyOn == Tri::Unknown,
              "M7i glow membership: a bar label is found on the stack, another object is not, a short read cannot judge");
        // labelHasGlowRecord on the fixture heap: the patch's srOwn present / absent / behind an unreadable entry
        struct SeenR {
          Tri none = Tri::Unknown, rec = Tri::Unknown, hidden = Tri::Yes;
        };
        SeenR sr;
        auto probeR = [](const Reader& fresh, const MenuModel&, void* u) {
          SeenR& s = *static_cast<SeenR*>(u);
          const uint32_t L = t_hintLabels[0];
          if (!t_heap || !L) return;
          Reader a = fresh;
          s.none = labelHasGlowRecord(a, L);
          t_heap->table(L + 8, {{"mText", t_heap->strv("SELECT")}, {"mFontColor", t_heap->intv(int(kGlowColor))},
                                {"srOwn", t_heap->intv(0xFFFFFF)}});
          Reader b = fresh;
          s.rec = labelHasGlowRecord(b, L);
          t_heap->table(L + 8, {{"srUnreadable", t_heap->intv(0)}, {"mText", t_heap->strv("SELECT")}});
          t_heap->w32(t_heap->u32(L + 8 + 4) + 8 * 1, 0x7F000000u);
          Reader c = fresh;
          s.hidden = labelHasGlowRecord(c, L);
        };
        fakeHeapTest(true, false, false, nullptr, 3, 9, nullptr, 60000, nullptr, probeR, &sr);
        check(sr.none == Tri::No && sr.rec == Tri::Yes && sr.hidden == Tri::Unknown,
              "M7i glow record: the patch's srOwn read as present / absent; an unreadable entry cannot judge");
        // M7i (Astra E5b r2-r6): identity reads only, top screen first, stopping at the label with nothing read after
        // it; a label on a COVERED screen is found and restored while covered; refusals elsewhere do not block it; a
        // refusal on the path (an unreadable own entry that could override an inherited value; gSM's own table) does
        struct Seen2 {
          Tri selOn = Tri::Unknown, bogusOn = Tri::Unknown, coveredOn = Tri::Unknown, boundaryOn = Tri::Unknown;
          int topReads = -1, bogusReads = -1, coveredReads = -1, matchReads = -2;
          bool coveredNotHint = false, coveredRefused = false;
          GlowStep restore;
          Glow after{1, 0, 0};
          int calls = 0;
          uint32_t covered = 0, calledLabel = 0;
          int32_t calledValue = 0;
        };
        Seen2 s10, s12, s13, s15, s16;
        auto probe2 = [](const Reader& fresh, const MenuModel& pm, void* u) {
          Seen2& s = *static_cast<Seen2*>(u);
          Reader a = fresh, b = fresh, c = fresh;
          s.selOn = labelOnAnyScreen(a, t_hintLabels[0]);
          s.topReads = 60000 - a.budget;
          s.bogusOn = labelOnAnyScreen(b, 0x7E000000u);   // no hint's label: the whole stack is walked
          s.bogusReads = 60000 - b.budget;
          if (!t_coveredLabel) return;
          s.covered = t_coveredLabel;
          s.coveredOn = labelOnAnyScreen(c, t_coveredLabel);
          s.coveredReads = 60000 - c.budget;
          s.matchReads = 60000 - g_glowMatchBudget;   // the reads up to the match itself
          s.coveredRefused = c.refused > 0;            // hintMode 13: the walk met a refusal before it found the label
          {
            Reader bnd = fresh;
            bnd.budget = s.coveredReads;   // exactly the reads the match needs
            s.boundaryOn = labelOnAnyScreen(bnd, t_coveredLabel);
          }
          s.coveredNotHint = pm.nHints == 2;
          for (int i = 0; i < pm.nHints; ++i)
            if (pm.hints[i].label == t_coveredLabel) s.coveredNotHint = false;
          struct FixIo {
            Reader proto;
            Seen2* s;
          } fx{fresh, &s};
          GlowIo io;
          io.user = &fx;
          io.standing = [](void* v, uint32_t l) {
            Reader r = static_cast<FixIo*>(v)->proto;
            return labelOnAnyScreen(r, l);
          };
          io.call = [](void* v, uint32_t l, int32_t value) {
            Seen2& t = *static_cast<FixIo*>(v)->s;
            ++t.calls;
            t.calledLabel = l;
            t.calledValue = value;
            return true;
          };
          Glow g{t_coveredLabel, 0, 0};
          s.restore = glowStep(g, 0, true, 1, io);   // no longer hovered (the popup is up): restore it
          s.after = g;
        };
        fakeHeapTest(true, false, false, nullptr, 3, 10, nullptr, 60000, nullptr, probe2, &s10);
        fakeHeapTest(true, false, false, nullptr, 3, 12, nullptr, 60000, nullptr, probe2, &s12);
        fakeHeapTest(true, false, false, nullptr, 3, 13, nullptr, 60000, nullptr, probe2, &s13);
        fakeHeapTest(true, false, false, nullptr, 3, 15, nullptr, 60000, nullptr, probe2, &s15);
        fakeHeapTest(true, false, false, nullptr, 3, 16, nullptr, 60000, nullptr, probe2, &s16);
        check(s10.selOn == Tri::Yes && s10.bogusOn == Tri::No && s12.selOn == Tri::Yes && s12.bogusOn == Tri::No &&
                  s12.topReads > 0 && s12.topReads < s12.coveredReads && s12.coveredReads < s12.bogusReads,
              "M7i glow membership: identity reads only (an unrelated refused icon read: still judged); the top "
              "screen first, stopping at the label");
        check(s12.coveredReads > 0 && s12.matchReads == s12.coveredReads && s13.matchReads == s13.coveredReads &&
                  s12.boundaryOn == Tri::Yes,
              "M7i glow membership: nothing is read after the match (a budget that ends at the match still judges)");
        check(s12.coveredOn == Tri::Yes && s12.coveredNotHint && s12.restore.act == GlowAct::Off && s12.restore.called &&
                  s12.restore.ok && s12.after.label == 0 && s12.calls == 1 && s12.covered &&
                  s12.calledLabel == s12.covered && s12.calledValue == kGlowRestore,
              "M7i glow on a covered screen: found under the popup and restored while covered (one restore call)");
        check(s13.coveredRefused && s13.coveredOn == Tri::Yes && s13.restore.act == GlowAct::Off && s13.restore.ok &&
                  s13.after.label == 0 && s13.calls == 1 && s13.calledLabel == s13.covered,
              "M7i glow on a covered screen past a refused read: the walk's refusal elsewhere does not block it");
        check(s15.coveredOn == Tri::Unknown && s15.restore.act == GlowAct::None && !s15.restore.called &&
                  s15.calls == 0 && s15.after.label == s15.covered,
              "M7i glow membership on an unclean path (an unreadable own entry could override the inherited "
              "mHintCount): unknown, no call, the glow kept");
        check(s16.coveredOn == Tri::Unknown && s16.restore.act == GlowAct::None && s16.calls == 0 &&
                  s16.after.label == s16.covered,
              "M7i glow membership past a refused read of the screen stack itself (gSM): unknown, no call");
        // M7i (Astra E5b r6-r7): the host over a heap that changes between glow pumps, with the GUEST modelled as the
        // patched PartText.SetFontColor (66_parttext_glow.patch): kGlowOn keeps the own colour on the object once and
        // shows the gold; kGlowRestore puts back only that object's record; any other value voids the record. The
        // record lives in the object (here: a map keyed by object, erased when the test replaces the object).
        struct Seen3 {
          bool on1 = false, gold1 = false, sameKept = false, off1 = false, white1 = false;
          bool replGoldStays = false, recolourStands = false, retextRestored = false, goneDropped = false;
          bool hoveredReplOn = false, hoveredReplRestored = false, steadyNoCall = false, fightBounded = false;
          bool fightReset = false, altBounded = false, readsEveryPump = false;
          int calls = 0, restoresNoop = 0, reads = 0;
        };
        Seen3 s3;
        auto probe3 = [](const Reader& fresh, const MenuModel&, void* u) {
          Seen3& s = *static_cast<Seen3*>(u);
          const uint32_t L = t_hintLabels[0];
          if (!t_heap || !L) return;
          struct Guest {
            Reader proto;
            Seen3* s;
            std::map<uint32_t, uint32_t> srOwn;   // the patch's this.srOwn, per object
          } gx{fresh, &s, {}};
          auto colourOf = [&](uint32_t l) {
            Reader r = fresh;
            double v = -1;
            return asNumber(r, prop(r, l, "mFontColor"), &v) ? uint32_t(v) : 0xFFFFFFFFu;
          };
          auto setColour = [](Guest& gu, uint32_t l, uint32_t c) {
            Reader r = gu.proto;
            const uint32_t pv = prop(r, l, "mFontColor");
            if (pv) t_heap->w32(pv + 8, c);
          };
          GlowIo io;
          io.user = &gx;
          io.standing = [](void* v, uint32_t l) {
            Reader r = static_cast<Guest*>(v)->proto;
            return labelOnAnyScreen(r, l);
          };
          io.glowing = [](void* v, uint32_t l) {   // the record the model keeps for the object
            Guest& gu = *static_cast<Guest*>(v);
            return gu.srOwn.count(l) ? Tri::Yes : Tri::No;
          };
          io.call = [](void* v, uint32_t l, int32_t value) {   // the patched SetFontColor(value)
            Guest& gu = *static_cast<Guest*>(v);
            ++gu.s->calls;
            Reader r = gu.proto;
            double cur = -1;
            asNumber(r, prop(r, l, "mFontColor"), &cur);
            auto set = [&](uint32_t c) {
              const uint32_t pv = prop(r, l, "mFontColor");
              if (pv) t_heap->w32(pv + 8, c);
            };
            if (value == kGlowOn) {
              if (!gu.srOwn.count(l)) gu.srOwn[l] = uint32_t(cur);
              set(kGlowColor);
            } else if (value == kGlowRestore) {
              auto it = gu.srOwn.find(l);
              if (it == gu.srOwn.end()) {
                ++gu.s->restoresNoop;
                return true;   // nothing of ours on this object: nothing changes
              }
              set(it->second);
              gu.srOwn.erase(it);
            } else {
              set(uint32_t(value));
              gu.srOwn.erase(l);
            }
            return true;
          };
          auto replace = [&](uint32_t colour) {   // a new PartText at the same address: no record of ours
            t_heap->table(L + 8, {{"mText", t_heap->strv("SELECT")}, {"mFontColor", t_heap->intv(int(colour))},
                                  {"mTextField", t_heap->obj(kTObject, {})}});
            gx.srOwn.erase(L);
          };
          Glow g;
          uint64_t pump = 0;
          const GlowStep a = glowStep(g, L, true, ++pump, io);
          s.on1 = a.act == GlowAct::On && a.ok && g.label == L;
          s.gold1 = colourOf(L) == kGlowColor;
          const GlowStep same = glowStep(g, L, true, ++pump, io);
          s.sameKept = same.act == GlowAct::None && !same.called && g.label == L;
          const int steadyCalls = s.calls;
          for (int i = 0; i < 5; ++i) glowStep(g, L, true, ++pump, io);   // hovered and carrying its record: no call
          s.steadyNoCall = s.calls == steadyCalls && g.label == L;
          const GlowStep b = glowStep(g, 0, true, ++pump, io);
          s.off1 = b.act == GlowAct::Off && b.ok && g.label == 0;
          s.white1 = colourOf(L) == 0xFFFFFFu;
          // a new object at the same address that happens to show our gold: the restore leaves it gold
          glowStep(g, L, true, ++pump, io);
          replace(kGlowColor);
          const GlowStep c = glowStep(g, 0, true, ++pump, io);
          s.replGoldStays = c.act == GlowAct::Off && c.ok && g.label == 0 && colourOf(L) == kGlowColor &&
                            s.restoresNoop == 1;
          // the game recolours the label while it glows: its colour stands
          replace(0xFFFFFFu);
          glowStep(g, L, true, ++pump, io);
          io.call(&gx, L, 0x808080);   // the game's own SetFontColor
          glowStep(g, 0, true, ++pump, io);
          s.recolourStands = colourOf(L) == 0x808080u && s.restoresNoop == 2 && g.label == 0;
          // SetText on the same object while it glows: still its record, its own colour comes back
          setColour(gx, L, 0xFFFFFFu);
          glowStep(g, L, true, ++pump, io);
          {
            Reader r = fresh;
            const uint32_t field = prop(r, L, "mTextField"), colour = prop(r, L, "mFontColor");
            t_heap->table(L + 8, {{"mText", t_heap->strv("BUY")}, {"mFontColor", colour}, {"mTextField", field}});
          }
          glowStep(g, 0, true, ++pump, io);
          s.retextRestored = colourOf(L) == 0xFFFFFFu && g.label == 0;
          // the label leaves the bar while it glows: gone, dropped with no call
          glowStep(g, L, true, ++pump, io);
          const int before = s.calls;
          {
            Reader r = fresh;
            const uint32_t holder = [&] {
              uint32_t h0 = 0;
              const uint32_t G = r.u32or0(kGlobalHolder);
              const uint32_t sm = G ? prop(r, G, "gSM") : 0;
              const std::vector<uint32_t> screens = asArray(r, sm ? prop(r, sm, "mScreens") : 0, 32);
              const uint32_t mc = screens.empty() ? 0 : prop(r, screens.back(), "mMovieClip");
              const uint32_t so = mc ? prop(r, mc, "Screen") : 0;
              forEachHintHolder(r, G, so, [&](uint32_t hd) {
                h0 = hd;
                return true;
              });
              return h0;
            }();
            const uint32_t hint0 = holder ? prop(r, holder, "Hint0") : 0;
            if (hint0) t_heap->table(t_heap->u32(t_heap->u32(hint0 + 0x20) + 12), {{"LabelText", 0x40000010u}});
          }
          const GlowStep gone = glowStep(g, 0, true, ++pump, io);
          s.goneDropped = gone.act == GlowAct::Drop && !gone.called && s.calls == before && g.label == 0;
        };
        // the label replaced at the same address WHILE STILL HOVERED (Astra E5b r8): the new object has no record, so it
        // gets the on again; leaving restores its own colour
        Seen3 s3b;
        auto probe3b = [](const Reader& fresh, const MenuModel&, void* u) {
          Seen3& s = *static_cast<Seen3*>(u);
          const uint32_t L = t_hintLabels[0];
          if (!t_heap || !L) return;
          struct Guest {
            Reader proto;
            Seen3* s;
            std::map<uint32_t, uint32_t> srOwn;
          } gx{fresh, &s, {}};
          auto colourOf = [&](uint32_t l) {
            Reader r = fresh;
            double v = -1;
            return asNumber(r, prop(r, l, "mFontColor"), &v) ? uint32_t(v) : 0xFFFFFFFFu;
          };
          GlowIo io;
          io.user = &gx;
          io.standing = [](void* v, uint32_t l) {
            Reader r = static_cast<Guest*>(v)->proto;
            return labelOnAnyScreen(r, l);
          };
          io.glowing = [](void* v, uint32_t l) {
            Guest& gu = *static_cast<Guest*>(v);
            ++gu.s->reads;
            return gu.srOwn.count(l) ? Tri::Yes : Tri::No;
          };
          io.call = [](void* v, uint32_t l, int32_t value) {
            Guest& gu = *static_cast<Guest*>(v);
            ++gu.s->calls;
            Reader r = gu.proto;
            double cur = -1;
            asNumber(r, prop(r, l, "mFontColor"), &cur);
            const uint32_t pv = prop(r, l, "mFontColor");
            if (value == kGlowOn) {
              if (!gu.srOwn.count(l)) gu.srOwn[l] = uint32_t(cur);
              if (pv) t_heap->w32(pv + 8, kGlowColor);
            } else if (value == kGlowRestore) {
              auto it = gu.srOwn.find(l);
              if (it == gu.srOwn.end()) return true;
              if (pv) t_heap->w32(pv + 8, it->second);
              gu.srOwn.erase(it);
            }
            return true;
          };
          Glow g;
          uint64_t pump = 0;
          glowStep(g, L, true, ++pump, io);
          t_heap->table(L + 8, {{"mText", t_heap->strv("SELECT")}, {"mFontColor", t_heap->intv(0xC0C0C0)},
                                {"mTextField", t_heap->obj(kTObject, {})}});
          gx.srOwn.erase(L);   // a new object: no record of ours
          const GlowStep held = glowStep(g, L, false, ++pump, io);   // another call this pump: not yet
          const GlowStep re = glowStep(g, L, true, ++pump, io);
          s.hoveredReplOn = !held.called && re.act == GlowAct::On && re.called && re.ok && g.label == L &&
                            colourOf(L) == kGlowColor && gx.srOwn.count(L) && gx.srOwn[L] == 0xC0C0C0u;
          glowStep(g, 0, true, ++pump, io);
          s.hoveredReplRestored = colourOf(L) == 0xC0C0C0u && g.label == 0;
          // the game recolours the hovered label every pump (its own SetFontColor voids the record each time): the
          // cursor re-sends its on after 2, 4, ... 64 pumps, not every pump
          glowStep(g, L, true, ++pump, io);
          const int callsBefore = s.calls;
          std::vector<uint64_t> ons;
          for (int i = 0; i < 300; ++i) {
            gx.srOwn.erase(L);   // the game's SetFontColor(other): the record is void
            const GlowStep st = glowStep(g, L, true, ++pump, io);
            if (st.called) ons.push_back(pump);
          }
          const uint64_t kGaps[6] = {2, 4, 8, 16, 32, 64};
          bool gaps = ons.size() >= 7;
          for (size_t i = 1; gaps && i < 7; ++i) gaps = ons[i] - ons[i - 1] == kGaps[i - 1];
          s.fightBounded = gaps && s.calls - callsBefore == int(ons.size()) && ons.size() <= 10;
          // the game stops: the record holds (after kGlowHeldReset pumps in a row the wait is reset); a later single
          // recolour is re-glowed at once, the next after 2 pumps
          for (int i = 0; i < 64 + 2 * kGlowHeldReset; ++i) glowStep(g, L, true, ++pump, io);
          std::vector<uint64_t> later;
          for (int i = 0; i < 10; ++i) {
            gx.srOwn.erase(L);
            const GlowStep st = glowStep(g, L, true, ++pump, io);
            if (st.called) later.push_back(pump);
          }
          s.fightReset = later.size() >= 2 && later[1] - later[0] == 2;
          // a game that recolours it every OTHER pump (Astra E5b r9): the record is seen between its recolours, but
          // never for kGlowHeldReset pumps in a row, so the waits keep growing to 64 and are never reset
          for (int i = 0; i < 64 + 2 * kGlowHeldReset; ++i) glowStep(g, L, true, ++pump, io);   // settle (reset)
          const int callsAlt = s.calls, readsAlt = s.reads;
          std::vector<uint64_t> alt;
          for (int i = 0; i < 600; ++i) {
            if (i % 2 == 0) gx.srOwn.erase(L);
            const GlowStep st = glowStep(g, L, true, ++pump, io);
            if (st.called) alt.push_back(pump);
          }
          bool grows = alt.size() >= 8 && alt.size() <= 16 && s.calls - callsAlt == int(alt.size());
          for (size_t i = 7; grows && i < alt.size(); ++i) grows = alt[i] - alt[i - 1] >= 64;
          s.altBounded = grows;
          // the record is read on every hovered pump, also while a re-sent on waits (only the calls wait)
          s.readsEveryPump = s.reads - readsAlt == 600;
        };
        fakeHeapTest(true, false, false, nullptr, 3, 9, nullptr, 60000, nullptr, probe3b, &s3b);
        fakeHeapTest(true, false, false, nullptr, 3, 9, nullptr, 60000, nullptr, probe3, &s3);
        check(s3.on1 && s3.gold1 && s3.sameKept && s3.off1 && s3.white1,
              "M7i glow on the fixture heap: on (gold shown), kept while hovered, off puts the own white back");
        check(s3.replGoldStays && s3.recolourStands && s3.retextRestored,
              "M7i glow vs the guest's record: a new object at the same address keeps its gold, a game recolour stands, "
              "SetText on the same object still restores");
        check(s3.goneDropped, "M7i glow: a label that left every bar is dropped with no call");
        check(s3.steadyNoCall && s3b.hoveredReplOn && s3b.hoveredReplRestored,
              "M7i glow vs a new object at the same address while still hovered: it gets the on again (not beside "
              "another call) and leaving restores ITS colour; a steady glow makes no call");
        check(s3b.fightBounded && s3b.fightReset,
              "M7i glow vs a game that recolours the hovered label every pump: re-sent ons wait 2, 4, ... 64 pumps; "
              "once the record has held for 64 pumps in a row the wait starts over");
        check(s3b.altBounded,
              "M7i glow vs a game that recolours the hovered label every other pump: the waits still grow to 64 pumps "
              "(the record seen between recolours does not reset them)");
        check(s3b.readsEveryPump,
              "M7i glow: the guest's record is read on every hovered pump, also during a re-apply wait (only calls wait)");
        // M7i (Astra E5b r10): the exact re-apply reset threshold, on a scripted record: 63 Yes in a row do not reset
        // the wait, the 64th does; a No in between starts the count over; held saturates (no overflow)
        {
          struct HeldScript {
            Tri rec = Tri::Yes;
            int calls = 0;
          } hs;
          GlowIo hio;
          hio.user = &hs;
          hio.standing = [](void*, uint32_t) { return Tri::Yes; };
          hio.glowing = [](void* v, uint32_t) { return static_cast<HeldScript*>(v)->rec; };
          hio.call = [](void* v, uint32_t, int32_t) {
            ++static_cast<HeldScript*>(v)->calls;
            return true;
          };
          const uint32_t LH = 0x40007000u;
          uint64_t hp = 0;
          Glow hg;
          hg.label = LH;
          hg.reapplies = 5;
          hg.reapplyAt = 0;
          hs.rec = Tri::Yes;
          for (int i = 0; i < 63; ++i) glowStep(hg, LH, true, ++hp, hio);   // literal (Astra E5b r11 P3)
          const bool no63 = hg.reapplies == 5 && hg.held == 63;
          glowStep(hg, LH, true, ++hp, hio);
          const bool at64 = hg.reapplies == 0 && hg.reapplyAt == 0 && hg.held == 64;
          for (int i = 0; i < 100000; ++i) glowStep(hg, LH, true, ++hp, hio);
          const bool saturated = hg.held == 64 && hs.calls == 0;
          // a No in between: 40 Yes, one No (a due re-apply), 40 Yes -> not 64 in a row: no reset
          hg.reapplies = 5;
          hg.reapplyAt = 0;
          hg.held = 0;
          for (int i = 0; i < 40; ++i) glowStep(hg, LH, true, ++hp, hio);
          hs.rec = Tri::No;
          const GlowStep mid = glowStep(hg, LH, true, ++hp, hio);
          hs.rec = Tri::Yes;
          for (int i = 0; i < 40; ++i) glowStep(hg, LH, true, ++hp, hio);
          const bool restarted = mid.called && hg.reapplies == 6 && hg.held == 40 && hs.calls == 1;
          check(no63 && at64 && saturated && restarted,
                "M7i glow re-apply reset: not after 63 records in a row, at the 64th; a No starts the count over; the "
                "count saturates (no overflow over 100000 pumps)");
          // E5b r13 (Astra E5b r11 P2): a game that voids the record on every pump - the re-sent ons saturate at 6
          // (waits of 64 from then on) over 100000 pumps; nothing overflows
          Glow rg;
          rg.label = LH;
          hs.rec = Tri::No;
          hs.calls = 0;
          std::vector<uint64_t> onAt;
          for (int i = 0; i < 100000; ++i) {
            const GlowStep st = glowStep(rg, LH, true, ++hp, hio);
            if (st.called) onAt.push_back(hp);
          }
          bool gaps64 = onAt.size() > 10;
          for (size_t i = 7; gaps64 && i < onAt.size(); ++i) gaps64 = onAt[i] - onAt[i - 1] == 64;
          check(rg.reapplies == 6 && gaps64 && hs.calls == int(onAt.size()),
                "M7i glow re-applies saturate: a record voided every pump keeps reapplies at 6 and the waits at 64 "
                "pumps over 100000 pumps (no overflow)");
          hs.rec = Tri::Yes;
        }
        // M7i (Astra E5b r2-r6): whole pump sequences through glowStep's injected boundary
        struct Script {
          Tri standing = Tri::Yes;
          int refuse = 0;   // refuse the next N calls (callMethod false: nothing was called)
          int calls = 0, reads = 0;
          uint32_t lastLabel = 0;
          int32_t lastValue = 0;
        };
        Script sc;
        GlowIo sio;
        sio.user = &sc;
        sio.standing = [](void* v, uint32_t) {
          Script& t = *static_cast<Script*>(v);
          ++t.reads;
          return t.standing;
        };
        sio.glowing = [](void*, uint32_t) { return Tri::Yes; };
        sio.call = [](void* v, uint32_t l, int32_t value) {
          Script& t = *static_cast<Script*>(v);
          ++t.calls;
          t.lastLabel = l;
          t.lastValue = value;
          if (t.refuse > 0) {
            --t.refuse;
            return false;
          }
          return true;
        };
        const uint32_t L1 = 0x40001000u, L2 = 0x40002000u;
        uint64_t pump = 0;
        Glow g;
        glowStep(g, 0, true, ++pump, sio);
        const bool idleQuiet = sc.reads == 0 && sc.calls == 0;
        sc.refuse = 1;
        const GlowStep on1 = glowStep(g, L1, true, ++pump, sio);
        const bool onRefused = on1.act == GlowAct::On && on1.called && !on1.ok && g.label == 0;
        const int callsHeld = sc.calls;
        const GlowStep onHeld = glowStep(g, L1, false, ++pump, sio);   // another call or a lock this pump
        const GlowStep on2 = glowStep(g, L1, true, ++pump, sio);
        check(idleQuiet && onRefused && !onHeld.called && sc.calls == callsHeld + 1 && on2.called && on2.ok &&
                  g.label == L1 && g.fails == 0 && sc.lastValue == kGlowOn,
              "M7i glow pumps: idle = no reads; a refused on changes nothing and is tried again; no on beside "
              "another call or under a lock");
        sc.refuse = 10;
        const int callsBefore = sc.calls;
        std::vector<uint64_t> at;
        bool kept = true, quietWaits = true;
        for (int i = 0; i < 2000 && g.label; ++i) {
          const int readsWas = sc.reads;
          const GlowStep st = glowStep(g, 0, false, ++pump, sio);   // the cursor left; the pump also has a main call
          if (st.called) at.push_back(pump);
          else if (sc.reads != readsWas) quietWaits = false;           // a waiting pump reads nothing
          if (st.called && !st.ok && g.label != L1) kept = false;
        }
        const uint64_t kGaps[10] = {2, 4, 8, 16, 32, 64, 64, 64, 64, 64};   // literal, not glowBackoff (Astra E5b r3)
        bool gaps = at.size() == 11;
        for (size_t i = 1; gaps && i < at.size(); ++i) gaps = at[i] - at[i - 1] == kGaps[i - 1];
        check(kept && quietWaits && gaps && g.label == 0 && sc.calls - callsBefore == 11 && sc.lastLabel == L1 &&
                  sc.lastValue == kGlowRestore,
              "M7i glow pumps: a restore runs beside a main call; ten refusals keep the glow (retried after 2, 4, "
              "... 64 quiet pumps, never given up); the 11th restores");
        // E5b r13 (Astra E5b r11 P2): a guest that refuses every restore - the refusals saturate at 6 (retries every 64
        // pumps from then on) over 100000 pumps; the glow is kept, nothing overflows, and the first accepted one restores
        {
          g = Glow{L1, 0, 0};
          sc.refuse = 1 << 30;
          std::vector<uint64_t> rat;
          for (int i = 0; i < 100000; ++i) {
            const GlowStep st = glowStep(g, 0, false, ++pump, sio);
            if (st.called) rat.push_back(pump);
          }
          bool g64 = rat.size() > 10;
          for (size_t i = 7; g64 && i < rat.size(); ++i) g64 = rat[i] - rat[i - 1] == 64;
          const bool keptAll = g.label == L1 && g.fails == 6;
          sc.refuse = 0;
          for (int i = 0; i < 200 && g.label; ++i) glowStep(g, 0, false, ++pump, sio);
          check(keptAll && g64 && g.label == 0 && sc.lastValue == kGlowRestore,
                "M7i glow refusals saturate: a guest refusing every restore keeps fails at 6 and the retries 64 pumps "
                "apart over 100000 pumps (the glow kept, no overflow); the first accepted restore ends it");
        }
        sc.refuse = 0;
        g = Glow{L1, 0, 0};
        sc.standing = Tri::Unknown;
        const int c0 = sc.calls;
        const GlowStep u1 = glowStep(g, 0, true, ++pump, sio);
        const bool unknownKept = u1.act == GlowAct::None && g.label == L1 && sc.calls == c0;
        sc.standing = Tri::Yes;
        sc.refuse = 1;
        glowStep(g, L2, true, ++pump, sio);   // to another hint: this one off first - refused
        const bool waiting = g.label == L1 && g.fails == 1 && g.retryAt > pump;
        glowStep(g, L1, true, ++pump, sio);   // back on it during the wait: wanted again
        const bool rewanted = g.label == L1 && g.fails == 0 && g.retryAt == 0 && sc.calls == c0 + 1;
        sc.standing = Tri::No;                // its bar is gone
        const GlowStep d1 = glowStep(g, L2, true, ++pump, sio);
        const bool dropped = d1.act == GlowAct::Drop && !d1.called && d1.obj == L1 && g.label == 0 &&
                             sc.calls == c0 + 1;
        sc.standing = Tri::Unknown;           // the hovered label cannot be judged: no on
        const GlowStep un = glowStep(g, L2, true, ++pump, sio);
        sc.standing = Tri::Yes;
        check(unknownKept && waiting && rewanted && dropped && !un.called && g.label == 0,
              "M7i glow pumps: unknown = kept, no call; hovered again during a wait = wanted; gone = forgotten "
              "with no call; no on for a label that cannot be judged");
        // the hover target: this pump's complete snapshot, the cursor awake / inside / focused, no mod library; the
        // input lock is reported (no new glow under it)
        MenuModel wm, lm;
        fakeHeapTest(true, false, false, &wm, 3, 9);
        fakeHeapTest(true, false, true, &lm, 3, 9);
        MouseState ms;
        ms.awake = ms.inside = true;
        ms.wx = 880;   // SELECT's hint: stage (840,660)-(920,700) at 1280 x 720
        ms.wy = 680;
        bool lk[7] = {true, true, true, true, true, true, false};
        MouseState asleep = ms, away = ms;
        asleep.awake = false;
        away.wx = 700;
        MenuModel unknownHints = wm;
        unknownHints.hintsUnknown = true;
        const uint32_t w0 = glowWanted(wm, ms, true, true, false, &lk[0]);
        const uint32_t w1 = glowWanted(wm, ms, false, true, false, &lk[1]);   // no snapshot this pump
        const uint32_t w2 = glowWanted(wm, asleep, true, true, false, &lk[2]);
        const uint32_t w3 = glowWanted(wm, ms, true, false, false, &lk[3]);   // the window is not focused
        const uint32_t w4 = glowWanted(wm, ms, true, true, true, &lk[4]);     // the mod library is open
        const uint32_t w5 = glowWanted(unknownHints, ms, true, true, false, &lk[5]);
        const uint32_t w6 = glowWanted(lm, ms, true, true, false, &lk[6]);
        const uint32_t w7 = glowWanted(wm, away, true, true, false, &lk[5]);
        check(wm.nHints == 2 && w0 && w0 == wm.hints[0].label && !lk[0] && !w1 && !w2 && !w3 && !w4 && !w5 && !w7 &&
                  w6 && lk[6],
              "M7i glow target: this pump's complete snapshot only, the cursor awake / inside / focused, no mod "
              "library; a lock is reported");
      }
      // M7h (Astra E5 r1-r2): a snapshot whose hints are unknown (a refused read) publishes none; a click acts on
      // the rows alone at once, as before E5 - on the BACK hint's place: nothing (no TRIANGLE, no CROSS), the click
      // consumed (never held for a later screen); on the row: it confirms
      MenuModel um;
      fakeHeapTest(true, false, false, &um, 3, 10);
      g_drv = Driver{};
      g_model = um;
      g_model.t = nowSec();
      g_mouse.wx = 1000;
      g_mouse.wy = 680;
      g_mouse.lClicks = 1;
      const Plan pu = decide();
      const int leftAfter = g_mouse.lClicks;
      g_drv = Driver{};
      g_mouse.wx = 0.5f * (row.r.x0 + row.r.x1);
      g_mouse.wy = 0.5f * (row.r.y0 + row.r.y1);
      g_mouse.lClicks = 1;
      g_model.t = nowSec();
      const Plan pr = decide();
      check(um.hintsUnknown && um.nHints == 0 && pu.kind == Plan::kNone && leftAfter == 0 &&
                pr.kind == Plan::kEvent && pr.key == kKeyCross,
            "M7h refused hint read: no hint; a click on its place does nothing and is consumed; a row click confirms");
      // M7m (lane E E5c): a click on the title is the game's own START (kStart: the PC action layer), not a bare
      // Apt key; once (the next pump does nothing)
      MenuModel tm;
      fakeHeapTest(false, false, false, &tm);
      g_drv = Driver{};
      g_model = tm;
      g_model.t = nowSec();
      g_mouse.wx = 640;
      g_mouse.wy = 520;
      g_mouse.lClicks = 1;
      const Plan pt = decide();
      const Plan pt2 = decide();
      check(tm.clickAnywhere && pt.kind == Plan::kStart && pt.key == kKeyStart && pt2.kind == Plan::kNone,
            "M7m title click: the game's own START (kStart, the PC action layer; HandleEvent(301) only as the "
            "fallback), once");
      g_feedActive.store(feedWas);
      g_relative.store(relWas);
      g_drv = drvWas;
      g_mouse = mouseWas;
      g_model = modelWas;
    }
  }
  return fails;
}

}  // namespace kernel
}  // namespace sr
