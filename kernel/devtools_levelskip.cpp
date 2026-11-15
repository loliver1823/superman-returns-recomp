// iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 4.1, 5.5, 10 steps 5-7 and 10): the KERNEL
// level-skip executor and the in-exe [OBJSTATE] dumps - the drain hook on the game's own
// tick thread.
//
// THE DRAIN POINT (design 4.1, B10). Entry of GameTick_ActiveWorldUpdate_sub_8223EB08, the
// active-world tick, on the simulation thread, BEFORE __imp__: the same tick's
// WorldFlowMgr_Update sees a write. The Frida executor (tools/devtools/frida_skip.js) hooks
// the same point in Xenia, so both sides write at the same place in the tick. The two other
// tick variants (sub_8223ECC0, GameTick_InactiveFlowOnlyUpdate_sub_8223F230) are OBSERVE
// ticks (B11): conditions and snapshots, never a write.
//
// GATE (devtools.hpp). Each strong def is
//     if (!devToolHookNeeded_sub_X()) { __imp__sub_X(ctx, base); return; }
// i.e. an EXACT pass-through when the dev tools are off (the zero-impact proof). On, the hook
// runs the executor (tool `levelskip`, devtool class) and/or the periodic [OBJSTATE] dump
// (tool `objstate`, observe class: reads only), then calls __imp__ with the registers
// untouched. The guest's registers are never read or written here.
//
// SEMANTICS: kernel/devtools_skip_core.cpp, the C++ port of tools/devtools/skipexec.py
// (differential: tools/devtools/test_skipcore_cpp.py). The plan is SR_DEVTOOLS_CMDS (the
// command language of design 5.4, as tools/devtools/skipplan.py writes it).
//
// SAFETY (design 4.2, B8). Every guest page is read only after hostRangeReadableCached()
// proved it committed, readable and not a guard page (the harness VEH makes any host AV
// fatal; nothing here relies on __try). An unreadable page is a refusal or a false
// condition, never a touch. Every write re-checks its pages FRESH with VirtualQuery
// (committed, writable, not guard) and is a single aligned big-endian store of 4 or 8 bytes
// that the game's own code makes (the DAS store of DAS_SetIntInScope_sub_82218F38; the
// trigger FSM's engage distance; the CurrentXP stat value) - the write list is logged.
//
// No host thread is added: the executor runs on the tick thread; the deadline poll rides the
// existing [GAUGE] census thread (kernel/natural_gauges.cpp) and touches no guest memory.
//
// DT-2 (iter-464; docs/DEVTOOLS.md 6). The same drain point also runs the IMMEDIATE COMMAND
// CHANNEL: devToolsSubmit() (the in-game Dev Tools menu, the F6 hotkey) queues one command line of
// the same language; it runs here as a one-step executor (tool `cheats`: pause_timers / minievents /
// metro_lock, single-byte or word stores the game's own code makes; tool `freecam`: the retail debug
// camera, see FREECAM below). Both executors share one CheatState, so `pause_timers off` resumes
// exactly the timers an earlier `pause_timers on` paused, from either channel.
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "devtools.hpp"
#include "devtools_skip_core.hpp"
#include "file_io.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" void __imp__sub_8223EB08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8223ECC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8223F230(PPCContext& __restrict ctx, uint8_t* base);

namespace {

namespace S = sr::devtools::skip;
namespace fs = std::filesystem;
using S::J;

#include "devtools_base_graph.inc"

struct Live {
  std::mutex mu;
  bool initDone = false;
  bool threadLogged = false;
  S::Graph graph;
  std::unique_ptr<S::Executor> ex;
  std::chrono::steady_clock::time_point t0;
  uint8_t* base = nullptr;
  std::string outDir;
  std::FILE* steps = nullptr;
  // the pages the current tick read, with the bytes it saw (pre-write)
  std::map<uint32_t, std::vector<uint8_t>> tickPages;
  std::vector<uint32_t> tickMissing;
  bool recording = false;
  // per-tick results the event callback leaves for after the tick
  std::vector<std::string> pendingSnapshots;
  std::vector<J> pendingWriteEvents;
  // the in-game Dev Tools menu (iter-464): a plan armed from the menu starts at the next
  // ACTIVE tick, so its first deadline does not run down while the player is still in a menu
  bool pending = false;
  std::vector<S::Step> pendingSteps;
  std::string planName;      // file name of the loaded / pending plan
  std::string lastEvent;     // one line, for the status page
  // objstate
  uint64_t activeTicks = 0;
  std::string lastObjKey;
  uint32_t objDumps = 0;
  // DT-2: shared by the plan executor and the command channel (skipexec.Executor.cheats)
  std::shared_ptr<S::CheatState> cheats = std::make_shared<S::CheatState>();
  PPCContext* ctx = nullptr;          // the hooked tick's context (guest calls), only inside onTick
  std::unique_ptr<S::Executor> cmdEx; // the command being run (one at a time)
  uint64_t cmdId = 0;
  bool cmdSkip = false;               // the running command is a skip_mission pass (writes: tool levelskip)
  // iter-465: after a skip passes a minigame (race / Bizarro rampage), lift Superman out of its start
  // circle as soon as pad 0's actor is Superman again, so the game does not restart it at once
  bool liftPending = false;
  uint64_t liftUntil = 0;             // chanTicks
  std::string liftFlow, liftWhy;
  int liftTries = 0;
  // iter-465 playthrough: SKIP CHAPTER, driven from the game's own objective state on every active tick
  struct ChapterSkip {
    bool on = false, xpDone = false;
    int64_t c0 = -1;
    uint64_t id = 0;
    int actions = 0;
    double t0 = 0;
    uint64_t cooldownUntil = 0;   // chanTicks
    uint64_t nextWaitLog = 0;     // chanTicks
  } chap;
  uint64_t lastReadoutTick = 0, chanTicks = 0;   // active ticks seen by the command channel
  // freecam (the retail debug camera): what `freecam on` saved for `freecam off`
  bool fcOn = false;
  uint32_t fcSavedState = 0, fcSavedCtx1 = 0;
};

Live& live() {
  static Live l;
  return l;
}

// ---- the virtual pad (tool `levelskip`, devtool class) ----------------------------------------
// A `pad` event is answered the way a player answers it: through the pad the game polls.
// kernel/stubs.cpp stub_XamInputGetState ORs devToolsVirtualPadButtons() into the state it
// already reports (no new guest write site; the stub's own XINPUT_STATE store carries it). The
// buttons are pressed in order, each held kHoldMs, kPeriodMs apart, after a start delay: 2 s for
// an in-tick event (the popup has just been created and may not take input yet; xenia_skip.py
// --pad-delay), 0.5 s for an idle event (the tick has already been stalled for >= 10 s).
struct VPad {
  std::mutex mu;
  std::vector<uint16_t> masks;
  std::vector<int> holdMs;
  std::chrono::steady_clock::time_point start;
};
VPad& vpad() {
  static VPad v;
  return v;
}
std::atomic<bool> g_vpadArmed{false};
constexpr int kPeriodMs = 450;
constexpr int kHoldMs = 150;
constexpr int kLongHoldMs = 400;     // '@' tokens (xenia_pad.py's longer hold)

uint16_t padMask(const std::string& name) {
  static const std::pair<const char*, uint16_t> kMap[] = {
      {"UP", 0x0001},   {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
      {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000}};
  for (const auto& m : kMap)
    if (name == m.first) return m.second;
  return 0;
}

void vpadQueue(const J* buttons, bool idle) {
  if (!buttons || !buttons->isArr()) return;
  VPad& v = vpad();
  std::lock_guard<std::mutex> lk(v.mu);
  v.masks.clear();
  v.holdMs.clear();
  for (const J& b : buttons->a()) {
    std::string t = b.s();
    const bool longHold = !t.empty() && t[0] == '@';
    if (longHold) t = t.substr(1);
    uint16_t m = 0;
    size_t a = 0;
    for (;;) {
      const size_t plus = t.find('+', a);
      m |= padMask(t.substr(a, plus == std::string::npos ? std::string::npos : plus - a));
      if (plus == std::string::npos) break;
      a = plus + 1;
    }
    v.masks.push_back(m);
    v.holdMs.push_back(longHold ? kLongHoldMs : kHoldMs);
  }
  v.start = std::chrono::steady_clock::now() + std::chrono::milliseconds(idle ? 500 : 2000);
  g_vpadArmed.store(!v.masks.empty());
}

S::Graph baseGraph() {
  S::Graph g;
  g.ids.assign(std::begin(kBaseGraphIds), std::end(kBaseGraphIds));
  size_t at = 0;
  for (uint16_t n : kBaseGraphTransCount) {
    std::vector<int64_t> t;
    for (uint16_t k = 0; k < n; k++) t.push_back(kBaseGraphTrans[at++]);
    g.trans.push_back(std::move(t));
  }
  g.hasFail = kBaseGraphHasFail;
  g.failObjective.assign(kBaseGraphFailObjective, kBaseGraphFailObjective + kBaseGraphFailObjectiveN);
  g.failGameOver.assign(kBaseGraphFailGameOver, kBaseGraphFailGameOver + kBaseGraphFailGameOverN);
  return g;
}

// One guest page, fault-free: hostRangeReadableCached proves it committed/readable/not guard.
S::PageView livePage(uint32_t pg) {
  Live& L = live();
  uint8_t* p = L.base + (static_cast<uint64_t>(pg) << 12);
  if (!sr::kernel::hostRangeReadableCached(p, 4096)) {
    if (L.recording) L.tickMissing.push_back(pg << 12);
    return {};
  }
  if (L.recording && !L.tickPages.count(pg)) L.tickPages.emplace(pg, std::vector<uint8_t>(p, p + 4096));
  return {p, 4096};
}

#ifdef _WIN32
// FRESH writability proof for one write (never cached): every page committed, writable, no guard.
bool hostRangeWritable(const uint8_t* p, size_t n) {
  const uint8_t* end = p + n;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
      return false;
    p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}
#else
bool hostRangeWritable(const uint8_t*, size_t) { return false; }
#endif

// One write-list entry -> (va, width, big-endian bytes); no guest access.
struct WriteSpec {
  uint32_t va = 0;
  int width = 0;
  uint8_t bytes[8] = {};
};
WriteSpec parseWrite(const J& w) {
  WriteSpec x;
  x.va = static_cast<uint32_t>(std::stoul(w.get("va")->s(), nullptr, 16));
  x.width = static_cast<int>(w.get("width")->i());
  const std::string hx = w.get("new")->s().substr(2);
  if ((x.width != 1 && x.width != 4 && x.width != 8) || hx.size() != static_cast<size_t>(2 * x.width) ||
      (x.va & (x.width - 1)))
    throw S::Refusal{"unwritable", "bad write shape " + w.dump()};
  for (int k = 0; k < x.width; k++) x.bytes[k] = static_cast<uint8_t>(std::stoul(hx.substr(2 * k, 2), nullptr, 16));
  return x;
}

// The ONLY guest stores of the plan executor: one store from a write list. A plan's stores need
// `levelskip`; a plan's cheat command (DT-2) needs `cheats` as well (docs/DEVTOOLS.md 6).
void liveWrite(uint8_t* base, const J& w, bool cheatCommand) {
  if (!sr::kernel::devToolAllowed("levelskip")) throw S::Refusal{"unwritable", "levelskip is off"};
  if (cheatCommand) {
    if (!sr::kernel::devToolAllowed("cheats")) throw S::Refusal{"unwritable", "cheats is off"};
  }
  const WriteSpec x = parseWrite(w);
  if (!hostRangeWritable(base + x.va, static_cast<size_t>(x.width))) throw S::Refusal{"unwritable", w.get("va")->s()};
  std::memcpy(base + x.va, x.bytes, static_cast<size_t>(x.width));
}

// The ONLY guest stores of the command channel (devToolsSubmit, F6 / F7): tool `cheats`.
void liveWriteCmd(uint8_t* base, const J& w) {
  if (!sr::kernel::devToolAllowed("cheats")) throw S::Refusal{"unwritable", "cheats is off"};
  const WriteSpec x = parseWrite(w);
  if (!hostRangeWritable(base + x.va, static_cast<size_t>(x.width))) throw S::Refusal{"unwritable", w.get("va")->s()};
  std::memcpy(base + x.va, x.bytes, static_cast<size_t>(x.width));
}

void logChunks(const char* tag, const std::string& s) {
  constexpr size_t kChunk = 3600;
  if (s.size() <= kChunk) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "%s %s", tag, s.c_str());
    return;
  }
  const size_t n = (s.size() + kChunk - 1) / kChunk;
  for (size_t i = 0; i < n; i++)
    SR_LOG_BOOT(sr::LogSeverity::Info, "%s part %zu/%zu %s", tag, i + 1, n, s.substr(i * kChunk, kChunk).c_str());
}

