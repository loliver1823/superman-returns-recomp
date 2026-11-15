// iter-466 controls2: THE ACTION LAYER. Contract and charter notes: pc_actions.hpp,
// docs/PC_CONTROLS.md section 2.
#include "pc_actions.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "collision_watch.hpp"
#include "pc_guest_access.hpp"
#include "host_automation.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sr::kernel::pcactions {

namespace {
using pcguest::Mem;
using collwatch::inWindow;

// ── the game's input manager (IDA, docs/ENGINE_MAP.md "Input actions") ───────────────────────
constexpr uint32_t kInputMgrPtr = 0x829A2C04u;   // InputMgr (sub_82721C88 returns *this)
constexpr uint32_t kActivePadPtr = 0x8296E8C8u;  // +12 = the active controller = player 1's slot
// InputMgr: +4 slot count, +12 MapInfo[] (32 B: +12 state (0 = live), +16 enabled, +20 context).
// Context: +4 command count, +8 InputCommand*[] indexed by event id (sub_82722DA8).
// The command classes (constructors sub_82723718 / 82725950 / 82724D68 / 82723070 / 82725290 /
// 82724800 / 82724160 / 82723C28; Update = vtable slot 6).
constexpr uint32_t kVtAxis = 0x8208CE5Cu, kVtButton = 0x8208CF34u, kVtDual = 0x8208D00Cu,
                   kVtCombo = 0x8208D0ECu, kVtAlt = 0x8208D1C4u, kVtMapped = 0x8208D29Cu,
                   kVtAxisMapped = 0x8208D374u, kVtRelease = 0x8208D44Cu;

struct NamedEvent { uint16_t id; const char* name; };
// The INPUT_EVENT_* table at 0x82912B68 (golden sound_gameplay; prefix dropped).
const NamedEvent kEvents[] = {
#include "pc_actions_events.inc"
};

std::mutex g_frameMu;
ControlFrame g_frame;

int envTri(const char* name) {
  const char* e = std::getenv(name);
  if (!e || !*e) return -1;
  return e[0] != '0' ? 1 : 0;
}

bool past(uint32_t mode, int16_t dz, int16_t v) {
  if (mode == 0) return v > dz;
  if (mode == 1) return v < -dz;
  if (mode < 3) return v < -dz || v > dz;
  return false;
}

uint8_t buttonOf(const ControlFrame& f, uint32_t ctl) {
  if (ctl >= kCtlCount) return 0;
  const uint8_t bit = ((f.buttons >> ctl) & 1u) ? 255 : 0;
  // an analog control (the triggers): the byte the game's own InputState_GetButton returns
  if ((f.analogMask >> ctl) & 1u) return f.analog[ctl] > bit ? f.analog[ctl] : bit;
  return bit;
}
int16_t axisOf(const ControlFrame& f, uint32_t ax) {
  return ax < 4 && ((f.axisDriven >> ax) & 1u) ? f.axis[ax] : 0;
}

void stepChildAndAge(CmdState* child, const ControlFrame& f, uint32_t* since) {
  if (!child) return;
  step(*child, f);
  if (child->fired) *since = 0;
  else if (*since != 0xFFFFFFFFu) *since += 1;
}
}  // namespace

bool ControlFrame::operator==(const ControlFrame& o) const {
  if (!(buttons == o.buttons && axisDriven == o.axisDriven && axis[0] == o.axis[0] && axis[1] == o.axis[1] &&
        axis[2] == o.axis[2] && axis[3] == o.axis[3] && analogMask == o.analogMask))
    return false;
  for (uint32_t i = 0; i < kCtlCount; ++i)
    if (((analogMask >> i) & 1u) && analog[i] != o.analog[i]) return false;
  return true;
}

const char* eventName(uint32_t id) {
  for (const NamedEvent& e : kEvents)
    if (e.id == id) return e.name;
  return "";
}

Kind kindOfVtable(uint32_t vt) {
  switch (vt) {
    case kVtButton: return Kind::Button;
    case kVtRelease: return Kind::Release;
    case kVtMapped: return Kind::Mapped;
    case kVtAxis: return Kind::Axis;
    case kVtAxisMapped: return Kind::AxisMapped;
    case kVtAlt: return Kind::Alternative;
    case kVtCombo: return Kind::Combo;
    case kVtDual: return Kind::Dual;
    default: return Kind::Unknown;
  }
}

const char* kindName(Kind k) {
  static const char* const kN[] = {"?", "Button", "Release", "Mapped", "Axis", "AxisMapped",
                                   "Alternative", "Combo", "Dual"};
  return kN[static_cast<int>(k) % 9];
}

// ── the host mirror: each class's Update, transcribed (IDA; difftest --pc-actions-test) ─────────
void step(CmdState& s, const ControlFrame& f) {
  switch (s.kind) {
    case Kind::Button:
    case Kind::Mapped: {   // EventButton sub_82723A60, EventButtonMapped sub_827250B8
      const uint8_t v = buttonOf(f, s.ctl);
      s.fired = false;
      if (v <= s.thr) { s.prev = v; return; }
      if (s.prev <= s.thr) { s.counter = 0; s.fired = true; s.prev = v; return; }
      s.counter += 1;
      const bool rep = s.repeat != 0 && s.counter >= s.repeat;
      s.prev = v;
      if (rep) { s.counter = 0; s.fired = true; }
      return;
    }
    case Kind::Release: {  // EventButtonRelease sub_82725C90: fires on the release
      const uint8_t v = buttonOf(f, s.ctl);
      s.fired = false;
      if (v > s.thr) { s.prev = v; return; }
      if (s.prev > s.thr) { s.counter = 0; s.fired = true; s.prev = v; return; }
      s.counter += 1;
      const bool rep = s.repeat != 0 && s.counter >= s.repeat;
      s.prev = v;
      if (rep) { s.counter = 0; s.fired = true; }
      return;
    }
    case Kind::Axis:
    case Kind::AxisMapped: {   // EventAxis sub_82723518, EventAxisMapped sub_82725740
      const int16_t raw = axisOf(f, s.ctl);
      const int16_t v = s.negate ? static_cast<int16_t>(-static_cast<int32_t>(raw)) : raw;
      s.fired = false;
      if (!past(s.mode, s.dz, v)) { s.value = v; return; }
      if (!past(s.mode, s.dz, s.value)) { s.counter = 0; s.fired = true; s.value = v; return; }
      s.counter += 1;
      const bool rep = static_cast<int32_t>(s.repeat) > 0 &&
                       static_cast<int32_t>(s.counter) >= static_cast<int32_t>(s.repeat);
      s.value = v;
      if (rep) { s.counter = 0; s.fired = true; }
      return;
    }
    case Kind::Alternative: {  // EventAlternative sub_82724A18: either child
      if (s.childA) step(*s.childA, f);
      if (s.childB) step(*s.childB, f);
      s.latchA = s.childA && s.childA->fired;
      s.latchB = s.childB && s.childB->fired;
      s.fired = s.latchA || s.latchB;
      return;
    }
    case Kind::Combo: {        // EventCombo sub_82724660: both children within the window
      s.fired = false;
      stepChildAndAge(s.childA, f, &s.sinceA);
      stepChildAndAge(s.childB, f, &s.sinceB);
      const bool both = s.sinceA <= s.window && s.sinceB <= s.window;
      if (both) {
        if (!s.held || (s.countdown -= 1) == 0) { s.fired = true; s.countdown = s.repeat; }
        s.held = true;
      } else {
        s.held = false;
      }
      return;
    }
    case Kind::Dual: {         // EventDualButton sub_82723D70: both held
      const uint8_t a = buttonOf(f, s.ctl), b = buttonOf(f, s.ctl2);
      if (a <= s.thr || b <= s.thr2) { s.fired = false; s.prevA = a; s.prevB = b; return; }
      if (s.prevA > static_cast<int32_t>(s.thr) && s.prevB > static_cast<int32_t>(s.thr2)) {
        s.counter += 1;
        if (s.repeat != 0 && s.counter >= s.repeat) {
          s.prevA = a; s.counter = 0; s.fired = true; s.prevB = b;
          return;
        }
        s.fired = false; s.prevA = a; s.prevB = b;
        return;
      }
      s.fired = true; s.counter = 0; s.prevA = a; s.prevB = b;
      return;
    }
    default:
      s.fired = false;
      return;
  }
}

// The game's Reset (vtable slot 7), transcribed from the recompiled bodies. InputContext_UpdateCommands_sub_82722DB8
// calls it instead of Update on every command of the context while no controller is present (InputState +8 != 1).
// EventButton 82723838 / EventButtonMapped 82724E80: fired, prev, counter; EventButtonRelease 82725A70: fired only
// (its prev survives); EventAxis 827231F0 / EventAxisMapped 82725408: fired, value, counter; EventDualButton 82723E48:
// fired, both values, counter; EventCombo 82724398: fired, held, both ages -1, the countdown = its repeat;
// EventAlternative 82724AB0: both latches. A composite does not reset its children.
void reset(CmdState& s) {
  switch (s.kind) {
    case Kind::Button: case Kind::Mapped: s.fired = false; s.prev = 0; s.counter = 0; return;
    case Kind::Release: s.fired = false; return;
    case Kind::Axis: case Kind::AxisMapped: s.fired = false; s.value = 0; s.counter = 0; return;
    case Kind::Dual: s.fired = false; s.prevA = 0; s.prevB = 0; s.counter = 0; return;
    case Kind::Combo:
      s.fired = false; s.held = false; s.sinceA = s.sinceB = 0xFFFFFFFFu; s.countdown = s.repeat;
      return;
    case Kind::Alternative: s.latchA = s.latchB = false; s.fired = false; return;
    default: return;
  }
}