uint32_t crc32(const std::string& data) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void putLe32(std::string& s, uint32_t v) {
  for (int k = 0; k < 4; k++) s += static_cast<char>((v >> (8 * k)) & 0xFF);
}

std::string isoNow() {
  const std::time_t now = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  return ts;
}

// tools/guest_snap.py format (guest_snap/1): the pages this tick read, as it read them.
bool writeSnap(const fs::path& path, const std::string& label, const std::string& reader) {
  Live& L = live();
  J meta = J::obj();
  meta.set("schema", J::str("guest_snap/1"));
  J src = J::obj();
  src.set("kind", J::str("ours-live"));
  src.set("writer", J::str("kernel/devtools_levelskip.cpp"));
  meta.set("source", std::move(src));
  meta.set("created", J::str(isoNow()));
  J miss = J::arr();
  for (uint32_t va : L.tickMissing) miss.push(J::str(S::h8(va)));
  meta.set("missing", std::move(miss));
  meta.set("label", J::str(label));
  meta.set("reader", J::str(reader));
  meta.set("run_class", J::str("dev-tool-assisted"));
  const std::string mj = meta.dump();
  std::string body = "SRSNAP";
  body += '\0';
  body += '\x01';
  putLe32(body, static_cast<uint32_t>(mj.size()));
  body += mj;
  putLe32(body, static_cast<uint32_t>(L.tickPages.size()));
  for (const auto& kv : L.tickPages) {
    putLe32(body, kv.first);
    putLe32(body, static_cast<uint32_t>(kv.second.size()));
  }
  for (const auto& kv : L.tickPages) body.append(reinterpret_cast<const char*>(kv.second.data()), kv.second.size());
  putLe32(body, crc32(body));
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  return static_cast<bool>(f);
}

void writeText(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

void beginRecording() {
  Live& L = live();
  L.tickPages.clear();
  L.tickMissing.clear();
  L.recording = true;
}

std::optional<double> nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - live().t0).count();
}

void armLift(const std::string& flow);   // iter-465 (below)
void onEvent(const J& e) {
  Live& L = live();
  const std::string ev = e.get("event")->s();
  const std::string line = e.dump();
  logChunks("[DEVTOOL] event", line);
  {
    std::string brief = ev;
    if (e.get("prim") && e.get("prim")->isStr()) brief += " " + e.get("prim")->s();
    if (e.get("label") && e.get("label")->isStr()) brief += " " + e.get("label")->s();
    if (e.get("code") && e.get("code")->isStr()) brief += " (" + e.get("code")->s() + ")";
    L.lastEvent = brief;
  }
  if (L.steps) {
    std::fputs((line + "\n").c_str(), L.steps);
    std::fflush(L.steps);
  }
  const J* w = e.get("writes");
  const J* c = e.get("calls");
  const std::string prim = e.get("prim")->isStr() ? e.get("prim")->s() : "?";
  // DT-2: a dev-tool command in a plan is tagged with its own tool (cheats / freecam), as on the channel
  const char* tool = prim == "freecam" ? "freecam" : (S::isArgPrim(prim) ? "cheats" : "levelskip");
  if ((ev == "executed" && ((w && w->size()) || (c && c->size()))) || ev == "fallback_pass" || ev == "restore") {
    sr::kernel::devToolActsAdd(1);
    const S::Step& st = L.ex->steps()[static_cast<size_t>(e.get("k")->i())];
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=%s.%s args=%s%s%s%s tick=%lld writes=%s%s%s", tool,
                ev == "restore" ? "reach_restore" : prim.c_str(),
                st.idx ? std::to_string(*st.idx).c_str() : "", st.flow ? " " : "", st.flow ? st.flow->c_str() : "",
                st.arg ? st.arg->c_str() : "", static_cast<long long>(e.get("tick_active")->i()),
                w ? w->dump().c_str() : "[]", c ? " calls=" : "", c ? c->dump().c_str() : "");
    if (prim == "pass" && ev == "executed" && st.flow) armLift(*st.flow);
    L.pendingWriteEvents.push_back(e);
  } else if (ev == "refused" || ev == "timeout" || ev == "aborted") {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=%s.%s why=%s: %s", tool, prim.c_str(),
                e.get("code")->s().c_str(), e.get("why") ? e.get("why")->s().c_str() : "");
  } else if (ev == "snapshot") {
    L.pendingSnapshots.push_back(e.get("label")->s());
  } else if (ev == "pad") {
    sr::kernel::devToolActsAdd(1);
    const J* var = e.get("variant");
    const std::string variant = var && var->isStr() ? var->s() : "";
    vpadQueue(e.get("buttons"), variant == "idle");
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=levelskip.pad buttons=%s variant=%s (the virtual pad presses them)",
                e.get("buttons") ? e.get("buttons")->dump().c_str() : "[]", variant.c_str());
  }
}

// ================================================================ FREECAM ====
// The retail debug camera (docs/DEVTOOLS.md 6.4). CameraManagerCtor_sub_8218C4C0
// builds it inside the camera manager *0x8296D86C at +0x1150 (vt 0x820059A4, listener vt 0x82005994 at
// +4, subscribed to the input event channel 0x8295C624 for its whole life); its fly controls are
// CameraInputKeyHandler_sub_821691A8 (acts only while mgr+0x1310 == the camera) and its per-frame update
// is CameraDebugEntityInfoDraw_sub_82168D70. Retail only lacks the switch. This adds one, on the
// game's own tick thread:
//   on:  claim the camera manager's owner mode (+0x10 := 1; +0x1328 := 10 "none pending",
//        +0x132C := 0.0) when it is free (0), EulerAnglesToRotationMatrix_sub_82169048(cam) (the camera
//        starts at the game camera's frame), SetInterfaceState_sub_821745D0(mgr, 7, 1) (= the
//        inlined selector write: +0x1310 := cam, +0x1320 := 7; the camera's vt+0x14 activate is a
//        nullsub), and controller slot 1's input context := 7 = DebugContext (*(*0x8296D884+0x20)+0x1C;
//        InputCtx_DebugContext_Bind_sub_8229AFA0 binds the camera's actions 86-106 there). The camera
//        wants a SECOND pad; our host routes its pad and keyboard to guest controller 2
//        while the camera is on (kernel/stubs.cpp) and reports controller 1 neutral.
//   off: SetInterfaceState(mgr, saved state, 1); release the owner mode (+0x10 := 0) and apply a state
//        request that pended meanwhile (+0x1328 != 10: SetInterfaceState(mgr, it, 0), or
//        RequestInterfaceState_sub_8218CD68 when it carries a transition time);
//        SlotSwitch_SetMode_sub_821E8588(*0x8296DF4C, 1) (game mode 1
//        = running, undoing a debug-camera pause); slot 1's context is restored when it is still ours.
// Every store re-checks its page with a fresh VirtualQuery; every guest call runs as a callee of the
// hooked tick function with all registers restored afterwards. Tool `freecam` (devtool class).
constexpr uint32_t G_CAMERA_MGR = 0x8296D86Cu, CAM_DEBUG = 0x1150, CAM_OWNER = 0x10, CAM_SEL = 0x1310,
                   CAM_STATE = 0x1320, CAM_PENDING = 0x1328, CAM_PENDING_T = 0x132C, VT_DEBUG_CAM = 0x820059A4u,
                   VT_DEBUG_CAM_LISTENER = 0x82005994u, STATE_DEBUG_CAM = 7, PENDING_NONE = 10;
constexpr uint32_t G_INPUT_CTX_MGR = 0x8296D884u, ICM_SLOT_COUNT = 0x1C, ICM_SLOTS = 0x20, SLOT1_CTX = 0x1C,
                   CTX_DEBUG = 7;
constexpr uint32_t G_SLOT_SWITCH = 0x8296DF4Cu, MODE_RUNNING = 1;
std::atomic<bool> g_freecamRoute{false};

using GuestFn = void(PPCContext& __restrict, uint8_t*);

// One guest call from the drain point, on the game's own tick thread, as a callee of the hooked
// function: every register is saved, the arguments set, the stack pointer moved 0x200 below the
// caller's frame, and every register restored afterwards (so __imp__ then runs untouched).
// warp's argument vector on the guest stack scratch (fcCall): four big-endian floats. Tool `cheats`.
void fcStackVec(uint32_t va, const float* vec, int tool) {
  // each gate a literal on its own (audit D40): 2 = levelskip (the skip's lift), else cheats (warp)
  if (tool == 2) {
    if (!sr::kernel::devToolAllowed("levelskip")) throw S::Refusal{"tool_off", "levelskip is off"};
  } else if (!sr::kernel::devToolAllowed("cheats")) {
    throw S::Refusal{"tool_off", "cheats is off"};
  }
  Live& L = live();
  if ((va & 0xF) || !hostRangeWritable(L.base + va, 16)) throw S::Refusal{"unwritable", "guest stack " + S::h8(va)};
  uint8_t be[16];
  for (int k = 0; k < 4; k++) {
    uint32_t b;
    std::memcpy(&b, &vec[k], 4);
    for (int j = 0; j < 4; j++) be[4 * k + j] = static_cast<uint8_t>(b >> (24 - 8 * j));
  }
  std::memcpy(L.base + va, be, 16);
}

// `vec` (warp): four floats stored big-endian 16-aligned at the new r1 + 0x140, inside the 0x200 B gap
// above the callee's frame and past the callee's parameter-home area, and passed as r4. Returns the
// callee's r3.
uint32_t fcCall(GuestFn* fn, const char* name, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                std::optional<double> f1, J& calls, int cheatsTool = 0, const float* vec = nullptr) {
  // the call's own tool, each gate a literal on its own (audit D40): 0 freecam, 1 cheats, 2 levelskip
  if (cheatsTool == 2) {
    if (!sr::kernel::devToolAllowed("levelskip")) throw S::Refusal{"tool_off", "levelskip is off"};
  } else if (cheatsTool) {
    if (!sr::kernel::devToolAllowed("cheats")) throw S::Refusal{"tool_off", "cheats is off"};
  } else if (!sr::kernel::devToolAllowed("freecam")) {
    throw S::Refusal{"tool_off", "freecam is off"};
  }
  Live& L = live();
  if (!L.ctx) throw S::Refusal{"no_tick_context", "a guest call needs the hooked tick"};
  PPCContext& ctx = *L.ctx;
  const PPCContext saved = ctx;
  ctx.r1.u64 = (saved.r1.u32 - 0x200u) & ~0xFull;
  if (vec) {
    const uint32_t va = ctx.r1.u32 + 0x140u;
    try {
      fcStackVec(va, vec, cheatsTool);
    } catch (...) {
      ctx = saved;
      throw;
    }
    r4 = va;
  }
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  if (f1) ctx.f1.f64 = *f1;
  fn(ctx, L.base);
  const uint32_t ret = ctx.r3.u32;
  ctx = saved;
  J c = J::obj();
  c.set("fn", J::str(name));
  c.set("r3", J::str(S::h8(r3)));
  c.set("r4", J::str(S::h8(r4)));
  c.set("r5", J::str(S::h8(r5)));
  if (f1) c.set("f1", J::dbl(*f1));
  if (vec) {
    J v = J::arr();
    for (int k = 0; k < 4; k++) v.push(J::dbl(vec[k]));
    c.set("vec", std::move(v));
  }
  c.set("ret", J::str(S::h8(ret)));
  calls.push(std::move(c));
  return ret;
}

// One store the switch makes, re-checked fresh (committed, writable, not guard).
void fcStore(uint32_t va, int width, uint32_t value, const char* what, J& writes) {
  if (!sr::kernel::devToolAllowed("freecam")) throw S::Refusal{"tool_off", "freecam is off"};
  Live& L = live();
  uint8_t* p = L.base + va;
  if ((width != 1 && width != 4) || (va & (width - 1)) || !hostRangeWritable(p, static_cast<size_t>(width)))
    throw S::Refusal{"unwritable", S::h8(va)};
  uint32_t old = 0;
  for (int k = 0; k < width; k++) old = (old << 8) | p[k];
  uint8_t nb[4];
  for (int k = 0; k < width; k++) nb[k] = static_cast<uint8_t>(value >> (8 * (width - 1 - k)));
  std::memcpy(p, nb, static_cast<size_t>(width));
  J w = J::obj();
  w.set("va", J::str(S::h8(va)));
  w.set("width", J::num(width));
  w.set("old", J::str(S::h8(old)));
  w.set("new", J::str(S::h8(value)));
  w.set("what", J::str(what));
  writes.push(std::move(w));
}

// Fresh pure reads (a new World per read group: a guest call may have changed what the last one cached).
uint32_t fcU32(uint32_t va) {
  S::World w(livePage, &live().graph);
  return w.mem().u32(va);
}

J freecamCall(const S::Step& st) {
  if (!sr::kernel::devToolAllowed("freecam")) throw S::Refusal{"tool_off", "the freecam tool is not enabled"};
  Live& L = live();
  J writes = J::arr(), calls = J::arr();
  const uint32_t mgr = fcU32(G_CAMERA_MGR);
  if (!mgr) throw S::Refusal{"no_camera_mgr", "*0x8296D86C is null"};
  const uint32_t cam = mgr + CAM_DEBUG;
  if (fcU32(cam) != VT_DEBUG_CAM || fcU32(cam + 4) != VT_DEBUG_CAM_LISTENER)
    throw S::Refusal{"camera_vt", "no debug camera at camera manager +0x1150"};
  const uint32_t icm = fcU32(G_INPUT_CTX_MGR);
  const uint32_t slots = icm ? fcU32(icm + ICM_SLOTS) : 0;
  if (!slots || fcU32(icm + ICM_SLOT_COUNT) < 2) throw S::Refusal{"no_input_slots", "input context manager"};
  if (st.arg && *st.arg == "on") {
    if (L.fcOn || fcU32(mgr + CAM_SEL) == cam) throw S::Refusal{"already_on", "the debug camera is active"};
    const uint32_t owner = fcU32(mgr + CAM_OWNER);
    if (owner != 0) throw S::Refusal{"camera_owned", "camera manager owner mode " + std::to_string(owner)};
    L.fcSavedState = fcU32(mgr + CAM_STATE);
    L.fcSavedCtx1 = fcU32(slots + SLOT1_CTX);
    fcStore(mgr + CAM_PENDING_T, 4, 0, "freecam on: pending transition time := 0.0", writes);
    fcStore(mgr + CAM_PENDING, 4, PENDING_NONE, "freecam on: pending state := 10 (none)", writes);
    fcStore(mgr + CAM_OWNER, 4, 1, "freecam on: owner mode := 1", writes);
    fcCall(sub_82169048, "EulerAnglesToRotationMatrix_sub_82169048", cam, 0, 0, 0, std::nullopt, calls);
    fcCall(sub_821745D0, "SetInterfaceState_sub_821745D0", mgr, STATE_DEBUG_CAM, 1, 0, std::nullopt, calls);
    if (fcU32(mgr + CAM_SEL) != cam) {   // put back what this command stored, then refuse
      fcStore(mgr + CAM_OWNER, 4, 0, "freecam on refused: owner mode := 0", writes);
      throw S::Refusal{"not_selected", "SetInterfaceState did not select the camera"};
    }
    fcStore(slots + SLOT1_CTX, 4, CTX_DEBUG, "freecam on: controller 2 input context := 7 (DebugContext)", writes);
    L.fcOn = true;
    g_freecamRoute.store(true);
  } else {
    if (!L.fcOn) throw S::Refusal{"not_on", "freecam was not turned on by this run"};
    fcCall(sub_821745D0, "SetInterfaceState_sub_821745D0", mgr, L.fcSavedState, 1, 0, std::nullopt, calls);
    const uint32_t owner = fcU32(mgr + CAM_OWNER);
    if (owner == 0 || owner == 1) {
      fcStore(mgr + CAM_OWNER, 4, 0, "freecam off: owner mode := 0", writes);
      const uint32_t pend = fcU32(mgr + CAM_PENDING);
      if (pend != PENDING_NONE) {
        const uint32_t tb = fcU32(mgr + CAM_PENDING_T);
        float t;
        std::memcpy(&t, &tb, 4);
        if (tb != 0)
          fcCall(sub_8218CD68, "RequestInterfaceState_sub_8218CD68", mgr, pend, 0, 0, static_cast<double>(t), calls);
        else
          fcCall(sub_821745D0, "SetInterfaceState_sub_821745D0", mgr, pend, 0, 0, std::nullopt, calls);
      }
    }
    const uint32_t sw = fcU32(G_SLOT_SWITCH);
    if (sw) fcCall(sub_821E8588, "SlotSwitch_SetMode_sub_821E8588", sw, MODE_RUNNING, 0, 0, std::nullopt, calls);
    if (fcU32(slots + SLOT1_CTX) == CTX_DEBUG)
      fcStore(slots + SLOT1_CTX, 4, L.fcSavedCtx1, "freecam off: controller 2 input context restored", writes);
    L.fcOn = false;
    g_freecamRoute.store(false);
  }
  J x = J::obj();
  x.set("writes", std::move(writes));
  x.set("calls", std::move(calls));
  return x;
}

// ================================================================ WARP ====
// Warp: the Superman actor is the pad actor, GameInput_GetPadActor_sub_8229E210(*0x8296E8C8, 0),
// class vt 0x82009798; its vt+0xC = sub_821AA218 stores actor+0x20 := pos and tail-calls the physics
// proxy's (+0x70) vt+0xC, sub_821BA300, which moves the body (sub_825EB3D0) when the position differs.
// `warp camera` takes the free camera's position (debug camera +0x70, copied there by
// EulerAnglesToRotationMatrix_sub_82169048 and moved by its fly controls). Tool `cheats`; a guest
// call on the game's tick thread, like freecam. Command channel only (not in the plan language).
constexpr uint32_t G_INPUT_CTL = 0x8296E8C8u, VT_SUPERMAN_ACTOR = 0x82009798u, ACTOR_POS = 0x20,
                   ACTOR_VT_SETPOS = 0x0C, CAM_POS = 0x70, SETPOS_FN = 0x821AA218u;
// A command-channel line that is not a plan step: `warp ...` or `infinite_stamina on|off`.
struct WarpReq {
  bool stamina = false;   // infinite_stamina (below) instead of warp
  bool on = false;        // infinite_stamina on|off
  bool toCamera = false;
  bool lift = false;      // iter-465: after a minigame skip, move Superman clear of its start circle (tool levelskip)
  float pos[3] = {0, 0, 0};
};

uint8_t fcU8(uint32_t va) {
  S::World w(livePage, &live().graph);
  return w.mem().u8(va);
}