int32_t outputValue(const CmdState& s) {
  switch (s.kind) {
    case Kind::Button: case Kind::Release: case Kind::Mapped: return s.prev;
    case Kind::Axis: case Kind::AxisMapped: return s.value;
    default: return s.fired ? 1 : 0;
  }
}

bool enabled() {
  static const bool on = [] {
    const bool pf = automation::portFeaturesOn();
    const int env = envTri("SR_PC_ACTIONS");
    const bool v = pf && env != 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PC_ACTIONS] action layer %s (port features %s, SR_PC_ACTIONS=%s)", v ? "ON" : "OFF",
                pf ? "on" : "off", env < 0 ? "unset" : (env ? "1" : "0"));
    return v;
  }();
  return on;
}

void publish(const ControlFrame& f) {
  std::lock_guard<std::mutex> lk(g_frameMu);
  g_frame = f;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// The guest side: the dispatch hook's two halves.
//
// The merged result of a command is ITS OWN RULE OVER THE PLAYER'S WHOLE INPUT: the controller's
// controls this tick (the InputState the game evaluated the context with, latched in
// InputContext_UpdateCommands_sub_82722DB8) combined with the PC's (buttons OR'd; an axis takes
// the larger magnitude). The mirror is seeded from the command's own state when it is first seen,
// so with no PC input it reproduces the game's own result exactly (difftest --pc-actions-test) -
// and a chord can be split across the controller and the keyboard.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

struct Write { uint32_t va; uint8_t size; uint16_t orig; };
struct PadLatch { uint32_t ctx; bool present; bool valid = true; ControlFrame f; };

struct Layer {
  bool readable = false, checked = false;
  uint32_t ctx = 0, slot = 0;        // the context the mirrors belong to
  std::deque<CmdState> arena;
  std::unordered_map<uint32_t, CmdState*> shadow;   // guest command address -> its mirror
  std::unordered_map<uint32_t, uint32_t> vtOf;      // guest command address -> vtable seen
  std::vector<uint32_t> bornNow;      // mirrors seeded this tick (they do not step this tick)
  std::vector<PadLatch> latches;      // the contexts the game evaluated in this dispatch
  std::vector<Write> writes;          // what the last merge wrote (restored before the next)
  uint32_t writesCtx = 0, writesSlot = 0;
  uint64_t tick = 0;
  uint32_t ctxLogs = 0, fireLogs = 0, fires = 0;
  FILE* trace = nullptr;
  bool traceTried = false;
} g;

void put8(const Mem& m, uint64_t va, uint8_t v) {
  if (!m.ok() || va > UINT32_MAX || !inWindow(uint32_t(va), 1)) return;
  const uint8_t old = m.u8(va);
  if (!m.ok() || old == v) return;
  if (m.put8(va, v)) g.writes.push_back({uint32_t(va), 1, old});
}
void put16(const Mem& m, uint64_t va, int16_t v) {
  if (!m.ok() || va > UINT32_MAX || !inWindow(uint32_t(va), 2)) return;
  const uint16_t old = m.u16(va), nv = static_cast<uint16_t>(v);
  if (!m.ok() || old == nv) return;
  if (m.put16(va, nv)) g.writes.push_back({uint32_t(va), 2, old});
}
int16_t s16(const Mem& m, uint64_t va) { return static_cast<int16_t>(m.u16(va)); }

// Player 1's live context, as InputMgr_IsCommandActive_sub_82722390 finds it (0 = none: the
// game's queries would answer false, so nothing is merged).
uint32_t liveContext(const Mem& m, uint32_t* slotOut) {
  const uint32_t mgr = m.u32(kInputMgrPtr);
  const uint32_t pad = m.u32(kActivePadPtr);
  if (!mgr || !pad) return 0;
  const uint32_t slot = m.u32(uint64_t(pad) + 12);
  const uint32_t n = m.u32(uint64_t(mgr) + 4);
  if (slot >= n || n > 16) return 0;
  const uint64_t mi = uint64_t(m.u32(uint64_t(mgr) + 12)) + 32ull * slot;
  if (!m.u8(uint64_t(mi) + 16) || m.u32(uint64_t(mi) + 12) != 0) return 0;
  *slotOut = slot;
  return m.u32(uint64_t(mi) + 20);
}

// The command's parameters as the game holds them now (the inversion options flip `negate`).
void refreshParams(const Mem& m, uint32_t cmd, CmdState& s, int depth) {
  switch (s.kind) {
    case Kind::Button: case Kind::Release: case Kind::Mapped:
      s.ctl = m.u32(uint64_t(cmd) + 4); s.repeat = m.u32(uint64_t(cmd) + 8); s.thr = m.u8(uint64_t(cmd) + 18);
      break;
    case Kind::Axis:
      s.ctl = m.u32(uint64_t(cmd) + 4); s.repeat = m.u32(uint64_t(cmd) + 8); s.dz = s16(m, uint64_t(cmd) + 18);
      s.mode = m.u32(uint64_t(cmd) + 20); s.negate = m.u32(uint64_t(cmd) + 24);
      break;
    case Kind::AxisMapped:
      s.ctl = m.u32(uint64_t(cmd) + 4); s.repeat = m.u32(uint64_t(cmd) + 8); s.dz = s16(m, uint64_t(cmd) + 18);
      s.mode = m.u32(uint64_t(cmd) + 24); s.negate = m.u32(uint64_t(cmd) + 28);
      break;
    case Kind::Alternative:
    case Kind::Combo:
      if (s.kind == Kind::Combo) { s.window = m.u32(uint64_t(cmd) + 12); s.repeat = m.u32(uint64_t(cmd) + 16); }
      if (depth < 4) {
        if (s.childA) refreshParams(m, m.u32(uint64_t(cmd) + 4), *s.childA, depth + 1);
        if (s.childB) refreshParams(m, m.u32(uint64_t(cmd) + 8), *s.childB, depth + 1);
      }
      break;
    case Kind::Dual:
      s.ctl = m.u32(uint64_t(cmd) + 4); s.ctl2 = m.u32(uint64_t(cmd) + 8); s.repeat = m.u32(uint64_t(cmd) + 12);
      s.thr = m.u8(uint64_t(cmd) + 28); s.thr2 = m.u8(uint64_t(cmd) + 29);
      break;
    default: break;
  }
}

// Seed a mirror from the command's own state (the fields its Update reads and writes).
void seedFromGuest(const Mem& m, uint32_t cmd, CmdState& s, int depth) {
  switch (s.kind) {
    case Kind::Button: case Kind::Release: case Kind::Mapped:
      s.counter = m.u32(uint64_t(cmd) + 12); s.fired = m.u8(uint64_t(cmd) + 16) != 0; s.prev = m.u8(uint64_t(cmd) + 17);
      break;
    case Kind::Axis:
      s.counter = m.u32(uint64_t(cmd) + 12); s.value = s16(m, uint64_t(cmd) + 16); s.fired = m.u8(uint64_t(cmd) + 28) != 0;
      break;
    case Kind::AxisMapped:
      s.counter = m.u32(uint64_t(cmd) + 12); s.value = s16(m, uint64_t(cmd) + 16); s.fired = m.u8(uint64_t(cmd) + 32) != 0;
      break;
    case Kind::Alternative:
      s.latchA = m.u8(uint64_t(cmd) + 12) != 0; s.latchB = m.u8(uint64_t(cmd) + 13) != 0; s.fired = s.latchA || s.latchB;
      break;
    case Kind::Combo:
      s.fired = m.u8(uint64_t(cmd) + 21) != 0; s.held = m.u8(uint64_t(cmd) + 22) != 0;
      s.sinceA = m.u32(uint64_t(cmd) + 24); s.sinceB = m.u32(uint64_t(cmd) + 28); s.countdown = m.u32(uint64_t(cmd) + 32);
      break;
    case Kind::Dual:
      s.counter = m.u32(uint64_t(cmd) + 16); s.prevA = static_cast<int32_t>(m.u32(uint64_t(cmd) + 20));
      s.prevB = static_cast<int32_t>(m.u32(uint64_t(cmd) + 24)); s.fired = m.u8(uint64_t(cmd) + 30) != 0;
      break;
    default: break;
  }
  if ((s.kind == Kind::Alternative || s.kind == Kind::Combo) && depth < 4) {
    if (s.childA) seedFromGuest(m, m.u32(uint64_t(cmd) + 4), *s.childA, depth + 1);
    if (s.childB) seedFromGuest(m, m.u32(uint64_t(cmd) + 8), *s.childB, depth + 1);
  }
}

// The mirror of a guest command (built once per context, seeded from the command's own state).
CmdState* shadowOf(const Mem& m, uint32_t cmd, int depth, bool* born) {
  if (!cmd || depth > 4 || !inWindow(cmd, 40) || !m.range(cmd, 40)) return nullptr;
  const uint32_t vt = m.u32(cmd);
  auto it = g.shadow.find(cmd);
  if (it != g.shadow.end() && g.vtOf[cmd] == vt) return it->second;
  const Kind k = kindOfVtable(vt);
  if (k == Kind::Unknown) return nullptr;
  g.arena.emplace_back();
  CmdState* s = &g.arena.back();
  s->kind = k;
  if (k == Kind::Alternative || k == Kind::Combo) {
    bool b = false;
    s->childA = shadowOf(m, m.u32(uint64_t(cmd) + 4), depth + 1, &b);
    s->childB = shadowOf(m, m.u32(uint64_t(cmd) + 8), depth + 1, &b);
  }
  g.shadow[cmd] = s;
  g.vtOf[cmd] = vt;
  if (depth == 0) {
    refreshParams(m, cmd, *s, 0);
    seedFromGuest(m, cmd, *s, 0);
  }
  if (born) *born = true;
  return s;
}

// The game's own result of a command this tick (what its IsActive / value fields hold).
bool guestFired(const Mem& m, uint32_t cmd, Kind k) {
  switch (k) {
    case Kind::Button: case Kind::Release: case Kind::Mapped: return m.u8(uint64_t(cmd) + 16) != 0;
    case Kind::Axis: return m.u8(uint64_t(cmd) + 28) != 0;
    case Kind::AxisMapped: return m.u8(uint64_t(cmd) + 32) != 0;
    case Kind::Alternative: return m.u8(uint64_t(cmd) + 12) != 0 || m.u8(uint64_t(cmd) + 13) != 0;
    case Kind::Combo: return m.u8(uint64_t(cmd) + 21) != 0;
    case Kind::Dual: return m.u8(uint64_t(cmd) + 30) != 0;
    default: return false;
  }
}
int32_t guestValue(const Mem& m, uint32_t cmd, Kind k) {
  switch (k) {
    case Kind::Button: case Kind::Release: case Kind::Mapped: return m.u8(uint64_t(cmd) + 17);
    case Kind::Axis: case Kind::AxisMapped: return s16(m, uint64_t(cmd) + 16);
    default: return guestFired(m, cmd, k) ? 1 : 0;
  }
}

// The consumer-visible fields take the mirror's result (IsActive's field, the value field).
void writeResult(const Mem& m, uint32_t cmd, const CmdState& s) {
  switch (s.kind) {
    case Kind::Button: case Kind::Release: case Kind::Mapped:
      put8(m, uint64_t(cmd) + 16, s.fired ? 1 : 0);
      put8(m, uint64_t(cmd) + 17, s.prev);
      break;
    case Kind::Axis: case Kind::AxisMapped:
      put8(m, uint64_t(cmd) + (s.kind == Kind::Axis ? 28u : 32u), s.fired ? 1 : 0);
      put16(m, uint64_t(cmd) + 16, s.value);
      break;
    case Kind::Alternative:
      put8(m, uint64_t(cmd) + 12, s.latchA ? 1 : 0);
      put8(m, uint64_t(cmd) + 13, s.latchB ? 1 : 0);
      break;
    case Kind::Combo: put8(m, uint64_t(cmd) + 21, s.fired ? 1 : 0); break;
    case Kind::Dual: put8(m, uint64_t(cmd) + 30, s.fired ? 1 : 0); break;
    default: break;
  }
}

// The controls a command reads (leaves and composites), as a bit mask: bits 0-15 buttons,
// 16-19 axes.
uint32_t controlsOf(const CmdState& s, int depth = 0) {
  switch (s.kind) {
    case Kind::Button: case Kind::Release: case Kind::Mapped:
      return s.ctl < 16 ? (1u << s.ctl) : 0;
    case Kind::Axis: case Kind::AxisMapped:
      return s.ctl < 4 ? (1u << (16 + s.ctl)) : 0;
    case Kind::Dual:
      return (s.ctl < 16 ? (1u << s.ctl) : 0) | (s.ctl2 < 16 ? (1u << s.ctl2) : 0);
    case Kind::Alternative: case Kind::Combo:
      return depth > 4 ? 0
                       : (s.childA ? controlsOf(*s.childA, depth + 1) : 0) |
                             (s.childB ? controlsOf(*s.childB, depth + 1) : 0);
    default: return 0;
  }
}

const char* controlName(int i) {
  static const char* const kN[] = {"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "START",
                                   "BACK", "L3", "R3", "LB", "RB", "A", "B", "X", "Y", "LT", "RT",
                                   "LX", "LY", "RX", "RY"};
  return i >= 0 && i < 20 ? kN[i] : "?";
}

// Once per new context: which of the game's events each control drives here (the evidence for
// the action -> event table in docs/PC_CONTROLS.md; bounded).
void logContext(const Mem& m, uint32_t ctx, uint32_t slot, uint32_t count, uint32_t arr) {
  if (g.ctxLogs >= 64) return;
  ++g.ctxLogs;
  std::string per[20];
  uint32_t kinds[9] = {};
  for (uint32_t id = 0; id < count; ++id) {
    const uint32_t cmd = m.u32(uint64_t(arr) + 4 * id);
    auto it = g.shadow.find(cmd);
    if (!cmd || it == g.shadow.end()) continue;
    kinds[static_cast<int>(it->second->kind) % 9]++;
    const uint32_t c = controlsOf(*it->second);
    for (int i = 0; i < 20; ++i) {
      if (!((c >> i) & 1u)) continue;
      char e[48];
      std::snprintf(e, sizeof e, "%s%u%s%s", per[i].empty() ? "" : ",", id,
                    *eventName(id) ? ":" : "", eventName(id));
      if (per[i].size() < 900) per[i] += e;
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PC_ACTIONS] context %08X (player slot %u, %u ids: %u Button %u Release %u Mapped "
              "%u Axis %u AxisMapped %u Alternative %u Combo %u Dual) tick=%llu",
              ctx, slot, count, kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], kinds[7],
              kinds[8], static_cast<unsigned long long>(g.tick));
  for (int i = 0; i < 20; ++i)
    if (!per[i].empty())
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_ACTIONS]   %s -> %s", controlName(i), per[i].c_str());
}