float fcF32(uint32_t va) {
  const uint32_t b = fcU32(va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

J warpRun(const WarpReq& rq) {
  if (rq.lift) {
    if (!sr::kernel::devToolAllowed("levelskip")) throw S::Refusal{"tool_off", "the levelskip tool is not enabled"};
  } else if (!sr::kernel::devToolAllowed("cheats")) {
    throw S::Refusal{"tool_off", "the cheats tool is not enabled"};
  }
  const int callTool = rq.lift ? 2 : 1;
  Live& L = live();
  float pos[4] = {rq.pos[0], rq.pos[1], rq.pos[2], 0.0f};
  if (rq.toCamera) {
    if (!L.fcOn) throw S::Refusal{"not_on", "warp camera needs the free camera on"};
    const uint32_t mgr = fcU32(G_CAMERA_MGR);
    if (!mgr || fcU32(mgr + CAM_DEBUG) != VT_DEBUG_CAM) throw S::Refusal{"camera_vt", "no debug camera"};
    for (int k = 0; k < 3; k++) pos[k] = fcF32(mgr + CAM_DEBUG + CAM_POS + 4u * k);
  }
  for (int k = 0; k < 3; k++)
    if (!std::isfinite(pos[k]) || std::fabs(pos[k]) > 1.0e6f) throw S::Refusal{"bad_position", "not finite / too far"};
  const uint32_t ictl = fcU32(G_INPUT_CTL);
  if (!ictl) throw S::Refusal{"no_input_ctl", "*0x8296E8C8 is null"};
  J calls = J::arr();
  const uint32_t actor = fcCall(sub_8229E210, "GameInput_GetPadActor_sub_8229E210", ictl, 0, 0, 0, std::nullopt,
                                calls, callTool);
  if (!actor) throw S::Refusal{"no_superman", "no actor on pad 0"};
  const uint32_t vt = fcU32(actor);
  if (vt != VT_SUPERMAN_ACTOR || fcU32(vt + ACTOR_VT_SETPOS) != SETPOS_FN)
    throw S::Refusal{"no_superman", "pad 0's actor " + S::h8(actor) + " vt " + S::h8(vt) + " is not the Superman actor class"};
  auto posJ = [&](uint32_t va) {
    J a = J::arr();
    for (int k = 0; k < 3; k++) a.push(J::dbl(fcF32(va + 4u * k)));
    return a;
  };
  J before = posJ(actor + ACTOR_POS);
  if (rq.lift) {   // relative: rq.pos is an offset from where Superman stands
    for (int k = 0; k < 3; k++) pos[k] = fcF32(actor + ACTOR_POS + 4u * k) + rq.pos[k];
    for (int k = 0; k < 3; k++)
      if (!std::isfinite(pos[k]) || std::fabs(pos[k]) > 1.0e6f) throw S::Refusal{"bad_position", "not finite / too far"};
  }
  fcCall(sub_821AA218, "SupermanActor_SetPosition_sub_821AA218", actor, 0, 0, 0, std::nullopt, calls, callTool, pos);
  J x = J::obj();
  x.set("actor", J::str(S::h8(actor)));
  x.set("before", std::move(before));
  J tgt = J::arr();
  for (int k = 0; k < 3; k++) tgt.push(J::dbl(pos[k]));
  x.set("target", std::move(tgt));
  x.set("after", posJ(actor + ACTOR_POS));
  x.set("writes", J::arr());   // every store is the game's own (sub_821AA218 and the proxy)
  x.set("calls", std::move(calls));
  return x;
}

// ================================================================ INFINITE STAMINA ====
// Superman's half of god mode; metro_lock is the Metropolis half. Retail's own route
// is its INFINITE_STAMINA code cheat (sub_82236840 case 1): the upgrade registry *0x8296DF6C entry
// "StaminaInfinite" (FindEntryByName_sub_821F0CB8, the cheat's own string 0x82009E6C) +0x23 owned
// := on, then ApplyPowerups_sub_8221F808(registry), when the Superman actor is up (+0x68 != 0).
// This command makes exactly those steps and NOT the cheat's own bytes: its active flag and the
// cheat-used byte (+0xC, which Achievements_CheckUnlockConditions reads). Tool `cheats`.
constexpr uint32_t G_UPGRADE_REG = 0x8296DF6Cu, STR_STAMINA_INFINITE = 0x82009E6Cu, UPG_OWNED = 0x23,
                   ACTOR_READY = 0x68;

// One byte store of a cheats command (fresh writability check; the old value is logged).
void cheatStore(uint32_t va, uint8_t value, const char* what, J& writes) {
  if (!sr::kernel::devToolAllowed("cheats")) throw S::Refusal{"tool_off", "cheats is off"};
  Live& L = live();
  uint8_t* p = L.base + va;
  if (!hostRangeWritable(p, 1)) throw S::Refusal{"unwritable", S::h8(va)};
  const uint8_t old = p[0];
  std::memcpy(p, &value, 1);
  J w = J::obj();
  w.set("va", J::str(S::h8(va)));
  w.set("width", J::num(1));
  w.set("old", J::str(S::h8(old)));
  w.set("new", J::str(S::h8(value)));
  w.set("what", J::str(what));
  writes.push(std::move(w));
}

J staminaRun(bool on) {
  if (!sr::kernel::devToolAllowed("cheats")) throw S::Refusal{"tool_off", "the cheats tool is not enabled"};
  const uint32_t reg = fcU32(G_UPGRADE_REG);
  if (!reg) throw S::Refusal{"no_upgrades", "*0x8296DF6C is null"};
  const uint32_t ictl = fcU32(G_INPUT_CTL);
  if (!ictl) throw S::Refusal{"no_input_ctl", "*0x8296E8C8 is null"};
  J calls = J::arr(), writes = J::arr();
  const uint32_t actor = fcCall(sub_8229E210, "GameInput_GetPadActor_sub_8229E210", ictl, 0, 0, 0, std::nullopt,
                                calls, /*cheatsTool=*/true);
  if (!actor || fcU32(actor) != VT_SUPERMAN_ACTOR || !fcU32(actor + ACTOR_READY))
    throw S::Refusal{"no_superman", "the Superman actor is not up (the cheat's own precondition)"};
  const uint32_t e = fcCall(sub_821F0CB8, "FindEntryByName_sub_821F0CB8", reg, STR_STAMINA_INFINITE, 0, 0,
                            std::nullopt, calls, /*cheatsTool=*/true);
  if (!e) throw S::Refusal{"no_entry", "no StaminaInfinite upgrade entry"};
  const uint8_t want = on ? 1 : 0;
  if (fcU8(e + UPG_OWNED) != want)
    cheatStore(e + UPG_OWNED, want, on ? "infinite_stamina on: StaminaInfinite owned := 1"
                                       : "infinite_stamina off: StaminaInfinite owned := 0", writes);
  fcCall(sub_8221F808, "ApplyPowerups_sub_8221F808", reg, 0, 0, 0, std::nullopt, calls, /*cheatsTool=*/true);
  J x = J::obj();
  x.set("actor", J::str(S::h8(actor)));
  x.set("entry", J::str(S::h8(e)));
  x.set("owned", J::num(fcU8(e + UPG_OWNED)));
  x.set("writes", std::move(writes));
  x.set("calls", std::move(calls));
  return x;
}

// ================================================================ COMMAND CHANNEL ====
// devToolsSubmit() parses on the caller's thread and queues; the tick thread runs one command at a
// time as a one-step executor (the plan's semantics: guard, timeout, refusal), sharing L.cheats.
struct Ticket {
  uint64_t id = 0;
  std::string line, status = "queued";
  J event;
};
struct Chan {
  std::mutex mu;                                   // guards everything below; never held over guest work
  struct Cmd {
    uint64_t id = 0;
    std::vector<S::Step> steps;   // a one-line plan, or empty for a warp
    std::optional<WarpReq> warp;
    bool skipMission = false;     // iter-465 playthrough: `skip_mission` (tool levelskip), resolved on the tick
    bool skipChapter = false;     // iter-465 playthrough: `skip_chapter` (tool levelskip), a tick-side controller
  };
  std::deque<Cmd> queue;
  std::deque<Ticket> tickets;                      // the last kMaxTickets, for the readout
  uint64_t next = 1;
  std::string cheatsJson = "null";                 // the tick thread's last World::cheatsReport (a host copy)
  // iter-465 playthrough: the active objective as the tick thread last read it (host copy for the menu)
  sr::kernel::DevToolsActiveMission active;
};
Chan& chan() {
  static Chan c;
  return c;
}
constexpr size_t kMaxQueued = 16, kMaxTickets = 32;

void ticketSet(uint64_t id, const std::string& status, const J* ev) {
  Chan& c = chan();
  std::lock_guard<std::mutex> lk(c.mu);
  for (auto& t : c.tickets)
    if (t.id == id) {
      t.status = status;
      if (ev) t.event = *ev;
    }
}

void onCmdEvent(const J& e) {
  Live& L = live();
  const std::string ev = e.get("event")->s();
  const std::string prim = e.get("prim")->isStr() ? e.get("prim")->s() : "?";
  const S::Step& st = L.cmdEx->steps()[static_cast<size_t>(e.get("k")->i())];
  const std::string tool = prim == "freecam" ? "freecam" : (L.cmdSkip ? "levelskip" : "cheats");
  if (ev == "executed") {
    const J* w = e.get("writes");
    const J* c = e.get("calls");
    if ((w && w->size()) || (c && c->size())) sr::kernel::devToolActsAdd(1);
    if (prim == "pass" && st.flow) armLift(*st.flow);
    std::string args = st.arg ? *st.arg : std::string();
    if (st.idx) args = std::to_string(*st.idx) + (st.flow ? " " + *st.flow : std::string());   // skip_mission's pass
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=%s.%s args=%s ticket=%llu tick=%lld writes=%s calls=%s",
                tool.c_str(), prim.c_str(), args.c_str(), static_cast<unsigned long long>(L.cmdId),
                static_cast<long long>(e.get("tick_active")->i()), w ? w->dump().c_str() : "[]",
                c ? c->dump().c_str() : "[]");
  } else if (ev == "refused" || ev == "timeout" || ev == "aborted") {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=%s.%s ticket=%llu why=%s: %s", tool.c_str(),
                prim.c_str(), static_cast<unsigned long long>(L.cmdId), e.get("code")->s().c_str(),
                e.get("why") ? e.get("why")->s().c_str() : "");
  }
  ticketSet(L.cmdId, ev == "executed" ? "done" : (ev == "timeout" ? "timeout" : (ev == "aborted" ? "stopped" : (ev == "refused" ? "refused" : "running"))), &e);
  if (!L.outDir.empty() && L.steps) {
    J line = e;
    line.set("ticket", J::num(static_cast<int64_t>(L.cmdId)));
    line.set("channel", J::str("command"));
    std::fputs((line.dump() + "\n").c_str(), L.steps);
    std::fflush(L.steps);
  }
}