FILE* traceFile() {
  if (!g.traceTried) {
    g.traceTried = true;
    if (const char* p = std::getenv("SR_PC_ACTIONS_TRACE"); p && *p) {
      g.trace = std::fopen(p, "w");
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_ACTIONS] trace -> %s (%s)", p, g.trace ? "open" : "FAILED");
    }
  }
  return g.trace;
}

// The player's whole input: the controller's controls with the PC's on top.
ControlFrame combine(const ControlFrame& pad, const ControlFrame& pc) {
  ControlFrame c = pad;
  c.buttons |= pc.buttons;
  for (uint32_t i = 0; i < kCtlCount; ++i) {   // an analog control the PC drives: the larger pull
    if (!((pc.analogMask >> i) & 1u)) continue;
    const uint8_t pv = (pad.analogMask >> i) & 1u ? pad.analog[i] : 0;
    c.analog[i] = pc.analog[i] > pv ? pc.analog[i] : pv;
    c.analogMask |= static_cast<uint16_t>(1u << i);
  }
  for (int a = 0; a < 4; ++a) {
    if (!((pc.axisDriven >> a) & 1u)) continue;
    const int16_t pv = (pad.axisDriven >> a) & 1u ? pad.axis[a] : 0;
    if (std::abs(int(pc.axis[a])) >= std::abs(int(pv))) c.axis[a] = pc.axis[a];
    c.axisDriven |= static_cast<uint8_t>(1u << a);
  }
  return c;
}

}  // namespace