bool isMinigameFlow(const std::string& f) { return f == "FastFlyer" || f == "PlayableBIZ"; }
void armLift(const std::string& flow) {
  Live& L = live();
  if (!isMinigameFlow(flow)) return;
  L.liftPending = true;
  L.liftUntil = L.chanTicks + 900;   // ~30 s of active ticks to see Superman back on pad 0
  L.liftFlow = flow;
  L.liftWhy.clear();
  L.liftTries = 0;
}
// One attempt per active tick while pending: +45 on X and +20 on Y from where Superman stands (the race and
// rampage start radii are 30 / 20); refused while pad 0 is not the Superman actor (Bizarro's rampage end).
void liftTick() {
  Live& L = live();
  if (!L.liftPending) return;
  if (L.chanTicks > L.liftUntil) {
    L.liftPending = false;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=levelskip.lift why=timeout after %d tries (%s): last %s",
                L.liftTries, L.liftFlow.c_str(), L.liftWhy.c_str());
    return;
  }
  WarpReq rq;
  rq.lift = true;
  rq.pos[0] = 45.0f;
  rq.pos[1] = 20.0f;
  try {
    J x = warpRun(rq);
    L.liftPending = false;
    sr::kernel::devToolActsAdd(1);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=levelskip.lift args=%s before=%s after=%s calls=%s",
                L.liftFlow.c_str(), x.get("before")->dump().c_str(), x.get("after")->dump().c_str(),
                x.get("calls")->dump().c_str());
  } catch (const S::Refusal& r) {
    // not Superman yet (or no input controller): try again next tick
    L.liftTries++;
    if (r.code + ": " + r.why != L.liftWhy) {
      L.liftWhy = r.code + ": " + r.why;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] levelskip.lift waiting (%s): %s", L.liftFlow.c_str(), L.liftWhy.c_str());
    }
  }
}

// ============================================================ SKIP CHAPTER (iter-465) ====
// The owner: "a skip ADVANCES THE STORY; never cycle through the races". One `skip_chapter` command
// (menu SKIP CHAPTER, F11) brings the game to the next chapter through its OWN objective state, read
// every active tick (no fixed list order beyond the story spine the game's data defines):
//   - an objective is ACTIVE: a cutscene / chapter transition ends by itself (wait); anything else (the
//     story objective, or a metro event the game started) gets xp_to_cap once, then `pass`;
//   - nothing active: `reach` the first story trigger of the spine (data/devtools/objective_table_retail.tsv
//     order, docs/OBJECTIVE_TABLE.md) that the game has made AVAILABLE and that is not won (xp_to_cap first
//     when it is a cutscene); none available = the chapter's story waits for hero points: xp_to_cap and wait
//     for the game's own next objective (a metro event), which the step above then completes.
//   - done when currentHeroPointChapter > the chapter at the press and nothing is active.
// It never starts a race or a Bizarro rampage (they are not on the spine); one the player is IN gets passed
// like any active objective. Every action is a one-line command on the same executor as skip_mission (the
// plan's guards and write lists; tool levelskip). A wall-clock limit of 20 minutes stops it.
struct SpineRow {
  int idx;
  const char* flow;
};
const SpineRow kSpine[] = {
    {147, "CinematicSupermanReturns"}, {0, "MSTobj01"}, {144, "MST1_2_Transition"}, {1, "MSTobj02"},
    {145, "MST2_3_Transition"}, {2, "MSTobj03"}, {143, "CinematicLoisAsks"}, {5, "WARobj01"},
    {142, "CinematicMongulAttacks"}, {6, "WARobj02"}, {121, "CinematicLuthorSteals"}, {135, "3_4_Transition"},
    {122, "CinematicBadNews"}, {123, "CinematicLexsFirst"}, {136, "6_7_Transition"}, {141, "CinematicMetallo2Intro"},
    {34, "MTOobj09"}, {124, "CinematicLexsPlot"}, {137, "10_11_Transition"}, {146, "BIZ03_Transition"},
    {12, "BIZobj03"}, {148, "BIZ15_Transition"}, {44, "BIZobj15"}, {128, "CinematicLoisLasso"},
    {125, "CinematicPrison"}, {138, "11_12_Transition"}, {139, "15_16_Transition"}, {140, "16_17_Transition"},
    {129, "CinematicSupesDecides"}, {22, "TWSobj04"}, {126, "CinematicLexIsland"},
};
bool endsByItself(const std::string& flow) {
  return flow.rfind("Cinematic", 0) == 0 || flow.find("_Transition") != std::string::npos;
}

void chapterFinish(const char* how, int64_t ch) {
  Live& L = live();
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_chapter ticket=%llu %s chapter %lld -> %lld actions=%d",
              static_cast<unsigned long long>(L.chap.id), how, static_cast<long long>(L.chap.c0),
              static_cast<long long>(ch), L.chap.actions);
  ticketSet(L.chap.id, std::string(how) == "done" ? "done" : "timeout", nullptr);
  L.chap.on = false;
}

void chapterStep(uint8_t* base) {
  Live& L = live();
  auto& C = L.chap;
  if (L.chanTicks < C.cooldownUntil) return;
  S::World w(livePage, &L.graph);
  const auto ch = w.dasVal("currentHeroPointChapter");
  const auto act = w.dasVal("objectiveActivated");
  const auto wfl = w.dasVal("waitingForLoad");
  const double el = nowSeconds().value_or(C.t0) - C.t0;
  if (ch && act && static_cast<int64_t>(*ch) > C.c0 && *act == 0) {
    chapterFinish("done", static_cast<int64_t>(*ch));
    return;
  }
  if (el > 1200) {
    chapterFinish("timeout", ch ? static_cast<int64_t>(*ch) : -1);
    return;
  }
  if (!ch || !act || (wfl && *wfl != 0)) return;
  const std::string xpFree =
      "timeout 1800\nwhen das objectiveActivated == 0 and das waitingForLoad == 0 and stable 30 do xp_to_cap";
  std::string line, why;
  if (*act == 1) {
    const auto bi = w.dasVal("objectiveBitIndex");
    const auto fl = w.dasStr(w.base().second, "objectiveWorldFlowName");
    if (!bi || !fl || fl->empty() || fl->find_first_of(" #;") != std::string::npos) return;
    if (endsByItself(*fl)) return;   // a cutscene / chapter transition: the game ends it
    if (!C.xpDone) {
      line = "timeout 1800\nxp_to_cap";
      why = "xp_to_cap in " + *fl;
      C.xpDone = true;
    } else {
      line = "timeout 1800\npass " + std::to_string(*bi) + " " + *fl;
      why = "pass the active " + *fl + " (" + std::to_string(*bi) + ")";
      C.cooldownUntil = L.chanTicks + 300;   // let its end (state 105) run before looking again
    }
  } else {
    const SpineRow* pick = nullptr;
    for (const auto& r : kSpine)
      if (!w.won(r.idx) && w.triggerCond(r.idx, "available")) {
        pick = &r;
        break;
      }
    if (pick && !(endsByItself(pick->flow) && !C.xpDone)) {
      line = "timeout 1800\nwhen trigger " + std::to_string(pick->idx) +
             " available and das objectiveActivated == 0 and das triggersAvailable == 1 and das waitingForLoad == 0 "
             "and base_state 1807139120 do reach " + std::to_string(pick->idx) + " " + pick->flow;
      why = std::string("reach the next story trigger ") + pick->flow;
      C.cooldownUntil = L.chanTicks + 150;
    } else if (!C.xpDone) {
      line = xpFree;
      why = pick ? "xp_to_cap before the cutscene" : "xp_to_cap; the story waits for hero points (next metro event)";
      C.xpDone = true;
    } else {
      // The story waits for hero points: the game's own source is its metro events, which a player FLIES
      // to (chap2: an idle Superman never engages one). Reach the first metro-event trigger the game has
      // spawned and made available (flow MEnn...); races / rampages are never picked.
      const J& trs = w.triggers();
      std::string seen;
      for (const J& t : trs.a()) {
        const J* ji = t.get("idx");
        const J* jf = t.get("flow");
        if (!ji || !jf || !jf->isStr()) continue;
        const std::string f = jf->s();
        const int64_t ix = ji->i();
        const bool avail = w.triggerCond(ix, "available");
        if (seen.size() < 600) seen += std::to_string(ix) + ":" + f + (avail ? "+ " : " ");
        if (!avail || f.rfind("ME", 0) != 0 || f.find_first_of(" #;") != std::string::npos) continue;
        line = "timeout 1800\nwhen trigger " + std::to_string(ix) +
               " available and das objectiveActivated == 0 and das waitingForLoad == 0 do reach " +
               std::to_string(ix) + " " + f;
        why = "reach the metro event " + f + " (" + std::to_string(ix) + ")";
        C.cooldownUntil = L.chanTicks + 150;
        break;
      }
      if (line.empty()) {
        if (L.chanTicks >= C.nextWaitLog) {
          C.nextWaitLog = L.chanTicks + 900;
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_chapter ticket=%llu chapter=%lld waiting for a metro event; "
                      "triggers (+ = available): %s", static_cast<unsigned long long>(C.id),
                      static_cast<long long>(*ch), seen.c_str());
        }
        return;
      }
    }
  }
  std::vector<S::Step> steps;
  try {
    steps = S::parsePlan(line);
  } catch (const S::PlanError& e) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_chapter parse error: %s", e.msg.c_str());
    chapterFinish("stopped", static_cast<int64_t>(*ch));
    return;
  }
  C.actions++;
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_chapter ticket=%llu chapter=%lld action %d: %s",
              static_cast<unsigned long long>(C.id), static_cast<long long>(*ch), C.actions, why.c_str());
  L.cmdId = C.id;
  L.cmdSkip = true;
  L.cmdEx = std::make_unique<S::Executor>(
      steps, [] { return S::World(livePage, &live().graph); },
      [base](const J& wr) { liveWrite(base, wr, false); },
      /*capsPad=*/false, nowSeconds, onCmdEvent, L.cheats, freecamCall);
}