bool readyFor(uint8_t* base) {
  if (!g.checked) {
    g.checked = true;
    char why[200] = {};
    g.readable = pcguest::ready(base, why, sizeof why);
    SR_LOG_BOOT(g.readable ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[PC_ACTIONS] guest reads %s (%s)", g.readable ? "ON" : "OFF", why);
  }
  return g.readable;
}

// The game evaluates a context with an InputState (InputContext_UpdateCommands_sub_82722DB8):
// latch the controller's controls it carried (InputState_GetButton_sub_825F7B08: bit i of the
// bitset at +0x20, i < the count at +0x14; the byte (+0x24)[i - count] for i in [count, count + the
// analog count at +0x18): the pad driver's LT / RT; sub_825F7B78: the int at +0x28[i]; +8 == 1 = present).
// step4/pad-triggers (2026-10-20): the analog bytes were never latched, so a controller's LT / RT read 0 here and
// the merge wrote "not fired" over the game's own LLOCK / POWER / THROW results (docs/PC_CONTROLS.md 2.6).
void noteContextUpdate(uint8_t* base, uint32_t ctx, uint32_t state) {
  const Mem m{base};
  PadLatch l;
  l.ctx = ctx;
  l.present = m.u32(uint64_t(state) + 8) == 1;
  const uint32_t n = m.u32(uint64_t(state) + 0x14);
  const uint32_t bits = m.u32(uint64_t(state) + 0x20);
  for (uint32_t i = 0; i < kCtlCount && i < n; ++i)
    if (m.u8(uint64_t(bits) + (i >> 3)) & (1u << (i & 7))) l.f.buttons |= static_cast<uint16_t>(1u << i);
  const uint32_t na = m.u32(uint64_t(state) + 0x18);
  const uint32_t bytes = m.u32(uint64_t(state) + 0x24);
  if (na) {
    // GetButton's bound is the guest's 32-bit sum n + na. A null byte array or a wrapping sum is no layout the pad
    // driver builds: reject the latch rather than read zeros or disagree with the guest's bound (Astra P3,
    // 2026-10-20). afterDispatch then leaves the game's result and reseeds the mirrors from it.
    const uint32_t end = n + na;
    if (!bytes || end < n) {
      l.valid = false;
    } else {
      for (uint32_t i = n; i < kCtlCount && i < end; ++i) {
        l.f.analog[i] = m.u8(uint64_t(bytes) + (i - n));
        l.f.analogMask |= static_cast<uint16_t>(1u << i);
      }
    }
  }
  const uint32_t axes = m.u32(uint64_t(state) + 0x28);
  for (int a = 0; a < 4; ++a) {
    l.f.axis[a] = static_cast<int16_t>(m.u32(uint64_t(axes) + 4 * a));
    l.f.axisDriven |= static_cast<uint8_t>(1u << a);
  }
  if (!m.ok()) return;
  for (PadLatch& x : g.latches)
    if (x.ctx == ctx) { x = l; return; }
  g.latches.push_back(l);
}

// Before the game's own evaluation: put back the game's values the last merge overwrote, so its
// own state (the previous value its edge detection compares with) is the controller's alone.
void beforeDispatch(uint8_t* base) {
  g.latches.clear();
  if (g.writes.empty()) return;
  const Mem m{base};
  uint32_t slot = 0;
  const uint32_t ctx = liveContext(m, &slot);
  if (ctx == g.writesCtx && slot == g.writesSlot) {
    for (auto it = g.writes.rbegin(); it != g.writes.rend(); ++it) {
      if (it->size == 1) m.put8(it->va, static_cast<uint8_t>(it->orig));
      else m.put16(it->va, it->orig);
    }
  }
  // A context that went away took its commands with it (the game's factory builds a new one):
  // nothing to put back.
  g.writes.clear();
}

// After the game's own evaluation: each command of player 1's live context by its own rule over
// the controller + the PC, written to the fields the consumers read.
void afterDispatch(uint8_t* base) {
  ++g.tick;
  const Mem m{base};
  uint32_t slot = 0;
  const uint32_t ctx = liveContext(m, &slot);
  if (!ctx || !inWindow(ctx, 12)) return;
  if (ctx != g.ctx || slot != g.slot) {
    g.ctx = ctx;
    g.slot = slot;
    g.shadow.clear();
    g.vtOf.clear();
    g.arena.clear();
  }
  const PadLatch* latch = nullptr;
  for (const PadLatch& l : g.latches)
    if (l.ctx == ctx) latch = &l;
  // The game did not evaluate the context this tick (no queued state): its commands stand as they are.
  if (!latch) return;
  if (latch->present && !latch->valid) {
    // A rejected layout (noteContextUpdate): the game's result stands and the mirrors reseed from it next tick.
    g.shadow.clear();
    g.vtOf.clear();
    g.arena.clear();
    return;
  }
  const uint32_t count = std::min<uint32_t>(m.u32(uint64_t(ctx) + 4), 256);
  const uint32_t arr = m.u32(uint64_t(ctx) + 8);
  if (!arr || !inWindow(arr, 4 * count)) return;
  // No controller: the game RESET every command of the context (vtable slot 7) instead of Update. The mirrors take
  // the same reset instead of a step, so a control still held when the controller comes back is a new edge for
  // both (before this, the layer returned here and a mirror kept its history - a held trigger's prev 255 -,
  // swallowed the edge the game fired on the reconnect and wrote over it; Astra P2, 2026-10-20). The reset result
  // is written like any other: EventButtonRelease's reset keeps its value, which for the merged input is the
  // mirror's, not the controller's alone.
  const bool absent = !latch->present;
  ControlFrame pc;
  {
    std::lock_guard<std::mutex> lk(g_frameMu);
    pc = g_frame;
  }
  const ControlFrame f = combine(latch->f, pc);
  const bool fresh = g.shadow.empty();
  g.bornNow.clear();
  std::vector<uint32_t> unique;
  unique.reserve(count);
  // Every occurrence steps (the game updates an object bound under two ids twice per tick).
  for (uint32_t id = 0; id < count; ++id) {
    const uint32_t cmd = m.u32(uint64_t(arr) + 4 * id);
    if (!cmd) continue;
    bool born = false;
    CmdState* s = shadowOf(m, cmd, 0, &born);
    if (!s) continue;
    if (born) g.bornNow.push_back(cmd);
    const bool seenBefore = std::find(unique.begin(), unique.end(), cmd) != unique.end();
    if (!seenBefore) unique.push_back(cmd);
    // seeded this tick from the game's own result: it starts stepping next tick
    if (std::find(g.bornNow.begin(), g.bornNow.end(), cmd) != g.bornNow.end()) continue;
    if (!seenBefore) refreshParams(m, cmd, *s, 0);
    if (absent) reset(*s);
    else step(*s, f);
  }
  if (!m.ok()) { g.shadow.clear(); g.vtOf.clear(); g.arena.clear(); return; }
  FILE* tr = traceFile();
  std::string tGame, tOut;
  for (uint32_t cmd : unique) {
    CmdState* s = g.shadow[cmd];
    const bool gFired = guestFired(m, cmd, s->kind);
    if (tr || (s->fired && !gFired)) {
      // the event ids this object answers (for the log / trace)
      for (uint32_t id = 0; id < count; ++id) {
        if (m.u32(uint64_t(arr) + 4 * id) != cmd) continue;
        if (tr) {
          char e[40];
          if (gFired) {
            std::snprintf(e, sizeof e, "%s[%u,%d]", tGame.empty() ? "" : ",", id, guestValue(m, cmd, s->kind));
            tGame += e;
          }
          if (s->fired) {
            std::snprintf(e, sizeof e, "%s[%u,%d]", tOut.empty() ? "" : ",", id, outputValue(*s));
            tOut += e;
          }
        }
        if (s->fired && !gFired) {
          ++g.fires;
          if (g.fireLogs < 300 || g.fires % 100 == 0) {
            ++g.fireLogs;
            SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_ACTIONS] fire %u %s (%s, value %d) tick=%llu", id,
                        eventName(id), kindName(s->kind), outputValue(*s),
                        static_cast<unsigned long long>(g.tick));
          }
        }
      }
    }
    if (!m.ok()) return;
    writeResult(m, cmd, *s);
  }
  g.writesCtx = ctx;
  g.writesSlot = slot;
  if (fresh) logContext(m, ctx, slot, count, arr);
  if (tr && (!tGame.empty() || !tOut.empty())) {
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(tr, "{\"t\":%llu,\"ms\":%lld,\"ctx\":\"%08X\",\"pc\":%u,\"game\":[%s],\"out\":[%s]}\n",
                 static_cast<unsigned long long>(g.tick), ms, ctx,
                 unsigned(pc.buttons) | (pc.axisDriven ? 0x10000u : 0u), tGame.c_str(), tOut.c_str());
    std::fflush(tr);
  }
}