// The tick thread: the readout copy (every 30 active ticks while a channel tool is on, and after
// every command), then one tick of the current command (or the next queued one).
void commandsTick(uint8_t* base, const std::string& variant) {
  Live& L = live();
  bool chanTools = false;   // either channel tool; each gate stands alone (audit D40)
  if (sr::kernel::devToolAllowed("cheats")) chanTools = true;
  if (sr::kernel::devToolAllowed("freecam")) chanTools = true;
  if (sr::kernel::devToolAllowed("levelskip")) chanTools = true;   // iter-465: skip_mission
  if (sr::kernel::devToolAllowed("devmenu")) chanTools = true;     // iter-465: the menu's "SKIP: <name>"
  if (!chanTools) return;
  bool refresh = variant == "active" && L.chanTicks - L.lastReadoutTick >= 30;
  bool ranWarp = false;   // one command per active tick, as in a plan
  if (!L.cmdEx && variant == "active") {
    Chan& c = chan();
    std::optional<WarpReq> warp;
    uint64_t wid = 0;
    {
      std::lock_guard<std::mutex> lk(c.mu);
      if (!c.queue.empty() && c.queue.front().warp) {
        wid = c.queue.front().id;
        warp = c.queue.front().warp;
        c.queue.pop_front();
      }
    }
    if (warp) {
      const char* prim = warp->stamina ? "infinite_stamina" : "warp";
      J ev = J::obj();
      ev.set("prim", J::str(prim));
      if (warp->stamina) ev.set("arg", J::str(warp->on ? "on" : "off"));
      ev.set("tick_active", J::num(static_cast<int64_t>(L.chanTicks)));
      try {
        J x = warp->stamina ? staminaRun(warp->on) : warpRun(*warp);
        ev.set("event", J::str("executed"));
        for (const char* k : {"actor", "entry", "owned", "before", "target", "after", "writes", "calls"})
          if (x.get(k)) ev.set(k, *x.get(k));
        sr::kernel::devToolActsAdd(1);
        if (warp->stamina)
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=cheats.infinite_stamina args=%s ticket=%llu actor=%s "
                      "entry=%s owned=%lld writes=%s calls=%s", warp->on ? "on" : "off",
                      static_cast<unsigned long long>(wid), x.get("actor")->s().c_str(), x.get("entry")->s().c_str(),
                      static_cast<long long>(x.get("owned")->i()), x.get("writes")->dump().c_str(),
                      x.get("calls")->dump().c_str());
        else
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] act=cheats.warp args=%s ticket=%llu actor=%s before=%s "
                      "target=%s after=%s calls=%s", warp->toCamera ? "camera" : "xyz",
                      static_cast<unsigned long long>(wid), x.get("actor")->s().c_str(),
                      x.get("before")->dump().c_str(), x.get("target")->dump().c_str(),
                      x.get("after")->dump().c_str(), x.get("calls")->dump().c_str());
        ticketSet(wid, "done", &ev);
      } catch (const S::Refusal& r) {
        ev.set("event", J::str("refused"));
        ev.set("code", J::str(r.code));
        ev.set("why", J::str(r.why));
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=cheats.%s ticket=%llu why=%s: %s", prim,
                    static_cast<unsigned long long>(wid), r.code.c_str(), r.why.c_str());
        ticketSet(wid, "refused", &ev);
      }
      if (!L.outDir.empty() && L.steps) {
        J line = ev;
        line.set("ticket", J::num(static_cast<int64_t>(wid)));
        line.set("channel", J::str("command"));
        std::fputs((line.dump() + "\n").c_str(), L.steps);
        std::fflush(L.steps);
      }
      refresh = true;
      ranWarp = true;
    }
  }
  if (!L.cmdEx && !ranWarp) {
    Chan& c = chan();
    std::vector<S::Step> steps;
    bool skip = false;
    bool startChapter = false;
    uint64_t chapterId = 0;
    {
      std::lock_guard<std::mutex> lk(c.mu);
      if (!c.queue.empty() && !c.queue.front().warp) {
        L.cmdId = c.queue.front().id;
        steps = std::move(c.queue.front().steps);
        skip = c.queue.front().skipMission;
        if (c.queue.front().skipChapter) {
          startChapter = true;
          chapterId = c.queue.front().id;
        }
        c.queue.pop_front();
      }
    }
    if (startChapter) {
      S::World w(livePage, &L.graph);
      const auto ch = w.dasVal("currentHeroPointChapter");
      L.chap = Live::ChapterSkip{};
      L.chap.on = true;
      L.chap.id = chapterId;
      L.chap.c0 = ch ? static_cast<int64_t>(*ch) : -1;
      L.chap.t0 = nowSeconds().value_or(0);
      ticketSet(chapterId, "running", nullptr);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_chapter ticket=%llu start chapter=%lld",
                  static_cast<unsigned long long>(chapterId), static_cast<long long>(L.chap.c0));
      refresh = true;
    }
    if (skip) {
      // iter-465 playthrough: SKIP CURRENT MISSION. Read which objective the game itself has active
      // (the base flow DAS: objectiveActivated, objectiveBitIndex, objectiveWorldFlowName) and pass
      // exactly that one - the same two stores as the plan's `pass` (objectiveComplete, objectiveWon),
      // so the game's own end-of-objective path (state 105) runs. Nothing active = refused.
      S::World w(livePage, &L.graph);
      const auto act = w.dasVal("objectiveActivated");
      const auto bi = w.dasVal("objectiveBitIndex");
      const auto fl = w.dasStr(w.base().second, "objectiveWorldFlowName");
      if (!act || *act != 1 || !bi || !fl || fl->empty() || fl->find_first_of(" #;") != std::string::npos) {
        J ev = J::obj();
        ev.set("prim", J::str("skip_mission"));
        ev.set("event", J::str("refused"));
        ev.set("code", J::str("nothing_active"));
        ev.set("why", J::str("no objective is active (objectiveActivated != 1)"));
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=levelskip.skip_mission ticket=%llu why=nothing_active",
                    static_cast<unsigned long long>(L.cmdId));
        ticketSet(L.cmdId, "refused", &ev);
        refresh = true;
      } else {
        const std::string line = "timeout 900\npass " + std::to_string(*bi) + " " + *fl;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] skip_mission ticket=%llu active idx=%u flow=%s -> pass",
                    static_cast<unsigned long long>(L.cmdId), *bi, fl->c_str());
        try {
          steps = S::parsePlan(line);
        } catch (const S::PlanError& e) {
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=levelskip.skip_mission ticket=%llu why=parse: %s",
                      static_cast<unsigned long long>(L.cmdId), e.msg.c_str());
          steps.clear();
          ticketSet(L.cmdId, "refused", nullptr);
        }
      }
    }
    if (!steps.empty()) {
      ticketSet(L.cmdId, "running", nullptr);
      L.cmdSkip = skip;
      L.cmdEx = std::make_unique<S::Executor>(
          steps, [] { return S::World(livePage, &live().graph); },
          [base](const J& w) {
            if (live().cmdSkip) liveWrite(base, w, false);   // a pass: tool levelskip, as in a plan
            else liveWriteCmd(base, w);
          },
          /*capsPad=*/false, nowSeconds, onCmdEvent, L.cheats, freecamCall);
    }
  }
  if (variant == "active") liftTick();
  if (!L.cmdEx && L.chap.on && variant == "active") chapterStep(base);
  if (L.cmdEx) {
    L.cmdEx->tick(variant);
    if (L.cmdEx->status() != "running") {
      if (L.cmdEx->status() == "done") ticketSet(L.cmdId, "done", nullptr);
      L.cmdEx.reset();
      refresh = true;
    }
  }
  if (refresh) {
    L.lastReadoutTick = L.chanTicks;
    S::World w(livePage, &L.graph);
    bool cheatsOn = false;   // each gate stands alone (audit D40)
    if (sr::kernel::devToolAllowed("cheats")) cheatsOn = true;
    if (sr::kernel::devToolAllowed("freecam")) cheatsOn = true;
    const std::string js = cheatsOn ? w.cheatsReport().dump() : std::string("null");
    sr::kernel::DevToolsActiveMission am;   // iter-465: what SKIP CURRENT MISSION would pass now
    try {
      const auto act = w.dasVal("objectiveActivated");
      const auto bi = w.dasVal("objectiveBitIndex");
      am.known = act.has_value();
      am.active = act && *act == 1;
      if (bi) am.idx = static_cast<int64_t>(*bi);
      if (am.active) am.flow = w.dasStr(w.base().second, "objectiveWorldFlowName").value_or("");
      am.tickActive = static_cast<int64_t>(L.chanTicks);
    } catch (...) {
      am = sr::kernel::DevToolsActiveMission{};
    }
    Chan& c = chan();
    std::lock_guard<std::mutex> lk(c.mu);
    c.cheatsJson = js;
    c.active = am;
  }
}

void initCommon() {
  Live& L = live();
  if (L.initDone) return;
  L.initDone = true;
  L.graph = baseGraph();
  L.t0 = std::chrono::steady_clock::now();
  L.outDir = sr::kernel::devToolsOutDir();
}

void initLevelskip(uint8_t* base) {
  Live& L = live();
  initCommon();
  L.base = base;
  if (!sr::kernel::devToolAllowed("levelskip")) return;
  const std::string path = sr::kernel::devToolsCmdsPath();
  if (path.empty()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip enabled without SR_DEVTOOLS_CMDS: nothing to run");
    return;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DEVTOOL] refused act=levelskip.load why=unreadable: \"%s\"", path.c_str());
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<S::Step> steps;
  try {
    steps = S::parsePlan(text);
  } catch (const S::PlanError& e) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DEVTOOL] refused act=levelskip.load why=parse: %s", e.msg.c_str());
    return;
  }
  for (const auto& st : steps) {
    if (!S::isArgPrim(st.prim)) continue;
    const bool fc = st.prim == "freecam";
    const char* tool = fc ? "freecam" : "cheats";
    const bool allowed = fc ? sr::kernel::devToolAllowed("freecam") : sr::kernel::devToolAllowed("cheats");
    if (!allowed) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[DEVTOOL] refused act=levelskip.load why=tool_off: the plan's '%s' needs "
                  "the %s tool (SR_DEVTOOLS=levelskip+%s)", st.prim.c_str(), tool, tool);
      return;
    }
  }
  if (!L.outDir.empty()) {
    std::error_code ec;
    fs::create_directories(L.outDir, ec);
    L.steps = std::fopen((fs::path(L.outDir) / "steps.jsonl").string().c_str(), "ab");
    writeText(fs::path(L.outDir) / "RUN_CLASS.json", sr::kernel::devToolsRunClassJson());
  }
  L.ex = std::make_unique<S::Executor>(
      steps, [] { return S::World(livePage, &live().graph); },
      [base](const J& w) {   // the current step's prim decides whether the cheats tool is needed too
        const S::Executor& ex = *live().ex;
        const bool cheat = ex.k() < ex.steps().size() && S::isArgPrim(ex.steps()[ex.k()].prim);
        liveWrite(base, w, cheat);
      },
      /*capsPad=*/true, nowSeconds, onEvent,      // the virtual pad (vpadQueue)
      L.cheats, freecamCall);                   // DT-2: shared state; freecam via guest calls
  L.planName = fs::path(path).filename().string();
  J parsed = J::arr();
  for (const auto& s : L.ex->steps()) parsed.push(S::stepJson(s));
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip loaded plan=\"%s\" steps=%zu out=\"%s\"", path.c_str(),
              L.ex->steps().size(), L.outDir.c_str());
  logChunks("[DEVTOOL] plan", parsed.dump());
}

std::string safeLabel(const std::string& s) {
  std::string o;
  for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
  return o;
}

// After a tick: snapshot steps get the full report + .snap; executed writes get the pre-write
// .snap of exactly what the tick read (the live write-list differential, skipexec on it).
void afterTick(const std::string& variant) {
  Live& L = live();
  for (const J& e : L.pendingWriteEvents) {
    if (L.outDir.empty()) continue;
    const std::string stem = "k" + std::to_string(e.get("k")->i()) + "_" + e.get("prim")->s() + "_" + e.get("event")->s();
    writeSnap(fs::path(L.outDir) / (stem + ".snap"), stem, "kernel executor tick (pre-write)");
    writeText(fs::path(L.outDir) / (stem + ".event.json"), e.dump() + "\n");
  }
  L.pendingWriteEvents.clear();
  for (const std::string& label : L.pendingSnapshots) {
    beginRecording();
    S::World w(livePage, &L.graph);
    std::vector<S::StepKey> keys;
    for (const auto& st : L.ex->steps())
      if (st.prim == "pass" || st.prim == "reach" || st.prim == "xp_to_cap") keys.push_back({st.prim, st.idx, st.flow});
    std::vector<std::string> conds;
    for (int i = 0; S::kReportConds[i]; i++) conds.push_back(S::kReportConds[i]);
    J rep = S::report(w, keys, conds);
    L.recording = false;
    rep.set("label", J::str(label));
    rep.set("variant", J::str(variant));
    rep.set("tick_active", J::num(L.ex->tickActive()));
    logChunks("[OBJSTATE]", rep.dump());
    if (!L.outDir.empty()) {
      writeSnap(fs::path(L.outDir) / (safeLabel(label) + ".snap"), label, "kernel report (devtools_skip_core)");
      writeText(fs::path(L.outDir) / (safeLabel(label) + ".report.json"), rep.dump() + "\n");
    }
  }
  L.pendingSnapshots.clear();
}

// tool `objstate` (observe): a story-state dump every N active ticks when the story key changed.
void objstateTick() {
  Live& L = live();
  if (++L.activeTicks % sr::kernel::devToolsObjstateEvery() != 0) return;
  beginRecording();
  S::World w(livePage, &L.graph);
  const std::string key = w.storyKey().dump();
  if (key == L.lastObjKey) {
    L.recording = false;
    return;
  }
  L.lastObjKey = key;
  std::vector<std::string> conds;
  for (int i = 0; S::kReportConds[i]; i++) conds.push_back(S::kReportConds[i]);
  J rep = S::report(w, {}, conds);
  L.recording = false;
  const uint32_t n = ++L.objDumps;
  rep.set("label", J::str("objstate_" + std::to_string(n)));
  rep.set("tick_active", J::num(static_cast<int64_t>(L.activeTicks)));
  logChunks("[OBJSTATE]", rep.dump());
  if (!L.outDir.empty()) {
    const std::string stem = "objstate_" + std::to_string(n);
    writeSnap(fs::path(L.outDir) / (stem + ".snap"), stem, "kernel report (devtools_skip_core)");
    writeText(fs::path(L.outDir) / (stem + ".report.json"), rep.dump() + "\n");
  }
}

void startPending() {
  Live& L = live();
  L.pending = false;
  if (!sr::kernel::devToolAllowed("levelskip")) return;
  if (L.outDir.empty()) {
    const std::string root = sr::kernel::contentRootDir();
    if (!root.empty()) L.outDir = (fs::path(root) / "devtools_out").string();
  }
  if (!L.outDir.empty() && !L.steps) {
    std::error_code ec;
    fs::create_directories(L.outDir, ec);
    L.steps = std::fopen((fs::path(L.outDir) / "steps.jsonl").string().c_str(), "ab");
  }
  if (!L.outDir.empty()) writeText(fs::path(L.outDir) / "RUN_CLASS.json", sr::kernel::devToolsRunClassJson());
  L.t0 = std::chrono::steady_clock::now();
  L.ex = std::make_unique<S::Executor>(
      L.pendingSteps, [] { return S::World(livePage, &live().graph); },
      [](const J& w) { const auto& ex = *live().ex;
        const bool cheat = ex.k() < ex.steps().size() && S::isArgPrim(ex.steps()[ex.k()].prim);
        liveWrite(live().base, w, cheat); }, /*capsPad=*/true, nowSeconds, onEvent, L.cheats, freecamCall);
  L.pendingSteps.clear();
  J parsed = J::arr();
  for (const auto& st : L.ex->steps()) parsed.push(S::stepJson(st));
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip loaded plan=\"%s\" steps=%zu out=\"%s\" (armed from the menu)",
              L.planName.c_str(), L.ex->steps().size(), L.outDir.c_str());
  logChunks("[DEVTOOL] plan", parsed.dump());
}

// One executor tick with the executor's own error handling (a host-side failure stops that
// executor, never the game).
void guardedTick(S::Executor* ex, const char* variant, const std::function<void()>& fn) {
  Live& L = live();
  try {
    fn();
  } catch (const S::Refusal& e) {
    L.recording = false;
    if (ex && ex->status() == "running") ex->stopExternal(variant, "executor_error", e.code + ": " + e.why);
  } catch (const S::ReadErr& e) {
    L.recording = false;
    if (ex && ex->status() == "running") ex->stopExternal(variant, "executor_error", e.msg);
  } catch (const S::UnknownErr& e) {
    L.recording = false;
    if (ex && ex->status() == "running") ex->stopExternal(variant, "executor_error", e.msg);
  } catch (const std::exception& e) {
    L.recording = false;
    if (ex && ex->status() == "running") ex->stopExternal(variant, "executor_error", e.what());
  }
}

void onTick(PPCContext& ctx, uint8_t* base, bool active) {
  Live& L = live();
  std::lock_guard<std::mutex> lk(L.mu);
  if (!L.initDone) initLevelskip(base);
  L.ctx = &ctx;   // the guest calls of a freecam command (fcCall), this tick only
  struct CtxClear {
    ~CtxClear() { live().ctx = nullptr; }
  } ctxClear;
  L.base = base;
  if (active && L.pending) startPending();
  if (!L.threadLogged) {
    L.threadLogged = true;
#ifdef _WIN32
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] first hooked tick (%s) on host tid=%lu (match [THREAD_SAMPLER])",
                active ? "active" : "observe", static_cast<unsigned long>(GetCurrentThreadId()));
#endif
  }
  const char* variant = active ? "active" : "observe";
  guardedTick(L.ex.get(), variant, [&] {
    if (active && sr::kernel::devToolAllowed("objstate")) objstateTick();
    if (L.ex && L.ex->status() == "running") {
      beginRecording();
      L.ex->tick(variant);
      L.recording = false;
      afterTick(variant);
      if (L.ex->status() != "running")
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip %s at k=%zu tick_active=%lld acts=%llu",
                    L.ex->status().c_str(), L.ex->k(), static_cast<long long>(L.ex->tickActive()),
                    static_cast<unsigned long long>(sr::kernel::devToolActs()));
    }
  });
  // the immediate command channel (DT-2), after the plan's step of this tick
  if (active) L.chanTicks++;
  guardedTick(L.cmdEx.get(), variant, [&] { commandsTick(base, variant); });
  if (L.cmdEx && L.cmdEx->status() != "running") L.cmdEx.reset();   // stopped by guardedTick (ticket: refused)
}

}  // namespace

namespace sr {
namespace kernel {
// ---- the in-game Dev Tools menu (kernel/devtools_menu.cpp): arm / stop / status ----
bool devToolsLevelskipArm(const std::string& planPath, std::string* err) {
  if (!devToolAllowed("levelskip")) {
    if (err) *err = "level skip is off";
    return false;
  }
  std::ifstream f(planPath, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot read " + planPath;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DEVTOOL] refused act=levelskip.load why=unreadable: \"%s\" (menu)", planPath.c_str());
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<S::Step> steps;
  try {
    steps = S::parsePlan(text);
  } catch (const S::PlanError& e) {
    if (err) *err = "plan does not parse: " + e.msg;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DEVTOOL] refused act=levelskip.load why=parse: %s (menu)", e.msg.c_str());
    return false;
  }
  Live& L = live();
  std::lock_guard<std::mutex> lk(L.mu);
  initCommon();
  if (L.ex && L.ex->status() == "running") L.ex->stopExternal("menu", "replaced", "another plan was armed from the menu");
  L.pendingSteps = std::move(steps);
  L.pending = true;
  L.planName = fs::path(planPath).filename().string();
  L.lastEvent = "armed";
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip armed plan=\"%s\" steps=%zu (menu; starts at the next active tick)",
              planPath.c_str(), L.pendingSteps.size());
  return true;
}

void devToolsLevelskipStop(const char* why) {
  Live& L = live();
  std::lock_guard<std::mutex> lk(L.mu);
  const bool wasPending = L.pending;
  L.pending = false;
  L.pendingSteps.clear();
  if (L.ex && L.ex->status() == "running") L.ex->stopExternal("menu", "stopped", why ? why : "stopped from the menu");
  else if (wasPending) L.lastEvent = "disarmed";
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip stop requested (%s)%s", why ? why : "menu",
              wasPending ? " - the armed plan never started" : "");
}

DevToolsSkipStatus devToolsLevelskipStatus() {
  Live& L = live();
  std::lock_guard<std::mutex> lk(L.mu);
  DevToolsSkipStatus st;
  st.plan = L.planName;
  st.last = L.lastEvent;
  if (L.pending) {
    st.state = "armed";
    st.n = L.pendingSteps.size();
  } else if (L.ex) {
    st.state = L.ex->status();
    st.k = L.ex->k();
    st.n = L.ex->steps().size();
    if (st.k < st.n) st.prim = L.ex->steps()[st.k].prim;
  } else {
    st.state = "idle";
  }
  return st;
}

// The host check while no tick runs (an FMV, a modal popup), every 10 s on the [GAUGE] thread:
// the wall-clock deadline, then the IDLE-PAD path (xenia_skip.py's: no hooked tick since the
// previous poll, 10 s ago, while a pad step is current; the save popup stops every hooked tick,
// so this is the only place its prompt is ever seen). The idle pad reads guest memory through
// the tick's own fault-free page reader (hostRangeReadableCached) and never writes it.
void devToolsPoll() {
  if (!devToolAllowed("levelskip")) return;
  Live& L = live();
  std::unique_lock<std::mutex> lk(L.mu, std::try_to_lock);
  if (!lk.owns_lock() || !L.ex || L.ex->status() != "running") return;
  static int64_t s_lastTickAll = -1;
  L.ex->poll(nowSeconds());
  if (L.ex->status() == "running" && L.ex->k() < L.ex->steps().size() && L.ex->steps()[L.ex->k()].prim == "pad" &&
      L.ex->tickAll() == s_lastTickAll) {
    try {
      const J r = L.ex->idlePad();
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip idle pad at k=%zu: %s", L.ex->k(), r.dump().c_str());
    } catch (const S::Refusal& e) {
      if (L.ex->status() == "running") L.ex->stopExternal("idle", "executor_error", e.code + ": " + e.why);
    } catch (const S::ReadErr& e) {
      if (L.ex->status() == "running") L.ex->stopExternal("idle", "executor_error", e.msg);
    } catch (const S::UnknownErr& e) {
      if (L.ex->status() == "running") L.ex->stopExternal("idle", "executor_error", e.msg);
    } catch (const std::exception& e) {
      if (L.ex->status() == "running") L.ex->stopExternal("idle", "executor_error", e.what());
    }
  }
  // iter-465 playthrough: SR_LEVELSKIP_DECLINE_SAVES=1 (dev tool, default off). While a plan runs, a save
  // prompt that is up (no hooked tick since the previous poll and pad_prompt holds) and is not the
  // current step's own `pad` step is answered Continue Without Saving (DOWN A) by the virtual pad - what
  // the autosave port feature does for an armed level skip - so a natural roll (port features off) can
  // cross the save prompt every chapter advance raises without a pad step per prompt in the plan.
  static const bool s_decline = [] {
    const char* e = std::getenv("SR_LEVELSKIP_DECLINE_SAVES");
    return e && e[0] == '1';
  }();
  if (s_decline && L.ex->status() == "running" && L.ex->tickAll() == s_lastTickAll &&
      !(L.ex->k() < L.ex->steps().size() && L.ex->steps()[L.ex->k()].prim == "pad") &&
      !g_vpadArmed.load(std::memory_order_relaxed)) {
    try {
      S::World w(livePage, &L.graph);
      S::Cond c;
      c.kind = "pad_prompt";
      if (w.cond(c)) {
        J b = J::arr();
        b.push(J::str("DOWN"));
        b.push(J::str("A"));
        vpadQueue(&b, true);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[DEVTOOL] levelskip decline-saves: the save prompt is up at k=%zu (no tick in a poll) -> pad DOWN A",
                    L.ex->k());
      }
    } catch (...) {
    }
  }
  s_lastTickAll = L.ex->tickAll();
  if (L.ex->status() != "running")
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] levelskip %s at k=%zu (host poll; no tick ran)", L.ex->status().c_str(),
                L.ex->k());
}