void resetForTest() {
  g.shadow.clear();
  g.vtOf.clear();
  g.arena.clear();
  g.writes.clear();
  g.latches.clear();
  g.bornNow.clear();
  g.ctx = g.slot = g.writesCtx = g.writesSlot = 0;
  std::lock_guard<std::mutex> lk(g_frameMu);
  g_frame = ControlFrame{};
}

// The mirror's own sanity (the parity against the recompiled class code is
// tools/difftest/pc_actions_test.cpp).
int selfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) { ++fail; std::printf("FAIL pcactions %s\n", what); }
  };
  ControlFrame up, down;
  down.buttons = 1u << kCtlX;
  CmdState b;
  b.kind = Kind::Button; b.ctl = kCtlX; b.thr = 1; b.repeat = 0;
  step(b, up); check(!b.fired, "idle");
  step(b, down); check(b.fired && outputValue(b) == 255, "press edge");
  step(b, down); check(!b.fired, "held, edge only");
  step(b, up); check(!b.fired && outputValue(b) == 0, "release");
  CmdState r;
  r.kind = Kind::Release; r.ctl = kCtlX; r.thr = 1;
  step(r, down); step(r, down); check(!r.fired, "release: held");
  step(r, up); check(r.fired, "release: fires on release");
  CmdState a;
  a.kind = Kind::AxisMapped; a.ctl = 1; a.dz = 250; a.mode = 2; a.repeat = 1;
  ControlFrame fw;
  fw.axisDriven = 2; fw.axis[1] = 1000;
  step(a, fw); check(a.fired && a.value == 1000, "axis crossing");
  step(a, fw); check(a.fired, "axis repeat 1");
  a.negate = 1; step(a, fw); check(a.value == -1000, "axis negate");
  ControlFrame pad, pcf;
  pad.buttons = 1u << kCtlA; pad.axisDriven = 0xF; pad.axis[0] = 600;
  pcf.buttons = 1u << kCtlB; pcf.axisDriven = 1; pcf.axis[0] = -1000;
  const ControlFrame c = combine(pad, pcf);
  check(c.buttons == ((1u << kCtlA) | (1u << kCtlB)) && c.axis[0] == -1000, "combine");
  check(kindOfVtable(0x8208CF34u) == Kind::Button && kindOfVtable(0x8208D0ECu) == Kind::Combo,
        "vtables");
  check(std::strcmp(eventName(66), "GAME_ACTION_ATTACK") == 0, "event names");
  return fail;
}

}  // namespace sr::kernel::pcactions

// ═════════════════════════════════════════════════════════════════════════════════════════════
// THE HOOKS (docs/OVERRIDES.json port-feature). Each delegates to the game's body exactly once.
// InputMgr_DispatchQueuedInput: once per sim tick (GameInput_Update_sub_8229AD10; sub_8229DA48).
// InputContext_UpdateCommands: observe only (the InputState the game evaluates a context with).
// ═════════════════════════════════════════════════════════════════════════════════════════════
extern "C" void __imp__sub_82721E68(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82722DB8(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_82721E68) {
  namespace PA = sr::kernel::pcactions;
  const bool on = PA::enabled() && PA::readyFor(base);
  if (on) PA::beforeDispatch(base);
  __imp__sub_82721E68(ctx, base);
  if (on) PA::afterDispatch(base);
}

PPC_FUNC(sub_82722DB8) {
  namespace PA = sr::kernel::pcactions;
  if (PA::enabled() && PA::readyFor(base)) PA::noteContextUpdate(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82722DB8(ctx, base);
}