// The virtual pad's buttons right now (0 when nothing is queued). stub_XamInputGetState calls it
// on every poll: one relaxed atomic load when idle, and always 0 unless levelskip queued a press.
uint16_t devToolsVirtualPadButtons() {
  if (!g_vpadArmed.load(std::memory_order_relaxed)) return 0;
  VPad& v = vpad();
  std::lock_guard<std::mutex> lk(v.mu);
  const auto now = std::chrono::steady_clock::now();
  if (now < v.start) return 0;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - v.start).count();
  const size_t i = static_cast<size_t>(ms / kPeriodMs);
  if (i >= v.masks.size()) {
    g_vpadArmed.store(false);
    return 0;
  }
  return (ms % kPeriodMs) < v.holdMs[i] ? v.masks[i] : 0;
}

// ---- DT-2 command channel API (devtools.hpp) ----
uint64_t devToolsSubmit(const std::string& line, std::string* err) {
  auto fail = [&](const std::string& why) -> uint64_t {
    if (err) *err = why;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] refused act=submit why=%s: \"%s\"", why.c_str(), line.c_str());
    return 0;
  };
  std::vector<S::Step> steps;
  std::optional<WarpReq> warp;
  {
    std::vector<std::string> tok;
    std::string cur;
    for (char ch : line + " ") {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!cur.empty()) tok.push_back(cur);
        cur.clear();
      } else {
        cur += ch;
      }
    }
    if (!tok.empty() && tok[0] == "warp") {
      WarpReq rq;
      if (tok.size() == 2 && tok[1] == "camera") {
        rq.toCamera = true;
      } else if (tok.size() == 4) {
        for (int k = 0; k < 3; k++) {
          const char* b = tok[1 + k].c_str();
          char* e = nullptr;
          rq.pos[k] = std::strtof(b, &e);
          if (e == b || *e || !std::isfinite(rq.pos[k]) || std::fabs(rq.pos[k]) > 1.0e6f)
            return fail("warp: '" + tok[1 + k] + "' is not a coordinate");
        }
      } else {
        return fail("warp takes 'camera' or three coordinates X Y Z");
      }
      if (!devToolAllowed("cheats")) return fail("the cheats tool is not enabled (dev.txt / SR_DEVTOOLS)");
      if (rq.toCamera && !devToolAllowed("freecam")) return fail("warp camera needs the freecam tool");
      warp = rq;
    } else if (!tok.empty() && tok[0] == "skip_mission") {
      if (tok.size() != 1) return fail("skip_mission takes no arguments");
      if (!devToolAllowed("levelskip")) return fail("the levelskip tool is not enabled (dev.txt / SR_DEVTOOLS)");
      Chan& c = chan();
      std::lock_guard<std::mutex> lk(c.mu);
      if (c.queue.size() >= kMaxQueued) return fail("queue full (" + std::to_string(kMaxQueued) + " commands wait)");
      const uint64_t id = c.next++;
      Chan::Cmd cmd;
      cmd.id = id;
      cmd.skipMission = true;
      c.queue.push_back(std::move(cmd));
      Ticket t;
      t.id = id;
      t.line = line;
      c.tickets.push_back(std::move(t));
      while (c.tickets.size() > kMaxTickets) c.tickets.pop_front();
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] submit ticket=%llu \"%s\"", static_cast<unsigned long long>(id),
                  line.c_str());
      return id;
    } else if (!tok.empty() && tok[0] == "skip_chapter") {
      if (tok.size() != 1) return fail("skip_chapter takes no arguments");
      if (!devToolAllowed("levelskip")) return fail("the levelskip tool is not enabled (dev.txt / SR_DEVTOOLS)");
      Chan& c = chan();
      std::lock_guard<std::mutex> lk(c.mu);
      if (c.queue.size() >= kMaxQueued) return fail("queue full (" + std::to_string(kMaxQueued) + " commands wait)");
      const uint64_t id = c.next++;
      Chan::Cmd cmd;
      cmd.id = id;
      cmd.skipChapter = true;
      c.queue.push_back(std::move(cmd));
      Ticket t;
      t.id = id;
      t.line = line;
      c.tickets.push_back(std::move(t));
      while (c.tickets.size() > kMaxTickets) c.tickets.pop_front();
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] submit ticket=%llu \"%s\"", static_cast<unsigned long long>(id),
                  line.c_str());
      return id;
    } else if (!tok.empty() && tok[0] == "infinite_stamina") {
      if (tok.size() != 2 || (tok[1] != "on" && tok[1] != "off")) return fail("infinite_stamina takes on|off");
      if (!devToolAllowed("cheats")) return fail("the cheats tool is not enabled (dev.txt / SR_DEVTOOLS)");
      WarpReq rq;
      rq.stamina = true;
      rq.on = tok[1] == "on";
      warp = rq;
    }
  }
  if (!warp) {
    try {
      steps = S::parsePlan(line);
    } catch (const S::PlanError& e) {
      return fail("parse: " + e.msg);
    }
    if (steps.size() != 1) return fail("one command per submit (got " + std::to_string(steps.size()) + ")");
    const S::Step& st = steps[0];
    if (!S::isArgPrim(st.prim))
      return fail("not a command: " + st.prim +
                  " (pause_timers, minievents, metro_lock, freecam, warp, infinite_stamina)");
    if (st.prim == "freecam") {
      if (!devToolAllowed("freecam")) return fail("the freecam tool is not enabled (dev.txt / SR_DEVTOOLS)");
    } else if (!devToolAllowed("cheats")) {
      return fail("the cheats tool is not enabled (dev.txt / SR_DEVTOOLS)");
    }
  }
  Chan& c = chan();
  std::lock_guard<std::mutex> lk(c.mu);
  if (c.queue.size() >= kMaxQueued) return fail("queue full (" + std::to_string(kMaxQueued) + " commands wait)");
  const uint64_t id = c.next++;
  Chan::Cmd cmd;
  cmd.id = id;
  cmd.steps = std::move(steps);
  cmd.warp = warp;
  c.queue.push_back(std::move(cmd));
  Ticket t;
  t.id = id;
  t.line = line;
  c.tickets.push_back(std::move(t));
  while (c.tickets.size() > kMaxTickets) c.tickets.pop_front();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] submit ticket=%llu \"%s\"", static_cast<unsigned long long>(id),
              line.c_str());
  return id;
}

std::string devToolsReadoutJson() {
  J out = J::obj();
  out.set("schema", J::str("devtools_readout/1"));
  J tools = J::arr();
  size_t n = 0;
  const DevToolInfo* tab = devToolTable(&n);
  const uint32_t mask = devToolMask();
  for (size_t i = 0; i < n; i++)
    if (mask & (1u << i)) tools.push(J::str(tab[i].name));
  out.set("tools", std::move(tools));
  out.set("run_class", J::str(devToolsAnyDevtoolClass() ? "dev-tool-assisted" : (mask ? "natural+hud" : "natural")));
  out.set("acts", J::num(static_cast<int64_t>(devToolActs())));
  out.set("freecam", J::str(g_freecamRoute.load() ? "on" : "off"));
  Chan& c = chan();
  std::lock_guard<std::mutex> lk(c.mu);
  J tickets = J::arr();
  for (const auto& t : c.tickets) {
    J r = J::obj();
    r.set("id", J::num(static_cast<int64_t>(t.id)));
    r.set("line", J::str(t.line));
    r.set("status", J::str(t.status));
    r.set("event", t.event);
    tickets.push(std::move(r));
  }
  out.set("commands", std::move(tickets));
  std::string s = out.dump();
  // the tick thread's host copy of World::cheatsReport, spliced in as raw JSON
  s.insert(s.size() - 1, ", \"cheats\": " + c.cheatsJson);
  J am = J::obj();
  am.set("known", J::boolean(c.active.known));
  am.set("active", J::boolean(c.active.active));
  am.set("idx", J::num(c.active.idx));
  am.set("flow", J::str(c.active.flow));
  s.insert(s.size() - 1, ", \"active_mission\": " + am.dump());
  return s;
}

DevToolsActiveMission devToolsActiveMission() {
  Chan& c = chan();
  std::lock_guard<std::mutex> lk(c.mu);
  return c.active;
}

bool devToolsFreecamRouting() { return g_freecamRoute.load(std::memory_order_relaxed); }
}  // namespace kernel
}  // namespace sr

// ---------------------------------------------------------------------------
// The tick hooks. Gate off = an exact pass-through.
// ---------------------------------------------------------------------------
void sub_8223EB08(PPCContext& __restrict ctx, uint8_t* base) {
  if (!sr::kernel::devToolHookNeeded_sub_8223EB08()) {
    __imp__sub_8223EB08(ctx, base);
    return;
  }
  onTick(ctx, base, /*active=*/true);
  __imp__sub_8223EB08(ctx, base);
}

void sub_8223ECC0(PPCContext& __restrict ctx, uint8_t* base) {
  if (!sr::kernel::devToolHookNeeded_sub_8223ECC0()) {
    __imp__sub_8223ECC0(ctx, base);
    return;
  }
  onTick(ctx, base, /*active=*/false);
  __imp__sub_8223ECC0(ctx, base);
}

void sub_8223F230(PPCContext& __restrict ctx, uint8_t* base) {
  if (!sr::kernel::devToolHookNeeded_sub_8223F230()) {
    __imp__sub_8223F230(ctx, base);
    return;
  }
  onTick(ctx, base, /*active=*/false);
  __imp__sub_8223F230(ctx, base);
}
